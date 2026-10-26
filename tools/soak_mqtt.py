#!/usr/bin/env python3
"""soak_mqtt.py — test di stabilita' per la zucca halloween_pumpkin (Fase 7).

Tre modalita' (tutte parlano MQTT col broker della zucca):

  trigger   pubblica N trigger simulati sul topic pumpkin/<id>/set/scare con
            payload "PRESS". NOTA: e' questo IL comando di trigger del firmware
            (components/ha_entities.c: HA_TARGET_SCARE -> fsm_cmd_scare;
            components/ha_mqtt/ha_commands.c accetta solo payload "PRESS").
            Il topic set/trigger_anim NON e' un trigger: cambia solo l'effetto
            associato al trigger nella configurazione.

  watch     si iscrive a pumpkin/<id>/update/status, status/stability,
            status/reset e availability e logga ogni messaggio con timestamp
            su stdout; con --csv registra tutto su file per il report.

  report    riassume un CSV prodotto da watch: heap libero/minimo max e min,
            max block, riconnessioni (da availability), reset visti ed eventi
            OTA.

Esempi:
  python3 tools/soak_mqtt.py trigger --host 192.168.1.10 --device-id 717e50
  python3 tools/soak_mqtt.py watch   --host 192.168.1.10 --device-id 717e50 --csv stabilita.csv
  python3 tools/soak_mqtt.py report  --csv stabilita.csv

Verifica senza broker (parse/aggregazione su dati di esempio):
  python3 tools/soak_mqtt.py --selftest
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import sys
import time
from datetime import datetime, timezone
from typing import Dict, List, Optional

BASE = "pumpkin"
TOPIC_AVAILABILITY = "availability"
TOPIC_UPDATE_STATUS = "update/status"
TOPIC_STATUS_STABILITY = "status/stability"
TOPIC_STATUS_RESET = "status/reset"
TOPIC_SET_SCARE = "set/scare"
SCARE_PAYLOAD = "PRESS"


def die(msg: str) -> None:
    print(f"Errore: {msg}", file=sys.stderr)
    sys.exit(2)


# ---------------------------------------------------------------------------
# Aggregazione (pura, usata anche dal selftest)
# ---------------------------------------------------------------------------

def aggregate(rows: List[Dict[str, str]]) -> Dict[str, object]:
    """Riassume le righe CSV di 'watch': colonne ts,topic,payload.

    Deduce: estremi heap (free/min storico/maxblk), riconnessioni Wi-Fi/MQTT,
    flap availability, reset visti, eventi OTA. Le righe non riconosciute
    sono contate ma ignorate.
    """
    out: Dict[str, object] = {
        "righe": 0,
        "righe_ignorate": 0,
        "heap_free_min": None,
        "heap_free_max": None,
        "heap_min_ultimo": None,
        "heap_min_min": None,
        "maxblk_min": None,
        "uptime_ultimo_s": None,
        "wifi_reconn_ultimo": None,
        "mqtt_reconn_ultimo": None,
        "availability_flap": 0,
        "availability_ultimo": None,
        "reset_count_ultimo": None,
        "reset_motivi": [],
        "ota_eventi": {},
        "versioni_viste": [],
        "primo_ts": None,
        "ultimo_ts": None,
    }
    prev_avail: Optional[str] = None
    for row in rows:
        topic = (row.get("topic") or "").strip()
        payload = (row.get("payload") or "").strip()
        ts = (row.get("ts") or "").strip()
        if not topic:
            out["righe_ignorate"] = int(out["righe_ignorate"]) + 1
            continue
        out["righe"] = int(out["righe"]) + 1
        if ts:
            if out["primo_ts"] is None:
                out["primo_ts"] = ts
            out["ultimo_ts"] = ts

        if topic.endswith("/" + TOPIC_AVAILABILITY):
            if payload != prev_avail and prev_avail is not None:
                out["availability_flap"] = int(out["availability_flap"]) + 1
            prev_avail = payload
            out["availability_ultimo"] = payload
            continue

        try:
            data = json.loads(payload)
        except (json.JSONDecodeError, ValueError):
            out["righe_ignorate"] = int(out["righe_ignorate"]) + 1
            continue

        if topic.endswith("/" + TOPIC_STATUS_STABILITY):
            free = data.get("free")
            hmin = data.get("min")
            blk = data.get("maxblk")
            for chiave, valore in (
                ("heap_free_min", free),
                ("heap_free_max", free),
                ("heap_min_min", hmin),
                ("maxblk_min", blk),
            ):
                if isinstance(valore, (int, float)):
                    cur = out[chiave]
                    out[chiave] = (
                        valore if cur is None
                        else (min(cur, valore) if "min" in chiave else max(cur, valore))
                    )
            if isinstance(hmin, (int, float)):
                out["heap_min_ultimo"] = hmin
            if isinstance(data.get("up"), (int, float)):
                out["uptime_ultimo_s"] = data["up"]
            for chiave in ("wr", "mr"):
                if isinstance(data.get(chiave), (int, float)):
                    out[f"{'wifi' if chiave == 'wr' else 'mqtt'}_reconn_ultimo"] = data[chiave]
            if isinstance(data.get("v"), str) and data["v"] not in out["versioni_viste"]:
                out["versioni_viste"].append(data["v"])
        elif topic.endswith("/" + TOPIC_STATUS_RESET):
            if isinstance(data.get("count"), (int, float)):
                out["reset_count_ultimo"] = int(data["count"])
            motivo = data.get("reason_str") or data.get("reason")
            if motivo is not None and motivo not in out["reset_motivi"]:
                out["reset_motivi"].append(motivo)
        elif topic.endswith("/" + TOPIC_UPDATE_STATUS):
            ev = data.get("event")
            if isinstance(ev, str):
                out["ota_eventi"][ev] = int(out["ota_eventi"].get(ev, 0)) + 1
        else:
            out["righe_ignorate"] = int(out["righe_ignorate"]) + 1
    return out


def print_report(agg: Dict[str, object]) -> None:
    print("=== Report stabilita' ===")
    print(f"righe analizzate:      {agg['righe']} (ignorate: {agg['righe_ignorate']})")
    if agg["primo_ts"]:
        print(f"finestra:              {agg['primo_ts']} -> {agg['ultimo_ts']}")
    if agg["heap_free_min"] is not None:
        print(f"heap libero:           min {agg['heap_free_min']} / max {agg['heap_free_max']} B")
    if agg["heap_min_ultimo"] is not None:
        print(f"heap minimo storico:   ultimo {agg['heap_min_ultimo']} B "
              f"(peggiore visto {agg['heap_min_min']} B)")
    if agg["maxblk_min"] is not None:
        print(f"max block:             peggiore {agg['maxblk_min']} B")
    if agg["uptime_ultimo_s"] is not None:
        print(f"uptime ultimo sample:  {agg['uptime_ultimo_s']} s")
    if agg["wifi_reconn_ultimo"] is not None or agg["mqtt_reconn_ultimo"] is not None:
        print(f"riconnessioni (dev):   wifi {agg['wifi_reconn_ultimo']} / "
              f"mqtt {agg['mqtt_reconn_ultimo']}")
    print(f"availability:          ultimo '{agg['availability_ultimo']}', "
          f"flap osservati {agg['availability_flap']} (dedotti da availability)")
    print(f"reset visti:           count {agg['reset_count_ultimo']}, "
          f"motivi {agg['reset_motivi'] or 'nessuno'}")
    print(f"eventi OTA:            {agg['ota_eventi'] or 'nessuno'}")
    if agg["versioni_viste"]:
        print(f"versioni firmware:     {', '.join(agg['versioni_viste'])}")


# ---------------------------------------------------------------------------
# Modalita' MQTT (paho importato solo qui)
# ---------------------------------------------------------------------------

def _paho():
    try:
        import paho.mqtt.client as mqtt  # noqa: PLC0415
    except ImportError:
        die("paho-mqtt non installato: pip3 install paho-mqtt "
            "(il report e il selftest funzionano senza)")
    return mqtt


def now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def mode_trigger(args: argparse.Namespace) -> int:
    mqtt = _paho()
    topic = f"{BASE}/{args.device_id}/{TOPIC_SET_SCARE}"
    cli = mqtt.Client(client_id=f"soak-trigger-{args.device_id}")
    if args.user:
        cli.username_pw_set(args.user, args.password)
    cli.connect(args.host, args.port, keepalive=30)
    cli.loop_start()
    print(f"invio {args.count} trigger simulati su {topic} "
          f"(payload '{SCARE_PAYLOAD}', intervallo {args.interval}s)")
    for i in range(1, args.count + 1):
        info = cli.publish(topic, SCARE_PAYLOAD, qos=0, retain=False)
        info.wait_for_publish(timeout=5)
        print(f"[{i}/{args.count}] {now_iso()} -> {topic} {SCARE_PAYLOAD}")
        if i < args.count and args.interval > 0:
            time.sleep(args.interval)
    cli.loop_stop()
    cli.disconnect()
    print("fatto: verifica su pumpkin/<id>/event/trigger e stato FSM "
          "(TRIGGERED/COOLDOWN) nei log del broker o con 'watch'")
    return 0


WATCH_TOPICS = (TOPIC_UPDATE_STATUS, TOPIC_STATUS_STABILITY,
                TOPIC_STATUS_RESET, TOPIC_AVAILABILITY)


def mode_watch(args: argparse.Namespace) -> int:
    mqtt = _paho()
    base = f"{BASE}/{args.device_id}"
    subtopics = [f"{base}/{t}" for t in WATCH_TOPICS]
    csv_file = None
    csv_writer = None
    if args.csv:
        csv_file = open(args.csv, "a", newline="", encoding="utf-8")
        csv_writer = csv.writer(csv_file)
        if csv_file.tell() == 0:
            csv_writer.writerow(["ts", "topic", "payload"])

    def on_connect(cli, _userdata, _flags, rc):
        for t in subtopics:
            cli.subscribe(t, qos=0)
            print(f"[{now_iso()}] iscritto a {t}")

    def on_message(_cli, _userdata, msg):
        payload = msg.payload.decode("utf-8", "replace")
        ts = now_iso()
        print(f"[{ts}] {msg.topic} {payload}")
        if csv_writer is not None:
            csv_writer.writerow([ts, msg.topic, payload])
            csv_file.flush()

    cli = mqtt.Client(client_id=f"soak-watch-{args.device_id}")
    if args.user:
        cli.username_pw_set(args.user, args.password)
    cli.on_connect = on_connect
    cli.on_message = on_message
    cli.connect(args.host, args.port, keepalive=30)
    print(f"watch in ascolto su {', '.join(subtopics)} (Ctrl+C per fermare)")
    try:
        cli.loop_forever()
    except KeyboardInterrupt:
        print("\nfine watch")
    finally:
        if csv_file is not None:
            csv_file.close()
    return 0


def mode_report(args: argparse.Namespace) -> int:
    if not args.csv:
        die("report richiede --csv <file prodotto da watch>")
    with open(args.csv, newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    print_report(aggregate(rows))
    return 0


# ---------------------------------------------------------------------------
# Selftest (senza paho e senza broker)
# ---------------------------------------------------------------------------

def selftest() -> int:
    sample = [
        {"ts": "2026-10-26T10:00:00", "topic": "pumpkin/717e50/availability",
         "payload": "online"},
        {"ts": "2026-10-26T10:01:00", "topic": "pumpkin/717e50/status/stability",
         "payload": json.dumps({"up": 60, "free": 120000, "min": 110000,
                                "maxblk": 90000, "wr": 1, "mr": 1, "rst": 1,
                                "v": "5612363"})},
        {"ts": "2026-10-26T11:00:00", "topic": "pumpkin/717e50/availability",
         "payload": "offline"},
        {"ts": "2026-10-26T11:00:20", "topic": "pumpkin/717e50/availability",
         "payload": "online"},
        {"ts": "2026-10-26T11:00:20", "topic": "pumpkin/717e50/status/reset",
         "payload": json.dumps({"count": 1, "reason": 1,
                                "reason_str": "POWERON", "v": "5612363"})},
        {"ts": "2026-10-26T12:00:00", "topic": "pumpkin/717e50/status/stability",
         "payload": json.dumps({"up": 3660, "free": 118000, "min": 105000,
                                "maxblk": 88000, "wr": 2, "mr": 2, "rst": 2,
                                "v": "5612363"})},
        {"ts": "2026-10-26T12:00:01", "topic": "pumpkin/717e50/update/status",
         "payload": json.dumps({"event": "started", "free_heap": 118000,
                                "max_block": 88000, "progress": 0})},
        {"ts": "2026-10-26T12:01:01", "topic": "pumpkin/717e50/update/status",
         "payload": json.dumps({"event": "success", "installed": "abc2345",
                                "progress": 100})},
        {"ts": "2026-10-26T12:02:00", "topic": "pumpkin/717e50/rumore",
         "payload": "non-json"},
    ]
    agg = aggregate(sample)
    atteso = [
        (agg["righe"], 9),
        (agg["righe_ignorate"], 1),
        (agg["heap_free_min"], 118000),
        (agg["heap_free_max"], 120000),
        (agg["heap_min_ultimo"], 105000),
        (agg["heap_min_min"], 105000),
        (agg["maxblk_min"], 88000),
        (agg["wifi_reconn_ultimo"], 2),
        (agg["mqtt_reconn_ultimo"], 2),
        (agg["availability_flap"], 2),
        (agg["availability_ultimo"], "online"),
        (agg["reset_count_ultimo"], 1),
        (agg["reset_motivi"], ["POWERON"]),
        (agg["ota_eventi"], {"started": 1, "success": 1}),
        (agg["versioni_viste"], ["5612363"]),
    ]
    for valore, atteso_v in atteso:
        if valore != atteso_v:
            print(f"SELFTEST FALLITO: atteso {atteso_v!r}, ottenuto {valore!r}")
            return 1
    print("selftest OK: parse CSV, estremi heap, flap availability, reset, "
          "eventi OTA verificati su dati di esempio")
    print_report(agg)
    return 0


# ---------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Test di stabilita' MQTT per la zucca halloween_pumpkin",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Esempi:")[1].strip()
        if __doc__ and "Esempi:" in __doc__ else None,
    )
    ap.add_argument("mode", nargs="?", choices=("trigger", "watch", "report"),
                    help="modalita': trigger (simula spaventi), watch (logga "
                         "telemetria), report (riassume il CSV di watch)")
    ap.add_argument("--host", default="127.0.0.1", help="broker MQTT (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=1883, help="porta broker (default 1883)")
    ap.add_argument("--device-id", help="id dispositivo nel topic: 6 cifre hex dal MAC (es. 717e50)")
    ap.add_argument("--count", type=int, default=100,
                    help="trigger: numero di simulazioni (default 100)")
    ap.add_argument("--interval", type=float, default=2.0,
                    help="trigger: secondi tra un trigger e l'altro (default 2.0)")
    ap.add_argument("--csv", help="watch: file CSV di uscita / report: file CSV di ingresso")
    ap.add_argument("--user", help="utente broker (opzionale)")
    ap.add_argument("--password", help="password broker (opzionale)")
    ap.add_argument("--selftest", action="store_true",
                    help="verifica interna senza broker (parse/aggregazione su dati di esempio)")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.mode is None:
        ap.error("modalita' richiesta: trigger | watch | report (o --selftest)")
    if args.device_id is None:
        ap.error("serve --device-id (6 cifre hex stampate nel log di boot, es. 717e50)")

    if args.mode == "trigger":
        return mode_trigger(args)
    if args.mode == "watch":
        return mode_watch(args)
    return mode_report(args)


if __name__ == "__main__":
    sys.exit(main())
