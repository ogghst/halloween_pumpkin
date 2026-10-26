# Wiki halloween_pumpkin

Catalogo delle pagine. Una riga per pagina; il diario delle modifiche sta in [log.md](log.md).

## Radice

- [Glossario](glossario.md) — termini del progetto: ADR, RMT, ToF, OTA, GRB, MOSFET low-side, ...
- [Domande aperte](domande-aperte.md) — tutto ciò che è stima o non ancora verificato.

## Hardware

- [ESP32 D1 mini](hardware/esp32-d1-mini.md) — board corrente (ESP32 classico, ADR-006): chip D0WD-V3, flash 4 MB, console UART0 via CP2102 su `/dev/ttyUSB0` (su quest'unità), partizioni OTA 2×1536K.
- [DevKitC-1 (obsoleta)](hardware/esp32-s3-devkitc1.md) — board precedente, mai ricevuta; sostituita dall'ESP32 D1 mini il 2026-10-25 ([ADR-006](decisioni/adr-006-pivot-esp32-d1-mini.md)).
- [LED WS2812B](hardware/led-ws2812b.md) — matrice 8×8 da 64 pixel (catena DIN unica, mappatura serpentina), ordine GRB, buffer 74AHCT125 e rail 5 V.
- [HLK-LD2420](hardware/hlk-ld2420.md) — **modulo radar fisico** (presenza 24 GHz, pin 3V3/GND/OT1/RX/OT2; ruolo OT1/OT2 e baud dipendono dal firmware): cablaggio 3,3 V e assunzioni aperte.
- [HLK-LD2410B (protocollo storico)](hardware/hlk-ld2410b.md) — obsoleta: il modulo fisico è un LD2420 e dal 2026-10-25 il firmware parla LD2420; resta il riferimento storico del protocollo v1.07 e delle sue errata.
- [VL53L1X](hardware/vl53l1x.md) — sensore ToF rimandato a una fase successiva (la Fase 2 usa il radar HLK-LD2420); pin I2C non ancora definiti.
- [Alimentazione](hardware/alimentazione.md) — 2×18650, boost 5 V per i LED, LDO 3,3 V per l'ESP32.

## Componenti

- [led_effects](componenti/led_effects.md) — motore effetti a 60 fps, sei effetti, mappatura matrice 8×8, pixel test, stima corrente, comandi console.
- [radar_ld2420](componenti/radar_ld2420.md) — radar presenza HLK-LD2420 su UART1 (firmware adattato al protocollo LD2420 il 2026-10-25): parser (frame energia + righe testo per fw ≤ 1.5.3), codec comandi, probe baud automatico, trigger, calibrazione, guasto/recovery con backoff, comandi console.
- [App principale](componenti/app-principale.md) — skeleton Fase 0 in `main/` e app di test `test_apps/led_effects`.

## Decisioni

- [ADR-001 — ESP-IDF v5.4 LTS](decisioni/adr-001-esp-idf-v5.4-lts.md) — toolchain stabile già installata e usata in Fase 0/1.
- [ADR-002 — N-MOSFET low-side](decisioni/adr-002-nmosfet-rail-switch.md) — interruttore del rail LED pilotato da GPIO4, LED_EN attivo alto.
- [ADR-003 — Alimentazione separata](decisioni/adr-003-alimentazione-separata.md) — ESP32 su LDO batteria sempre attivo, boost solo per i LED.
- [ADR-004 — Trigger via MQTT](decisioni/adr-004-trigger-mqtt-contratto.md) — contratto di evento verso il nodo audio/video.
- [ADR-005 — Discovery device-based](decisioni/adr-005-discovery-device-based.md) — un solo messaggio di discovery per Home Assistant.
- [ADR-006 — Pivot su ESP32 D1 mini](decisioni/adr-006-pivot-esp32-d1-mini.md) — dalla S3 mai ricevuta al classico al banco: flash 4 MB, console UART0/CP2102, radar su GPIO26/27/25.

## Misure

- [Bilancio energetico (stima)](misure/bilancio-energetico-stima.md) — ~11,5 Wh per 8 h con 30 min a piena luce, da misurare.

## Procedure

- [Flash e monitor](procedure/flash-monitor.md) — build, flash e monitor seriale con ESP-IDF.
- [Host test](procedure/host-tests.md) — unit test della logica pura su host con CMake + CTest.
- [Misura corrente rail](procedure/misura-corrente-rail.md) — misura di banco della corrente del rail LED, confronto con la stima SW.
- [Server MQTT locale (Mosquitto)](procedure/mosquitto.md) — broker di test sul PC: attivo ma solo localhost, client CLI da installare, listener per l'ESP32 in Fase 5.

## Insidie

- [GPIO da evitare](insidie/gpio-da-evitare.md) — pin strapping, flash SPI, UART0 e input-only sull'ESP32 classico (con sezione storica per l'S3).
- [LD2410B: errata protocollo v1.07](insidie/ld2410b-protocollo-errata.md) — contraddizioni del documento ufficiale; interesse storico/di confronto (il firmware ora parla LD2420).
- [LD2420: buffer comandi 32 byte](insidie/ld2420-buffer-comandi.md) — scritture oltre 5 pair scartate in silenzio: chunking obbligatorio (verificato a banco).
- [Tick FreeRTOS a 100 Hz](insidie/freertos-tick-100hz.md) — serve `CONFIG_FREERTOS_HZ=1000` per il periodo da 16,7 ms.
- [VL53L1X e luce solare](insidie/vl53l1x-luce-solare.md) — ipotesi di disturbo del ToF in luce diretta.
- [Light sleep, Wi-Fi e RMT](insidie/light-sleep-wifi-rmt.md) — convivenza da verificare per power_mgr.
