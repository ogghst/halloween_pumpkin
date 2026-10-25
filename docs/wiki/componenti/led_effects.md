---
titolo: Componente led_effects
tipo: componente
stato: verificato
aggiornato: 2026-10-25
fonti: [README.md, components/led_effects/include/led_fx.h, components/led_effects/Kconfig, components/led_effects/idf_component.yml]
correlate: [../hardware/led-ws2812b.md, ../insidie/freertos-tick-100hz.md, ../procedure/host-tests.md, ../procedure/flash-monitor.md, ../../../components/led_effects/include/led_fx.h]
---

# Componente `led_effects`

Motore di rendering degli effetti LED in `components/led_effects/`, consegnato con la Fase 1.

## Architettura

- Task `anim` a **60 fps** con `vTaskDelayUntil` (periodo ~16,7 ms): richiede `CONFIG_FREERTOS_HZ=1000` — [insidia tick](../insidie/freertos-tick-100hz.md).
- Coda comandi: le richieste dell'API vengono accodate e applicate dal task di rendering.
- **6 effetti**: `solid`, `candle`, `breathe`, `rainbow`, `flicker`, `scare` (one-shot).
- Driver: dipendenza `espressif/led_strip` **3.0.1** via RMT ([idf_component.yml](../../../components/led_effects/idf_component.yml), `idf >= 5.4`).
- Rail: `led_fx_power()` realizza le sequenze ON/OFF della [striscia](../hardware/led-ws2812b.md).

## Stima corrente (hook Fase 6)

- Calcolata sul **frame lineare PRIMA di gamma e luminosità**: quindi **non riflette il cap** `LED_FX_MAX_BRIGHTNESS_PCT` (default 60).
- Base: `FX_MA_PER_CHANNEL` = **20 mA per canale a piena scala**, costante in `fx_current.h`.
- Nessun limitatore attivo oggi: solo stima e fattore di scala (`led_fx_set_current_scale_pct`).

## API e configurazione

- API pubblica in [led_fx.h](../../../components/led_effects/include/led_fx.h): init, power/is_powered, set_effect, set_color, set_brightness, set_speed, play_oneshot, statistiche, stima corrente.
- Header per dominio in `components/led_effects/`: `fx_effects.h`, `fx_color.h`, `fx_current.h`, `fx_types.h`.
- [Kconfig](../../../components/led_effects/Kconfig): pixel (32), GPIO dati (5), GPIO EN (4), ordine GRB/RGB, RGBW opzionale, cap luminosità (60%), fps (60).

## Comandi console (app di test)

`fx`, `oneshot`, `color`, `bright`, `speed`, `power`, `stats`, `current`, `scale` — tabella completa nel [README](../../../README.md).
