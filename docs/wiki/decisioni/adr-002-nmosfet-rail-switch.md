---
titolo: ADR-002 — Interruttore rail con N-MOSFET low-side
tipo: decisione
stato: verificato
aggiornato: 2026-10-25
fonti: [docs/pinout.md, brief utente 2026-10-25]
correlate: [../hardware/led-ws2812b.md, ../componenti/led_effects.md, ../domande-aperte.md, ../../../docs/pinout.md]
---

# ADR-002 — N-MOSFET low-side per il rail LED

## Contesto

Il rail LED a 5 V (32× WS2812B, fino a ~1,9 A) deve poter essere spento via firmware per
risparmiare batteria; i GPIO dell'ESP32-S3 sono a 3,3 V.

## Scelta

- **N-MOSFET low-side**: Source a GND, Drain al GND della striscia; Gate pilotato da GPIO4
  tramite resistenza da 100 Ω, con pull-down da 100 kΩ (rail spento al boot e al reset).
- **LED_EN attivo alto**: comando `power` in console / `led_fx_power(true)`.
- Implementato in Fase 1: cablaggio in [docs/pinout.md](../../../docs/pinout.md), comando `power`.

## Alternative scartate

- **P-MOSFET high-side**: un GPIO a 3,3 V non spegne un P-MOSFET high-side alimentato a 5 V
  (Vgs insufficiente a portare il gate vicino al source).

## Conseguenze

- La linea dati resta bassa a rail spento (RMT idle): nessun alimentamento parassita via DIN.
- Nota in pinout: confermare a banco il tipo di interruttore effettivamente cablato
  (low-side vs high-side) — [domande aperte](../domande-aperte.md); il firmware non cambia
  (LED_EN attivo alto in entrambi i casi).
