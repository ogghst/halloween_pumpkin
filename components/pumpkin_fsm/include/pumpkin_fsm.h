/* Public API of the pumpkin_fsm component (pumpkin firmware, Fase 3).
 *
 * State machine gluing the presence radar to the LED effects. One owner
 * task processes a single event queue (radar events, config changes,
 * user commands, timer expirations); esp_timer arms the durations.
 * See docs/fsm.md for the diagram and the transition table.
 */
#ifndef PUMPKIN_FSM_H
#define PUMPKIN_FSM_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(FSM_EVENT);

/* FSM states. */
typedef enum {
    FSM_STATE_IDLE = 0,      /* soft idle look, waiting for presence */
    FSM_STATE_TRIGGERED,     /* trigger look for trigger_duration_s */
    FSM_STATE_COOLDOWN,      /* trigger done, presence ignored cooldown_s */
    FSM_STATE_MANUAL,        /* user-driven light, auto logic suspended */
    FSM_STATE_OFF,           /* rail off */
} fsm_state_t;

/* Target mode of fsm_cmd_set_mode. */
typedef enum {
    FSM_MODE_AUTO = 0, /* run the IDLE->TRIGGERED->COOLDOWN cycle */
    FSM_MODE_MANUAL,   /* hold the current light, auto logic suspended */
    FSM_MODE_OFF,      /* rail off */
} fsm_mode_t;

/* Event ids posted on FSM_EVENT. */
typedef enum {
    FSM_EVENT_STATE_CHANGED = 0, /* event_data: fsm_state_change_t */
} fsm_event_id_t;

/* Payload of FSM_EVENT_STATE_CHANGED. */
typedef struct {
    fsm_state_t state;
    fsm_state_t prev_state;
} fsm_state_change_t;

/* Start the FSM: subscribes to PRESENCE_EVENT and CONFIG_EVENT on the
 * default loop, loads the configuration and applies the boot look (IDLE,
 * or OFF when the persisted power_on is false). Idempotent-safe: calling
 * twice returns ESP_ERR_INVALID_STATE. */
esp_err_t pumpkin_fsm_init(void);

/* Current state (mutex-free atomic read of the owner-published copy). */
fsm_state_t pumpkin_fsm_get_state(void);

/* Commands are queued to the FSM owner task and applied there; the return
 * value only reports queueing/validation errors, state-dependent
 * acceptance is decided by the FSM (invalid transitions are logged).
 * - set_mode: any state -> OFF or MANUAL; AUTO re-enters IDLE. OFF/MANUAL
 *   also persist CFG_PARAM_POWER_ON accordingly.
 * - set_light / set_effect: in MANUAL they shape the manual look
 *   (transient); in the AUTO cycle they update the look of the current
 *   state AND persist it (idle_* in IDLE/COOLDOWN, trigger look in
 *   TRIGGERED). Rejected in OFF.
 * - scare: play the trigger look once from any state except OFF, then
 *   return to the previous presentation (MANUAL -> MANUAL, else IDLE). */
esp_err_t fsm_cmd_set_mode(fsm_mode_t mode);
esp_err_t fsm_cmd_set_light(uint8_t r, uint8_t g, uint8_t b,
                            uint8_t brightness_pct);
esp_err_t fsm_cmd_set_effect(const char *name);
esp_err_t fsm_cmd_scare(void);

#ifdef __cplusplus
}
#endif
#endif /* PUMPKIN_FSM_H */
