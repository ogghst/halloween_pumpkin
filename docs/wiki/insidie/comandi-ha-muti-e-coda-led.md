---
titolo: Comandi HA muti, nomi effetto corrotti e coda LED in overflow
tipo: insidia
stato: verificato
aggiornato: 2026-10-27
fonti: [banco 2026-10-27 (ESP32 D1 mini, app `pumpkin` v1.0.0-1-g3af9873-dirty, console `/dev/ttyUSB0` + broker MQTT 192.168.1.10 con paho): comandi `set/light` riprodotti e verificati riga per riga; components/ha_mqtt/ha_commands.c, components/ha_entities/ha_entities.c, components/led_effects/led_fx.c]
correlate: [../componenti/ha-mqtt.md, ../componenti/led_effects.md, ../componenti/pumpkin_fsm.md, ../decisioni/adr-007-fsm-config-store.md]
---

# Tre difetti a catena: comandi HA muti, nomi corrotti, coda LED in overflow

## 1. Comandi muti: `ha_cmd_parse` non impostava mai `out->target`

### Il sintomo

Da Home Assistant distanza e presenza si aggiornano, ma ogni comando
(`light`, `auto`, `trigger_mm`, `cooldown_s`, `trigger_anim`, `scare`) non
produce **né effetto né log**: nessuna riga `W` di `ha_ent`, nessun echo su
`state/<key>`.

### La causa

`ha_cmd_parse` faceva `memset(out, 0, sizeof *out)` e valorizzava i soli
campi del payload: `out->target` restava `HA_TARGET_NONE` (0). Il dispatch di
`command_cb` (`switch (cmd.target)`) cadeva nel `default: break;` silenzioso.
Solo `update` funzionava perché dispatchato per chiave prima del parse; la
distanza usciva dal feed radar che non passa dal parser. Gli host test non lo
catturavano perché asserivano i singoli campi ma mai `cmd.target`.

### La regola nel codice

`target_for_key()` in `ha_commands.c` mappa la chiave al target in un punto
unico, chiamato in `ha_cmd_parse` prima dei rami di parse (contratto:
`target` è significativo solo con risultato `HA_CMD_OK`; il chiamante deve
controllare il risultato). Test di regressione: `test_target_dispatch` in
`host_tests/test_commands.c` asserisce il target per ogni chiave.

## 2. Nomi effetto corrotti nell'echo: copia senza NUL su buffer riusato

### Il sintomo

`state/light` pubblicava `effect` con code del nome precedente:
`rainbow` → scrivi `candle` → echo `candlew`; poi `solidew`, `scareow`.

### La causa

`cache_anim_update` in `ha_entities.c` copiava `strnlen(anim, MAX-1)` byte
**senza il terminatore** e NULava solo l'ultima cella del buffer: su buffer
già usato restavano i byte coda del nome precedente. (`copy_anim` del parser
era invece corretto.)

### La regola nel codice

Il NUL va scritto subito dopo la copia, a `s_anim_effect[n]`, dentro la
sezione critica. I test host non lo coprono (buffer azzerato da `setUp`:
la prima scrittura passa comunque) — copertura al banco via echo MQTT.

## 3. Coda LED in overflow: `xQueueSend` a timeout 0 contro raffiche di re-emissioni

### Il sintomo

Anche con i comandi dispatchati, ogni `set/light` produceva
`W fsm: led_fx_set_brightness/effect failed (ESP_ERR_TIMEOUT)` e i LED non
cambiavano intensità/effetto (drop sistematico, riprodotto al banco 4/4).

### La causa

Un comando `light` genera 2-3 scritture in `config_store` (brightness, anim,
colore); `CONFIG_EVENT_CHANGED` parte **subito** (nessun debounce
sull'evento) e il FSM ri-emette il look per ogni evento: fino a ~12 send su
una coda `led_fx` profonda 8 con `xQueueSend(..., 0)` non bloccante. Il task
`anim` svuota fino a 8 comandi per frame (16,7 ms): la coda trabocca ad
ogni raffica e i send di coda cadono. In più il numero di re-emissioni
cresce nello stato `TRIGGERED` (le scritte vanno su `trigger_*`).

### La regola nel codice

`fx_send_cmd` in `led_fx.c` ora blocca fino a ~2 frame (il task `anim`
liquefa la raffica in 2 frame) e `FX_QUEUE_DEPTH` è 12. Contesto chiamante:
solo task, mai ISR/timer. Nota di design: le ri-emissioni per evento config
sono ridondanti ma innocue; se in futuro si vuole eliminarle, la
soppressione dell'eco è nel FSM (ADR-007), non nel LED.

## 4. Variante Fase 8 (2026-10-27): effect validato contro la lista sbagliata

### Il sintomo

`set/eyes` con un effect del registro occhi (`color_cycle`, `blink`) scartato
in silenzio (nessun echo `state/eyes`, look invariato); lo stesso payload con
un effect del corpo (`solid`) passava.

### La causa

`ha_cmd_parse` riceve **una** lista opzioni dal chiamante e `parse_light`
valida `effect` contro di essa; `command_cb` passava sempre la lista del
corpo. Fix: selezione della lista per chiave (`eyes` → lista occhi, tutto il
resto → lista corpo) nel punto unico del dispatch, prima del parse.

### La regola nel codice

Una chiave con registro proprio deve ricevere **la sua** lista opzioni in
`command_cb`. Copertura host: `test_eyes_light` (lista occhi); il wiring per
chiave resta coperto dalla verifica a banco via echo MQTT.
