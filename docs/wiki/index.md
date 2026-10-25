# Wiki halloween_pumpkin

Catalogo delle pagine. Una riga per pagina; il diario delle modifiche sta in [log.md](log.md).

## Radice

- [Glossario](glossario.md) — termini del progetto: ADR, RMT, ToF, OTA, GRB, MOSFET low-side, ...
- [Domande aperte](domande-aperte.md) — tutto ciò che è stima o non ancora verificato.

## Hardware

- [ESP32-S3 DevKitC-1](hardware/esp32-s3-devkitc1.md) — board, porta USB-CDC, flash 8 MB e partizioni OTA.
- [LED WS2812B](hardware/led-ws2812b.md) — striscia da 32 pixel, ordine GRB, buffer 74AHCT125 e rail 5 V.
- [VL53L1X](hardware/vl53l1x.md) — sensore ToF della Fase 2, pin I2C non ancora definiti.
- [Alimentazione](hardware/alimentazione.md) — 2×18650, boost 5 V per i LED, LDO 3,3 V per l'ESP32.

## Componenti

- [led_effects](componenti/led_effects.md) — motore effetti a 60 fps, sei effetti, stima corrente, comandi console.
- [App principale](componenti/app-principale.md) — skeleton Fase 0 in `main/` e app di test `test_apps/led_effects`.

## Decisioni

- [ADR-001 — ESP-IDF v5.4 LTS](decisioni/adr-001-esp-idf-v5.4-lts.md) — toolchain stabile già installata e usata in Fase 0/1.
- [ADR-002 — N-MOSFET low-side](decisioni/adr-002-nmosfet-rail-switch.md) — interruttore del rail LED pilotato da GPIO4, LED_EN attivo alto.
- [ADR-003 — Alimentazione separata](decisioni/adr-003-alimentazione-separata.md) — ESP32 su LDO batteria sempre attivo, boost solo per i LED.
- [ADR-004 — Trigger via MQTT](decisioni/adr-004-trigger-mqtt-contratto.md) — contratto di evento verso il nodo audio/video.
- [ADR-005 — Discovery device-based](decisioni/adr-005-discovery-device-based.md) — un solo messaggio di discovery per Home Assistant.

## Misure

- [Bilancio energetico (stima)](misure/bilancio-energetico-stima.md) — ~11,5 Wh per 8 h con 30 min a piena luce, da misurare.

## Procedure

- [Flash e monitor](procedure/flash-monitor.md) — build, flash e monitor seriale con ESP-IDF.
- [Host test](procedure/host-tests.md) — unit test della logica pura su host con CMake + CTest.
- [Misura corrente rail](procedure/misura-corrente-rail.md) — misura di banco della corrente del rail LED, confronto con la stima SW.

## Insidie

- [GPIO da evitare](insidie/gpio-da-evitare.md) — pin strapping, USB, flash e PSRAM sull'ESP32-S3.
- [Tick FreeRTOS a 100 Hz](insidie/freertos-tick-100hz.md) — serve `CONFIG_FREERTOS_HZ=1000` per il periodo da 16,7 ms.
- [VL53L1X e luce solare](insidie/vl53l1x-luce-solare.md) — ipotesi di disturbo del ToF in luce diretta.
- [Light sleep, Wi-Fi e RMT](insidie/light-sleep-wifi-rmt.md) — convivenza da verificare per power_mgr.
