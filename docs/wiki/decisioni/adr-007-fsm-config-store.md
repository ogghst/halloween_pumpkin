---
titolo: ADR-007 — config_store fonte unica di verità e confini della FSM
tipo: decisione
stato: assunto
aggiornato: 2026-10-26
fonti: [components/config_store/include/config_store.h, components/pumpkin_fsm/include/pumpkin_fsm.h, components/pumpkin_fsm/fsm_core.h, docs/fsm.md, host test verdi 2026-10-26 (config_store 13, pumpkin_fsm 27)]
correlate: [../componenti/config_store.md, ../componenti/pumpkin_fsm.md, ../componenti/radar_ld2420.md, ../../fsm.md, ../domande-aperte.md]
---

# ADR-007 — Config store fonte unica di verità e confini della FSM

## Contesto

La Fase 3 introduce la FSM (`pumpkin_fsm`) e il repository dei parametri utente
(`config_store`). Prima di questo punto ogni driver teneva la propria persistenza: il
radar `radar_ld2420` ha ancora NVS namespace `radar` (`trigger_mm`, soglie di
calibrazione, ...). Servivano decisioni su: chi possiede i parametri utente, come la FSM
comanda i LED senza legarsi all'hardware, come rendere testabile su host una logica
fatta di tempi, e cosa succede al boot.

## Scelta

- **`config_store` è la fonte unica di verità** dei parametri utente (namespace
  `pumpkin`). Per `trigger_mm` vale la sincronizzazione **verso** il radar: il radar
  resta con la sua copia NVS interna (`radar`) e la sua logica di trigger continua a
  leggere il valore locale; `config_store` lo risincronizza quando cambia.
- **La FSM parla con `led_effects` solo tramite l'interfaccia sottile `fsm_output_t`**
  (look = effetto + colore + luminosità, rail, timer, persistenze): il core
  ([fsm_core.c](../../../components/pumpkin_fsm/fsm_core.c)) non include né `led_effects`
  né `config_store`; il layer IDF esegue gli output in ordine.
- **Tempo astratto con clock iniettato** (`fsm_clock_t.now_us`): fake clock sui test
  host, `esp_timer_get_time` sul target; le scadenze sono µs assoluti e le transizioni
  di timeout passano da `fsm_core_poll`, deterministica sui test.
- **Politica di boot dal `power_on` persistito**: true → `IDLE` (rail on + look idle),
  false → `OFF` (rail spento). Nessun ripristino dello stato precedente al power-off.
  I comandi `set_mode OFF/MANUAL` aggiornano `power_on` in `config_store`.
- **Semantica dello scare e del cooldown**: `scare` da `MANUAL` torna al look manuale
  alla scadenza, da `IDLE`/`COOLDOWN` torna a `IDLE`; in `TRIGGERED` riarma solo la
  durata. Il cooldown mostra il look idle e **ignora `PRESENCE_ON`** finché non scade
  (`cooldown_s` = 0 lo disattiva).
- **Persistenza dei comandi di look solo in AUTO**: `set_light`/`set_effect` in
  `IDLE`/`TRIGGERED`/`COOLDOWN` aggiornano il look corrente **e** lo scrivono in
  `config_store` (la scelta sopravvive al reboot); in `MANUAL` sono transitori (il
  look manuale non tocca la configurazione); in `OFF` sono rifiutati.

## Alternative scartate

- **Ogni componente owner della sua NVS** (radar incluso per `trigger_mm`): due fonti
  di verità con semantica ambigua di "chi vince al boot", e la Fase 5 (HA/MQTT)
  dovrebbe scrivere in più posti per un solo parametro. Il radar mantiene però la
  copia locale per le sue soglie di calibrazione, che non sono parametri utente.
- **Core FSM che chiama `led_fx_*` e `config_store_*` direttamente**: logica di
  decisione non più testabile su host e dipendenze hardware nel punto più delicato del
  firmware; anche l'ordine di applicazione (effetto, colore, luminosità, rail) sarebbe
  sfuggito al controllo del layer.
- **Tempo reale nel core** (`esp_timer_get_time` chiamato dentro `fsm_core.c`): i 27
  test host sulle scadenze non sarebbero deterministici né veloci.
- **Boot sempre in `IDLE` o ripristino dello stato precedente**: riaccendere la zucca
  dopo uno spegnimento volontario (o durante il trasporto) è l'effetto contrario alla
  politica di `power_on`.
- **Cooldown che accetta re-trigger immediati / scare che riparte sempre da `IDLE`**:
  sorpasserebbe la pausa antirimbalzo del trigger e perderebbe il contesto manuale
  dell'utente.

## Conseguenze

- La Fase 5 (`ha_mqtt`) scrive **solo** su `config_store`: un parametro utente = una
  chiave, un evento, un consumatore (la FSM) che rinfresca il look.
- Il **sync di `trigger_mm` è già cablato nell'app di test** `test_apps/pumpkin_fsm`
  (su `CONFIG_EVENT_CHANGED` di `CFG_PARAM_TRIGGER_MM` chiama `radar_set_trigger_mm`);
  la **doppia persistenza** (`radar` vs `pumpkin`) e la precedenza al boot restano da
  esercitare a banco: [domande aperte](../domande-aperte.md).
- Tempi (`trigger_duration_s`, `cooldown_s`), persistenza dopo reboot reale e debounce
  NVS su flash sono da confermare a banco (host test verdi, hardware non ancora in
  ciclo con la FSM).
- Cambiare la semantica di transizioni/tempi richiede estendere prima i test host del
  core (regola in [componente](../componenti/pumpkin_fsm.md)).
