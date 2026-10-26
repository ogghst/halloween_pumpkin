---
titolo: ADR-005 — Discovery Home Assistant device-based
tipo: decisione
stato: assunto
aggiornato: 2026-10-26
fonti: [brief utente 2026-10-25, prompt Fase 5 2026-10-26, doc ufficiale MQTT Home Assistant (consultata 2026-10-26), ../../mqtt.md]
correlate: [adr-004-trigger-mqtt-contratto.md, ../componenti/ha-mqtt.md, ../domande-aperte.md, ../../mqtt.md]
---

# ADR-005 — Discovery Home Assistant con messaggio device-based

## Contesto

Home Assistant deve scoprire la zucca (entità LED, sensori, trigger) senza configurazione
manuale. Le funzionalità appartengono tutte allo stesso oggetto fisico.

## Scelta

- Usare la **MQTT discovery di Home Assistant in forma device-based**: un unico messaggio
  (blocco compatto) che registra il device con le sue entità, invece di un messaggio per entità.
- Realizzato nella Fase 5 (`ha_mqtt`): schema concreto su
  `homeassistant/device/<id>/config`, retained, con root `device` (identifiers
  `pumpkin_<id>`, `sw_version` da `esp_app_desc` → copre l'OTA), `origin`,
  `availability` (topic LWT) e `components` con una voce per ciascuna delle 11 entità.
- `unique_id` **stabili tra versioni**: `pumpkin_<id>_<key>` con le chiavi entità
  congelate (light, presence, distance, battery_v, battery_pct, rssi, auto, trigger_mm,
  cooldown_s, trigger_anim, scare).
- Ciclo di vita: ripubblicata a ogni connessione MQTT e al **birth** di HA
  (`homeassistant/status` = `online`) dopo un ritardo casuale di 1000–5000 ms;
  **rimozione** = payload vuoto retained sul medesimo topic.
- Payload d'esempio completo e ciclo di vita: [docs/mqtt.md](../../mqtt.md).

## Alternative scartate

- Discovery per-entità con topic separati: più messaggi da mantenere coerenti tra loro e
  più traffico a ogni boot, per nessun beneficio con un device unico.

## Conseguenze

- Il formato richiede **Home Assistant ≥ 2024.11** (chiave `components`): verificato
  contro la **documentazione ufficiale** MQTT di HA (consultata il 2026-10-26), ma NON
  ancora contro l'installazione HA dell'utente (versione non nota): per questo lo stato
  resta **`assunto`** — se la versione installata è più vecchia la discovery va
  convertita per-entità ([domande aperte](../domande-aperte.md)).
- La verifica end-to-end (device che appare in HA con le 11 entità) è da banco.
- Dipende dal contratto MQTT di [ADR-004](adr-004-trigger-mqtt-contratto.md).
