---
titolo: ADR-004 — Trigger via MQTT come contratto
tipo: decisione
stato: assunto
aggiornato: 2026-10-25
fonti: [brief utente 2026-10-25, README.md]
correlate: [adr-005-discovery-device-based.md, ../componenti/app-principale.md, ../insidie/light-sleep-wifi-rmt.md]
---

# ADR-004 — Evento di trigger MQTT verso il nodo audio/video

## Contesto

Lo spavento deve essere coordinato tra luci (questo firmware) e audio/video su un altro
dispositivo. Le due parti non condividono codice: serve solo un contratto di messaggi.

## Scelta

- Il firmware pubblica un **evento di trigger via MQTT** quando la FSM (fase `pumpkin_fsm`)
  decide lo spavento; il nodo audio/video consuma quell'evento.
- Fase `ha_mqtt` futura: oggi nel repo non c'è codice MQTT, solo il contratto concettuale.

## Alternative scartate

- Accoppiamento diretto (HTTP o UART tra i dispositivi): più fragile e vincolante di un
  broker MQTT già presente nell'ecosistema domotico.

## Conseguenze

- Topic e payload esatti vanno definiti pianificando `ha_mqtt` (assunto, non specificato).
- Il Wi-Fi deve restare disponibile (o riconnettersi rapidamente) per pubblicare il trigger:
  intersezione con l'[ipotesi light sleep + Wi-Fi + RMT](../insidie/light-sleep-wifi-rmt.md).
- Scoperta del device in Home Assistant separata in [ADR-005](adr-005-discovery-device-based.md).
