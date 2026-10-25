# Pinout e cablaggio LED — Fase 1

## Schema di cablaggio

```text
ESP32-S3 DevKitC-1                 74AHCT125 (VCC=5V, GND comune, OE1..4=GND)
  GPIO5 (LED_DATA) ──[330 Ω]── 1A          1Y ────► DIN matrice 8×8 (64× WS2812B, catena unica)
  GPIO4 (LED_EN) ──[100 Ω]─────────────────► Gate N-MOSFET (interruttore rail LED)
                                             100 kΩ tra Gate e GND (pulldown)
Rail LED: +5 V dal boost ──► matrice; GND matrice ──► Drain MOSFET, Source ──► GND (switch low-side)
Condensatore 1000 µF tra +5 V e GND del rail, vicino al primo pixel.
```

> **Ipotesi**: interruttore low-side con N-MOSFET; se il MOSFET è high-side (P o driver dedicato), il Gate va pilotato di conseguenza — adattare senza cambiare il firmware (LED_EN attivo alto in entrambi i casi).

## Tabella pin

| Segnale  | GPIO | Direzione | Funzione                                                            | Kconfig (default nel menuconfig) |
| -------- | ---- | --------- | ------------------------------------------------------------------- | -------------------------------- |
| LED_DATA | 5    | output    | linea dati della matrice (catena DIN unica), via buffer 74AHCT125 (dopo la resistenza da 330 Ω) | `LED_FX_DATA_GPIO` (default 5)   |
| LED_EN   | 4    | output    | gate del MOSFET che abilita il rail LED (attivo alto)               | `LED_FX_EN_GPIO` (default 4)     |

I default sono definiti nel `Kconfig` del componente `led_effects` e modificabili da `idf.py menuconfig`.

## Sequenze di alimentazione (implementate dal motore)

- **ON**: `LED_EN` alto → attesa 20 ms → primo frame renderizzato (il frame esce nella stessa iterazione del task `anim`).
- **OFF**: frame nero (`led_strip_clear` + `led_strip_refresh`) → `LED_EN` basso.
- **A rail spento** la linea dati resta bassa (RMT idle): i LED non vengono alimentati dal pin dati durante lo spegnimento.

## Assunzioni hardware dichiarate

- Matrice **8×8 = 64× WS2812B** in un'unica catena DIN; conteggio da `CONFIG_LED_FX_NUM_PIXELS` (default 64), unica fonte di verità.
- Ordine canali **GRB** (default, `CONFIG_LED_FX_ORDER_GRB`); variante SK6812 RGBW opzionale via `CONFIG_LED_FX_RGBW`.
- **Mappatura serpentina row-major, origine alto-sinistra** (pixel 0 = x0,y0; righe pari sx→dx, righe dispari dx→sx):
  `index = y*8 + (y pari ? x : 7-x)` — **assunto da verificare al banco**: con il comando console
  `pixel <x> <y> <r> <g> <b>` accendere i quattro angoli e i primi pixel delle righe 0/1; se l'ordine
  reale differisce si corregge solo la mappatura software (`led_fx_xy_index`), non il cablaggio.
- Corrente assorbita da un canale a piena scala: **20 mA** (`FX_MA_PER_CHANNEL`, base della stima software di corrente).

## Misura corrente di banco

Procedura: multimetro in serie sul **+5 V del rail LED** (oppure INA219); per ogni scenario leggere la stima con il comando console `current` e confrontarla con il valore misurato.

Scenari:

1. `fx candle` + `bright 26` (~10% di luminosità).
2. `fx solid` + `color 255 255 255` + `bright 255` con cap di luminosità al 60% (`CONFIG_LED_FX_MAX_BRIGHTNESS_PCT`, default) → atteso ~2,3 A; per il test al 100% ricompilare l'app con `CONFIG_LED_FX_MAX_BRIGHTNESS_PCT=100` → atteso ~3,84 A.

| Scenario | Stima SW (mA) | Misurato (mA) |
| -------- | ------------- | ------------- |
| `fx candle` + `bright 26` (~10%) | — (da leggere con `current` a banco) | — (hardware non ancora collegato) |
| `fx solid` + `color 255 255 255` + `bright 255` (cap 60%) | 3840 (`current`, frame lineare bianco pieno a scala 100) | — (hardware non ancora collegato) |
| come sopra, con `CONFIG_LED_FX_MAX_BRIGHTNESS_PCT=100` | 3840 | — (hardware non ancora collegato) |

Nota: la stima software (`current`) è calcolata sul frame lineare prima di gamma/luminosità, quindi non riflette il cap di luminosità dell'uscita; i valori attesi ~2,3 A (cap 60%) e ~3,84 A (cap 100%) si riferiscono alla misura fisica (64 px × 3 canali × 20 mA = 3840 mA a scala 100).
