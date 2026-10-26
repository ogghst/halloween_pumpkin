---
titolo: Sensore ToF VL53L1X
tipo: hardware
stato: assunto
aggiornato: 2026-10-25
fonti: [README.md, brief utente 2026-10-25]
correlate: [esp32-d1-mini.md, ../insidie/vl53l1x-luce-solare.md, ../domande-aperte.md]
---

# Sensore ToF VL53L1X

Sensore di distanza **Time of Flight** **rimandato a una fase successiva** (dopo la Fase 2, che
usa il radar [HLK-LD2420](hlk-ld2420.md)): rileverà la mano di chi si avvicina alla zucca per
attivare gli effetti.

## Stato attuale

- **Non ancora presente nel repo**: nessun componente, driver o cablaggio definito.
- **Rimandato**: la Fase 2 usa il radar [HLK-LD2420](hlk-ld2420.md); il ToF non è cablato né
  pianificato.
- **Pin I2C non ancora definiti**: domanda aperta, da chiudere solo quando si pianifica la fase
  che lo adotterà (vedi [domande aperte](../domande-aperte.md)). Sul classico GPIO8/9 non sono
  disponibili (flash SPI): [GPIO da evitare](../insidie/gpio-da-evitare.md).
- Il README lo colloca nella roadmap tra `led_effects` e `pumpkin_fsm`.

## Cosa già noto

- Il bus sarà I2C su due GPIO liberi della D1 mini (ESP32 classico), da scegliere fuori dai
  pin vietati: [GPIO da evitare](../insidie/gpio-da-evitare.md).
- Ipotesi di disturbo in luce solare diretta, da caratterizzare con il sensore in mano:
  [VL53L1X e luce solare](../insidie/vl53l1x-luce-solare.md).

## Stato

- **Assunto**: esiste solo come voce di roadmap (README) e brief utente, rimandata a una fase
  successiva; nessun dettaglio tecnico verificato.
