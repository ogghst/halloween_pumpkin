---
titolo: Matrice LED WS2812B 8×8 (64 pixel)
tipo: hardware
stato: assunto
aggiornato: 2026-10-27
fonti: [docs/pinout.md, components/led_effects/Kconfig, brief utente 2026-10-25 (conversione da striscia 32 px a matrice 8×8), conferma utente 2026-10-27 (occhi = 65o e 66o LED della stessa catena)]
correlate: [esp32-s3-devkitc1.md, ../componenti/led_effects.md, ../decisioni/adr-002-nmosfet-rail-switch.md, ../domande-aperte.md, ../../../docs/pinout.md]
---

# Catena LED WS2812B: matrice 8×8 + occhi (66 pixel)

**Matrice 8×8 = 64× WS2812B + 2 occhi in coda = 66× WS2812B** in un'unica catena DIN:
LED RGB indirizzabili, protocollo monofilo, ordine canali **GRB**. Variante **SK6812
RGBW** opzionale via `CONFIG_LED_FX_RGBW`
(default n). Sostituisce la striscia da 32 pixel della Fase 1 (conversione del 2026-10-25);
dalla Fase 8 la catena include i **2 LED degli occhi** (65° e 66° LED, indici 0-based
64 e 65, stesso tipo e cablaggio del corpo, alimentati dal rail LED); il conteggio vive
nel [Kconfig del componente](../../../components/led_effects/Kconfig):
`CONFIG_LED_FX_NUM_PIXELS` (default **66**) e `CONFIG_LED_FX_EYES_START` (default 64)
sono l'unica fonte di verità. Cablaggio e iniezione di alimentazione in coda:
[docs/pinout.md](../../../docs/pinout.md) e [ADR-010](../decisioni/adr-010-occhi-zone.md).

## Mappatura dei pixel (assunto, da verificare a banco)

Layout **serpentina row-major, origine alto-sinistra**: pixel 0 = (x0,y0); righe pari
sx→dx, righe dispari dx→sx.

```text
index = y*8 + (y pari ? x : 7-x)      // 0..63
```

- Accesso dal codice: `led_fx_xy_index(x, y)` ritorna l'indice 0..63 e `-1` fuori da [0,8);
  dimensioni in `LED_FX_MATRIX_W`/`LED_FX_MATRIX_H` (= 8) in [led_fx.h](../../../components/led_effects/include/led_fx.h).
- Verifica di banco col comando console `pixel <x> <y> <r> <g> <b>` (un solo pixel acceso):
  provare i quattro angoli (0,0), (7,0), (0,7), (7,7) e l'inizio delle righe 0/1; se l'ordine
  fisico reale differisce, si corregge solo la mappatura software, non il cablaggio.
- La conferma a banco va annotata in [domande aperte](../domande-aperte.md) e in [log](../log.md).

## Cablaggio (da docs/pinout.md)

```text
GPIO5 ──[330 Ω]── 1A ┤74AHCT125├ 1Y ──► DIN matrice     (VCC buffer = 5 V, OE a GND)
GPIO4 ──[100 Ω]────────────────► Gate N-MOSFET (interruttore rail, low-side, LED_EN attivo alto)
Rail +5 V dal boost ──► matrice; 1000 µF tra +5 V e GND vicino al primo pixel
```

- Il buffer 74AHCT125 porta i livelli logici della linea dati a 5 V, per un segnale affidabile
  lungo l'intera catena dei 64 pixel.
- Il condensatore da 1000 µF assorbe i picchi di corrente della matrice sul rail.
- A rail spento la linea dati resta bassa (RMT idle): i LED non vengono alimentati dal pin dati.

## Sequenze gestite dal motore

- **ON**: `LED_EN` alto → attesa 20 ms → primo frame renderizzato.
- **OFF**: frame nero (`led_strip_clear` + `led_strip_refresh`) → `LED_EN` basso.

## Parametri collegati

- Conteggio e pin di default nel [Kconfig del componente](../../../components/led_effects/Kconfig):
  `LED_FX_NUM_PIXELS` (64), `LED_FX_DATA_GPIO` (5), `LED_FX_EN_GPIO` (4).
- Dettagli del motore, pixel test e stima corrente: [componente led_effects](../componenti/led_effects.md).
