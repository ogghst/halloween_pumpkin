---
titolo: Sensore ToF VL53L1X
tipo: hardware
stato: assunto
aggiornato: 2026-10-25
fonti: [README.md, brief utente 2026-10-25]
correlate: [esp32-s3-devkitc1.md, ../insidie/vl53l1x-luce-solare.md, ../domande-aperte.md]
---

# Sensore ToF VL53L1X

Sensore di distanza **Time of Flight** previsto per la **Fase 2** (`tof_sensor`): rileva la mano
di chi si avvicina alla zucca per attivare gli effetti.

## Stato attuale

- **Non ancora presente nel repo**: nessun componente, driver o cablaggio definito.
- **Pin I2C non ancora definiti**: domanda aperta, da chiudere quando si pianifica la Fase 2
  (vedi [domande aperte](../domande-aperte.md)).
- Il README lo colloca nella roadmap tra `led_effects` e `pumpkin_fsm`.

## Cosa già noto

- Il bus sarà I2C su due GPIO liberi del DevKitC-1, da scegliere fuori dai pin vietati:
  [GPIO da evitare](../insidie/gpio-da-evitare.md).
- Ipotesi di disturbo in luce solare diretta, da caratterizzare con il sensore in mano:
  [VL53L1X e luce solare](../insidie/vl53l1x-luce-solare.md).

## Stato

- **Assunto**: esiste solo come voce di roadmap (README) e brief utente; nessun dettaglio tecnico verificato.
