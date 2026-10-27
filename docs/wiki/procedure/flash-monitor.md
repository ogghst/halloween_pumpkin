---
titolo: Build, flash e monitor seriale
tipo: procedura
stato: verificato
aggiornato: 2026-10-26
fonti: [esptool su /dev/ttyUSB0 2026-10-25, README.md, partitions.csv, sdkconfig.defaults, build incrementali misurate al banco 2026-10-26]
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

## Build incrementali (il default)

`idf.py build` è **incrementale**: ninja ricompila solo i sorgenti cambiati e
riusa `build/` e `sdkconfig`. È il percorso normale — non cancellare `build/`
né `sdkconfig` tra una build e l'altra.

Tempi misurati su questo PC (2026-10-26):

| Cosa | Tempo |
| --- | --- |
| Modifica sorgente, build incrementale (app di banco) | ~35 s |
| Modifica sorgente, build incrementale (app principale) | ~75 s |
| Build completa da `build/` vuoto (app di banco) | ~3,5 min |
| `set-target` + build completa (app principale) | ~5 min |

Rigenerare da zero **solo** quando:

- cambi `sdkconfig.defaults` (i default si applicano solo alla creazione del
  sdkconfig): `rm sdkconfig && idf.py set-target esp32`;
- cambi `partitions.csv`;
- errori di link inspiegati o sospetti di artefatti obsoleti: `idf.py fullclean`
  come ultima spiaggia.

Nota flash: la porta deve essere libera — un monitor seriale aperto (idf.py
monitor, app desktop) blocca `idf.py flash` con
`device reports readiness to read but returned no data / multiple access on port`;
chiudere il monitor (`fuser -v /dev/ttyUSB0` per trovare il processo).

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
