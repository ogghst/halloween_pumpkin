---
titolo: Board ESP32-S3 DevKitC-1
tipo: hardware
stato: verificato
aggiornato: 2026-10-25
fonti: [README.md, partitions.csv, sdkconfig.defaults, commit d0644b9]
correlate: [led-ws2812b.md, ../decisioni/adr-001-esp-idf-v5.4-lts.md, ../procedure/flash-monitor.md, ../insidie/gpio-da-evitare.md, ../../../partitions.csv]
---

# ESP32-S3 DevKitC-1

Board di sviluppo ufficiale Espressif che ospita il firmware della zucca.

## Fatti

- Toolchain **ESP-IDF v5.4.4** installata nativamente in `~/esp/esp-idf`, tool in `~/.espressif` (fuori dal repo); ogni nuova shell richiede `source ~/esp/esp-idf/export.sh`.
- Programmazione via **USB-CDC**: porta attesa `/dev/ttyACM0` (il README dice "in genere": confermare al primo collegamento fisico — vedi [domande aperte](../domande-aperte.md)).
- **Flash 8 MB** con partition table custom a due slot OTA in [partitions.csv](../../../partitions.csv): `nvs` 24K, `phy_init` 4K, `otadata` 8K, `ota_0` 3M, `ota_1` 3M, ~1,9 MB di riserva in coda.
- [sdkconfig.defaults](../../../sdkconfig.defaults) minimo: target `esp32s3`, flash 8 MB, partition table custom, `CONFIG_FREERTOS_HZ=1000`.
- La configurazione globale va aggiunta **solo in `sdkconfig.defaults`**: il `sdkconfig` generato è gitignored.

## Uso

- Build/flash/monitor: [procedura flash e monitor](../procedure/flash-monitor.md).
- Peripherals in Fase 1: GPIO5 (dati LED) e GPIO4 (enable rail) — [matrice WS2812B](led-ws2812b.md).
- Prima di assegnare GPIO nuovi escludere i pin vietati: [GPIO da evitare](../insidie/gpio-da-evitare.md).
