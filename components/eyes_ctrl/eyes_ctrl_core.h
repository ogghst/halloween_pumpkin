/* Core puro del componente eyes_ctrl (pumpkin firmware, Fase 8).
 *
 * Traduce lo stato della FSM nel piano per gli occhi WS2812B (pixel 64-65
 * in coda alla matrice 8x8, stesso pin dati) secondo la configurazione
 * corrente. Nessuna dipendenza da ESP-IDF: compilato anche negli host
 * test (vedi host_tests/). Il layer IDF (eyes_ctrl.c) consuma il piano
 * tramite led_fx_eyes_apply_fsm_plan e gestisce gli eventi.
 */
#ifndef EYES_CTRL_CORE_H
#define EYES_CTRL_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "config_store.h"
#include "led_fx.h"
#include "pumpkin_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Percent 0-100 -> scala 0-255 con arrotondamento ((pct*255+50)/100),
 * stessa convenzione di ha_entities (10 % -> 26). */
uint8_t eyes_pct_to_255(uint8_t pct);

/* Gate "segui la FSM": vero solo con config presente, occhi abilitati e
 * eyes_follow_fsm attivo. */
bool eyes_follow_allowed(const pumpkin_config_t *cfg);

/* Costruisce il piano per lo stato FSM indicato. Ritorna false (e NON
 * tocca *out) quando gli occhi non vanno toccati: config mancante,
 * !eyes_enabled, !eyes_follow_fsm, stato MANUAL o stato sconosciuto.
 * - IDLE/COOLDOWN: look di riposo (rgb idle_*, effetto eyes_idle_effect
 *   sanitizzato, luminosita' da eyes_idle_brightness_pct); COOLDOWN usa
 *   lo stesso look: il lato led_effects sfuma la transizione.
 * - TRIGGERED: look di spavento (scare_*) per
 *   duration_ms = trigger_duration_s * 1000.
 * - OFF: piano azzerato, solo il kind e' significativo.
 * L'effetto di riposo e' sanitizzato: nome vuoto o lungo >= 16 caratteri
 * (incluso buffer non terminato) -> "breathe". */
bool eyes_plan_build(fsm_state_t state, const pumpkin_config_t *cfg,
                     led_fx_eyes_plan_t *out);

#ifdef __cplusplus
}
#endif
#endif /* EYES_CTRL_CORE_H */
