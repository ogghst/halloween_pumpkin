# halloween_pumpkin

Firmware per una **zucca di Halloween animata**: una matrice 8×8 di LED WS2812B che
gioca effetti luminosi, un radar a microonde che sente la presenza dei visitatori,
una macchina a stati che gestisce trigger e cooldown, e un ponte MQTT verso
**Home Assistant** per controllo, telemetria e **aggiornamenti firmware via OTA**.

Hardware: **ESP32 D1 mini** (ESP32 classico), connesso alla rete in Wi-Fi con
provisioning BLE. Testi e log in italiano.

## Hardware

| Pezzo | Dettagli |
| --- | --- |
| Board | ESP32 D1 mini (ESP32-D0WD-V3, flash 4 MB, bridge CP2102 → `/dev/ttyUSB0`, console 115200) |
| LED | WS2812B 8×8 = 64 pixel, catena DIN unica a **serpentina**, ordine GRB, buffer 74AHCT125, rail 5 V |
| Pin LED | dati **GPIO5**, enable rail **GPIO4** (LED_EN attivo alto) |
| Radar | **HLK-LD2420** (presenza 24 GHz, alimentazione 3,3 V): RX **GPIO26** (OT1), TX **GPIO27**, uscita presenza **GPIO25** (OT2) |
| Reset provisioning | pulsante **GPIO32** (pull-up, premuto 5 s = credenziali Wi-Fi azzerate) |

Cablaggio completo, pin e alimentazione: [`docs/pinout.md`](docs/pinout.md).

## Struttura del repo

```
main/                 app di produzione (wiring completo di tutti i componenti)
components/           i 10 componenti del firmware (vedi docs/architettura.md)
test_apps/            app di banco monofunzione (led_effects, net_prov, ha_mqtt, ...)
tools/                wiki_lint.py, static_analysis.sh, desktop/ (pannello Python)
docs/                 wiki di progetto (docs/wiki/) + guide (mqtt, fsm, pinout)
partitions.csv        tabella partizioni dual-OTA su 4 MB
sdkconfig.defaults    configurazione di default (target esp32 incluso)
```

Documentazione principale: [`docs/architettura.md`](docs/architettura.md) (componenti,
task, topic MQTT, OTA), [`docs/mqtt.md`](docs/mqtt.md) (topic e discovery Home
Assistant), [`docs/fsm.md`](docs/fsm.md) (macchina a stati),
[`docs/troubleshooting.md`](docs/troubleshooting.md) (problemi noti e rimedi),
wiki completa in [`docs/wiki/index.md`](docs/wiki/index.md).

## Prerequisiti

- Ubuntu 24.04 (versione su cui è stato testato) con i pacchetti di build dell'ESP-IDF:

  ```bash
  sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
  ```

- **Python 3** per gli unit test host, il lint della wiki e l'app desktop
  (per quest'ultima: `python3-tk`, `openssl` nella PATH).

## Setup (una tantum)

Toolchain **ESP-IDF v5.4.4** installata nativamente in `~/esp/esp-idf`, tool in `~/.espressif` (fuori dal repo):

```bash
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 --shallow-submodules --recursive -b v5.4.4 https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf && ./install.sh esp32
```

## Caricare l'ambiente (ogni nuova shell)

```bash
source ~/esp/esp-idf/export.sh
```

## Compilazione

```bash
idf.py set-target esp32            # solo la prima volta (rigenera sdkconfig dai defaults)
idf.py build
```

- **Build incrementali: il default.** `idf.py build` (e `flash`/`monitor`) riutilizza
  `build/` e ricompila solo ciò che è cambiato: su questo PC una modifica sorgente
  richiede ~35-75 s contro ~3-5 min di una build da zero. **Non cancellare `build/`
  né `sdkconfig`** tra una build e l'altra. Rigenerazione completa necessaria solo se:
  cambi `sdkconfig.defaults` o target (`rm sdkconfig && idf.py set-target esp32`),
  cambi `partitions.csv`, o in caso di errori di link inspiegati
  (`idf.py fullclean` come ultima spiaggia).
- La **versione del firmware** (`PROJECT_VER`, visibile in Home Assistant come
  `sw_version` e usata dall'OTA per il confronto) arriva da `git describe --tags --dirty --always`;
  senza tag produce l'hash del commit (es. `5612363`), senza git il fallback `0.0.0-dev`.
- Build di riferimento (integrazione 2026-10-26, zero warning): `pumpkin.bin`
  **1.400.352 B** sullo slot da 1984K (**31,1% libero**), IRAM 125.590/131.072
  (**95,8%**, 5.482 B liberi), DRAM statica al 51,4%, bootloader 0x6600 (~9% libero).

## Flash

```bash
ls /dev/ttyUSB* /dev/ttyACM*       # trova la porta (attesa: /dev/ttyUSB0)
idf.py -p /dev/ttyUSB0 flash       # primo caricamento (o cambio board)
idf.py -p /dev/ttyUSB0 -b 921600 flash monitor
```

- La D1 mini usa il bridge USB-UART **CP2102** (verificato su quest'unità), in genere
  `/dev/ttyUSB0`; alcune board montano un CH340 o USB-CDC (`/dev/ttyACM*`).
- Baud console 115200; per il flash si può alzare a 921600 (default dell'app desktop).
- Per uscire dal monitor: `Ctrl+]`.
- Dopo la prima OTA non serve più il cavo: gli aggiornamenti successivi arrivano via
  HTTPS (sezione [OTA](#aggiornamento-firmware-ota)).

## Partition table (flash 4 MB, OTA a due slot)

`partitions.csv`: `nvs` 16K, `otadata` 8K, `phy_init` 4K, **`ota_0`/`ota_1` 1984K**,
**`coredump` 64K** in coda (4 MB usati al completo).

Verifica dopo la build:

```bash
idf.py partition_table
```

## Monitor

```bash
idf.py -p /dev/ttyUSB0 monitor
```

Prompt `pumpkin>`: i comandi console principali sono `help`, `net`, `prov`,
`cfg_get`, `fsm_state`, `fsm_mode`, `eyes`, `ha`, `heap`, `stats`, `ota <url>`,
`ota_status`, `sysinfo`, `stability`, `tasks`, `heaptrace`. Dettagli in
[`docs/troubleshooting.md`](docs/troubleshooting.md) (sezione console).
Con `eyes` si comanda la zona occhi (Fase 8): `eyes` (look corrente),
`eyes effects` (registro), `eyes effect <nome>`, `eyes color <r> <g> <b>`,
`eyes bright <0-255>`, `eyes flash [ms]`, `eyes glare <s>`,
`eyes follow <0|1>`, `eyes blink <0|1>`; con `stats` si leggono frame
renderizzati/persi e heap minimo (verifica 60 fps a banco).

## Provisioning BLE

1. Al primo avvio (nessuna credenziale Wi-Fi) la board parte in provisioning BLE:
   servizio `PUMPKIN-XXXXXX`, **PoP `pump-XXXXXX` derivata dal MAC e stampata nel log**.
2. App telefonica: **ESP BLE Provisioning** (Espressif), schema BLE, Security 1.
3. Nella sezione custom della stessa app c'è l'endpoint **TLV `mqtt`** per passare
   host/porta/utente/password/TLS del broker: viene scritto in `config_store`.
4. Senza endpoint MQTT si può fare dopo da console: `prov mqtt host=X port=N user=U pass=P tls=0|1`.
5. Timeout provisioning 10 min; pulsante GPIO32 per 5 s = reset credenziali.
   Attenzione: **il re-provisioning BLE richiede un reboot** (la memoria BT
   viene rilasciata in modo permanente dopo il primo provisioning).

Procedura completa e checklist di banco:
[`docs/wiki/procedure/provisioning.md`](docs/wiki/procedure/provisioning.md).

## Certificato CA per MQTT/OTA (TLS)

Il certificato della CA (self-signed incluso) si carica da console, riga per riga;
il **commit in NVS è automatico** alla riga `-----END CERTIFICATE-----`:

```
ha ca begin
ha ca +-----BEGIN CERTIFICATE-----
ha ca +MIIBszCCAVmgAwIBAgIUKnO...   (una riga PEM per comando)
ha ca +-----END CERTIFICATE-----
ha ca status
```

La stessa CA serve al **trasporto MQTT** (`mqtts://`, se `tls=1`) e all'**OTA
HTTPS**: senza CA l'OTA rifiuta l'update (`no_ca`) e l'MQTT con TLS non parte.

## Aggiornamento firmware (OTA)

Comando (da Home Assistant sull'entità *update*, oppure pubblicando su
`pumpkin/<id>/set/update`):

```json
{"url":"https://host/pumpkin.bin","version":"1.1.0","release_summary":"...","title":"..."}
```

Solo `url` è obbligatorio ed **HTTPS è obbligatorio**; `version`,
`release_summary` e `title` sono metadati informativi (la versione decide
veramente il descrittore dell'immagine scaricata). Dalla console: `ota <url> [versione]`.

**Gate prima di toccare la flash** (rifiuto con evento `rejected` e motivo):

| Motivo | Soglia |
| --- | --- |
| `bad_url` | URL assente/vuota, non `https://`, oltre 512 B |
| `battery_low` | batteria sotto il 40% **se nota**; sconosciuta → consentita con `"battery":"unknown"` |
| `led_full_light` | stima corrente rail LED oltre 500 mA (`PUMPKIN_OTA_MAX_LED_CURRENT_MA`) |
| `heap_low` | heap libero o massima allocazione contigua sotto 64 KB (`PUMPKIN_OTA_MIN_FREE_HEAP`) |
| `busy` | un aggiornamento alla volta |
| `same_version` | immagine scaricata identica alla versione in esecuzione |
| `no_ca` | certificato CA assente in NVS/partizione |

Flusso:

1. Eventi su `pumpkin/<id>/update/status` (JSON): `started`, `progress` (ogni 10%),
   `success` o `failed`/`rejected`.
2. Alla fine la zucca pubblica `success`, attende 3 s per lo svuotamento MQTT e
   **si riavvia sulla nuova immagine**, che resta in stato `PENDING_VERIFY`
   (rollback abilitato): **nessuna conferma automatica al termine del download**.
3. Dopo il riavvio la conferma avviene solo se funzionano **Wi-Fi + MQTT +
   sensore radar + FSM in IDLE o OFF** (log `immagine ... confermata valida`).
   Se qualcosa non va entro i primi avvii, il **bootloader fa rollback**
   automatico allo slot precedente al riavvio successivo.
4. La verifica del certificato HTTPS è **condivisa col trasporto MQTT**
   (stessa CA in NVS, namespace `pumpkin_ca`) e non è disabilitabile.

Verifica end-to-end (server locale, invio CA, lancio): app desktop scheda *OTA*
(sezione sotto) o procedura manuale in [`docs/mqtt.md`](docs/mqtt.md).
**Lo stato è "attrezzato ma non ancora verificato a banco"**: vedi
[`docs/report_stabilita.md`](docs/report_stabilita.md).

## App desktop

Pannello Python/Tkinter in [`tools/desktop/`](tools/desktop/) (istruzioni complete:
[`tools/desktop/LEGGI.md`](tools/desktop/LEGGI.md)): configurazione, build ESP-IDF,
flash USB, monitor seriale, server **HTTPS locale per l'OTA** (genera il
certificato con `openssl`, invia la CA al dispositivo via seriale, pubblica
`update/latest` e lancia il comando `set/update`, mostra gli eventi).

```bash
sudo apt install python3-tk          # tkinter (GUI)
cd tools/desktop && python3 pumpkin_desktop.py
```

Dipendenze **opzionali** (`pip3 install pyserial paho-mqtt`): senza `pyserial` il
monitor seriale resta disabilitato, senza `paho-mqtt` la parte MQTT/OTA; il resto
funziona con la sola stdlib. Verifica headless senza GUI: `python3 pumpkin_desktop.py --selftest`.

## Test host

Ogni componente con logica pura ha i suoi unit test (CMake + Unity + CTest, senza
hardware). Con l'ambiente caricato:

```bash
for d in components/*/host_tests; do
  cmake -S "$d" -B "$d/build" && cmake --build "$d/build" && \
  ctest --test-dir "$d/build" --output-on-failure || exit 1
done
```

`components/ha_entities` non ha host_tests: è puro glue sugli eventi (niente logica
algoritmica separata). Dettagli: [`docs/wiki/procedure/host-tests.md`](docs/wiki/procedure/host-tests.md).

## Analisi statica

```bash
bash tools/static_analysis.sh
```

Cppcheck è **opzionale**: se non è installato lo script lo segnala e continua
con i controlli disponibili.

## Sicurezza: secure boot e flash encryption

**Secure boot e flash encryption NON sono abilitati** in questa configurazione.

- **A favore** dell'attivazione: il firmware e i segreti in flash (credenziali
  Wi-Fi/MQTT) non sono leggibili/rimediabili con un lettore esterno; l'anti-rollback
  impedisce di reinstallare versioni vecchie.
- **Contro**: gli eFuse sono **IRREVERSIBILI** (nessun ritorno indietro); le chiavi
  vanno custodite e una perdita/cambio board richiede provisioning dedicato; debug
  e JTAG risultano limitati; il flash esterno (`idf.py flash`) viene bloccato dopo
  l'attivazione; la gestione OTA si complica (immagini firmate, chiavi di firma).
- **Raccomandazione**: per una zucca da giardino il costo/rischio non è giustificato
  oggi; valutare l'attivazione **solo a progetto stabile**, come decisione esplicita
  e documentata (nuovo ADR).

## App di banco (test_apps/)

App monofunzione per provare un sottoinsieme senza l'intera catena:
`idf.py -C test_apps/<nome> build flash monitor`.

- `test_apps/led_effects` — effetti LED in ciclo con console dedicata:

  | Comando | Azione |
  | ------- | ------ |
  | `fx <nome>` | imposta l'effetto (nome invalido → stampa la lista dei nomi) |
  | `oneshot <nome>` | riproduce una volta un effetto one-shot, poi ripristina il precedente |
  | `color <r> <g> <b>` | colore base, 0-255 per canale |
  | `bright <0-255>` | luminosità globale |
  | `speed <x.x>` | velocità dell'effetto (1.0 = velocità normale) |
  | `power <0\|1>` | rail LED on/off |
  | `pixel <x> <y> <r> <g> <b>` | pixel test: accende il solo pixel (x,y) della matrice (coordinate 0-7, colori 0-255; coordinate non valide → errore con usage) |
  | `pixel off` | rilascia il pixel test: il motore riprende l'effetto corrente |
  | `stats` | statistiche di rendering: frames, dropped, avg_us, max_us |
  | `current` | stima corrente istantanea (mA) e scale attuale |
  | `scale <1-100>` | fattore di scala della stima di corrente |

- `test_apps/net_prov` — provisioning + rete (senza MQTT).
- `test_apps/ha_mqtt` — catena completa Fase 5 con console `ha`.
- `test_apps/pumpkin_fsm`, `test_apps/radar_ld2420` — banco FSM e radar.

Nota: il task `anim` usa `vTaskDelayUntil` con periodo ~16,7 ms (60 fps); richiede
`CONFIG_FREERTOS_HZ=1000` (il tick di default a 100 Hz non ha risoluzione sufficiente).

## VS Code

- Estensione consigliata: `ms-vscode.esp-idf-extension` (vedi `.vscode/extensions.json`).
- Alla prima apertura eseguire il comando **"ESP-IDF: Configure ESP-IDF Extension"**:
  i path di IDF e dei tool sono già impostati in `.vscode/settings.json`.
