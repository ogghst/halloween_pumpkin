---
titolo: ADR-004 — Trigger via MQTT come contratto
tipo: decisione
stato: assunto
aggiornato: 2026-10-26
fonti: [brief utente 2026-10-25, prompt Fase 5 2026-10-26, ../../mqtt.md]
correlate: [adr-005-discovery-device-based.md, ../componenti/app-principale.md, ../insidie/light-sleep-wifi-rmt.md, ../../mqtt.md]
---

# ADR-004 — Evento di trigger MQTT verso il nodo audio/video

## Contesto

Lo spavento deve essere coordinato tra luci (questo firmware) e audio/video su un altro
dispositivo. Le due parti non condividono codice: serve solo un contratto di messaggi.

## Scelta

- Il firmware pubblica un **evento di trigger via MQTT** quando la FSM (fase `pumpkin_fsm`)
  decide lo spavento; il nodo audio/video consuma quell'evento.
- Realizzato nella Fase 5 con il componente `ha_mqtt`; la guida dei topic/payload è in
  [docs/mqtt.md](../../mqtt.md).

## Contratto definito (Fase 5)

- Topic: `pumpkin/<id>/event/trigger` con `<id>` = 6 hex dal MAC STA
  ([albero topic](../../mqtt.md)).
- Payload JSON: `{"v":1,"ts":<ms uptime>,"distance_mm":<n>,"anim":"<nome effetto>"}`.
- **QoS 0, non retained**: nessuna conferma né ritrasmissione — il consumatore deve
  tollerare una perdita (il ciclo riparte al prossimo trigger).
- Pubblicato alla transizione **→ `TRIGGERED`**: `ts` = uptime ms (`esp_timer`, non
  orologio da parete), `distance_mm` = ultima lettura radar, `anim` = effetto avviato
  (`trigger_anim`).
- Nessun handshake di ack: il nodo audio/video avvia la propria traccia alla ricezione.

## Alternative scartate

- Accoppiamento diretto (HTTP o UART tra i dispositivi): più fragile e vincolante di un
  broker MQTT già presente nell'ecosistema domotico.

## Conseguenze

- Il contratto è ora **definito e implementato** (Fase 5); la verifica end-to-end con un
  nodo audio/video reale resta **da banco**: lo stato resta `assunto`
  ([domande aperte](../domande-aperte.md)).
- Il Wi-Fi deve restare disponibile (o riconnettersi rapidamente) per pubblicare il trigger:
  intersezione con l'[ipotesi light sleep + Wi-Fi + RMT](../insidie/light-sleep-wifi-rmt.md).
- Scoperta del device in Home Assistant in [ADR-005](adr-005-discovery-device-based.md).
