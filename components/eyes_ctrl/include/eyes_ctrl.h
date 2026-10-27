/* Public API of the eyes_ctrl component (pumpkin firmware, Fase 8).
 *
 * Ponte tra la FSM e gli occhi WS2812B (pixel 64-65 in coda alla matrice
 * 8x8, stesso pin dati). All'avvio applica il piano dello stato corrente,
 * poi segue gli eventi FSM_EVENT_STATE_CHANGED e CONFIG_EVENT_CHANGED
 * (parametri occhi) sul default event loop: ogni transizione ricalcola il
 * piano con eyes_plan_build e lo passa a led_fx_eyes_apply_fsm_plan.
 * Con eyes_enabled o eyes_follow_fsm a false gli occhi non vengono piu'
 * toccati dalla FSM (restano alla libera di HA/led_effects).
 */
#ifndef EYES_CTRL_H
#define EYES_CTRL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registra i listener eventi e applica subito il piano dello stato
 * corrente. Prerequisiti (chiamare prima, in quest'ordine):
 * config_store_init, led_fx_init, pumpkin_fsm_init. Chiamata due volte
 * ritorna ESP_ERR_INVALID_STATE (come pumpkin_fsm_init). Non blocca e non
 * alloca: gli handler girano nel task del default event loop. */
esp_err_t eyes_ctrl_init(void);

#ifdef __cplusplus
}
#endif
#endif /* EYES_CTRL_H */
