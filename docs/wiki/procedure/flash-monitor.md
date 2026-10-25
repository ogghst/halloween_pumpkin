---
titolo: Build, flash e monitor seriale
tipo: procedura
stato: verificato
aggiornato: 2026-10-25
fonti: [README.md]
correlate: [../hardware/esp32-s3-devkitc1.md, ../componenti/app-principale.md, ../../../README.md]
---

# Flash e monitor seriale

## Preparare l'ambiente (ogni nuova shell)

```bash
source ~/esp/esp-idf/export.sh
```

## Prima volta su una cartella di progetto

```bash
idf.py set-target esp32s3     # rigenera il sdkconfig dai defaults
```

## Build, flash, monitor

```bash
ls /dev/ttyACM*               # trova la porta (attesa: /dev/ttyACM0)
idf.py -C test_apps/led_effects build flash monitor   # app di test LED
idf.py -p /dev/ttyACM0 flash monitor                  # app principale
```

- Per uscire dal monitor: `Ctrl+]`.
- La porta `/dev/ttyACM0` è quella attesa ma non ancora confermata fisicamente
  ([domande aperte](../domande-aperte.md)); alternativa: `ls /dev/ttyUSB*`.

## Verificare le partizioni dopo la build

```bash
idf.py partition_table
```

Atteso su flash 8 MB: `nvs` 24K, `phy_init` 4K, `otadata` 8K, `ota_0` 3M, `ota_1` 3M.
Dettagli board: [ESP32-S3 DevKitC-1](../hardware/esp32-s3-devkitc1.md).
