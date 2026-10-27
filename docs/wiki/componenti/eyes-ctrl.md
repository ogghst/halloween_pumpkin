---
titolo: Componente eyes_ctrl
tipo: componente
stato: assunto
aggiornato: 2026-10-27
fonti: [fase8, components/eyes_ctrl/eyes_ctrl_core.c, components/eyes_ctrl/eyes_ctrl.c, components/eyes_ctrl/include/eyes_ctrl.h, banco 2026-10-27 ( scare MQTT → glare verificato, gate follow verificato)]
correlate: [../componenti/led_effects.md, ../componenti/pumpkin_fsm.md, ../componenti/config_store.md, ../decisioni/adr-010-occhi-zone.md, ../../../docs/fsm.md]
---

# Componente `eyes_ctrl`

Ponte tra gli eventi di sistema e la zona occhi del motore LED (Fase 8). Piccolo
di proposito: la FSM **non sa nulla degli occhi** (simmetrico di
[ADR-007](../decisioni/adr-007-fsm-config-store.md)); il componente traduce
`FSM_STATE_CHANGED` in un piano per il motore occhi.

## Struttura

- `eyes_ctrl_core.c` — **puro e host-testato** (8 test): `eyes_plan_build(state,
  cfg, out)` costruisce il piano `led_fx_eyes_plan_t` a partire dallo stato FSM
  e dalla configurazione; `eyes_follow_allowed(cfg)` è la truth table del gate.
- `eyes_ctrl.c` — layer IDF: si registra sul loop di default a
  `FSM_EVENT_STATE_CHANGED` e `CONFIG_EVENT_CHANGED` (solo i 7 parametri
  `CFG_PARAM_EYES_*`) e applica i piani con `led_fx_eyes_apply_fsm_plan()`.
  Nessun delay, nessun heap, nessuna lettura diretta del framebuffer: tutto
  passa dalla coda comandi del task `anim`.

## Piano per stato

| Stato FSM | Piano occhi | Effetto a video |
| --- | --- | --- |
| `IDLE` | `IDLE`: colore/effect/brightness di riposo da config, blink overlay ON | respiro lento ambra con blink casuali |
| `TRIGGERED` | `TRIGGERED`: colore/brightness di spavento + `glare` di durata `trigger_duration_s` | salita ≤50 ms, mantenimento, strobo, dissolvenza |
| `COOLDOWN` | come `IDLE` (il lerp del colore in led_effects fa la transizione progressiva) | da rosso ad ambra in ~1,5 s |
| `MANUAL` | **nessun piano**: colore ed effetto decisi da HA | come impostato dall'utente |
| `OFF` | `OFF`: occhi spenti | nero |

## Gate

Il piano viene inviato solo se `eyes_follow_allowed()`: `eyes_enabled` **e**
`eyes_follow_fsm` veri. Il gate è **doppio**: `eyes_ctrl` non costruisce nemmeno
il piano, e il motore occhi ignora comunque i piani se
`led_fx_eyes_follow_fsm(false)`. Gli effetti base impostati via API pubblica
(HA/console) non passano dal gate: con il follow disattivato gli occhi restano
come impostato (verificato a banco 2026-10-27: secondo `scare` con follow OFF →
`state/eyes` invariato).

Su ogni `CONFIG_EVENT_CHANGED` degli occhi il componente risincronizza il gate e
ri-applica il piano dello stato corrente: così i comandi HA sui colori di
riposo/spavento (e i due switch) hanno effetto immediato senza attendere la
prossima transizione.

## Init

`eyes_ctrl_init()` va chiamata **dopo** `config_store_init`, `led_fx_init` e
`pumpkin_fsm_init` (legge lo stato corrente con `pumpkin_fsm_get_state()`) e
prima di `ha_entities_init`; applica subito il piano dello stato di boot. Doppia
init → `ESP_ERR_INVALID_STATE`.

## Verifica

- Host: 8 test su `eyes_ctrl_core` (truth table gate, piani per stato,
  sanitizzazione nome effetto, `pct_to_255` con arrotondamento 10→26).
- Banco 2026-10-27 (app `pumpkin` su D1 mini): migrazione config v2→v3 al boot,
  piano IDLE applicato (breathe ambra 102/255, blink ON), `scare` via MQTT →
  glare rosso a 255 entro ~100 ms e ritorno al riposo a fine trigger, gate
  follow OFF → occhi immobili al secondo scare, comandi HA manuali su `eyes`
  e switch → effetto immediato.
