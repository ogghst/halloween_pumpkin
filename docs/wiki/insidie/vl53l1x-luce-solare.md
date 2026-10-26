---
titolo: VL53L1X e luce solare diretta
tipo: insidia
stato: ipotesi
aggiornato: 2026-10-25
fonti: [brief utente 2026-10-25]
correlate: [../hardware/vl53l1x.md]
---

# VL53L1X e luce solare diretta

## L'ipotesi

La **luce solare diretta** sul sensore può disturbare le misure ToF (rumore ambientale nello
spettro ricevuto), riducendo il raggio utile o generando letture spurie. Ipotesi non verificata:
il sensore non è ancora nel repo ([VL53L1X](../hardware/vl53l1x.md), rimandato a una fase
successiva: la Fase 2 usa il radar HLK-LD2420).

## Impatto potenziale

- Falsi trigger o mancati trigger quando `tof_sensor` alimenterà `pumpkin_fsm`.
- La zucca è pensata per l'uso notturno: l'esposizione diretta è un caso limite (esterno di
  giorno), ma da caratterizzare prima di fidarsi del sensore in qualsiasi condizione.

## Come uscire dall'ipotesi

1. A sensore cablato: serie di misure a distanza fissa in ombra e a sole diretto, confronto
   di rumore e raggio utile.
2. Se confermato: prima i filtri lato firmware (mediana, soglie di coerenza tra letture),
   poi eventuali rimedi ottici (posizionamento, schermature).
