---
titolo: ADR-003 — ESP32 su LDO separato dal boost
tipo: decisione
stato: assunto
aggiornato: 2026-10-25
fonti: [brief utente 2026-10-25]
correlate: [../hardware/alimentazione.md, adr-002-nmosfet-rail-switch.md, ../insidie/light-sleep-wifi-rmt.md, ../misure/bilancio-energetico-stima.md]
---

# ADR-003 — Alimentazione separata: LDO per l'ESP32, boost per i LED

## Contesto

Due carichi con profili opposti: l'ESP32 deve restare attivo (ascolto MQTT futuro, sensore ToF),
il rail LED assorbe fino a ~3,84 A (matrice 8×8 da 64 pixel) ma serve solo quando la zucca è accesa.

## Scelta

- ESP32 alimentato **dalla batteria via LDO 3,3 V**: mai spento.
- Rail LED alimentato dal **boost 5 V**, abilitabile via N-MOSFET ([ADR-002](adr-002-nmosfet-rail-switch.md)):
  a riposo il boost resta spento, l'ESP32 no.

## Alternative scartate

- ESP32 a valle del boost: costringerebbe il boost a restare sempre acceso, con un consumo
  a riposo inaccettabile per un oggetto a batteria.

## Conseguenze

- L'assetto a riposo si riduce a LDO + ESP32: il consumo effettivo va misurato
  (corrente di riposo del boost tra le [domande aperte](../domande-aperte.md)).
- Presupposto per il risparmio energetico (light sleep) di power_mgr, fase futura:
  vedi [ipotesi light sleep + Wi-Fi + RMT](../insidie/light-sleep-wifi-rmt.md).
- **Assunto dal brief**: hardware non ancora montato, nessuna misura a supporto.
