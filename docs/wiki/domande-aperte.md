# Domande aperte

Tutto ciò che resta stima, non verificato o da decidere. Ogni voce rinvia alla pagina correlata.

1. **Immunità VL53L1X alla luce solare diretta** — ipotesi di disturbo del ToF (sensore rimandato a una fase successiva: la Fase 2 usa il radar HLK-LD2420), nessuna misura prevista finché il sensore non è cablato. → [insidie/vl53l1x-luce-solare.md](insidie/vl53l1x-luce-solare.md)
2. **Light sleep + Wi-Fi attivo + RMT** — compatibilità da verificare: il rail LED usa RMT e power_mgr è una fase futura. → [insidie/light-sleep-wifi-rmt.md](insidie/light-sleep-wifi-rmt.md)
3. **Bilancio energetico ~11,5 Wh per 8 h** — solo stima da brief, da misurare a banco. → [misure/bilancio-energetico-stima.md](misure/bilancio-energetico-stima.md)
4. **Pin I2C per il VL53L1X** — non ancora definiti né serviti: il sensore è rimandato a una fase successiva (la Fase 2 usa il radar HLK-LD2420); sul classico GPIO8/9 sono flash SPI. → [hardware/vl53l1x.md](hardware/vl53l1x.md)
5. **Celle 18650 effettive e consumo a riposo** — modello/capacità reali e corrente di riposo del boost ancora da determinare (stima consumo a riposo). → [hardware/alimentazione.md](hardware/alimentazione.md)
6. **Tipo di interruttore rail cablato** — confermare a banco low-side N-MOSFET vs high-side (nota in `docs/pinout.md`); il firmware resta identico (LED_EN attivo alto). → [decisioni/adr-002-nmosfet-rail-switch.md](decisioni/adr-002-nmosfet-rail-switch.md)
7. **File di progetto mancanti** — `AGENTS.md` viene creato con questo seed della wiki; `CLAUDE.md` e `prompts/fase*.md` non esistono: chiedere all'utente se crearli. → [log.md](log.md)
8. **Mappatura serpentina della matrice 8×8** — l'indice `y*8 + (y pari ? x : 7-x)` con origine alto-sinistra è un **assunto hardware**: verificare a banco col comando `pixel` (angoli e primi pixel delle righe 0/1); se l'ordine fisico differisce si corregge solo la mappatura software. → [hardware/led-ws2812b.md](hardware/led-ws2812b.md)
9. **Ruolo dell'uscita presenza (OT2) e polarità** — i ruoli OT1 (seriale) / OT2 (presenza) sono CONFERMATI a banco (fw v1.5.9 ≥1.5.3, 115200); resta da verificare la polarità dell'uscita presenza (assunto: alto = presente) misurando il pin con un bersaglio. → [hardware/hlk-ld2420.md](hardware/hlk-ld2420.md)
10. **Consumo del radar su 3,3 V** — da misurare (alimentato dal LDO insieme all'ESP32, verifica anche della capacità dell'LDO) per chiudere il bilancio. → [misure/bilancio-energetico-stima.md](misure/bilancio-energetico-stima.md)
11. **Immunità EMI boost→radar** — il boost a pieno carico (rainbow 100%) disturba il radar (ora su LDO 3,3 V)? Test 30 min a stanza vuota previsto dalla procedura. → [procedura radar](../procedura_radar.md)
12. **Doppia persistenza di `trigger_mm`** — il radar LD2420 conserva la sua copia NVS (namespace `radar`), ma la fonte di verità è `config_store` (namespace `pumpkin`, [ADR-007](decisioni/adr-007-fsm-config-store.md)): al boot vince `config_store`, che risincronizza il radar. Il sync e la precedenza vanno esercitati a banco. → [decisioni/adr-007-fsm-config-store.md](decisioni/adr-007-fsm-config-store.md)
13. **Validazione dei nomi effetto differita al consumo** — `config_store` salva `idle_anim`/`trigger_anim` come stringhe senza validarle contro il registry `led_effects`: la validità emerge solo quando la FSM le applica (`led_fx_set_effect` in errore → log, **nessun fallback automatico**); un nome invalido in NVS lascia il look senza effetto applicato. → [componenti/config_store.md](componenti/config_store.md)
14. **Tempi e persistenza della FSM da confermare a banco** — durata trigger (default 8 s) e cooldown (30 s) reali su board, persistenza di `power_on` e dei look dopo reboot, debounce NVS (2 s) e `cfg_flush` su flash reale. → [docs/fsm.md](../fsm.md), [componenti/pumpkin_fsm.md](componenti/pumpkin_fsm.md)
15. **Riconnessioni dopo il ritorno del router** — 5 cicli router off/on: riconnessione entro 2 min, backoff osservato nei log `net` (atteso 1→60 s ±15%), overlay rosso→verde. Checklist in [procedure/provisioning.md](procedure/provisioning.md).
16. **Latenza con power save e listen interval** — ping a banco con `net ps min` vs `net ps max` e `net listen 1` vs `net listen 10`; i valori misurati andranno in una pagina misure. → [componenti/net-mgr.md](componenti/net-mgr.md)
17. **Coesistenza BLE + radar UART + RMT** — nessun glitch LED (overlay compreso) durante provisioning BLE attivo e traffico di rete con effetti in ciclo; seriali e timing stabili. → [componenti/wifi-prov.md](componenti/wifi-prov.md)
18. **Rilascio BTDM e re-provisioning** — heap libero prima/dopo il provisioning BLE (log tag `prov`); conferma che il re-provisioning BLE dopo il primo richiede reboot (comportamento volato, [ADR-008](decisioni/adr-008-rete-provisioning.md)).
19. **mDNS sulla LAN** — `avahi-browse -r _pumpkin._tcp` mostra hostname `pumpkin-xxxxxx` e TXT `fw/rssi/ch/reconnects` aggiornati dal timer diagnostico. → [componenti/net-mgr.md](componenti/net-mgr.md)
20. **PoP derivata dal MAC** — assunzione da validare in sala: il MAC è pubblico, quindi `pump-XXXXXX` è debole contro un attaccante locale motivato ([ADR-008](decisioni/adr-008-rete-provisioning.md)); se serve più robustezza, PoP su display/QR o provisioning SoftAP in locale. → [componenti/wifi-prov.md](componenti/wifi-prov.md)
21. **Corrente a riposo con Wi-Fi connesso** — misura col `ps min modem` attivo per completare il bilancio energetico (Wi-Fi acceso a riposo non è ancora in stima). → [misure/bilancio-energetico-stima.md](misure/bilancio-energetico-stima.md)
22. **Margine IRAM della pila di rete** — con BLE+Wi-Fi+mDNS l'app di banco usa il 93% dell'IRAM (9 KB liberi): ogni componente aggiuntivo va misurato con `idf.py size` prima di accettarlo. → [insidie/iram-nimble-wifi.md](insidie/iram-nimble-wifi.md)
23. **Versione Home Assistant installata dall'utente** — la discovery device-based con chiave `components` richiede HA ≥ 2024.11: se l'installazione è più vecchia la discovery non appare e va convertita per-entità. → [decisioni/adr-005-discovery-device-based.md](decisioni/adr-005-discovery-device-based.md)
24. **Discovery end-to-end sul device HA** — dopo il provisioning MQTT, il device "Zucca pumpkin-`<id>`" appare in HA con le 12 entità (11 della Fase 5 + `update` di Fase 7; screenshot della scheda da conservare). → [docs/mqtt.md](../mqtt.md)
25. **Availability offline entro il keepalive** — staccando l'alimentazione della board, `pumpkin/<id>/availability` passa a `offline` (LWT) entro 30 s + margine, osservato con `mosquitto_sub`. → [componenti/ha-mqtt.md](componenti/ha-mqtt.md)
26. **Ripubblicazione al riavvio di HA** — birth `homeassistant/status` = `online` → discovery+stato ripubblicati dopo il ritardo casuale 1–5 s (visibile nel log e nel sub). → [docs/mqtt.md](../mqtt.md)
27. **Riconnessione broker/router** — riavvio di Mosquitto o del router: riconnessione esp_mqtt (~10 s) seguita da ripubblicazione di discovery e stato. → [docs/mqtt.md](../mqtt.md)
28. **Margine flash/IRAM dopo `mqtt`** — `idf.py size` dell'app di banco `test_apps/ha_mqtt` con esp_mqtt+cJSON: binario entro lo slot 1536K e IRAM libera misurata. → [insidie/iram-nimble-wifi.md](insidie/iram-nimble-wifi.md)
29. **Listener Mosquitto esposto e credenziali in NVS** — listener 0.0.0.0 con `allow_anonymous false` + `password_file` (sudo dell'utente, mai in chat/repo) e credenziali MQTT provisionate in `config_store`. → [procedure/mosquitto.md](procedure/mosquitto.md), [procedure/provisioning.md](procedure/provisioning.md)
30. **Semantica dell'entità update con HA reale** — l'entità appare in HA e
    `update/state` funziona; l'install dal bottone "Aggiorna" di HA **non è
    supportata per scelta** (2026-10-27): la discovery non manda
    `payload_install` (HA risponde `'payload_install'`) e il firmware esige un
    `url` https in `set/update` che HA non può fornire (nessun backend
    release). Install solo da pannello desktop, console `ota <url>` o
    `mosquitto_pub`. Resta da osservare: il flusso informativo
    `update/latest` → "aggiornamento disponibile". → [componenti/ha-mqtt.md](componenti/ha-mqtt.md), [insidie/ota-ca-config.md](insidie/ota-ca-config.md), [docs/mqtt.md](../mqtt.md)
31. **OTA end-to-end** — build nuova → server HTTPS dell'app desktop (CA via seriale) → `set/update` → `success` → riavvio → conferma `sys_health` (`ota_state=VALID` in `sysinfo`); mai esercitato: board non collegata. → [componenti/pumpkin-ota.md](componenti/pumpkin-ota.md), [report di stabilità](../report_stabilita.md)
32. **Gate batteria da attivare con power_mgr** — oggi nessun provider: batteria sconosciuta = update consentito con `"battery":"unknown"`; quando `power_mgr` esisterà registrare `pumpkin_ota_set_battery_provider` e verificare il rifiuto sotto il 40%. → [decisioni/adr-009-ota-rollback.md](decisioni/adr-009-ota-rollback.md)
33. **Corsa di stabilità 24 h** — 0 reset inattesi, heap minimo stabile, riconnessioni Wi-Fi/broker con recovery automatico, 3 riavvii router, 2 riavvii broker, 100 trigger simulati (`tools/soak_mqtt.py`); criteri e template in [report di stabilità](../report_stabilita.md).
34. **IRAM sotto soglia con MQTTs+OTA in runtime** — 95,8% in build (5.482 B liberi): il link passa, resta da confermare al banco che il runtime (TLS attivo, download OTA, heap frammentato) non consumi il margine fino a degradare. → [insidie/iram-nimble-wifi.md](insidie/iram-nimble-wifi.md)

35. **Tensione al pixel 65 a piena luce** — criterio Fase 8: ≥ 4,5 V con tutta la catena
    a bianco pieno, senza viraggio dei colori; richiede iniezione di alimentazione in
    coda vicino agli occhi ([docs/pinout.md](../pinout.md), [ADR-010](decisioni/adr-010-occhi-zone.md)).
36. **Corrente reale con occhi al massimo** — criterio ±15% dalla stima (~120 mA occhi
    soli; ~3,96 A catena intera a scala 100); procedura in [docs/pinout.md](../pinout.md)
    e [misura corrente rail](procedure/misura-corrente-rail.md).
37. **Distanza pixel 63 → occhi** — se > ~30 cm: cavo intrecciato dato+GND, eventuale
    resistenza serie sul dato, 100 µF vicino agli occhi; da confermare in montaggio.
38. **Soak Fase 8** — 30 min a 60 fps con zero frame persi (`stats` console) anche con
    Wi-Fi/BLE attivi, e 1 h con 100 trigger simulati senza anomalie (glare sync, nessun
    brownout/reset); da eseguire a banco dopo il riaccensione post-download-mode.
39. **Riaccensione post download-mode (2026-10-27)** — la board è entrata in download
    mode a ogni reset (GPIO0 basso) durante le manovre seriale; se al riaccensione
    persiste, verificare tasto FLASH bloccato o filo su G0.
40. **RISOLTA (2026-10-27)** — `eyes` console mostrava `effect=off` con valori piano
    IDLE: la FSM era in **MANUAL** (letto da console: `state=MANUAL (3)`), stato in cui
    `eyes_ctrl` per design non tocca gli occhi; l'effetto "off" era un comando esterno
    ("Occhi" OFF da HA). Parametri base restati quelli del piano IDLE applicato al boot.
    Nessun difetto.

## Risolte

1. **Porta seriale reale** — **risolta il 2026-10-25**: collegata la board, il bridge è un
   **CP2102** su **`/dev/ttyUSB0`** (verificato con esptool; la vecchia attesa era
   `/dev/ttyACM0` via USB-CDC sulla board S3 mai ricevuta). → [hardware/esp32-d1-mini.md](hardware/esp32-d1-mini.md)
2. **Adattamento protocollo o sostituzione modulo** — **risolta il 2026-10-25**: firmware
   **adattato al protocollo LD2420** (componente rinominato `radar_ld2420`; 50/50 test host,
   build esp32 zero warning, banco senza alimentazione del sensore: guasto/degrado/backoff
   verificati). → [hardware/hlk-ld2420.md](hardware/hlk-ld2420.md), [componenti/radar_ld2420.md](componenti/radar_ld2420.md)
