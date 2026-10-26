---
titolo: ADR-006 — Pivot hardware su ESP32 D1 mini
tipo: decisione
stato: verificato
aggiornato: 2026-10-25
fonti: [esptool su /dev/ttyUSB0 2026-10-25, partitions.csv, sdkconfig.defaults, components/radar_ld2420/Kconfig]
correlate: [../hardware/esp32-d1-mini.md, ../hardware/esp32-s3-devkitc1.md, ../insidie/gpio-da-evitare.md, ../procedure/flash-monitor.md, ../../../partitions.csv]
---

# ADR-006 — Pivot hardware: da ESP32-S3 DevKitC-1 a ESP32 D1 mini

## Contesto

Le verifiche di Fase 1/2 (run 30 min senza frame persi, misura corrente rail, collegamento
fisico del radar) richiedono hardware reale al banco: la simulazione non basta. La
**ESP32 D1 mini (ESP32 classico)** è presente ed è stata verificata con esptool il
2026-10-25 (chip ESP32-D0WD-V3 rev v3.1, flash 4 MB, bridge CP2102) — vedi
[board](../hardware/esp32-d1-mini.md). La DevKitC-1 S3 non è invece fisicamente disponibile.

## Scelta

- Target **`esp32` (classico)** in [sdkconfig.defaults](../../../sdkconfig.defaults) e
  nell'app di test `test_apps/led_effects`.
- Board di riferimento: **ESP32 D1 mini**, console UART0 115200 via CP2102 su `/dev/ttyUSB0`.

## Alternative scartate

- **Attendere / riacquistare una DevKitC-1 S3**: bloccherebbe la chiusura a banco di Fase 1/2
  senza benefici per il firmware (ESP-IDF v5.4 e i componenti sono indipendenti dal target;
  le API usate — RMT, UART, FreeRTOS — esistono identiche sul classico).

## Conseguenze

- **Flash 4 MB**: partition table dual-OTA ridotta — `ota_0`/`ota_1` da 1536K
  ([partitions.csv](../../../partitions.csv)), ~0,9 MB di riserva in coda.
- **Console**: niente USB-CDC/JTAG nativo — UART0 115200 tramite bridge **CP2102** su
  `/dev/ttyUSB0` ([flash e monitor](../procedure/flash-monitor.md)).
- **Radar spostato su GPIO 26 (RX) / 27 (TX) / 25 (OUT)**
  ([Kconfig](../../../components/radar_ld2420/Kconfig)): i vecchi 12/11 sono vietati sul
  classico — GPIO12 strapping (MTDI, tensione flash: alto al reset = boot fallito), GPIO11
  flash SPI interna. Dettagli: [GPIO da evitare](../insidie/gpio-da-evitare.md).
- **LED_DATA su GPIO5**: è strapping SDIO ma **benigno** — al boot il rail LED è spento per il
  pull-down del MOSFET ([ADR-002](adr-002-nmosfet-rail-switch.md)); LED_EN resta su GPIO4.
- **Stime di [ADR-002](adr-002-nmosfet-rail-switch.md) e
  [ADR-003](adr-003-alimentazione-separata.md) invariate**: logica a 3,3 V e rail LED a 5 V
  sono identici sul classico.
- Light sleep / modem sleep con Wi-Fi: supportati dal classico, verifica su questa combinazione
  ancora aperta ([insidia](../insidie/light-sleep-wifi-rmt.md)).
