---
titolo: ADR-005 — Discovery Home Assistant device-based
tipo: decisione
stato: assunto
aggiornato: 2026-10-25
fonti: [brief utente 2026-10-25]
correlate: [adr-004-trigger-mqtt-contratto.md]
---

# ADR-005 — Discovery Home Assistant con messaggio device-based

## Contesto

Home Assistant deve scoprire la zucca (entità LED, sensori, trigger) senza configurazione
manuale. Le funzionalità appartengono tutte allo stesso oggetto fisico.

## Scelta

- Usare la **MQTT discovery di Home Assistant in forma device-based**: un unico messaggio
  (blocco compatto) che registra il device con le sue entità, invece di un messaggio per entità.

## Alternative scartate

- Discovery per-entità con topic separati: più messaggi da mantenere coerenti tra loro e
  più traffico a ogni boot, per nessun beneficio con un device unico.

## Conseguenze

- Dipende dal contratto MQTT di [ADR-004](adr-004-trigger-mqtt-contratto.md); si realizza
  nella fase `ha_mqtt`, futura.
- La sintassi discovery va validata contro la versione di Home Assistant in uso: **assunto**,
  non ancora verificato.
