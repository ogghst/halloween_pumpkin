---
titolo: Striscia LED WS2812B (32 pixel)
tipo: hardware
stato: verificato
aggiornato: 2026-10-25
fonti: [docs/pinout.md, components/led_effects/Kconfig, brief utente 2026-10-25]
correlate: [esp32-s3-devkitc1.md, ../componenti/led_effects.md, ../decisioni/adr-002-nmosfet-rail-switch.md, ../../../docs/pinout.md]
---

# Striscia LED WS2812B

Striscia da **32× WS2812B**: LED RGB indirizzabili, protocollo monofilo, ordine canali **GRB**.
Variante **SK6812 RGBW** opzionale via `CONFIG_LED_FX_RGBW` (default n).

## Cablaggio (da docs/pinout.md)

```text
GPIO5 ──[330 Ω]── 1A ┤74AHCT125├ 1Y ──► DIN striscia     (VCC buffer = 5 V, OE a GND)
GPIO4 ──[100 Ω]────────────────► Gate N-MOSFET (interruttore rail, low-side, LED_EN attivo alto)
Rail +5 V dal boost ──► striscia; 1000 µF tra +5 V e GND vicino al primo pixel
```

- Il buffer 74AHCT125 porta i livelli logici della linea dati a 5 V, per un segnale affidabile.
- Il condensatore da 1000 µF assorbe i picchi di corrente della striscia sul rail.
- A rail spento la linea dati resta bassa (RMT idle): i LED non vengono alimentati dal pin dati.

## Sequenze gestite dal motore

- **ON**: `LED_EN` alto → attesa 20 ms → primo frame renderizzato.
- **OFF**: frame nero (`led_strip_clear` + `led_strip_refresh`) → `LED_EN` basso.

## Parametri collegati

- Default pin nel [Kconfig del componente](../../../components/led_effects/Kconfig): `LED_FX_DATA_GPIO` (5), `LED_FX_EN_GPIO` (4).
- Dettagli del motore e stima corrente: [componente led_effects](../componenti/led_effects.md).
