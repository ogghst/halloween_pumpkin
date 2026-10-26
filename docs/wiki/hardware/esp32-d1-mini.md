---
titolo: Board ESP32 D1 mini
tipo: hardware
stato: verificato
aggiornato: 2026-10-25
fonti: [esptool su /dev/ttyUSB0 2026-10-25, conferma utente pin header 2026-10-25, partitions.csv, sdkconfig.defaults]
correlate: [../decisioni/adr-006-pivot-esp32-d1-mini.md, ../procedure/flash-monitor.md, ../insidie/gpio-da-evitare.md, led-ws2812b.md, ../../../partitions.csv]
---

# ESP32 D1 mini

Board di sviluppo per l'**ESP32 classico** che ospita il firmware della zucca dal pivot del
2026-10-25 (decisione: [ADR-006](../decisioni/adr-006-pivot-esp32-d1-mini.md)).

## Fatti (esptool 2026-10-25)

- Chip **ESP32-D0WD-V3**, revisione **v3.1**: Wi-Fi + BT, dual core 240 MHz, cristallo 40 MHz.
- **Flash 4 MB** (manufacturer 0x54, device 0x4016); tensione flash decisa dallo strapping: 3,3 V.
- Seriale: bridge **CP2102** (Silicon Labs, USB 10c4:ea60) → porta **`/dev/ttyUSB0`** —
  **su quest'unità**: molte D1 mini montano invece un CH340. L'ESP32 classico **non ha**
  USB-CDC/JTAG nativo: la console è **UART0 a 115200**.
- MAC: `e4:65:b8:71:7e:50`.
- **Cloni D1 mini non standardizzati**: i pin esposti variano tra le unità; i pin di progetto
  (LED **GPIO4/5**, radar **GPIO26/27/25**) sono in genere presenti sull'header — da confermare
  con le scritte a bordo (assunto, non verificato).

## Uso

```bash
source ~/esp/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 flash monitor
```

Procedura completa: [flash e monitor](../procedure/flash-monitor.md).

## Partizioni attese (flash 4 MB)

[partitions.csv](../../../partitions.csv) dual-OTA:

| Partizione | Dimensione | Offset |
| ---------- | ---------- | ------ |
| `nvs` | 16K | 0x9000 |
| `otadata` | 8K | 0xd000 |
| `phy_init` | 4K | 0xf000 |
| `ota_0` | 1984K | 0x10000 |
| `ota_1` | 1984K | 0x200000 |
| `coredump` | 64K | 0x3f0000 |

Fase 7: slot OTA allargati a 1984K e coredump in coda; 4 MB usati al completo
(verifica dopo build: `idf.py partition_table`).

Pin di progetto: LED_DATA **GPIO5**, LED_EN **GPIO4** ([matrice WS2812B](led-ws2812b.md));
radar **GPIO26/27/25** ([HLK-LD2410B](hlk-ld2410b.md)). Prima di assegnare GPIO nuovi:
[GPIO da evitare](../insidie/gpio-da-evitare.md).

> **Verificato al banco (2026-10-25)**: le scritte a bordo confermano che i pin di
> progetto **4, 5, 25, 26, 27** sono presenti sull'header (conferma utente). Per gli
> altri pin i cloni D1 mini restano variabili: controllare l'header prima di usare
> un GPIO nuovo.
