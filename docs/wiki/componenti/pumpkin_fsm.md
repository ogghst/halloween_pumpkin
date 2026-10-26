---
titolo: Componente pumpkin_fsm
tipo: componente
stato: verificato
aggiornato: 2026-10-26
fonti: [components/pumpkin_fsm/include/pumpkin_fsm.h, components/pumpkin_fsm/fsm_core.h, components/pumpkin_fsm/fsm_core.c, components/pumpkin_fsm/host_tests (27 casi, verdi 2026-10-26)]
correlate: [config_store.md, ../decisioni/adr-007-fsm-config-store.md, ../../fsm.md, ../procedure/host-tests.md, ../domande-aperte.md]
---

# Componente `pumpkin_fsm`

Macchina a stati che lega il radar di presenza agli effetti LED
(`components/pumpkin_fsm/`), consegnata con la Fase 3. Diagramma, tabella delle
transizioni e verifica a banco: [docs/fsm.md](../../fsm.md).

**Stato**: core di decisione **verificato su host** (27 test verdi: boot, presence,
poll/scadenze, cooldown, modalità, scare, persist dei look); tempi reali su board,
persistenza dopo reboot e cablaggio radar→FSM→LED **da confermare a banco**
([domande aperte](../domande-aperte.md)).

## Perché queste scelte

- **Un solo proprietario dello stato**: radar, comandi utente, eventi di configurazione
  e timer convergono in **una coda** servita da un task proprietario; niente sezioni
  critiche sparse né race sullo stato. `pumpkin_fsm_get_state` legge una copia
  pubblicata, solo per osservabilità.
- **Core puro con orologio iniettato** (decisione in
  [ADR-007](../decisioni/adr-007-fsm-config-store.md)): `fsm_core.c` non include
  FreeRTOS, `esp_timer`, `led_effects` né `config_store`; il tempo arriva da
  `fsm_clock_t.now_us` (fake clock su host, `esp_timer_get_time` sul target) e le
  decisioni su scadenze sono testabili in modo deterministico.
- **Output sottile** (`fsm_output_t`): il core *descrive* gli effetti collaterali (look
  = effetto + colore + luminosità, rail, riarmo/cancel del timer, parametri da
  persistere) e il layer IDF li esegue in ordine. Il core resta agnostico rispetto a
  `led_effects` e NVS.

## Come funziona

- Stati `IDLE / TRIGGERED / COOLDOWN / MANUAL / OFF`; al più una scadenza armata alla
  volta (durata trigger o cooldown), ricalcolata a ogni input; sul target un
  **`esp_timer` one-shot** manda un wake-up `FSM_IN_POLL` nella coda e il core esegue
  la transizione confrontando l'orologio iniettato.
- **Watchdog** (Fase 7): il task `fsm` (prio 4, sotto radar/anim/ha_mqtt a 5) è
  iscritto al task watchdog e si alimenta a ogni iterazione della coda, così un
  deadlock degli eventi non lascia la zucca in uno stato congelato.
- Input in coda: `PRESENCE_ON/OFF` (base `PRESENCE_EVENT` del radar),
  `FSM_IN_POLL` (timer), comandi `fsm_cmd_set_mode/set_light/set_effect/scare`,
  `FSM_IN_CONFIG_CHANGED` (config tradotta in `fsm_config_t` a boot e a ogni
  `CONFIG_EVENT_CHANGED`).
- Output pubblicato: `FSM_EVENT` / `FSM_EVENT_STATE_CHANGED` con payload
  `fsm_state_change_t{state, prev_state}`.
- Semantica completa dei comandi e della persistenza (AUTO persiste, MANUAL transiente,
  OFF rifiuta): tabella in [docs/fsm.md](../../fsm.md).

## Cosa NON fare

- **Non modellare il look "di stato" chiamando `led_fx_*` da altri task**: la FSM è
  l'unica proprietaria della presentazione automatica; chi altro scrive sui LED la
  gareggia contro il look corrente.
- **Non contare su `PRESENCE_OFF` per accorciare il trigger**: la durata è fissa
  (`trigger_duration_s`); la scomparsa del bersaglio non interrompe lo scare.
- **Non usare `pumpkin_fsm_get_state` per sincronizzare** operazioni esterne: è una
  copia per log/osservabilità, la decisione spetta alla FSM (i comandi sono asincroni
  per costruzione).
- **Non aggiungere logica al core senza test host**: la regola del repo vale al
  doppio qui — estendere [test_fsm_core.c](../../../components/pumpkin_fsm/host_tests/test_fsm_core.c)
  prima di toccare transizioni o tempi.
- **Non chiamare le `fsm_cmd_*` da ISR**: accodano alla coda del task proprietario, non
  sono pensate per il contesto interrupt.
