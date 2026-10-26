---
titolo: Componente led_effects
tipo: componente
stato: verificato
aggiornato: 2026-10-26
fonti: [README.md, components/led_effects/include/led_fx.h, components/led_effects/fx_overlay.c, components/led_effects/fx_overlay.h, components/led_effects/Kconfig, components/led_effects/idf_component.yml, brief utente 2026-10-25 (conversione a matrice 8×8)]
correlate: [../hardware/led-ws2812b.md, ../insidie/freertos-tick-100hz.md, ../procedure/host-tests.md, ../procedure/flash-monitor.md, ../../../components/led_effects/include/led_fx.h]
---

# Componente `led_effects`

Motore di rendering degli effetti LED in `components/led_effects/`, consegnato con la Fase 1.

## Architettura

- Task `anim` a **60 fps** con `vTaskDelayUntil` (periodo ~16,7 ms): richiede `CONFIG_FREERTOS_HZ=1000` — [insidia tick](../insidie/freertos-tick-100hz.md).
- Coda comandi: le richieste dell'API vengono accodate e applicate dal task di rendering.
- **6 effetti**: `solid`, `candle`, `breathe`, `rainbow`, `flicker`, `scare` (one-shot).
- **Matrice 8×8** (64 px, `CONFIG_LED_FX_NUM_PIXELS`): la mappatura serpentina è logica pura in
  `fx_matrix.c`, accesso con `led_fx_xy_index(x, y)` — [hardware](../hardware/led-ws2812b.md).
- **Pixel test**: `led_fx_pixel_test(x, y, r, g, b)` accende il solo pixel (x,y); il task `anim`
  renderizza un frame nero con quel pixel invece dell'effetto corrente, e `led_fx_pixel_test_off()`
  ripristina l'effetto senza reset del timer. Coordinate fuori da [0,8) → `ESP_ERR_INVALID_ARG`.
- Driver: dipendenza `espressif/led_strip` **3.0.1** via RMT ([idf_component.yml](../../../components/led_effects/idf_component.yml), `idf >= 5.4`).
- Rail: `led_fx_power()` realizza le sequenze ON/OFF della [matrice](../hardware/led-ws2812b.md).

## Overlay di stato (Fase 4)

- `led_fx_overlay_set`/`led_fx_overlay_get` (implementati in `fx_overlay.c`, host
  test dedicati in `host_tests/test_fx_overlay.c`): disegnano un pattern tenue
  **sopra** l'effetto corrente, senza toccarlo né toccare la FSM.
- Pattern: `PROV` pulsazione blu lenta (2 s), `CONNECTING` doppio lampeggio blu,
  `NET_UP` tre lampi verdi in 1,5 s con **auto-clear**, `NET_DOWN` pulsazione
  rossa lenta (3 s) — [ADR-008](../decisioni/adr-008-rete-provisioning.md).
- **Fusione max()** per canale DOPO gamma/luminosità, componenti dell'overlay
  limitate a 48/255: visibile anche a luminosità bassa senza accecare; mai
  applicata durante un pixel test; con rail spento nessun frame → invisibile.

## Stima corrente (hook Fase 6)

- Calcolata sul **frame lineare PRIMA di gamma e luminosità**: quindi **non riflette il cap** `LED_FX_MAX_BRIGHTNESS_PCT` (default 60).
- Base: `FX_MA_PER_CHANNEL` = **20 mA per canale a piena scala**, costante in `fx_current.h`.
- Su 64 px: bianco pieno (3 canali) = **3840 mA** a scala 100, **1920 mA** a scala 50;
  con canale RGBW = **5120 mA**.
- Nessun limitatore attivo oggi: solo stima e fattore di scala (`led_fx_set_current_scale_pct`).

## API e configurazione

- API pubblica in [led_fx.h](../../../components/led_effects/include/led_fx.h): init, power/is_powered, set_effect, set_color, set_brightness, set_speed, play_oneshot, led_fx_xy_index, pixel test, statistiche, stima corrente.
- Header per dominio in `components/led_effects/`: `fx_effects.h`, `fx_color.h`, `fx_current.h`, `fx_types.h`; mappatura matrice in `fx_matrix.c` (dichiarata in `led_fx.h`).
- [Kconfig](../../../components/led_effects/Kconfig): pixel (64, matrice 8×8), GPIO dati (5), GPIO EN (4), ordine GRB/RGB, RGBW opzionale, cap luminosità (60%), fps (60).

## Comandi console (app di test)

`fx`, `oneshot`, `color`, `bright`, `speed`, `power`, `pixel` (`pixel <x> <y> <r> <g> <b>` / `pixel off`), `stats`, `current`, `scale` — tabella completa nel [README](../../../README.md).
