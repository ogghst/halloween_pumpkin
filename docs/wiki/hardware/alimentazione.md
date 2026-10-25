---
titolo: Alimentazione (2×18650, boost e LDO)
tipo: hardware
stato: assunto
aggiornato: 2026-10-25
fonti: [brief utente 2026-10-25, docs/pinout.md]
correlate: [led-ws2812b.md, ../decisioni/adr-003-alimentazione-separata.md, ../misure/bilancio-energetico-stima.md, ../procedure/misura-corrente-rail.md]
---

# Alimentazione

Schema previsto dal brief utente 2026-10-25; **hardware non ancora montato**, tutto da confermare a banco.

## Architettura prevista

- **2× celle 18650 in parallelo** come sorgente primaria (modello e capacità effettivi da definire).
- **Boost a 5 V** dedicato al rail LED: alimenta solo la matrice WS2812B, resta spento a riposo.
- **LDO 3,3 V** per l'ESP32, collegato direttamente alla batteria: l'ESP32 resta sempre alimentato,
  indipendente dal boost (motivazione in [ADR-003](../decisioni/adr-003-alimentazione-separata.md)).
- Interruttore del rail LED: N-MOSFET low-side pilotato da GPIO4 — [ADR-002](../decisioni/adr-002-nmosfet-rail-switch.md).

## Correnti attese sul rail LED (docs/pinout.md)

- Cap di luminosità 60% (default): **~2,3 A** sul +5 V.
- Cap 100% (richiede ricompilazione con `CONFIG_LED_FX_MAX_BRIGHTNESS_PCT=100`): **~3,84 A**
  (matrice 8×8: 64 px × 3 canali × 20 mA).
- Metodo di misura: [misura corrente rail](../procedure/misura-corrente-rail.md).

## Da chiarire

- Consumo a riposo del boost e capacità reale delle celle: [domande aperte](../domande-aperte.md).
- Autonomia complessiva: [stima ~11,5 Wh per 8 h](../misure/bilancio-energetico-stima.md).
