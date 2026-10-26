---
titolo: ADR-009 — OTA HTTPS con rollback e conferma di salute
tipo: decisione
stato: assunto
aggiornato: 2026-10-26
fonti: [components/pumpkin_ota (pumpkin_ota.c, ota_policy.c, Kconfig), components/sys_health (sys_health.c, sys_health_core.c, Kconfig), CMakeLists.txt (PROJECT_VER), partitions.csv, sdkconfig.defaults, host test pumpkin_ota/sys_health verdi 2026-10-26]
correlate: [../componenti/pumpkin-ota.md, ../componenti/sys-health.md, ../componenti/ha-mqtt.md, adr-005-discovery-device-based.md, adr-007-fsm-config-store.md, ../procedure/flash-monitor.md, ../../../docs/report_stabilita.md]
---

# ADR-009 — OTA HTTPS con rollback e conferma di salute

## Contesto

Con MQTT e Home Assistant attivi (Fase 5) la zucca vive su un tetto in giardino:
l'aggiornamento firmware deve avvenire senza staccare alimentazione né portare il
PC in giardino. Vincoli: flash 4 MB (Fase 7 riduce gli slot OTA a 1984K per
sistemare il coredump), IRAM già stretta con BLE+Wi-Fi+mDNS, nessun backend di
release, CA TLS già gestita da `ha_mqtt` (Fase 5), batteria senza misuratore
(power_mgr non esiste ancora).

## Scelta

- **esp_https_ota (API ADVANCED)** con verifica del certificato **condivisa col
  trasporto MQTT**: la CA arriva da `ha_mqtt_ca_get` (NVS `pumpkin_ca` o
  partizione, caricata da console con `ha ca begin/+riga`). Verifica non
  disabilitabile; senza CA l'update è rifiutato (`no_ca`) — HTTP in chiaro mai.
- **Gate di politica prima di toccare la flash** (`ota_policy.c`, puro e
  host-testato): `bad_url` (assente/non https/>512 B), `battery_low` (<40% se
  nota), `led_full_light` (stima corrente LED > 500 mA: si evita di sommare il
  picco della scrittura flash alla piena luce), `heap_low` (libero o max block
  < 64 KB), `busy` (una istanza per volta), `same_version`.
- **Rollback abilitato** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`): l'immagine
  installata parte in `PENDING_VERIFY` e il firmware **non** chiama
  `esp_ota_mark_app_valid` al termine del download. La **conferma è di
  `sys_health` dopo il riavvio**: Wi-Fi su + MQTT connesso + sensore radar ok +
  FSM in IDLE o OFF; altrimenti il bootloader fa rollback allo slot precedente
  al riavvio successivo. **Deviazione documentata dal criterio "solo FSM IDLE"
  del prompt della fase**: la FSM deve poter stare anche in OFF perché la zucca
  spenta (comando HA) deve restare aggiornabile da remoto; OFF è comunque uno
  stato "quiete" con rail LED spento, equivalente per rischio a IDLE.
- **Batteria sconosciuta = update consentito con avviso**: `power_mgr` non
  esiste, il provider restituisce -1; l'evento `started` porta
  `"battery":"unknown"`. Quando un provider di batteria si registrerà
  (`pumpkin_ota_set_battery_provider`), il gate `<40%` si attiverà da solo,
  senza tocchi al codice.
- **Stessa versione rifiutata**: confronto tra il descrittore dell'immagine
  scaricata (`esp_app_desc`) e la versione in esecuzione, ignorando una `v`
  iniziale; `version`/`release_summary`/`title` del comando restano metadati
  informativi.
- **Partizioni**: `ota_0`/`ota_1` a **1984K** (binario di integrazione
  1.400.352 B, 31,1% libero) e **`coredump` 64K** in coda ai 4 MB (4 MB usati
  al completo).
- **`PROJECT_VER` da `git describe --tags --dirty --always`** (CMakeLists
  radice, fallback `0.0.0-dev`): entra in `sw_version` del device HA, in
  `update/state` e nel confronto `same_version` senza tocchi manuali.
- Eventi di stato su `update/status` (started/progress ogni 10%/success/failed/
  rejected, JSON < 512 B); al `success` attesa 3 s per il flush MQTT e riavvio.

## Alternative scartate

- **Update HTTP in chiaro o TLS senza verifica**: un firmware che si
  sovrascrive da un canale non autenticato annulla ogni altra precauzione; la
  CA condivisa col MQTT costa zero gestione in più.
- **API semplice di esp_https_ota**: niente controllo versione prima di
  scrivere né abort pulito: serve l'ADVANCED per rifiutare `same_version`
  senza toccare la flash.
- **Conferma al termine del download** (mark valid prima del reboot): se la
  nuova immagine non bootava, la zucca restava bricked a distanza; il rollback
  del bootloader copre proprio il caso "non torna su".
- **Criterio di conferma "solo FSM IDLE"** (come da prompt): esclude le zucche
  spente da remoto dall'aggiornamento; OFF accettato come quiete equivalente
  (deviazione documentata sopra).
- **Rifiutare update con batteria sconosciuta**: oggi bloccherebbe ogni update
  per sempre (nessun misuratore a bordo); meglio consapevolezza
  (`"battery":"unknown"` nell'evento) che stallo.
- **Slot OTA 1536K (Fase 4)**: il binario completo supera 1,3 MB, margine
  insufficiente per le prossime fasi; il coredump non aveva dove stare.

## Conseguenze

- **IRAM al 95,8%** (125.590/131.072, 5.482 B liberi) con TLS+OTA a bordo:
  ogni aggiunta va misurata subito con `idf.py size`
  ([insidia IRAM](../insidie/iram-nimble-wifi.md)). Rischio principale.
- Slot da 1984K (~31% di margine) e coredump (64K) rendono i panic
  analizzabili a distanza (`espcoredump.py -c coredump info build/pumpkin.elf`).
- Il task OTA (8192 B, prio 5) esiste solo durante l'update ed è iscritto al
  task watchdog (`PUMPKIN_OTA_TASK_WDT`): un download bloccato > 5 s riavvia la
  board (rientra nel rollback: immagine ancora PENDING_VERIFY).
- La politica (`ota_policy`) è pura e host-testata; il layer IDF misura
  batteria/corrente/heap e pubblica gli eventi via `ha_mqtt_publish`.
- **Tutta la catena OTA resta da verificare a banco** (download reale, conferma,
  rollback con immagine che non conferma, 24 h): procedura e criteri nel
  [report di stabilità](../../../docs/report_stabilita.md); stato: assunto
  (verificato in build, non sul campo).
