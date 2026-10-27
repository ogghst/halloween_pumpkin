---
titolo: Componente ha_mqtt
tipo: componente
stato: assunto
aggiornato: 2026-10-27
fonti: [prompt Fase 5 2026-10-26, components/ha_mqtt/include/*.h, components/ha_entities/ha_entities.c (17 entità, Fase 8), components/ha_mqtt/host_tests (29 casi verdi 2026-10-27), doc ufficiale MQTT Home Assistant (consultata 2026-10-26)]
correlate: [app-principale.md, pumpkin-ota.md, sys-health.md, ../decisioni/adr-004-trigger-mqtt-contratto.md, ../decisioni/adr-005-discovery-device-based.md, ../decisioni/adr-009-ota-rollback.md, ../procedure/mosquitto.md, ../procedure/host-tests.md, ../domande-aperte.md, ../../mqtt.md]
---

# Componente `ha_mqtt`

Trasporto MQTT + discovery Home Assistant della zucca (`components/ha_mqtt/`, Fase 5),
con il glue `components/ha_entities` che riempie il registro delle **17 entità**
(dalle 11 della Fase 5 alle 12 con l'update della Fase 7 alle 17 con gli occhi della
Fase 8). La guida
d'uso (topic, payload, checklist di banco) è in [docs/mqtt.md](../../mqtt.md);
**stato**: moduli puri verificati su host (29 casi, 2026-10-27), discovery
end-to-end e comandi verificati a banco (Fasi 5-8).

## Architettura

- **Puro (host-testato)**: `ha_registry` (registro entità estendibile, tabella
  `ha_entity_desc_t`), `ha_payloads` (payload JSON via cJSON: discovery, luce,
  eventi, scalari), `ha_commands` (parser/validazione `set/<key>`), `ha_state`
  (rate limit distance/periodico con clock iniettato).
- **Trasporto** (`ha_mqtt.c`): esp_mqtt con LWT `availability` ("offline" retained),
  subscribe `set/#` e `homeassistant/status`, **worker task**: le callback
  `on_connected`/`on_command` girano lì, mai nel callback MQTT. Keepalive 30 s
  (Kconfig), QoS 0, riconnessione automatica 10 s.
- **Glue** (`ha_entities`): registra le **17 entità** sulle sorgenti esistenti (radar,
  FSM, config, diag rete) più l'entità **update** (Fase 7) e le **5 entità occhi**
  (Fase 8: light `eyes`, light colore-only `eyes_idle_color`/`eyes_scare_color`
  via nuovo tipo `HA_ET_LIGHT_RGB`, switch `eyes_enabled`/`eyes_follow_fsm`);
  il registro (cap alzato a 24 slot) resta estendibile senza toccare la
  pubblicazione. L'echo `state/eyes` arriva dal nuovo evento `LED_FX_EVENT`
  del motore LED. Attenzione validazione: per la chiave `eyes` il parser
  valida l'effetto contro la **lista occhi** (5 effetti persistenti), non
  contro quella del corpo.

## API principale

- `ha_mqtt_init/start/stop`, `ha_mqtt_is_connected`; `start` legge
  `config_store_get_mqtt` + Kconfig CA, costruisce `<id>` dai byte `[3..5]` del MAC
  STA (coerente con l'hostname `net_mgr`).
- Getter topic (`base`/`availability`/`discovery`), `ha_mqtt_sw_version` (da
  `esp_app_desc`: copre l'OTA).
- `ha_mqtt_publish(rel_topic, payload, len, retain)` — mai bloccante: scarta se non
  connessi o payload > 512 B.
- `ha_mqtt_publish_discovery` (retained), `_availability(online)` (retained),
  `_remove` (payload vuoto retained sul topic discovery).
- CA TLS: `ha_mqtt_ca_clear/append_line/status` — PEM accumulato da console in NVS
  namespace `pumpkin_ca`, commit alla riga `-----END CERTIFICATE-----`;
  `ha_mqtt_ca_get(out, len, &written)` **condivide il certificato con l'OTA**
  (Fase 7: [pumpkin_ota](pumpkin-ota.md) usa la stessa verifica, non
  disabilitabile).
- **Listener extra di (ri)connessione**: `ha_mqtt_on_connected_add(cb, ctx)`
  (max `HA_MQTT_CONNECTED_LISTENERS_MAX` = 4 slot) — chiamati nel task worker
  subito dopo `on_connected`; sopravvivono a stop/start. Consumatore: [sys_health](sys-health.md)
  (publish retained di `status/reset` e riverifica rollback).

## Kconfig

- `PUMPKIN_MQTT_KEEPALIVE_S` (default 30): keepalive MQTT.
- `PUMPKIN_MQTT_CA`: sorgente CA (`none` | `nvs` | partizione); `mqtts` senza CA
  non parte (nessun fallback in chiaro).

## Host test

`components/ha_mqtt/host_tests` (pattern CTest del repo,
[procedura](../procedure/host-tests.md)): payload discovery/stato/evento, parser
comandi (range, tipi, malformati), rate limit distance/periodico.

## Cosa NON fare

- **Nessun lavoro nel callback esp_mqtt**: tutto passa dal worker (i comandi toccano
  FSM/config_store solo lì).
- **Retained solo sulle chiavi concordate** (`availability`, `light`, `auto`,
  `trigger_mm`, `cooldown_s`, `trigger_anim` più, da Fase 7, `update/state` e
  `status/reset`): un retained in più riappare dal broker con dati vecchi dopo
  un riavvio.
- **Non pubblicare `event/trigger` retained né con QoS > 0**: è il contratto del
  nodo audio/video ([ADR-004](../decisioni/adr-004-trigger-mqtt-contratto.md)).
- **Non costruire i topic a mano fuori dai getter**: `<id>` e base topic sono
  normalizzati in `ha_mqtt` (topic discovery e `unique_id` derivano da lì).

Tag log del componente: `ha`.
