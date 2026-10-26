#!/usr/bin/env python3
"""pumpkin_desktop — pannello desktop portatile per la zucca di Halloween.

GUI Tkinter (solo stdlib) con cinque schede:
  Impostazioni, Build, Flash, Monitor, OTA.

Dipendenze opzionali (vedi requisiti.txt):
  - pyserial  : abilita la scheda "Monitor" e l'invio della CA via seriale;
  - paho-mqtt : abilita pubblicazione/lettura MQTT nella scheda "OTA".

Tutto il resto è stdlib. Nessuna scrittura fuori da tools/desktop/:
config.json, monitor.log, cert.pem e key.pem vivono accanto a questo script.

Selftest headless (non apre la GUI):
  python3 pumpkin_desktop.py --selftest
"""

from __future__ import annotations

import functools
import glob
import importlib.util
import json
import queue
import re
import shlex
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time
from collections import deque
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# ---------------------------------------------------------------------------
# Costanti e percorsi (portabilità: tutto accanto allo script)
# ---------------------------------------------------------------------------

APP_DIR = Path(__file__).resolve().parent
CONFIG_PATH = APP_DIR / "config.json"
MONITOR_LOG_PATH = APP_DIR / "monitor.log"
CERT_PATH = APP_DIR / "cert.pem"
KEY_PATH = APP_DIR / "key.pem"

DEFAULT_REPO_PATH = str(APP_DIR.parent.parent)
DEFAULT_IDF_PATH = str(Path.home() / "esp" / "esp-idf")

OTA_SLOT_KB = 1984
OTA_SLOT_BYTES = OTA_SLOT_KB * 1024

DEFAULT_CONFIG = {
    "repo_path": DEFAULT_REPO_PATH,
    "idf_path": DEFAULT_IDF_PATH,
    "serial_port": "",
    "console_baud": 115200,
    "flash_baud": 921600,
    "mqtt_host": "127.0.0.1",
    "mqtt_port": 1883,
    "device_id": "",
    "https_port": 8443,
    "ota_margin_pct": 15,
}

# Chiavi salvate su disco (le credenziali MQTT NON si salvano mai).
SAVED_KEYS = tuple(DEFAULT_CONFIG)


# ---------------------------------------------------------------------------
# Funzioni pure (usate anche dal selftest headless)
# ---------------------------------------------------------------------------

_INT_KEYS = ("console_baud", "flash_baud", "mqtt_port", "https_port", "ota_margin_pct")


def load_config(path=CONFIG_PATH):
    """Carica config.json; valori mancanti/non validi tornano ai default."""
    cfg = dict(DEFAULT_CONFIG)
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        data = None
    if isinstance(data, dict):
        for key in DEFAULT_CONFIG:
            if isinstance(data.get(key), str):
                cfg[key] = data[key]
            elif data.get(key) is not None:
                cfg[key] = data[key]
    for key in _INT_KEYS:
        try:
            cfg[key] = int(cfg[key])
        except (TypeError, ValueError):
            cfg[key] = int(DEFAULT_CONFIG[key])
    return cfg


def save_config(cfg, path=CONFIG_PATH):
    """Salva su disco solo le chiavi non segrete; le credenziali restano in RAM."""
    data = {key: cfg.get(key, DEFAULT_CONFIG[key]) for key in SAVED_KEYS}
    path = Path(path)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def detect_serial_ports():
    """Autodetect porte USB seriali (/dev/ttyUSB* e /dev/ttyACM*)."""
    return sorted(set(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")))


def _idf_source_prefix(idf_path):
    return f"source {shlex.quote(str(Path(idf_path) / 'export.sh'))}"


def _idf_base(repo_path, idf_path):
    build_dir = shlex.quote(str(Path(repo_path) / "build"))
    return f"{_idf_source_prefix(idf_path)} && idf.py -B {build_dir}"


def build_cmd_build(repo_path, idf_path):
    return f"{_idf_base(repo_path, idf_path)} build"


def build_cmd_size(repo_path, idf_path):
    return f"{_idf_base(repo_path, idf_path)} size"


def build_cmd_clean(repo_path, idf_path):
    return f"{_idf_base(repo_path, idf_path)} fullclean"


def build_cmd_flash(repo_path, idf_path, port, baud):
    return f"{_idf_base(repo_path, idf_path)} -p {shlex.quote(str(port))} -b {int(baud)} flash"


def build_cmd_erase(repo_path, idf_path, port):
    return f"{_idf_base(repo_path, idf_path)} -p {shlex.quote(str(port))} erase-flash"


def validate_ota_url(url):
    """Accetta solo URL https (il dispositivo si fida solo del certificato installato)."""
    url = (url or "").strip()
    if not url.startswith("https://") or len(url) <= len("https://"):
        raise ValueError(
            "URL OTA non valido: deve iniziare con https:// "
            "(il dispositivo accetta solo il certificato CA installato)."
        )
    return url


def build_update_payload(url, version=None, release_summary=None, title=None):
    """Costruisce il payload JSON del comando pumpkin/<id>/set/update (minimo: url)."""
    payload = {"url": validate_ota_url(url)}
    for key, value in (("version", version), ("release_summary", release_summary), ("title", title)):
        if value and str(value).strip():
            payload[key] = str(value).strip()
    return payload


def ota_build_dir(repo_path):
    return Path(repo_path) / "build"


def ota_bin_path(repo_path):
    return ota_build_dir(repo_path) / "pumpkin.bin"


def device_topic(device_id, suffix):
    return f"pumpkin/{device_id}/{suffix}"


def event_topics(device_id):
    base = f"pumpkin/{device_id}"
    return [
        f"{base}/update/status",
        f"{base}/status/reset",
        f"{base}/status/stability",
        f"{base}/status/tasks",
        f"{base}/availability",
    ]


def ota_url(ip, port):
    return f"https://{ip}:{int(port)}/pumpkin.bin"


_SIZE_HEX_PATTERNS = (
    re.compile(r"Binary size:\s*(0x[0-9a-fA-F]+|\d+)\s*bytes"),
    re.compile(r"Total image size:\s*(0x[0-9a-fA-F]+|\d+)\s*bytes"),
)
_SIZE_HUMAN_PATTERN = re.compile(r"Total image size:\s*([\d.]+)\s*(KB|MB)")


def parse_idf_size_output(text):
    """Estrae (byte immagine, % slot usato, % margine) dall'output di `idf.py size`.

    Torna None se l'output non contiene una dimensione riconoscibile.
    """
    if not text:
        return None
    size = None
    for pattern in _SIZE_HEX_PATTERNS:
        match = pattern.search(text)
        if match:
            token = match.group(1)
            size = int(token, 16) if token.lower().startswith("0x") else int(token)
            break
    if size is None:
        match = _SIZE_HUMAN_PATTERN.search(text)
        if match:
            value = float(match.group(1))
            size = int(value * 1024 * 1024 if match.group(2) == "MB" else value * 1024)
    if size is None:
        return None
    used_pct = size / OTA_SLOT_BYTES * 100.0
    return {
        "size_bytes": size,
        "slot_used_pct": round(used_pct, 1),
        "slot_margin_pct": round(100.0 - used_pct, 1),
    }


def human_bytes(num):
    value = float(num)
    if value >= 1024 * 1024:
        return f"{value / 1024 / 1024:.2f} MB"
    if value >= 1024:
        return f"{value / 1024:.1f} KB"
    return f"{int(value)} B"


def local_lan_ip(timeout=0.5):
    """IP sulla LAN principale (socket connect, senza traffico); fallback 127.0.0.1."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(timeout)
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


def ensure_selfsigned_cert(cert_path=CERT_PATH, key_path=KEY_PATH):
    """Genera una volta sola cert.pem/key.pem con openssl; le riusa se presenti."""
    if cert_path.exists() and key_path.exists():
        return cert_path, key_path
    if shutil.which("openssl") is None:
        raise RuntimeError(
            "openssl non trovato sul sistema: installarlo (es. 'sudo apt install openssl') "
            "per generare il certificato HTTPS del server OTA."
        )
    cmd = [
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "825",
        "-subj", "/CN=pumpkin-ota",
        "-keyout", str(key_path), "-out", str(cert_path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if proc.returncode != 0 or not cert_path.exists():
        detail = (proc.stderr or proc.stdout or "").strip()[:400]
        raise RuntimeError(f"Generazione del certificato fallita: {detail}")
    return cert_path, key_path


def serial_available():
    return importlib.util.find_spec("serial") is not None


def paho_available():
    return importlib.util.find_spec("paho") is not None


# ---------------------------------------------------------------------------
# Monitor seriale (niente GUI: riutilizzato da Monitor e OTA)
# ---------------------------------------------------------------------------

class SerialMonitor:
    """Porta seriale con thread lettore, buffer righe recenti e log opzionale."""

    def __init__(self, emit):
        self._emit = emit
        self._lock = threading.Lock()
        self._ser = None
        self._thread = None
        self._running = False
        self._logfile = None
        self._buf = ""
        self.recent = deque(maxlen=600)

    @property
    def is_active(self):
        return self._running and self._ser is not None

    def start(self, port, baud=115200, logfile=None):
        with self._lock:
            if self._running:
                raise RuntimeError("Il monitor seriale è già attivo.")
            try:
                import serial  # pyserial
            except ImportError:
                raise RuntimeError(
                    "pyserial non installato: eseguire 'pip3 install pyserial' (vedi requisiti.txt)."
                )
            try:
                ser = serial.Serial(
                    port=port,
                    baudrate=int(baud),
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE,
                    timeout=0.3,
                )
            except Exception as exc:
                raise RuntimeError(f"Impossibile aprire {port}: {exc}")
            self._ser = ser
            self._buf = ""
            self._running = True
            if logfile:
                self._logfile = open(logfile, "a", encoding="utf-8", errors="replace")
            self._thread = threading.Thread(target=self._reader, name="serial-reader", daemon=True)
            self._thread.start()

    def stop(self):
        with self._lock:
            self._running = False
        thread = self._thread
        if thread is not None and thread.is_alive():
            thread.join(timeout=2.0)
        self._close_resources()

    def send_line(self, text):
        with self._lock:
            if not self.is_active:
                raise RuntimeError("Monitor seriale non attivo.")
            data = (text.rstrip("\n") + "\n").encode("utf-8")
            try:
                self._ser.write(data)
                self._ser.flush()
            except Exception as exc:
                raise RuntimeError(f"Scrittura su porta seriale fallita: {exc}")

    def _reader(self):
        while True:
            with self._lock:
                ser = self._ser
                running = self._running
            if not running or ser is None:
                break
            try:
                data = ser.read(512)
            except Exception as exc:
                self._feed(f"\n[monitor] porta chiusa: {exc}\n")
                self._emit(f"\n[monitor] porta chiusa: {exc}\n")
                break
            if data:
                text = data.decode("utf-8", errors="replace")
                self._feed(text)
                self._emit(text)
        self._close_resources()

    def _feed(self, text):
        self._buf += text
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            self.recent.append(line.rstrip("\r"))
        if self._logfile is not None:
            try:
                self._logfile.write(text)
                self._logfile.flush()
            except Exception:
                pass

    def _close_resources(self):
        with self._lock:
            self._running = False
            if self._ser is not None:
                try:
                    self._ser.close()
                except Exception:
                    pass
                self._ser = None
            if self._logfile is not None:
                try:
                    self._logfile.close()
                except Exception:
                    pass
                self._logfile = None


# ---------------------------------------------------------------------------
# Server HTTPS locale per l'OTA
# ---------------------------------------------------------------------------

class _OtaFileHandler(SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log = getattr(self.server, "app_log", None)
        if log is not None:
            log("OTA: " + (fmt % args))


class OtaServer:
    """Serve la directory build/ su HTTPS (pumpkin.bin raggiungibile su /pumpkin.bin)."""

    def __init__(self, build_dir, port, log):
        self.build_dir = Path(build_dir)
        self.port = int(port)
        self._log = log
        self._httpd = None
        self._thread = None

    @property
    def is_running(self):
        return self._httpd is not None

    def start(self, certfile, keyfile):
        handler = functools.partial(_OtaFileHandler, directory=str(self.build_dir))
        self._httpd = ThreadingHTTPServer(("0.0.0.0", self.port), handler)
        self._httpd.app_log = self._log
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=str(certfile), keyfile=str(keyfile))
        self._httpd.socket = context.wrap_socket(self._httpd.socket, server_side=True)
        self._thread = threading.Thread(target=self._httpd.serve_forever, name="ota-https", daemon=True)
        self._thread.start()

    def stop(self):
        if self._httpd is not None:
            try:
                self._httpd.shutdown()
                self._httpd.server_close()
            except Exception:
                pass
            self._httpd = None
        thread = self._thread
        if thread is not None and thread.is_alive():
            thread.join(timeout=3.0)
        self._thread = None


# ---------------------------------------------------------------------------
# Client MQTT (paho, import condizionato)
# ---------------------------------------------------------------------------

class MqttLink:
    """Connessione MQTT pulita senza LWT; callback inoltrate al chiamante."""

    def __init__(self):
        self._cli = None

    @property
    def connected(self):
        return self._cli is not None

    def connect(self, host, port, username=None, password=None, on_message=None, on_status=None, topics=()):
        import paho.mqtt.client as mqtt

        try:
            cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id="pumpkin-desktop", clean_session=True)
        except (AttributeError, TypeError):
            cli = mqtt.Client(client_id="pumpkin-desktop", clean_session=True)
        if on_message is not None:
            cli.on_message = on_message

        def _on_connect(client, userdata, flags, rc, *args, **kwargs):
            if rc == 0:
                for topic in topics:
                    client.subscribe(topic)
            if on_status is not None:
                on_status(rc)

        cli.on_connect = _on_connect
        if username:
            cli.username_pw_set(username, password or None)
        cli.connect(host, int(port), keepalive=30)
        cli.loop_start()
        self._cli = cli

    def publish(self, topic, payload, retain=False, timeout=5.0):
        if self._cli is None:
            raise RuntimeError("MQTT non connesso.")
        info = self._cli.publish(topic, payload, retain=retain)
        info.wait_for_publish(timeout)
        if info.rc != 0:
            raise RuntimeError(f"Pubblicazione fallita (rc={info.rc}).")

    def disconnect(self):
        if self._cli is not None:
            try:
                self._cli.disconnect()
                self._cli.loop_stop()
            except Exception:
                pass
            self._cli = None


# ---------------------------------------------------------------------------
# Selftest headless (nessuna GUI)
# ---------------------------------------------------------------------------

SAMPLE_SIZE_OUTPUT = """\
Toolchain type: esp32 ...
Total sizes:
 Used static DRAM:   30192 bytes ( 197680 remain,  208512 available)
 Used static IRAM:    8782 bytes ( 107650 remain)
      Flash code:   158568 bytes
  Flash rodata:     96324 bytes
 Total image size: 1329440 bytes (.bin file)

Project build complete.
Binary size: 0x144920 bytes. Free space on ota_0 partition.
"""


def run_selftest():
    """Verifiche headless: config, payload OTA, comandi, parser size."""
    checks_ok = 0
    checks_failed = 0

    def check(name, condition, detail=""):
        nonlocal checks_ok, checks_failed
        if condition:
            checks_ok += 1
            print(f"[OK]      {name}")
        else:
            checks_failed += 1
            print(f"[FALLITO] {name} {detail}")

    import tempfile

    # (1) config.json roundtrip + default su file assente
    with tempfile.TemporaryDirectory() as tmp:
        cfg_path = Path(tmp) / "config.json"
        cfg = dict(DEFAULT_CONFIG)
        cfg.update({"repo_path": "/tmp/repo", "mqtt_port": 1884, "ota_margin_pct": 20,
                    "console_baud": "115200"})
        save_config(cfg, cfg_path)
        back = load_config(cfg_path)
        check("config: roundtrip save/load",
              back["repo_path"] == "/tmp/repo" and back["mqtt_port"] == 1884
              and back["ota_margin_pct"] == 20 and back["console_baud"] == 115200, repr(back))
        check("config: default su file assente",
              load_config(Path(tmp) / "inesistente.json")["console_baud"] == 115200)
        saved = json.loads(cfg_path.read_text(encoding="utf-8"))
        check("config: credenziali non salvate su disco",
              "mqtt_user" not in saved and "mqtt_pass" not in saved, repr(sorted(saved)))

    # (2) payload set/update + validazione URL https-only
    payload = build_update_payload("https://192.168.1.50:8443/pumpkin.bin", "v1.0.1",
                                   "Correzione soglia radar", "Aggiornamento zucca")
    check("payload: campi completi",
          payload["url"] == "https://192.168.1.50:8443/pumpkin.bin"
          and payload["version"] == "v1.0.1"
          and payload["release_summary"] == "Correzione soglia radar"
          and payload["title"] == "Aggiornamento zucca", repr(payload))
    check("payload: serializzabile JSON", isinstance(json.dumps(payload, ensure_ascii=False), str))
    check("payload: minimo solo url",
          build_update_payload("https://x.example/pumpkin.bin") == {"url": "https://x.example/pumpkin.bin"})
    for bad in ("http://192.168.1.50:8443/pumpkin.bin", "ftp://x/y.bin", "", None):
        try:
            build_update_payload(bad)
            raised = False
        except ValueError:
            raised = True
        check(f"payload: URL rifiutato {bad!r}", raised)

    # (3) comandi build/flash/erase/clean
    cmd_build = build_cmd_build("/repo", "~/esp/esp-idf")
    check("cmd build", "export.sh" in cmd_build and "idf.py" in cmd_build
          and "-B" in cmd_build and cmd_build.rstrip().endswith("build"), repr(cmd_build))
    cmd_flash = build_cmd_flash("/repo", "~/esp/esp-idf", "/dev/ttyUSB0", 921600)
    check("cmd flash", "/dev/ttyUSB0" in cmd_flash and "921600" in cmd_flash
          and cmd_flash.rstrip().endswith("flash"), repr(cmd_flash))
    check("cmd erase-flash",
          build_cmd_erase("/repo", "~/esp/esp-idf", "/dev/ttyUSB0").rstrip().endswith("erase-flash"))
    check("cmd fullclean",
          build_cmd_clean("/repo", "~/esp/esp-idf").rstrip().endswith("fullclean"))

    # (4) parser output idf.py size (esempio incastonato)
    expected_bytes = int("144920", 16)
    expected_used = expected_bytes / OTA_SLOT_BYTES * 100.0
    result = parse_idf_size_output(SAMPLE_SIZE_OUTPUT)
    check("parser size: byte, % usato, % margine",
          result is not None and result["size_bytes"] == expected_bytes
          and abs(result["slot_used_pct"] - expected_used) < 0.11
          and abs(result["slot_margin_pct"] - (100.0 - expected_used)) < 0.11, repr(result))
    result_mb = parse_idf_size_output("Total image size: 1.27 MB (.bin file)")
    check("parser size: forma umanizzata MB",
          result_mb is not None and abs(result_mb["size_bytes"] - 1.27 * 1024 * 1024) < 2, repr(result_mb))
    check("parser size: nessuna dimensione -> None", parse_idf_size_output("build completata") is None)

    print()
    print(f"SELFTEST: {checks_ok} verifiche OK, {checks_failed} fallite.")
    return 0 if checks_failed == 0 else 1


# ---------------------------------------------------------------------------
# GUI (import lazy di tkinter: --selftest funziona anche senza python3-tk)
# ---------------------------------------------------------------------------

tk = ttk = filedialog = messagebox = simpledialog = None


def ensure_tk():
    global tk, ttk, filedialog, messagebox, simpledialog
    try:
        import tkinter as tk_mod
        from tkinter import ttk as ttk_mod, filedialog as fd_mod, messagebox as mb_mod, simpledialog as sd_mod
    except ImportError:
        print(
            "tkinter non disponibile: installare il pacchetto python3-tk "
            "(es. 'sudo apt install python3-tk').",
            file=sys.stderr,
        )
        print(
            "Il selftest headless funziona comunque: python3 pumpkin_desktop.py --selftest",
            file=sys.stderr,
        )
        sys.exit(2)
    tk, ttk = tk_mod, ttk_mod
    filedialog, messagebox, simpledialog = fd_mod, mb_mod, sd_mod


def _text_append(widget, text):
    widget.configure(state="normal")
    widget.insert("end", text)
    widget.see("end")
    widget.configure(state="disabled")


def _button_state(widget, enabled):
    widget.configure(state="normal" if enabled else "disabled")


class TabBase:
    """Scheda del Notebook: costruisce il proprio frame e registra il titolo."""

    title = ""

    def __init__(self, app, notebook):
        self.app = app
        self.frame = ttk.Frame(notebook, padding=8)
        notebook.add(self.frame, text=self.title)
        self._build()

    def _build(self):
        raise NotImplementedError


class ImpostazioniTab(TabBase):
    title = "Impostazioni"

    def _build(self):
        cfg = self.app.cfg
        pad = {"padx": 4, "pady": 3}
        self._entries = {}

        def add_row(row, label, widget, extra=None):
            ttk.Label(self.frame, text=label).grid(row=row, column=0, sticky="w", **pad)
            widget.grid(row=row, column=1, sticky="we", **pad)
            if extra is not None:
                extra.grid(row=row, column=2, sticky="w", **pad)

        self.frame.columnconfigure(1, weight=1)

        entry_repo = ttk.Entry(self.frame)
        entry_repo.insert(0, cfg["repo_path"])
        add_row(0, "Percorso repo", entry_repo,
                ttk.Button(self.frame, text="Sfoglia…", command=self._browse_repo))
        self._entries["repo_path"] = entry_repo

        entry_idf = ttk.Entry(self.frame)
        entry_idf.insert(0, cfg["idf_path"])
        add_row(1, "Percorso ESP-IDF", entry_idf,
                ttk.Button(self.frame, text="Sfoglia…", command=self._browse_idf))
        self._entries["idf_path"] = entry_idf

        ports = detect_serial_ports()
        if cfg["serial_port"] and cfg["serial_port"] not in ports:
            ports = [cfg["serial_port"]] + ports
        combo_port = ttk.Combobox(self.frame, values=ports)
        combo_port.set(cfg["serial_port"])
        add_row(2, "Porta USB", combo_port,
                ttk.Button(self.frame, text="Aggiorna", command=self._refresh_ports))
        self._entries["serial_port"] = combo_port

        for row, (key, label, width) in enumerate((
            ("console_baud", "Baud console", 10),
            ("flash_baud", "Baud flash", 10),
        ), start=3):
            entry = ttk.Entry(self.frame, width=width)
            entry.insert(0, str(cfg[key]))
            add_row(row, label, entry)
            self._entries[key] = entry

        entry_host = ttk.Entry(self.frame)
        entry_host.insert(0, str(cfg["mqtt_host"]))
        add_row(5, "Broker MQTT — host", entry_host)
        self._entries["mqtt_host"] = entry_host

        entry_mport = ttk.Entry(self.frame, width=10)
        entry_mport.insert(0, str(cfg["mqtt_port"]))
        add_row(6, "Broker MQTT — porta", entry_mport)
        self._entries["mqtt_port"] = entry_mport

        self._w_user = ttk.Entry(self.frame)
        self._w_pass = ttk.Entry(self.frame, show="*")
        add_row(7, "Broker MQTT — utente (opz., non salvato)", self._w_user)
        add_row(8, "Broker MQTT — password (opz., non salvata)", self._w_pass)

        entry_dev = ttk.Entry(self.frame)
        entry_dev.insert(0, str(cfg["device_id"]))
        add_row(9, "Device id zucca (vuoto = chiedi all'atto OTA)", entry_dev)
        self._entries["device_id"] = entry_dev

        entry_hport = ttk.Entry(self.frame, width=10)
        entry_hport.insert(0, str(cfg["https_port"]))
        add_row(10, "Porta server HTTPS locale", entry_hport)
        self._entries["https_port"] = entry_hport

        entry_margin = ttk.Entry(self.frame, width=10)
        entry_margin.insert(0, str(cfg["ota_margin_pct"]))
        add_row(11, "Soglia margine slot OTA (%)", entry_margin)
        self._entries["ota_margin_pct"] = entry_margin

        ttk.Button(self.frame, text="Salva", command=self._save).grid(
            row=12, column=1, sticky="w", **pad)
        self._save_status = ttk.Label(self.frame, text="")
        self._save_status.grid(row=13, column=0, columnspan=3, sticky="w", **pad)

    def _browse_repo(self):
        chosen = filedialog.askdirectory(initialvalue=self._entries["repo_path"].get() or str(Path.home()),
                                         title="Seleziona la directory del repo")
        if chosen:
            self._entries["repo_path"].delete(0, "end")
            self._entries["repo_path"].insert(0, chosen)

    def _browse_idf(self):
        chosen = filedialog.askdirectory(initialvalue=self._entries["idf_path"].get() or str(Path.home()),
                                         title="Seleziona la directory di ESP-IDF")
        if chosen:
            self._entries["idf_path"].delete(0, "end")
            self._entries["idf_path"].insert(0, chosen)

    def _refresh_ports(self):
        ports = detect_serial_ports()
        combo = self._entries["serial_port"]
        current = combo.get()
        if current and current not in ports:
            ports = [current] + ports
        combo.configure(values=ports)
        if ports and not current:
            combo.set(ports[0])
        self.app.set_status("Porte seriali: " + (", ".join(ports) if ports else "nessuna trovata"))

    def collect(self):
        """Raccoglie i valori correnti (incluse credenziali NON persistenti)."""
        cfg = dict(self.app.cfg)
        for key, widget in self._entries.items():
            cfg[key] = widget.get().strip()
        for key in _INT_KEYS:
            try:
                cfg[key] = int(cfg[key])
            except (TypeError, ValueError):
                cfg[key] = int(DEFAULT_CONFIG[key])
        cfg["mqtt_user"] = self._w_user.get().strip()
        cfg["mqtt_pass"] = self._w_pass.get()
        return cfg

    def _save(self):
        if self.app.apply_config(self.collect()):
            self._save_status.configure(
                text=f"Salvato in {CONFIG_PATH} (credenziali MQTT non incluse).",
                foreground="green")


class BuildTab(TabBase):
    title = "Build"

    def _build(self):
        bar = ttk.Frame(self.frame)
        bar.pack(fill="x", pady=(0, 4))
        self.btn_build = ttk.Button(bar, text="Compila", command=self._on_build)
        self.btn_build.pack(side="left", padx=2)
        self.btn_clean = ttk.Button(bar, text="Pulisci build", command=self._on_clean)
        self.btn_clean.pack(side="left", padx=2)
        self.size_var = ttk.Label(self.frame, text="Dimensione binario: n/d")
        self.size_var.pack(fill="x", pady=2)

        wrap = ttk.Frame(self.frame)
        wrap.pack(fill="both", expand=True)
        self.text = tk.Text(wrap, height=18, wrap="none", state="disabled")
        scroll = ttk.Scrollbar(wrap, command=self.text.yview)
        self.text.configure(yscrollcommand=scroll.set)
        self.text.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")

    def _append(self, line):
        _text_append(self.text, line if line.endswith("\n") else line + "\n")

    def _set_busy(self, busy):
        state = "disabled" if busy else "normal"
        self.btn_build.configure(state=state)
        self.btn_clean.configure(state=state)

    def _run_stream(self, cmd, on_done, sink=None):
        cfg = self.app.current_config()
        repo = cfg["repo_path"]

        def worker():
            returncode = -1
            try:
                proc = subprocess.Popen(
                    cmd, cwd=repo, shell=True, executable="/bin/bash",
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, bufsize=1, errors="replace",
                )
                for line in proc.stdout:
                    if sink is not None:
                        sink.append(line)
                    self.app.post(self._append, line.rstrip("\n"))
                returncode = proc.wait()
            except Exception as exc:
                self.app.post(self._append, f"ERRORE: {exc}")
            self.app.post(on_done, returncode)

        threading.Thread(target=worker, name="build-worker", daemon=True).start()

    def _on_build(self):
        cfg = self.app.current_config()
        self._set_busy(True)
        self.size_var.configure(text="Compilazione in corso…")
        self.app.set_status("idf.py build avviato.")
        self._run_stream(build_cmd_build(cfg["repo_path"], cfg["idf_path"]), self._build_done)

    def _build_done(self, returncode):
        self._set_busy(False)
        if returncode != 0:
            self.size_var.configure(text="Build fallita.", foreground="red")
            self.app.set_status("idf.py build: fallito.")
            return
        self.app.set_status("Build completata; lettura dimensioni con idf.py size…")
        cfg = self.app.current_config()
        self._set_busy(True)
        self._size_buffer = []
        self._run_stream(build_cmd_size(cfg["repo_path"], cfg["idf_path"]),
                         self._size_done, sink=self._size_buffer)

    def _size_done(self, returncode):
        self._set_busy(False)
        cfg = self.app.current_config()
        threshold = int(cfg["ota_margin_pct"])
        result = parse_idf_size_output("\n".join(getattr(self, "_size_buffer", [])))
        if returncode != 0 or result is None:
            self.size_var.configure(text="Dimensione binario: non disponibile (idf.py size senza output utile).",
                                    foreground="black")
            self.app.set_status("Build completata.")
            return
        warn = result["slot_margin_pct"] < threshold
        self.size_var.configure(
            text=(f"pumpkin.bin: {human_bytes(result['size_bytes'])} — slot ota_0: "
                  f"{result['slot_used_pct']}% usato, margine {result['slot_margin_pct']}% "
                  f"(soglia {threshold}%)" + ("  ⚠ MARGINE SOTTO SOGLIA" if warn else "")),
            foreground="red" if warn else "green")
        self.app.set_status("Build completata.")

    def _on_clean(self):
        cfg = self.app.current_config()
        if not messagebox.askyesno("Pulisci build", "Eliminare l'intera directory build (fullclean)?"):
            return
        self._set_busy(True)
        self.app.set_status("idf.py fullclean avviato.")
        self._run_stream(build_cmd_clean(cfg["repo_path"], cfg["idf_path"]), self._clean_done)

    def _clean_done(self, returncode):
        self._set_busy(False)
        self.app.set_status("fullclean completato." if returncode == 0
                            else f"fullclean fallito (rc={returncode}).")


class FlashTab(TabBase):
    title = "Flash"

    def _build(self):
        bar = ttk.Frame(self.frame)
        bar.pack(fill="x", pady=(0, 4))
        self.btn_flash = ttk.Button(bar, text="Flash", command=self._on_flash)
        self.btn_flash.pack(side="left", padx=2)
        self.btn_erase = ttk.Button(bar, text="Cancella flash", command=self._on_erase)
        self.btn_erase.pack(side="left", padx=2)
        self.info_var = ttk.Label(self.frame, text="")
        self.info_var.pack(fill="x", pady=2)

        wrap = ttk.Frame(self.frame)
        wrap.pack(fill="both", expand=True)
        self.text = tk.Text(wrap, height=20, wrap="none", state="disabled")
        scroll = ttk.Scrollbar(wrap, command=self.text.yview)
        self.text.configure(yscrollcommand=scroll.set)
        self.text.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")

    def _append(self, line):
        _text_append(self.text, line if line.endswith("\n") else line + "\n")

    def _set_busy(self, busy):
        state = "disabled" if busy else "normal"
        self.btn_flash.configure(state=state)
        self.btn_erase.configure(state=state)

    def _check_port(self, cfg):
        if not cfg["serial_port"]:
            messagebox.showerror("Porta USB", "Impostare la porta USB nella scheda Impostazioni.")
            return False
        return True

    def _run_stream(self, cmd, on_done):
        cfg = self.app.current_config()
        repo = cfg["repo_path"]

        def worker():
            returncode = -1
            try:
                proc = subprocess.Popen(
                    cmd, cwd=repo, shell=True, executable="/bin/bash",
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, bufsize=1, errors="replace",
                )
                for line in proc.stdout:
                    self.app.post(self._append, line.rstrip("\n"))
                returncode = proc.wait()
            except Exception as exc:
                self.app.post(self._append, f"ERRORE: {exc}")
            self.app.post(on_done, returncode)

        threading.Thread(target=worker, name="flash-worker", daemon=True).start()

    def _on_flash(self):
        cfg = self.app.current_config()
        if not self._check_port(cfg):
            return
        self._set_busy(True)
        self.info_var.configure(text=f"Flash su {cfg['serial_port']} @ {cfg['flash_baud']}…")
        self.app.set_status("idf.py flash avviato.")
        self._run_stream(
            build_cmd_flash(cfg["repo_path"], cfg["idf_path"], cfg["serial_port"], cfg["flash_baud"]),
            self._flash_done)

    def _flash_done(self, returncode):
        self._set_busy(False)
        ok = returncode == 0
        self.info_var.configure(text="Flash completata." if ok else f"Flash fallita (rc={returncode}).",
                                foreground="green" if ok else "red")
        self.app.set_status("Flash: " + ("completata." if ok else "fallita."))

    def _on_erase(self):
        cfg = self.app.current_config()
        if not self._check_port(cfg):
            return
        if not messagebox.askyesno(
                "Cancella flash",
                "Cancellare l'intera flash del dispositivo?\n"
                "Vengono rimossi anche firmware e configurazione NVS (provisioning compreso)."):
            return
        self._set_busy(True)
        self.info_var.configure(text="erase-flash in corso…")
        self.app.set_status("idf.py erase-flash avviato.")
        self._run_stream(
            build_cmd_erase(cfg["repo_path"], cfg["idf_path"], cfg["serial_port"]),
            self._erase_done)

    def _erase_done(self, returncode):
        self._set_busy(False)
        ok = returncode == 0
        self.info_var.configure(text="Flash cancellata." if ok else f"erase-flash fallito (rc={returncode}).",
                                foreground="green" if ok else "red")
        self.app.set_status("erase-flash: " + ("completato." if ok else "fallito."))


class MonitorTab(TabBase):
    title = "Monitor"

    def _build(self):
        self.monitor = self.app.monitor
        bar = ttk.Frame(self.frame)
        bar.pack(fill="x", pady=(0, 4))
        self.btn_start = ttk.Button(bar, text="Avvia", command=self._on_start)
        self.btn_start.pack(side="left", padx=2)
        self.btn_stop = ttk.Button(bar, text="Ferma", command=self._on_stop, state="disabled")
        self.btn_stop.pack(side="left", padx=2)
        self.log_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text=f"Log su file ({MONITOR_LOG_PATH.name})",
                        variable=self.log_var).pack(side="left", padx=8)

        self.status_var = tk.StringVar(value="Monitor fermo.")
        ttk.Label(self.frame, textvariable=self.status_var).pack(fill="x")

        wrap = ttk.Frame(self.frame)
        wrap.pack(fill="both", expand=True)
        self.text = tk.Text(wrap, height=20, wrap="none", state="disabled")
        scroll = ttk.Scrollbar(wrap, command=self.text.yview)
        self.text.configure(yscrollcommand=scroll.set)
        self.text.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")

        send_bar = ttk.Frame(self.frame)
        send_bar.pack(fill="x", pady=(4, 0))
        ttk.Label(send_bar, text="Comando:").pack(side="left")
        self.send_entry = ttk.Entry(send_bar)
        self.send_entry.pack(side="left", fill="x", expand=True, padx=4)
        self.send_entry.bind("<Return>", lambda _event: self._on_send())
        self.btn_send = ttk.Button(send_bar, text="Invia", command=self._on_send, state="disabled")
        self.btn_send.pack(side="left")

        if not serial_available():
            self.status_var.set(
                "pyserial non installato: scheda disabilitata. "
                "Installare con 'pip3 install pyserial' (vedi requisiti.txt) e riavviare l'app.")
            for widget in (self.btn_start, self.btn_stop, self.btn_send, self.send_entry):
                widget.configure(state="disabled")

    def append_text(self, text):
        _text_append(self.text, text)

    def _set_running(self, running):
        _button_state(self.btn_start, not running)
        _button_state(self.btn_stop, running)
        _button_state(self.btn_send, running)
        _button_state(self.send_entry, running)

    def _on_start(self):
        cfg = self.app.current_config()
        if not cfg["serial_port"]:
            messagebox.showerror("Monitor", "Impostare la porta USB nella scheda Impostazioni.")
            return
        logfile = MONITOR_LOG_PATH if self.log_var.get() else None

        def worker():
            try:
                self.monitor.start(cfg["serial_port"], cfg["console_baud"], logfile)
            except Exception as exc:
                self.app.post(self.status_var.set, f"Errore monitor: {exc}")
                return
            self.app.post(self._set_running, True)
            self.app.post(self.status_var.set,
                          f"Monitor attivo su {cfg['serial_port']} @ {cfg['console_baud']} baud.")
            self.app.post(self.app.set_status, "Monitor seriale attivo.")

        threading.Thread(target=worker, name="monitor-start", daemon=True).start()

    def _on_stop(self):
        self.monitor.stop()
        self._set_running(False)
        self.status_var.set("Monitor fermo.")
        self.app.set_status("Monitor seriale fermato.")

    def _on_send(self):
        line = self.send_entry.get().strip()
        if not line:
            return
        try:
            self.monitor.send_line(line)
            self.append_text(f"» {line}\n")
            self.send_entry.delete(0, "end")
        except Exception as exc:
            self.status_var.set(f"Invio fallito: {exc}")


class OtaTab(TabBase):
    title = "OTA"

    def _build(self):
        self.server = None
        self.link = MqttLink()

        # --- Server HTTPS locale -------------------------------------------------
        box_server = ttk.LabelFrame(self.frame, text="Server HTTPS locale (serve build/pumpkin.bin)")
        box_server.pack(fill="x", pady=(0, 6))
        self.btn_srv_start = ttk.Button(box_server, text="Avvia server", command=self._server_start)
        self.btn_srv_start.pack(side="left", padx=4, pady=4)
        self.btn_srv_stop = ttk.Button(box_server, text="Ferma server", command=self._server_stop,
                                       state="disabled")
        self.btn_srv_stop.pack(side="left", padx=4, pady=4)
        ttk.Label(box_server, text="URL:").pack(side="left", padx=(12, 2), pady=4)
        self.url_var = tk.StringVar(value="")
        self.url_entry = ttk.Entry(box_server, textvariable=self.url_var)
        self.url_entry.pack(side="left", fill="x", expand=True, padx=4, pady=4)
        self.srv_status = tk.StringVar(value="Server fermo.")
        ttk.Label(box_server, textvariable=self.srv_status).pack(fill="x", padx=4, pady=(0, 4))

        # --- Invio certificato CA via seriale -----------------------------------
        box_ca = ttk.LabelFrame(self.frame, text="Certificato CA sul dispositivo (console seriale)")
        box_ca.pack(fill="x", pady=(0, 6))
        self.btn_ca = ttk.Button(box_ca, text="Invia CA al dispositivo (seriale)", command=self._send_ca)
        self.btn_ca.pack(side="left", padx=4, pady=4)
        self.ca_status = tk.StringVar(
            value="Il dispositivo si fida del server OTA grazie a questo certificato "
                  "(comandi: ca begin → righe PEM → commit automatico).")
        ttk.Label(box_ca, textvariable=self.ca_status, wraplength=560).pack(fill="x", padx=4, pady=4)
        ca_wrap = ttk.Frame(box_ca)
        self.ca_text = tk.Text(ca_wrap, height=6, state="disabled")
        scroll_ca = ttk.Scrollbar(ca_wrap, command=self.ca_text.yview)
        self.ca_text.configure(yscrollcommand=scroll_ca.set)
        self.ca_text.pack(side="left", fill="both", expand=True)
        scroll_ca.pack(side="right", fill="y")
        ca_wrap.pack(fill="x", padx=4, pady=(0, 4))

        # --- MQTT ----------------------------------------------------------------
        box_mqtt = ttk.LabelFrame(self.frame, text="MQTT (broker locale; paho-mqtt opzionale)")
        box_mqtt.pack(fill="x", pady=(0, 6))
        self.btn_mqtt = ttk.Button(box_mqtt, text="Connetti broker", command=self._mqtt_toggle)
        self.btn_mqtt.pack(side="left", padx=4, pady=4)
        self.mqtt_status = tk.StringVar(value="MQTT disconnesso.")
        ttk.Label(box_mqtt, textvariable=self.mqtt_status).pack(side="left", padx=8)

        row_ver = ttk.Frame(box_mqtt)
        row_ver.pack(fill="x", padx=4, pady=2)
        ttk.Label(row_ver, text="Versione disponibile:").pack(side="left")
        self.ver_entry = ttk.Entry(row_ver, width=14)
        self.ver_entry.pack(side="left", padx=4)
        ttk.Label(row_ver, text="Note release:").pack(side="left")
        self.rel_entry = ttk.Entry(row_ver)
        self.rel_entry.pack(side="left", fill="x", expand=True, padx=4)
        ttk.Label(row_ver, text="Titolo:").pack(side="left")
        self.title_entry = ttk.Entry(row_ver, width=18)
        self.title_entry.pack(side="left", padx=4)

        row_btn = ttk.Frame(box_mqtt)
        row_btn.pack(fill="x", padx=4, pady=(2, 4))
        self.btn_pub_latest = ttk.Button(row_btn, text="Pubblica disponibile",
                                         command=self._publish_latest)
        self.btn_pub_latest.pack(side="left", padx=2)
        self.btn_ota = ttk.Button(row_btn, text="Lancia OTA", command=self._launch_ota)
        self.btn_ota.pack(side="left", padx=2)

        # --- Viewer eventi --------------------------------------------------------
        box_evt = ttk.LabelFrame(self.frame, text="Eventi dal dispositivo (ultimo messaggio per topic)")
        box_evt.pack(fill="both", expand=True)
        cols = ("ora", "topic", "messaggio")
        self.tree = ttk.Treeview(box_evt, columns=cols, show="headings", height=8)
        for col, text, width in (("ora", "Ora", 80), ("topic", "Topic", 260), ("messaggio", "Messaggio", 520)):
            self.tree.heading(col, text=text)
            self.tree.column(col, width=width, anchor="w")
        scroll_evt = ttk.Scrollbar(box_evt, command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll_evt.set)
        self.tree.pack(side="left", fill="both", expand=True, padx=(4, 0), pady=4)
        scroll_evt.pack(side="right", fill="y", pady=4, padx=(0, 4))

        if not paho_available():
            self.mqtt_status.set(
                "paho-mqtt non installato: MQTT disabilitato. "
                "Installare con 'pip3 install paho-mqtt' (vedi requisiti.txt).")
            for widget in (self.btn_mqtt, self.btn_pub_latest, self.btn_ota):
                widget.configure(state="disabled")

    # --- server HTTPS ------------------------------------------------------------

    def _server_start(self):
        cfg = self.app.current_config()
        _button_state(self.btn_srv_start, False)

        def worker():
            try:
                bin_path = ota_bin_path(cfg["repo_path"])
                if not bin_path.exists():
                    raise RuntimeError(f"File non trovato: {bin_path} — eseguire prima la Build.")
                cert, key = ensure_selfsigned_cert()
                server = OtaServer(ota_build_dir(cfg["repo_path"]), cfg["https_port"],
                                   log=lambda msg: self.app.post(self.app.set_status, msg))
                server.start(cert, key)
                url = ota_url(local_lan_ip(), cfg["https_port"])
                self.server = server
                self.app.post(self._server_started, url)
            except Exception as exc:
                self.app.post(self._server_failed, str(exc))

        threading.Thread(target=worker, name="ota-server-start", daemon=True).start()

    def _server_started(self, url):
        self.url_var.set(url)
        self.srv_status.set(f"Server attivo su 0.0.0.0:{self.app.current_config()['https_port']}.")
        _button_state(self.btn_srv_start, False)
        _button_state(self.btn_srv_stop, True)
        self.app.set_status(f"Server OTA attivo: {url}")

    def _server_failed(self, message):
        self.srv_status.set(f"Errore: {message}")
        _button_state(self.btn_srv_start, True)
        self.app.set_status("Avvio server OTA fallito.")

    def _server_stop(self):
        if self.server is not None:
            self.server.stop()
            self.server = None
        self.srv_status.set("Server fermo.")
        _button_state(self.btn_srv_start, True)
        _button_state(self.btn_srv_stop, False)
        self.app.set_status("Server OTA fermato.")

    # --- invio CA via seriale ------------------------------------------------------

    def _send_ca(self):
        cfg = self.app.current_config()
        _button_state(self.btn_ca, False)

        def worker():
            try:
                cert, _key = ensure_selfsigned_cert()
                monitor = self.app.monitor
                if not monitor.is_active:
                    monitor.start(cfg["serial_port"], cfg["console_baud"])
                    time.sleep(0.6)
                lines = cert.read_text(encoding="utf-8").splitlines()
                self.app.post(self._ca_log, f"[CA] invio {len(lines)} righe su {cfg['serial_port']}…\n")
                monitor.send_line("ca begin")
                time.sleep(0.5)
                for line in lines:
                    monitor.send_line(line)
                    time.sleep(0.04)
                marker = len(monitor.recent)
                time.sleep(1.2)  # il firmware committa alla riga END CERTIFICATE
                monitor.send_line("ca status")
                time.sleep(1.5)
                response = "\n".join(list(monitor.recent)[marker:]) or "(nessuna risposta)"
                self.app.post(self._ca_log, f"\n[CA] risposta a 'ca status':\n{response}\n")
                self.app.post(self._ca_done, None)
            except Exception as exc:
                self.app.post(self._ca_done, str(exc))

        threading.Thread(target=worker, name="ca-send", daemon=True).start()

    def _ca_log(self, text):
        _text_append(self.ca_text, text)

    def _ca_done(self, error):
        _button_state(self.btn_ca, True)
        if error:
            self.ca_status.set(f"Invio CA fallito: {error}")
            self.app.set_status("Invio CA fallito.")
        else:
            self.ca_status.set("Certificato inviato: verificare la risposta di 'ca status' qui sopra.")
            self.app.set_status("Invio CA completato.")

    # --- MQTT ------------------------------------------------------------------------

    def _device_or_ask(self):
        device_id = self.app.current_config()["device_id"].strip()
        if not device_id:
            device_id = simpledialog.askstring(
                "Device ID", "Inserisci il device id della zucca (es. a1b2c3):", parent=self.frame)
            device_id = (device_id or "").strip()
        return device_id or None

    def _mqtt_toggle(self):
        if self.link.connected:
            self.link.disconnect()
            self.mqtt_status.set("MQTT disconnesso.")
            _button_state(self.btn_mqtt, True)
            return
        device_id = self._device_or_ask()
        if device_id is None:
            return
        cfg = self.app.current_config()
        try:
            self.link.connect(
                cfg["mqtt_host"], cfg["mqtt_port"],
                username=cfg.get("mqtt_user"), password=cfg.get("mqtt_pass"),
                on_message=self._on_mqtt_message,
                on_status=lambda rc: self.app.post(self._mqtt_connected, rc, device_id),
                topics=event_topics(device_id),
            )
        except Exception as exc:
            self.mqtt_status.set(f"Connessione fallita: {exc}")

    def _mqtt_connected(self, rc, device_id):
        if rc == 0:
            self.mqtt_status.set(f"MQTT connesso; iscritto agli eventi di pumpkin/{device_id}.")
            _button_state(self.btn_mqtt, True)
            self.btn_mqtt.configure(text="Disconnetti")
            self.app.set_status("MQTT connesso.")
        else:
            self.mqtt_status.set(f"Broker ha rifiutato la connessione (rc={rc}).")

    def _ensure_mqtt(self, device_id):
        if self.link.connected:
            return True
        cfg = self.app.current_config()
        try:
            self.link.connect(
                cfg["mqtt_host"], cfg["mqtt_port"],
                username=cfg.get("mqtt_user"), password=cfg.get("mqtt_pass"),
                on_message=self._on_mqtt_message,
                on_status=lambda rc: self.app.post(self._mqtt_connected, rc, device_id),
                topics=event_topics(device_id),
            )
            time.sleep(0.3)  # piccolo margine per il connect asincrono
        except ImportError:
            messagebox.showerror("paho-mqtt",
                                 "paho-mqtt non installato. Installare con 'pip3 install paho-mqtt' "
                                 "(vedi requisiti.txt).")
            return False
        except Exception as exc:
            messagebox.showerror("Broker MQTT", f"Connessione fallita: {exc}")
            return False
        return self.link.connected

    def _publish_latest(self):
        device_id = self._device_or_ask()
        if device_id is None:
            return
        version = self.ver_entry.get().strip()
        if not version:
            messagebox.showwarning("Versione", "Inserire la versione disponibile (es. v1.0.1).")
            return
        if not self._ensure_mqtt(device_id):
            return
        topic = device_topic(device_id, "update/latest")
        try:
            self.link.publish(topic, version, retain=True)
        except Exception as exc:
            messagebox.showerror("MQTT", f"Pubblicazione fallita: {exc}")
            return
        self.app.set_status(f"Pubblicato (retained) {topic} = {version}")
        messagebox.showinfo("Pubblicato", f"{topic}\n= {version}\n(retained)")

    def _launch_ota(self):
        device_id = self._device_or_ask()
        if device_id is None:
            return
        try:
            payload = build_update_payload(self.url_var.get(), self.ver_entry.get().strip(),
                                           self.rel_entry.get().strip(), self.title_entry.get().strip())
        except ValueError as exc:
            messagebox.showerror("URL OTA", str(exc))
            return
        if not self._ensure_mqtt(device_id):
            return
        topic = device_topic(device_id, "set/update")
        body = json.dumps(payload, ensure_ascii=False)
        try:
            self.link.publish(topic, body, retain=False)
        except Exception as exc:
            messagebox.showerror("MQTT", f"Pubblicazione fallita: {exc}")
            return
        self.app.set_status(f"OTA lanciato su {topic}")
        messagebox.showinfo("OTA lanciato", f"{topic}\n{body}")

    def _on_mqtt_message(self, client, userdata, msg):
        payload = msg.payload.decode("utf-8", errors="replace").replace("\n", " ")
        if len(payload) > 200:
            payload = payload[:200] + "…"
        topic = msg.topic
        stamp = time.strftime("%H:%M:%S")
        self.app.post(self._tree_update, topic, stamp, payload)

    def _tree_update(self, topic, stamp, payload):
        short = topic.split("/", 1)[1] if topic.startswith("pumpkin/") else topic
        values = (stamp, short, payload)
        if self.tree.exists(topic):
            self.tree.item(topic, values=values)
        else:
            self.tree.insert("", "end", iid=topic, values=values)

    def shutdown(self):
        self._server_stop()
        self.link.disconnect()


class App:
    """Finestra principale: Notebook delle schede + coda thread→GUI."""

    def __init__(self, root):
        self.root = root
        root.title("Zucca di Halloween — pannello desktop")
        root.geometry("1000x700")

        self.cfg = load_config()
        self.q = queue.Queue()
        self.monitor = SerialMonitor(emit=self._on_serial_raw)

        notebook = ttk.Notebook(root)
        notebook.pack(fill="both", expand=True, padx=6, pady=(6, 0))
        self.settings_tab = ImpostazioniTab(self, notebook)
        self.build_tab = BuildTab(self, notebook)
        self.flash_tab = FlashTab(self, notebook)
        self.monitor_tab = MonitorTab(self, notebook)
        self.ota_tab = OtaTab(self, notebook)

        self.status_var = tk.StringVar(value="Pronto.")
        ttk.Label(root, textvariable=self.status_var, relief="sunken",
                  anchor="w", padding=(6, 2)).pack(fill="x", side="bottom")

        root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._poll()

    def current_config(self):
        return self.settings_tab.collect()

    def apply_config(self, cfg):
        self.cfg = cfg
        try:
            save_config(cfg)
        except OSError as exc:
            messagebox.showerror("Salvataggio", f"Impossibile salvare config.json: {exc}")
            return False
        self.set_status("Impostazioni salvate.")
        return True

    def set_status(self, message):
        self.status_var.set(message)

    def post(self, func, *args):
        """Accoda una callable da eseguire nel thread della GUI."""
        self.q.put((func, args))

    def _poll(self):
        try:
            while True:
                func, args = self.q.get_nowait()
                try:
                    func(*args)
                except Exception as exc:  # mai far morire il mainloop
                    self.set_status(f"Errore interno: {exc}")
        except queue.Empty:
            pass
        self.root.after(80, self._poll)

    def _on_serial_raw(self, text):
        self.post(self._on_serial_text, text)

    def _on_serial_text(self, text):
        if getattr(self, "monitor_tab", None) is not None:
            self.monitor_tab.append_text(text)

    def _on_close(self):
        try:
            self.monitor.stop()
        except Exception:
            pass
        try:
            self.ota_tab.shutdown()
        except Exception:
            pass
        self.root.destroy()


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--selftest" in argv:
        return run_selftest()
    ensure_tk()
    root = tk.Tk()
    try:
        App(root)
    except Exception:
        root.destroy()
        raise
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
