---
titolo: Misura corrente del rail LED
tipo: procedura
stato: verificato
aggiornato: 2026-10-25
fonti: [docs/pinout.md, README.md]
correlate: [../componenti/led_effects.md, ../misure/bilancio-energetico-stima.md, flash-monitor.md, ../../../docs/pinout.md]
---

# Misura corrente di banco del rail LED

Procedura documentata in [docs/pinout.md](../../../docs/pinout.md). Le **misure sono pending**
(hardware non ancora collegato): questa pagina fissa il metodo e i valori attesi.

## Setup

- Multimetro (oppure INA219) **in serie sul +5 V del rail LED**, tra boost e matrice.
- App di test flashata e console pronta: [flash e monitor](flash-monitor.md).

## Scenari

| Scenario | Comandi console | Atteso (misura fisica) |
| -------- | --------------- | ---------------------- |
| Luce ridotta | `fx candle` + `bright 26` (~10%) | da leggere a banco |
| Piena luce, cap 60% (default) | `fx solid` + `color 255 255 255` + `bright 255` | **~2,3 A** |
| Piena luce, cap 100% | idem, con `CONFIG_LED_FX_MAX_BRIGHTNESS_PCT=100` (ricompilare) | **~3,84 A** |

## Confronto con la stima software

- In ogni scenario leggere il comando `current` e confrontarlo col multimetro.
- **Attenzione**: la stima SW è calcolata sul frame lineare **prima** di gamma/luminosità,
  quindi **non riflette il cap**: può restare ~3,84 A anche con il cap al 60% (64 px ×
  3 canali × 20 mA = 3840 mA a scala 100).

## Registrare

- Aggiornare la tabella in pinout.md e il [bilancio energetico](../misure/bilancio-energetico-stima.md);
  annotare l'esito in [log](../log.md).
