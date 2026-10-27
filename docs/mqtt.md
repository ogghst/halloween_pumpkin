# MQTT e Home Assistant discovery (`ha_mqtt`)

Il ponte MQTT della zucca: componenti `ha_mqtt` (trasporto + discovery) e `ha_entities`
(glue che riempie il registro delle entità), consegnati con la Fase 5. Le decisioni di
progetto sono in [ADR-004](wiki/decisioni/adr-004-trigger-mqtt-contratto.md) (contratto
del trigger verso il nodo audio/video) e
[ADR-005](wiki/decisioni/adr-005-discovery-device-based.md) (discovery device-based);
la pagina wiki del componente è [ha_mqtt](wiki/componenti/ha-mqtt.md). Per la semantica
della FSM richiamata qua: [fsm.md](fsm.md); per il broker di prova:
[Server MQTT locale (Mosquitto)](wiki/procedure/mosquitto.md).

## Panoramica e dipendenze

- Trasporto **esp_mqtt** (componente IDF `mqtt`): QoS 0, **keepalive 30 s**
  (Kconfig `PUMPKIN_MQTT_KEEPALIVE_S`), riconnessione automatica 10 s. Dipende da
  `net_mgr` (client parte su `NET_EVENT_UP`, si ferma su `NET_EVENT_DOWN`) e da
  `config_store` v2 per host/porta/utente/password/flag TLS, scritti dal
  provisioning (`prov mqtt ...`). Nessuna credenziale nel repo.
- Nessun lavoro pesante nel callback esp_mqtt: le callback `on_connected` e
  `on_command` girano in un **task worker** di `ha_mqtt`; i comandi toccano
  FSM/config_store solo lì.
- Le entità sono alimentate dagli eventi esistenti: radar (`PRESENCE_EVENT`),
  FSM (`FSM_EVENT_STATE_CHANGED`), config (`CONFIG_EVENT_CHANGED`) e diagnostica
  di rete per l'rssi. Il registro (`ha_registry`) è estendibile: le fasi 6–8
  aggiungono entità (batteria, tof) senza toccare la pubblicazione.
- `<id>` = **6 cifre esadecimali minuscole** dai byte `[3..5]` del MAC STA (per la
  board di banco `e4:65:b8:71:7e:50` → `717e50`): stesso formato del suffisso
  hostname `net_mgr` (`pumpkin-<id>`). `<id>` entra nel base topic, nel topic di
  discovery, negli `identifiers` (`pumpkin_<id>`) e in ogni `unique_id`
  (`pumpkin_<id>_<key>`, stabili tra versioni).

## Albero topic

| Topic | Direzione | Retained | Contenuto |
| --- | --- | --- | --- |
| `pumpkin/<id>/availability` | zucca → broker | sì | `online` a ogni connessione; `offline` via LWT |
| `pumpkin/<id>/state/<key>` | zucca → broker | solo alcune (sotto) | stato dell'entità `<key>` |
| `pumpkin/<id>/set/<key>` | HA → zucca | no | comando per l'entità `<key>` |
| `pumpkin/<id>/update/state` | zucca → broker | sì | versione firmware installata (stringa) |
| `pumpkin/<id>/update/latest` | HA → broker | — | ultima versione nota (riservato: nessun backend) |
| `pumpkin/<id>/update/status` | zucca → broker | no | eventi OTA: `started`/`progress`/`success`/`failed`/`rejected` |
| `pumpkin/<id>/event/trigger` | zucca → broker | no | evento di spavento (contratto ADR-004) |
| `homeassistant/device/<id>/config` | zucca → broker | sì | discovery device-based; payload vuoto = rimozione |
| `homeassistant/status` | HA → broker | (retained di HA) | birth `online`: la zucca ripubblica discovery+stato |

## Discovery

Un **unico** messaggio retained su `homeassistant/device/<id>/config` registra il
device con tutte le entità (formato device-based con chiave `components`,
Home Assistant ≥ 2024.11). Payload d'esempio per `<id>` = `a1b2c3`:

```json
{
  "device": {
    "identifiers": [
      "pumpkin_a1b2c3"
    ],
    "name": "Zucca pumpkin-a1b2c3",
    "manufacturer": "halloween_pumpkin",
    "model": "ESP32 D1 mini + WS2812B 8x8 + LD2420",
    "sw_version": "0.5.0"
  },
  "origin": {
    "name": "halloween_pumpkin firmware",
    "sw": "0.5.0"
  },
  "availability": [
    {
      "topic": "pumpkin/a1b2c3/availability"
    }
  ],
  "components": {
    "light": {
      "unique_id": "pumpkin_a1b2c3_light",
      "state_topic": "pumpkin/a1b2c3/state/light",
      "command_topic": "pumpkin/a1b2c3/set/light",
      "platform": "light",
      "schema": "json",
      "brightness": true,
      "rgb": true,
      "effect": true,
      "effect_list": [
        "solid",
        "candle",
        "breathe",
        "rainbow",
        "flicker",
        "scare"
      ]
    },
    "presence": {
      "unique_id": "pumpkin_a1b2c3_presence",
      "state_topic": "pumpkin/a1b2c3/state/presence",
      "platform": "binary_sensor",
      "device_class": "occupancy"
    },
    "distance": {
      "unique_id": "pumpkin_a1b2c3_distance",
      "state_topic": "pumpkin/a1b2c3/state/distance",
      "platform": "sensor",
      "device_class": "distance",
      "unit_of_measurement": "mm",
      "state_class": "measurement"
    },
    "battery_v": {
      "unique_id": "pumpkin_a1b2c3_battery_v",
      "state_topic": "pumpkin/a1b2c3/state/battery_v",
      "platform": "sensor",
      "device_class": "voltage",
      "unit_of_measurement": "V",
      "state_class": "measurement",
      "entity_category": "diagnostic"
    },
    "battery_pct": {
      "unique_id": "pumpkin_a1b2c3_battery_pct",
      "state_topic": "pumpkin/a1b2c3/state/battery_pct",
      "platform": "sensor",
      "device_class": "battery",
      "unit_of_measurement": "%",
      "state_class": "measurement",
      "entity_category": "diagnostic"
    },
    "rssi": {
      "unique_id": "pumpkin_a1b2c3_rssi",
      "state_topic": "pumpkin/a1b2c3/state/rssi",
      "platform": "sensor",
      "device_class": "signal_strength",
      "unit_of_measurement": "dBm",
      "state_class": "measurement",
      "entity_category": "diagnostic"
    },
    "auto": {
      "unique_id": "pumpkin_a1b2c3_auto",
      "state_topic": "pumpkin/a1b2c3/state/auto",
      "command_topic": "pumpkin/a1b2c3/set/auto",
      "platform": "switch"
    },
    "trigger_mm": {
      "unique_id": "pumpkin_a1b2c3_trigger_mm",
      "state_topic": "pumpkin/a1b2c3/state/trigger_mm",
      "command_topic": "pumpkin/a1b2c3/set/trigger_mm",
      "platform": "number",
      "min": 100,
      "max": 10000,
      "step": 50,
      "mode": "box",
      "unit_of_measurement": "mm",
      "entity_category": "config"
    },
    "cooldown_s": {
      "unique_id": "pumpkin_a1b2c3_cooldown_s",
      "state_topic": "pumpkin/a1b2c3/state/cooldown_s",
      "command_topic": "pumpkin/a1b2c3/set/cooldown_s",
      "platform": "number",
      "min": 0,
      "max": 3600,
      "step": 1,
      "mode": "box",
      "unit_of_measurement": "s",
      "entity_category": "config"
    },
    "trigger_anim": {
      "unique_id": "pumpkin_a1b2c3_trigger_anim",
      "state_topic": "pumpkin/a1b2c3/state/trigger_anim",
      "command_topic": "pumpkin/a1b2c3/set/trigger_anim",
      "platform": "select",
      "options": [
        "solid",
        "candle",
        "breathe",
        "rainbow",
        "flicker",
        "scare"
      ],
      "entity_category": "config"
    },
    "scare": {
      "unique_id": "pumpkin_a1b2c3_scare",
      "command_topic": "pumpkin/a1b2c3/set/scare",
      "platform": "button",
      "payload_press": "PRESS"
    },
    "update": {
      "unique_id": "pumpkin_a1b2c3_update",
      "state_topic": "pumpkin/a1b2c3/update/state",
      "command_topic": "pumpkin/a1b2c3/set/update",
      "latest_version_topic": "pumpkin/a1b2c3/update/latest",
      "platform": "update",
      "title": "Firmware zucca",
      "entity_category": "config"
    },
    "eyes": {
      "unique_id": "pumpkin_a1b2c3_eyes",
      "state_topic": "pumpkin/a1b2c3/state/eyes",
      "command_topic": "pumpkin/a1b2c3/set/eyes",
      "platform": "light",
      "schema": "json",
      "brightness": true,
      "rgb": true,
      "effect": true,
      "effect_list": [
        "off",
        "solid",
        "breathe",
        "color_cycle",
        "blink",
        "glow"
      ]
    },
    "eyes_idle_color": {
      "unique_id": "pumpkin_a1b2c3_eyes_idle_color",
      "state_topic": "pumpkin/a1b2c3/state/eyes_idle_color",
      "command_topic": "pumpkin/a1b2c3/set/eyes_idle_color",
      "platform": "light",
      "schema": "json",
      "rgb": true
    },
    "eyes_scare_color": {
      "unique_id": "pumpkin_a1b2c3_eyes_scare_color",
      "state_topic": "pumpkin/a1b2c3/state/eyes_scare_color",
      "command_topic": "pumpkin/a1b2c3/set/eyes_scare_color",
      "platform": "light",
      "schema": "json",
      "rgb": true
    },
    "eyes_enabled": {
      "unique_id": "pumpkin_a1b2c3_eyes_enabled",
      "state_topic": "pumpkin/a1b2c3/state/eyes_enabled",
      "command_topic": "pumpkin/a1b2c3/set/eyes_enabled",
      "platform": "switch"
    },
    "eyes_follow_fsm": {
      "unique_id": "pumpkin_a1b2c3_eyes_follow_fsm",
      "state_topic": "pumpkin/a1b2c3/state/eyes_follow_fsm",
      "command_topic": "pumpkin/a1b2c3/set/eyes_follow_fsm",
      "platform": "switch"
    }
  }
}
```

Ciclo di vita:

- La discovery è **ripubblicata a ogni connessione MQTT** (retained: il broker la
  ridà a chi si iscrive, anche dopo un riavvio del broker). `sw_version` viene da
  `esp_app_get_description()->version`, quindi un **OTA** cambia il campo senza
  tocchi manuali.
- Al **birth** di HA (`online` retained su `homeassistant/status`, a cui la zucca è
  iscritta) discovery e stato completo vengono ripubblicati dopo un **ritardo casuale
  di 1000–5000 ms**, per non far parlare tutti i device nello stesso istante al riavvio di HA.
- **Rimozione** del device da HA: payload **vuoto** retained sul medesimo topic di
  discovery (`ha remove` da console). Non cancella la configurazione né la
  persistenza: il device può tornare con `ha announce`.

`battery_v`/`battery_pct` sono dichiarate ma **senza sorgente** finché manca
`power_mgr`: in HA restano a `unknown`.

## Stati e comandi per entità

Le 17 entità con chiavi stabili (12 di Fase 5–7 + 5 occhi di Fase 8). Scala della
luce: percentuali interne → 0–255 (es. `idle_brightness_pct` 10 ≈ brightness 26).

| key | Entità HA | `state/<key>` (esempio) | Retained | `set/<key>` (payload) |
| --- | --- | --- | --- | --- |
| `light` | light (schema JSON) | `{"state":"ON","brightness":26,"color":{"r":255,"g":120,"b":0},"effect":"candle"}` | sì | stesso JSON; `brightness`/`color`/`effect` opzionali |
| `presence` | binary_sensor `occupancy` | `ON` / `OFF` | no | — |
| `distance` | sensor `distance` | `1234` (mm) | no | — |
| `battery_v` | sensor `voltage` (diagnostic) | `3.87` (V) | no | — |
| `battery_pct` | sensor `battery` (diagnostic) | `76` (%) | no | — |
| `rssi` | sensor `signal_strength` (diagnostic) | `-58` (dBm) | no | — |
| `auto` | switch | `ON` / `OFF` (= FSM AUTO/MANUAL) | sì | `ON` → AUTO, `OFF` → MANUAL |
| `trigger_mm` | number `config` | `1200` (mm) | sì | `1200` (100–10000, step 50) |
| `cooldown_s` | number `config` | `30` (s) | sì | `30` (0–3600, step 1) |
| `trigger_anim` | select `config` | `scare` | sì | uno dei 6 effetti |
| `scare` | button | — | — | `PRESS` |
| `update` | update `config` | `update/state`: versione installata | sì | JSON `{"url":"https://..","version":"..","release_summary":".."}` (`url` obbligatorio) |
| `eyes` | light (schema JSON) | `{"state":"ON","brightness":200,"color":{"r":255,"g":40,"b":0},"effect":"blink"}` | sì | stesso JSON; `brightness`/`color`/`effect` opzionali, `effect` tra i 5 persistenti |
| `eyes_idle_color` | light (schema JSON, solo rgb) | `{"state":"ON","color":{"r":255,"g":120,"b":0}}` | sì | `{"state":"ON"/"OFF"}` e/o `{"color":{"r":..,"g":..,"b":..}}` (tutti opzionali) |
| `eyes_scare_color` | light (schema JSON, solo rgb) | `{"state":"ON","color":{"r":255,"g":0,"b":0}}` | sì | `{"state":"ON"/"OFF"}` e/o `{"color":{"r":..,"g":..,"b":..}}` (tutti opzionali) |
| `eyes_enabled` | switch | `ON` / `OFF` | sì | `ON` / `OFF` |
| `eyes_follow_fsm` | switch | `ON` / `OFF` | sì | `ON` / `OFF` |

### Semantica della luce

- **`light` OFF = FSM OFF**: rail LED spento. **ON da OFF → MANUAL** (rail riacceso).
- `brightness`/`color`/`effect` passano da `fsm_cmd_set_light`/`fsm_cmd_set_effect`:
  in **MANUAL** sono transienti, in **AUTO** persistono in `config_store`
  (dettagli in [fsm.md](fsm.md)).
- Lo stato pubblicato riflette la FSM: `on` = stato ≠ OFF; `brightness` =
  `idle_brightness_pct` in AUTO, altrimenti il look manuale corrente; `effect` =
  `idle_anim` in AUTO, altrimenti l'effetto manuale corrente.

### Semantica degli occhi

- **`eyes` comanda direttamente il motore occhi** (pixel 64–65, stessa fila dati
  dei 64 del corpo): è **indipendente dalla FSM** — non cambia il modo della zucca
  e non viene toccato da `light`. `effect` accetta solo i **6 effetti
  persistenti** (`off`, `solid`, `breathe`, `color_cycle`, `blink`, `glow`): gli oneshot
  (`flash`, `glare`) restano interni (scare/FSM) e sono scartati dal parse.
- Lo stato `state/eyes` è retained e ripubblicato a **ogni cambio di look**
  (evento del motore occhi): `state` = `ON` se un oneshot è attivo oppure
  `brightness > 0` con `effect` ≠ `off`; `brightness`/`color`/`effect` sempre presenti.
- I due light **`eyes_idle_color`/`eyes_scare_color` non hanno brightness né
  effect**: portano solo `color` (r/g/b di riposo/spavento) e il loro campo
  `state` è **`eyes_enabled`** (`{"state":"OFF"}` sugli occhi colore = occhi
  disabilitati). Lo stesso ON/OFF è esposto anche come switch `eyes_enabled`.
- **`eyes_follow_fsm`** (switch): con `ON` il look degli occhi segue
  riposo/spavento della FSM (usi i due colori e `eyes_idle_effect`); con `OFF`
  risponde solo ai comandi diretti su `eyes`.

## Contratto `event/trigger` (nodo audio/video)

Alla transizione **→ `TRIGGERED`** della FSM la zucca pubblica su
`pumpkin/<id>/event/trigger` (QoS 0, non retained):

```json
{"v":1,"ts":123456,"distance_mm":980,"anim":"scare"}
```

Campi: `v` versione del contratto (1); `ts` millisecondi di **uptime**
(`esp_timer`, non orologio da parete); `distance_mm` ultima distanza radar nota
al trigger; `anim` effetto avviato (`trigger_anim`).

Il nodo audio/video consuma l'evento e avvia la propria traccia: **nessun
handshake**. Con QoS 0 la consegna non è garantita: il consumatore deve tollerare
una perdita (il ciclo riparte al prossimo trigger). Contratto completo:
[ADR-004](wiki/decisioni/adr-004-trigger-mqtt-contratto.md).

## Politica retained e rate limit

Retained **solo**: `availability`, `light`, `auto`, `trigger_mm`, `cooldown_s`,
`trigger_anim`, `update/state`, `eyes`, `eyes_idle_color`, `eyes_scare_color`,
`eyes_enabled`, `eyes_follow_fsm` — al riavvio del broker il device riappare con
l'ultimo look, i parametri e la versione installata. Non retained: `presence`,
`distance`, `battery_v`, `battery_pct`, `rssi`, `update/status`,
`event/trigger` (valori effimeri, un retained qui conterebbe dati vecchi).

- `distance` è pubblicata se **|Δ| > 50 mm** **e** sono trascorsi **≥ 1000 ms** dalla
  precedente; `rssi` (e in futuro la batteria) va in onda **ogni 60 s**.
- `presence`, `light`, `auto` e i parametri escono sul cambio (eventi radar/FSM/config);
  a ogni connessione parte lo **stato completo** insieme alla discovery.

## Validazione dei comandi

Ogni payload su `set/<key>` passa da `ha_cmd_parse` prima di toccare FSM o
`config_store`:

- payload oltre **512 B** (`HA_CMD_PAYLOAD_MAX`), JSON malformato, tipo errato o
  valore fuori range → comando **ignorato** con log (nessuna azione sul device);
- range specchiati da `config_store`: `trigger_mm` 100–10000, `cooldown_s` 0–3600;
  `trigger_anim` solo nomi del registry `led_effects`;
- **occhi**: `eyes` è JSON come `light` (max **512 B**) con `brightness` **0–255**
  e `effect` validato contro i **5 effetti persistenti** (oneshot scartati);
  `eyes_idle_color`/`eyes_scare_color` accettano `{"state":"ON"/"OFF"}` e/o
  `{"color":{"r","g","b"}}` con canali **0–255** (campi tutti opzionali);
  `eyes_enabled`/`eyes_follow_fsm` solo `ON`/`OFF`;
- applicazione nel **task worker** di `ha_mqtt`, mai nel callback MQTT.

## Aggiornamento firmware (`update`, Fase 7)

Comando su `pumpkin/<id>/set/update`:

```json
{"url":"https://example.com/pumpkin.bin","version":"1.1.0","release_summary":"..."}
```

Solo `url` è obbligatorio ed è **HTTPS obbligatorio** (CA condivisa con MQTT:
stesso namespace `pumpkin_ca`). Gate nel componente `pumpkin_ota`, prima di
toccare la flash — `version` e `release_summary` sono metadati informativi:

| Gate | Rifiuto | Soglia |
| --- | --- | --- |
| URL | `bad_url` | assente/vuota, non `https://`, oltre 512 B |
| Batteria | `battery_low` | sotto il 40% se nota; `-1` sconosciuta → **consentita** (evento `started` con `"battery":"unknown"`) |
| LED | `led_full_light` | stima corrente oltre `PUMPKIN_OTA_MAX_LED_CURRENT_MA` (default 500 mA) |
| Heap | `heap_low` | libero o largest block sotto `PUMPKIN_OTA_MIN_FREE_HEAP` (default 64 KB) |
| Occupato | `busy` | un aggiornamento alla volta |
| Versione | `same_version` | immagine scaricata identica alla versione in esecuzione (confronto su `esp_app_desc`, `v` iniziale ignorata) |

Eventi su `pumpkin/<id>/update/status` (non retained, JSON < 512 B):
`{"event":"started|progress|success|failed|rejected","detail":"..","installed":"..","free_heap":N,"max_block":N,"progress":0-100,"battery":"unknown"?}`
(`detail`/`installed` presenti solo se significativi; `progress` ogni 10%).

Al successo la zucca pubblica `success`, attende 3 s per il flush MQTT e si
riavvia: l'immagine resta **PENDING_VERIFY** (rollback automatico abilitato)
fino alla conferma post-reboot di `sys_health` — il firmware NON chiama
`esp_ota_mark_app_valid` al termine del download.

## TLS e CA

- Il flag `tls` di `config_store` (provisioning: `prov mqtt ... tls=1`) seleziona schema
  `mqtts://` al posto di `mqtt://`; CA con Kconfig `PUMPKIN_MQTT_CA`:
  `none` | `nvs` | partizione. In modalità `nvs` il PEM si accumula da console
  (namespace NVS **`pumpkin_ca`**, chiave `ca`) con commit automatico alla riga
  `-----END CERTIFICATE-----`.
- **`mqtts` senza CA non parte**: fallimento esplicito all'avvio, mai fallback in chiaro.

## App di banco

`test_apps/ha_mqtt` (Fase 5), `PROJECT_VER 0.5.0` → `sw_version` del device; avvio MQTT
su `NET_EVENT_UP`, stop su `NET_EVENT_DOWN`. Console: tutti i comandi
`net`/`prov`/`cfg`/`fsm` di `net_prov` più:

| Comando | Effetto |
| --- | --- |
| `ha status` | connessione, `<id>`, topic base, CA |
| `ha announce` | ripubblica discovery + stato completo |
| `ha remove` | payload vuoto retained sul topic discovery (device rimosso da HA) |
| `ha ca clear` / `ha ca begin` / `ha ca +<riga>` / `ha ca status` | gestione CA in NVS |
| `ha pub <topic> <payload>` | publish di prova sul base topic |

## Checklist di banco

1. Installare i client: `sudo apt install mosquitto-clients` (sudo dell'utente).
2. Aprire il listener per la LAN (`listener 1883 0.0.0.0`, `allow_anonymous false`,
   `password_file`) e riavviare Mosquitto — procedura in
   [mosquitto.md](wiki/procedure/mosquitto.md); le credenziali non entrano mai in chat/repo.
3. Provisioning MQTT della board: `prov mqtt host=<IP del PC> port=1883 user=...
   pass=... tls=0` ([procedura](wiki/procedure/provisioning.md)).
4. Ascoltare: `mosquitto_sub -v -h <broker> -t 'pumpkin/#' -t 'homeassistant/#'` → attesi
   `availability` `online`, discovery retained e gli stati iniziali.
5. Home Assistant → device **Zucca pumpkin-`<id>`**: 17 entità visibili; salvare
   uno screenshot della scheda.
6. Riavviare HA: birth `online` → ripubblicazione dopo 1–5 s (visibile nel log e
   nel sub).
7. Staccare l'alimentazione della board: `availability` → `offline` (LWT) entro
   keepalive 30 s + margine.
8. Riavviare broker o router: riconnessione (~10 s) seguita da discovery + stato.
9. `idf.py size` dell'app di banco: margine flash/IRAM dopo l'aggiunta di
   esp_mqtt+cJSON (vedi [insidia IRAM](wiki/insidie/iram-nimble-wifi.md)).
10. `ha remove` → il device sparisce da HA; `ha announce` → torna.

## Note versione

Formato discovery (device-based con chiave `components`, disponibilità device-level)
verificato contro la **documentazione ufficiale MQTT di Home Assistant** in data
2026-10-26: richiede **HA ≥ 2024.11**. La versione installata dall'utente non è ancora
nota: se più vecchia, la discovery non appare e il payload va convertito per-entità
(vedi [domande aperte](wiki/domande-aperte.md)).
