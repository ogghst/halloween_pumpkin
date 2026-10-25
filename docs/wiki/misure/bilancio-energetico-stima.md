---
titolo: Bilancio energetico (stima)
tipo: misura
stato: assunto
aggiornato: 2026-10-25
fonti: [brief utente 2026-10-25, docs/pinout.md]
correlate: [../hardware/alimentazione.md, ../procedure/misura-corrente-rail.md, ../domande-aperte.md]
---

# Bilancio energetico — stima ~11,5 Wh per 8 h

## Scenario di riferimento (brief utente 2026-10-25)

- Autonomia target: **8 ore** di funzionamento serale.
- Di cui **30 minuti a piena luce** (rail LED al massimo, cap 100%).

## Numeri oggi disponibili

- Rail LED a piena luce: **~3,84 A** sul +5 V (cap 100%, matrice 8×8 da 64 px, atteso da pinout);
  al cap 60%: ~2,3 A.
- Sorgente: 2×18650 in parallelo — [alimentazione](../hardware/alimentazione.md).
- Totale **~11,5 Wh** per le 8 h: valore di stima dal brief, non una misura. Il brief è
  **anteriore alla conversione a matrice** (con 32 px il pieno era stimato ~1,92 A): dopo le
  misure di banco il bilancio va ricalcolato.

## Come trasformarla in misura

1. Misurare la corrente reale del rail negli scenari del brief:
   [procedura misura corrente](../procedure/misura-corrente-rail.md).
2. Misurare il consumo a riposo (ESP32 su LDO, boost spento) — la corrente di riposo del boost
   è ancora tra le [domande aperte](../domande-aperte.md).
3. Integrare su una serata reale e ricalcolare il bilancio.

## Stato

- **Assunto**: nessun hardware collegato, nessuna misura; pagina da aggiornare dopo le prime
  misure di banco (registrare l'esito in [log](../log.md)).
