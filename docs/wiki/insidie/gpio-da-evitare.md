---
titolo: GPIO da evitare sull'ESP32-S3
tipo: insidia
stato: verificato
aggiornato: 2026-10-25
fonti: [datasheet Espressif ESP32-S3, docs/pinout.md]
correlate: [../hardware/esp32-s3-devkitc1.md, ../hardware/led-ws2812b.md, ../hardware/vl53l1x.md]
---

# GPIO da evitare sull'ESP32-S3

Prima di assegnare un GPIO a un segnale nuovo (es. l'I2C del VL53L1X), escludere:

| GPIO | Motivo |
| ---- | ------ |
| 0, 3, 45, 46 | **Strapping**: campionati al reset, influenzano la modalità di avvio |
| 19, 20 | **USB D-/D+** (USB-CDC / serial-JTAG del DevKitC-1) |
| 26–32 | **Flash SPI** interna |
| 33–37 | **PSRAM octale** |

Fonte: datasheet Espressif ESP32-S3.

## Regola pratica

- Pin liberi e validi attualmente usati in Fase 1: **GPIO4** (LED_EN) e **GPIO5** (LED_DATA),
  coerenti con la tabella sopra e con il [Kconfig](../../../components/led_effects/Kconfig).
- Per l'I2C della Fase 2 scegliere due GPIO fuori da tutte le righe e documentarli subito
  in [docs/pinout.md](../../../docs/pinout.md).
