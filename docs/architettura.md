# Architettura del firmware

Panoramica dell'app di produzione (`main/main.c`) e dei componenti dopo la Fase 7.
Aggiornato al 2026-10-26 (build di integrazione verde, zero warning). Le decisioni
di progetto sono negli ADR in `docs/wiki/decisioni/`; i dettagli per componente
nella wiki (`docs/wiki/componenti/`).

## Componenti

| Componente | Responsabilità | Note |
| --- | --- | --- |
| `main/` (app) | Boot e wiring: NVS, event loop, catena config→led→radar→FSM→ha_entities→OTA→sys_health, init Wi-Fi (netif STA creato UNA volta), decisione provisioning vs rete, console REPL | `s_heap_baseline` per i comandi heap/ota_status |
| `led_effects` | Rendering 60 fps su matrice 8×8, 6 effetti, overlay rete/prov, stima corrente, rail LED | Task `anim` prio 5; base di tutto |
| `radar_ld2420` | Radar presenza HLK-LD2420 su UART1: parser, calibrazione, trigger, guasto/recovery con backoff | Task `radar` prio 5; NVS `radar` per soglie locali |
| `config_store` | Parametri utente in NVS `pumpkin` (blob v2 con blocco MQTT), debounce 2 s, evento `CONFIG_EVENT_CHANGED` | **Fonte unica** dei parametri (ADR-007) |
| `pumpkin_fsm` | Macchina a stati IDLE/TRIGGERED/COOLDOWN/MANUAL/OFF: coda unica + task proprietario, timer one-shot, persistenza in AUTO | Task `fsm` prio 4; parla coi LED solo via `fsm_output_t` |
| `wifi_prov` | Provisioning BLE NimBLE (default) / SoftAP, PoP `pump-XXXXXX` dal MAC, endpoint TLV `mqtt`, pulsante GPIO32 | BTDM rilasciata dopo il provisioning: re-provisioning BLE = reboot |
| `net_mgr` | Wi-Fi STA: eventi UP/DOWN, backoff 1→60 s ±15%, hostname + mDNS `_pumpkin._tcp`, power save | Adotta il netif creato dall'app |
| `ha_mqtt` | Trasporto esp_mqtt: LWT availability, discovery device-based HA, worker task per callback/comandi, CA TLS in NVS `pumpkin_ca` | `ha_mqtt_ca_get` condivisa con l'OTA; listener extra `ha_mqtt_on_connected_add` (max 4) |
| `ha_entities` | Glue: registro delle 12 entità HA, traduzione eventi→publish, comandi→FSM/config_store | Nessuna logica algoritmica propria (nessun host test) |
| `pumpkin_ota` | Aggiornamento HTTPS via esp_https_ota (ADVANCED): gate di politica, controllo versione, eventi su `update/status` | Task dedicato (8192 B) solo durante l'update; **non** marca l'immagine valida |
| `sys_health` | Conferma post-OTA (rollback), contatore reset NVS `pumpkin_sys`, telemetria `status/*` ogni 60 s, heap tracing, console | Task `sys_health` prio 3; iscritto al task watchdog |
| `tools/desktop` (app desktop) | Pannello Python/Tkinter: build, flash, monitor seriale, server HTTPS OTA locale, invio CA, publish update/latest + set/update | Fuori dal firmware; `tools/desktop/LEGGI.md` |

## Task e priorità

Task creati dal codice di progetto (prio FreeRTOS, numeri più alti = più priorità;
tutti quelli sotto sono iscritti al task watchdog, timeout 5 s):

| Task | Creato da | Prio | Stack | Ruolo |
| --- | --- | --- | --- | --- |
| `anim` | led_effects | 5 | 4096 | rendering 60 fps, overlay, feed TWDT a ogni frame |
| `radar` | radar_ld2420 | 5 | 4096 (`RADAR_TASK_STACK`) | loop RX UART, sessioni config, recovery backoff |
| `ha_mqtt` | ha_mqtt | 5 | 6144 | worker: callback on_connected/on_command, announce |
| `pumpkin_ota` | pumpkin_ota | 5 | 8192 | download/flash HTTPS (esiste solo durante l'update; TWDT via Kconfig `PUMPKIN_OTA_TASK_WDT`) |
| `fsm` | pumpkin_fsm | 4 (`PUMPKIN_FSM_TASK_PRIO`) | 4096 (`PUMPKIN_FSM_TASK_STACK_SIZE`) | owner della coda eventi FSM |
| `sys_health` | sys_health | 3 | 4096 | telemetria 60 s a passi da 1 s con feed TWDT, riverifica rollback |
| `prov_btn` | wifi_prov | 3 (idle+3) | 2048 | lettura pulsante GPIO32 reset credenziali |

Restano task di infrastruttura IDF (REPL console, `esp_timer`, `esp_event`, Wi-Fi/
tcpip, IDLE0/1): non li tocca codice di progetto.

## Mappa topic MQTT

Base topic `pumpkin/<id>/` con `<id>` = 6 cifre hex dai byte [3..5] del MAC STA
(es. `717e50`). Direzione **→** = zucca pubblica, **←** = zucca riceve.

| Topic | Dir | Retained | Contenuto |
| --- | --- | --- | --- |
| `pumpkin/<id>/availability` | → | sì | `online` a ogni connessione; `offline` via LWT (keepalive 30 s) |
| `pumpkin/<id>/state/<key>` | → | vedi sotto | stato delle entità (12 chiavi) |
| `pumpkin/<id>/set/light` | ← | no | JSON luce `{"state","brightness","color","effect"}` |
| `pumpkin/<id>/set/auto` | ← | no | `ON` (AUTO) / `OFF` (MANUAL) |
| `pumpkin/<id>/set/trigger_mm` | ← | no | numero 100–10000 mm (step 50) |
| `pumpkin/<id>/set/cooldown_s` | ← | no | numero 0–3600 s |
| `pumpkin/<id>/set/trigger_anim` | ← | no | nome effetto (registry led_fx) |
| `pumpkin/<id>/set/scare` | ← | no | `PRESS` (pulsante: spavento manuale) |
| `pumpkin/<id>/set/update` | ← | no | JSON OTA `{"url","version","release_summary","title"}` |
| `pumpkin/<id>/update/state` | → | sì | versione firmware installata |
| `pumpkin/<id>/update/latest` | ← | — | ultima versione nota: **nessun backend nel firmware** (lo pubblica HA app/desktop) |
| `pumpkin/<id>/update/status` | → | no | eventi OTA `started`/`progress`/`success`/`failed`/`rejected` |
| `pumpkin/<id>/status/reset` | → | sì | riepilogo reset pubblicato a ogni (ri)connessione MQTT: `{"count","reason","reason_str","v"}` |
| `pumpkin/<id>/status/stability` | → | no | ogni 60 s: uptime, heap free/min, max block, riconnessioni Wi-Fi/MQTT, reset, versione |
| `pumpkin/<id>/status/tasks` | → | no | ogni 60 s: nome + high water mark per task |
| `pumpkin/<id>/event/trigger` | → | no | alla transizione →TRIGGERED: `{"v":1,"ts","distance_mm","anim"}` (ADR-004) |
| `homeassistant/device/<id>/config` | → | sì | discovery device-based; payload vuoto = rimozione |
| `homeassistant/status` | ← | (retained HA) | birth `online` → ripubblicazione discovery+stato dopo 1–5 s casuali |

Subscribe: `pumpkin/<id>/set/#` e `homeassistant/status`. Retained sulle sole
`availability`, `light`, `auto`, `trigger_mm`, `cooldown_s`, `trigger_anim`,
`update/state`, `status/reset`. Semantica completa e payload:
[`docs/mqtt.md`](mqtt.md).

## Partizioni (flash 4 MB)

| Nome | Tipo | Offset | Dimensione |
| --- | --- | --- | --- |
| `nvs` | data/nvs | 0x9000 | 16K |
| `otadata` | data/ota | 0xd000 | 8K |
| `phy_init` | data/phy | 0xf000 | 4K |
| `ota_0` | app/ota_0 | 0x10000 | **1984K** |
| `ota_1` | app/ota_1 | 0x200000 | **1984K** |
| `coredump` | data/coredump | 0x3f0000 | 64K |

4 MB usati al completo (0x10000+0x1F0000=0x200000; 0x200000+0x1F0000=0x3F0000;
+64K=0x400000). Build di riferimento 2026-10-26: `pumpkin.bin` 1.400.352 B su
1984K (**31,1% libero**), IRAM 125.590/131.072 (**95,8%**, 5.482 B liberi),
DRAM statica 51,4%, bootloader 0x6600 (~9% libero). Con `BOOTLOADER_APP_ROLLBACK_ENABLE=y`
(`sdkconfig.defaults`) l'immagine nuova parte in `PENDING_VERIFY`.

## Flusso OTA step-by-step

1. **Comando** su `set/update` (da entità update di HA, app desktop o console `ota`).
   `ha_entities` estrae `url` (obbligatorio) e passa a `pumpkin_ota_start`.
2. **Gate** (prima di toccare la flash): URL https ≤512 B (`bad_url`), batteria
   ≥40% se nota (`battery_low`; sconosciuta = consentita con `"battery":"unknown"`),
   corrente LED ≤500 mA (`led_full_light`), heap libero e max block ≥64 KB
   (`heap_low`), nessun update in corso (`busy`). Rifiuto → evento `rejected`.
3. **Avvio task** `pumpkin_ota` (8192 B, prio 5): legge la CA con `ha_mqtt_ca_get`
   (`no_ca` se assente), apre HTTPS (verifica certificato **condivisa col
   trasporto MQTT**, non disabilitabile).
4. **Versione**: legge il descrittore dell'immagine scaricata; identica alla
   versione in esecuzione → abort con `same_version` (`v` iniziale ignorata).
5. **Download** in chunk con evento `progress` ogni 10% e feed del task watchdog.
6. **Finish**: `esp_https_ota_finish` marca l'immagine `PENDING_VERIFY` (rollback
   abilitato). Evento `success`, attesa 3 s per il flush MQTT, `esp_restart()`.
7. **Conferma post-reboot** (`sys_health`): l'immagine diventa VALID solo se
   Wi-Fi su + MQTT connesso + sensore radar ok + FSM in IDLE o OFF. Altrimenti
   nessuna conferma: al riavvio successivo il **bootloader fa rollback** allo
   slot precedente. Tutto da verificare **a banco** (non ancora esercitato):
   [`docs/report_stabilita.md`](report_stabilita.md).

## Confini architetturali

- **`config_store` è la fonte unica** dei parametri utente (ADR-007): HA, console
  e provisioning scrivono solo lì; il radar tiene la sua copia NVS locale per
  `trigger_mm` e viene risincronizzato a ogni `CONFIG_EVENT_CHANGED` (e al boot).
- **FSM → LED solo via `fsm_output_t`** (ADR-007): il core FSM non include
  led_effects né config_store; l'ordine di applicazione è del layer IDF.
- **Nessun lavoro pesante nel callback esp_mqtt**: le callback utente girano nel
  worker di `ha_mqtt`; i listener `ha_mqtt_on_connected_add` (sys_health) pure.
- **CA TLS unica per MQTT e OTA** (ADR-009): namespace NVS `pumpkin_ca` o
  partizione, letta da `ha_mqtt_ca_get`; da console con `ha ca begin/+riga`
  (commit automatico su `END CERTIFICATE`).
- **Rete di proprietà dell'app** (ADR-008): netif STA + `esp_wifi_init` creati
  una volta sola in `main`; `net_mgr_start` li adotta; overlay LED per lo stato
  rete/provisioning, la FSM non sa nulla della rete.
- **Contratto trigger** verso il nodo audio/video (ADR-004): QoS 0 non retained
  su `event/trigger`, nessun handshake.
- **Discovery device-based** con `components` e `unique_id` stabili (ADR-005):
  un solo messaggio retained, ripubblicato a ogni connessione e alla birth di HA.
- **OTA con rollback e conferma di salute** (ADR-009): il firmware non marca mai
  l'immagine valida al termine del download; la conferma è di `sys_health` con
  criterio Wi-Fi+MQTT+sensore+FSM (deviazione documentata dal "solo IDLE" del
  prompt: la zucca spenta deve essere aggiornabile da remoto).
