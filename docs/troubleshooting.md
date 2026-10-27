# Troubleshooting

Sintomo → causa probabile → rimedio. Riferimenti: [`docs/architettura.md`](architettura.md),
wiki (`docs/wiki/`), in particolare le insidie citate.

## Build

| Sintomo | Causa | Rimedio |
| --- | --- | --- |
| Config incoerente, simboli Kconfig mancanti, build che "non vede" un default cambiato | `sdkconfig` generato con defaults vecchi (i default si applicano solo alla creazione) | `rm sdkconfig && idf.py set-target esp32` |
| Link fallito: `region iram0_0_seg overflowed` | IRAM colmo (BLE + Wi-Fi + mDNS + TLS) | Knob obbligatori già in `sdkconfig.defaults` (`ESP_WIFI_IRAM_OPT=n`, ruoli NimBLE central/observer off) — vedi [`wiki/insidie/iram-nimble-wifi.md`](wiki/insidie/iram-nimble-wifi.md); misurare con `idf.py size` dopo ogni aggiunta |
| Errore su un warning (`-Werror`) | `main/` e i componenti compilano con `-Wall -Wextra -Werror` | Correggere il warning, non abbassare il flag |
| `cmake` host test fallisce su `unity` | `IDF_PATH` non impostato | `source ~/esp/esp-idf/export.sh` prima dei test host |

## Flash

| Sintomo | Causa | Rimedio |
| --- | --- | --- |
| `Could not open port` / porta assente | porta sbagliata o permessi | `ls /dev/ttyUSB* /dev/ttyACM*`; l'unità di riferimento è `/dev/ttyUSB0` (CP2102); verificare i permessi (gruppo `dialout`) |
| `device reports readiness to read but returned no data (device disconnected or multiple access on port?)` | un monitor seriale è aperto (idf.py monitor, app desktop) e tiene la porta | Chiudere il monitor; trovarlo con `fuser -v /dev/ttyUSB0`, poi riflashare |
| Flash parte ma fallisce a metà | baud troppo alto o cavo scarso | Riprovare a 115200 (`idf.py -p /dev/ttyUSB0 -b 115200 flash`) o cambiare cavo |
| Board non entra in download mode | sketch già in run che tiene la UART occupata | Riprovare tenendo premuto BOOT durante il reset; su D1 mini è sufficiente in genere riprovare |

Dopo la prima OTA il cavo non serve più: aggiornare via HTTPS (sezione OTA sotto).

## Provisioning

| Sintomo | Causa | Rimedio |
| --- | --- | --- |
| `net: disconnected (reason=201)` in loop, provisioning BLE non visibile | credenziali Wi-Fi presenti ma errate/irraggiungibili (l'advertising parte solo senza credenziali) | Console: **`prov start`** (cancella SSID/password da NVS e avvia il provisioning BLE; MQTT e CA sopravvivono) oppure pulsante **GPIO32 5 s**. L'ESP32 fa solo 2,4 GHz |
| App non trova il servizio BLE | app generica o BLE spento/vicino al telefono | Usare **ESP BLE Provisioning** (Espressif), schema BLE; service `PUMPKIN-XXXXXX` |
| PoP rifiutata | PoP letta male | La PoP è **`pump-XXXXXX` stampata nel log di boot** (derivata dal MAC, ADR-008) |
| Il re-provisioning BLE non parte | memoria BT rilasciata dopo il primo provisioning (permanente per sessione) | **Reboot della board** e riprovare (`wifi_prov_begin` lo logga e riavvia) |
| Parametri MQTT da app | endpoint custom | Sezione TLV `mqtt` dell'app oppure console: `prov mqtt host=X port=N user=U pass=P tls=0|1` |

Procedura completa: [`wiki/procedure/provisioning.md`](wiki/procedure/provisioning.md).

## MQTT

| Sintomo | Causa | Rimedio |
| --- | --- | --- |
| `availability` passa a `offline` | LWT scattato: board irraggiungibile o crash | Verificare alimentazione/Wi-Fi; dopo la riconnessione torna `online` (keepalive 30 s) |
| `mqtts` non parte, log "CA assente" | nessun certificato in NVS `pumpkin_ca` | Caricare la CA da console: `ha ca begin`, `ha ca +<riga>` — commit automatico su `END CERTIFICATE` — poi `ha ca status`. Nessun fallback in chiaro |
| La board non si connette al broker | broker solo su localhost (default Mosquitto 2.0) o credenziali | Aprire un listener sulla LAN con `password_file` ([`wiki/procedure/mosquitto.md`](wiki/procedure/mosquitto.md)); credenziali via provisioning/console |
| Discovery non appare in HA | HA < 2024.11 o discovery rimossa | Verificare versione HA ([`wiki/domande-aperte.md`](wiki/domande-aperte.md) n.23); ripubblicare con `ha announce` |

## OTA

Motivi di rifiuto (evento `rejected` su `update/status`, motivo in `detail`;
riepilogo con `ota_status`):

| Motivo | Causa | Rimedio |
| --- | --- | --- |
| `bad_url` | URL assente/vuota, non `https://` o > 512 B | Usare un URL HTTPS completo |
| `battery_low` | batteria < 40% (solo se nota) | Ricaricare; con batteria sconosciuta l'update è consentito (`"battery":"unknown"`) |
| `led_full_light` | corrente stimata rail LED > 500 mA | Abbassare luminosità/effetto e riprovare |
| `heap_low` | heap libero o max block < 64 KB | Riavviare la board o liberare heap; verificare con `heap` |
| `busy` | un update alla volta | Attendere il termine (`ota_status`) |
| `same_version` | immagine identica alla versione in esecuzione | Cambiare versione (`git describe` diverso) |
| `no_ca` | CA assente | Caricare la CA (`ha ca ...`) come per MQTT: verifica condivisa, non disabilitabile |

Rollback e conferma:

- Al termine del download la zucca pubblica `success`, attende 3 s e si riavvia
  sull'immagine **PENDING_VERIFY** (nessuna conferma implicita).
- Se dopo il riavvio **Wi-Fi + MQTT + sensore radar + FSM (IDLE/OFF)** sono a posto,
  `sys_health` logga `immagine ... confermata valida: rollback annullato` e
  l'immagine passa VALID (`sysinfo` → `ota_state=VALID`).
- Se la conferma non arriva, **al riavvio successivo il bootloader fa rollback**
  allo slot precedente (righe di rollback/validazione nel log di boot): la zucca
  riparte con la versione precedente, `update/state` torna indietro.
- Per riprovare: risolta la causa (CA, heap, URL), rilanciare il comando
  `set/update` o `ota <url>`. **Nessun OTA reale è ancora stato eseguito a banco**:
  procedura in [`docs/report_stabilita.md`](report_stabilita.md).

## Watchdog

- Timeout task watchdog **5 s** (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`); controlla i
  task iscritti e i task IDLE (default IDF).
- Task iscritti dal codice di progetto: `anim`, `radar`, `fsm`, `ha_mqtt`,
  `sys_health` (feed a passi da 1 s), `pumpkin_ota` (solo durante l'update,
  Kconfig `PUMPKIN_OTA_TASK_WDT`, default y).
- Messaggio tipico: `Task watchdog got triggered. The following tasks/users did
  not reset the watchdog in time` con l'elenco dei task colpevoli, seguito da
  backtrace: la causa è un task bloccato più di 5 s senza feed (es. driver in
  attesa, loop senza `vTaskDelay`).
- Con `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y` (default del repo) un panic o un
  WDT debole riavvia la board; il motivo del reset resta in NVS (`sysinfo` → `reason`).

## Core dump

Configurato su flash (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, formato ELF, CRC32),
partizione `coredump` 64K. Dopo un panic:

```bash
source ~/esp/esp-idf/export.sh
espcoredump.py -p /dev/ttyUSB0 -c coredump info build/pumpkin.elf
espcoredump.py -p /dev/ttyUSB0 -c coredump dbg build/pumpkin.elf   # gdb
```

## Console comandi utili (prompt `pumpkin>`)

| Comando | Cosa mostra/fà |
| --- | --- |
| `ota <url> [versione]` | avvia l'OTA dalla console (stessi gate del comando MQTT) |
| `ota_status` | attivo, starts/rejects, ultimo esito, heap |
| `sysinfo` | versione, progetto, `ota_state`, partizione, reset (count/motivo), flag salute, riconnessioni, heap |
| `stability` | lo stesso JSON di `status/stability`, su console |
| `tasks` | elenco task con high water mark (JSON come `status/tasks`) |
| `heaptrace start\|stop\|dump [n]` | heap tracing standalone (abilitato di default nel repo) per cercare perdite |
| `ha status` / `ha announce` / `ha remove` | connessione, `<id>`, topic, CA; ripubblicazione/rimozione discovery |
| `ha ca begin` / `ha ca +<riga>` / `ha ca status` | caricamento CA (commit automatico su `END CERTIFICATE`) |
| `net` / `net ps min\|max` / `net listen <1..10>` / `net reconnect` | diagnostica e tuning Wi-Fi |
| `prov` / `prov mqtt k=v ...` | stato provisioning, parametri broker |
| `cfg_get [mqtt]` / `cfg_flush` | dump configurazione; commit immediato su NVS |
| `fsm_state` / `fsm_mode <auto\|manual\|off>` | stato/modo FSM |
| `heap` | heap libero e largest block |

## App desktop (`tools/desktop`)

| Sintomo | Causa | Rimedio |
| --- | --- | --- |
| L'app non parte: `import tkinter` fallito | python3-tk assente | `sudo apt install python3-tk`; il selftest headless funziona comunque (`python3 pumpkin_desktop.py --selftest`) |
| Scheda *Monitor* disabilitata con messaggio | `pyserial` mancante | `pip3 install pyserial` (o `pip3 install -r requisiti.txt`); nessun traceback: la scheda resta spenta, il resto funziona |
| Scheda *OTA* senza MQTT / viewer muto | `paho-mqtt` mancante | `pip3 install paho-mqtt`; build, flash e server HTTPS funzionano comunque |
| Il server OTA non si avvia (prima volta) | `openssl` non in PATH | Installare openssl; il certificato (`cert.pem`/`key.pem`) nasce accanto allo script |
| "Impossibile leggere idf.py size" | build non fatta o path IDF errato | Verificare percorso repo e IDF nella scheda *Impostazioni*, poi *Compila* |

Dettagli e sequenza OTA consigliata: [`tools/desktop/LEGGI.md`](tools/desktop/LEGGI.md).
