---
titolo: Build, flash e monitor seriale
tipo: procedura
stato: verificato
aggiornato: 2026-10-25
fonti: [esptool su /dev/ttyUSB0 2026-10-25, README.md, partitions.csv, sdkconfig.defaults]
correlate: [../hardware/esp32-d1-mini.md, ../componenti/app-principale.md, ../../../README.md]
---

# Flash e monitor seriale

## Preparare l'ambiente (ogni nuova shell)

```bash
source ~/esp/esp-idf/export.sh
```

## Prima volta su una cartella di progetto

```bash
idf.py set-target esp32       # rigenera il sdkconfig dai defaults
```

## Build, flash, monitor

```bash
ls /dev/ttyUSB*               # trova la porta (attesa: /dev/ttyUSB0)
idf.py -C test_apps/led_effects build flash monitor   # app di test LED
idf.py -p /dev/ttyUSB0 flash monitor                  # app principale
```

- Per uscire dal monitor: `Ctrl+]`.
- La porta **`/dev/ttyUSB0`** è confermata (esptool 2026-10-25): l'unità al banco usa il bridge
  **CP2102** (altre D1 mini montano un CH340), non USB-CDC — console su UART0 115200.
  Dettagli: [board](../hardware/esp32-d1-mini.md).

## Verificare le partizioni dopo la build

```bash
idf.py partition_table
```

Atteso su flash 4 MB (Fase 7): `nvs` 16K, `phy_init` 4K, `otadata` 8K, `ota_0` 1984K,
`ota_1` 1984K, `coredump` 64K in coda (4 MB usati al completo). Dettagli board:
[ESP32 D1 mini](../hardware/esp32-d1-mini.md).
