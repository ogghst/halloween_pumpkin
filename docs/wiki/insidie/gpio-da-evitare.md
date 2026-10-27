---
titolo: GPIO da evitare sull'ESP32
tipo: insidia
stato: verificato
aggiornato: 2026-10-25
fonti: [datasheet Espressif ESP32 (classico), components/radar_ld2420/Kconfig, docs/pinout.md]
correlate: [../hardware/esp32-d1-mini.md, ../hardware/led-ws2812b.md, ../hardware/hlk-ld2410b.md, ../hardware/vl53l1x.md, ../decisioni/adr-006-pivot-esp32-d1-mini.md]
---

# GPIO da evitare sull'ESP32 (classico, D1 mini)

Prima di assegnare un GPIO a un segnale nuovo, escludere:

| GPIO | Motivo |
| ---- | ------ |
| 6–11 | **Flash SPI** interna (vietati; l'11 era il vecchio OUT del radar, spostato) |
| 0, 2, 12, 15 | **Strapping**: campionati al reset. Il 12 (MTDI) regola la tensione flash: **se alto al reset il boot fallisce** (era il vecchio RX del radar, spostato) |
| 1, 3 | **UART0** (console via CP2102) |
| 34–39 | **Input-only**, senza pull-up interni |
| 16, 17 | Liberi solo su moduli **WROOM** (PSRAM su WROVER) |

Fonte: datasheet Espressif ESP32.

## Pin di progetto attuali (Fase 1/2)

- **GPIO5** LED_DATA e **GPIO4** LED_EN ([matrice WS2812B](../hardware/led-ws2812b.md)):
  GPIO5 è strapping SDIO ma **benigno** — al boot il rail LED è spento per il pull-down del
  MOSFET ([ADR-002](../decisioni/adr-002-nmosfet-rail-switch.md)).
- **GPIO26 (RX), 27 (TX), 25 (OUT)** del radar ([HLK-LD2410B](../hardware/hlk-ld2410b.md),
  [Kconfig](../../../components/radar_ld2420/Kconfig)): tutti full-featured, senza strap né
  conflitti; sostituiscono i vecchi 12/13/11, vietati o a rischio sul classico.
- Per GPIO nuovi (es. l'I2C di un futuro sensore) scegliere fuori da tutte le righe e
  documentare subito in [docs/pinout.md](../../../docs/pinout.md).

## Se si tornasse all'ESP32-S3

Vincoli della board DevKitC-1, validi fino al pivot del 2026-10-25
([ADR-006](../decisioni/adr-006-pivot-esp32-d1-mini.md), pagina storica:
[DevKitC-1](../hardware/esp32-s3-devkitc1.md)):

| GPIO | Motivo |
| ---- | ------ |
| 0, 3, 45, 46 | **Strapping**: campionati al reset, influenzano la modalità di avvio |
| 19, 20 | **USB D-/D+** (USB-CDC / serial-JTAG del DevKitC-1) |
| 26–32 | **Flash SPI** interna |
| 33–37 | **PSRAM octale** |
