/* Core puro di eyes_ctrl: piano FSM -> occhi (host-testato, vedi host_tests/). */
#include "eyes_ctrl_core.h"

#include <string.h>

uint8_t eyes_pct_to_255(uint8_t pct)
{
    /* Arrotonda al piu' vicino come fa ha_entities per la brightness HA. */
    return (uint8_t)(((uint32_t)pct * 255u + 50u) / 100u);
}

bool eyes_follow_allowed(const pumpkin_config_t *cfg)
{
    return cfg != NULL && cfg->eyes_enabled && cfg->eyes_follow_fsm;
}

/* Nome effetto di riposo sanitizzato: vuoto, troppo lungo o buffer non
 * terminato -> "breathe". Restituisce sempre un puntatore a stringa
 * terminata lunga al piu' 15 caratteri (idle_effect e' char[16]). */
static const char *idle_effect_name(const pumpkin_config_t *cfg)
{
    size_t len = strnlen(cfg->eyes_idle_effect, sizeof(cfg->eyes_idle_effect));
    if (len == 0 || len >= sizeof(cfg->eyes_idle_effect)) {
        return "breathe";
    }
    return cfg->eyes_idle_effect;
}

bool eyes_plan_build(fsm_state_t state, const pumpkin_config_t *cfg,
                     led_fx_eyes_plan_t *out)
{
    led_fx_eyes_plan_t plan;

    if (cfg == NULL || out == NULL) {
        return false;
    }
    if (!eyes_follow_allowed(cfg)) {
        return false;
    }

    memset(&plan, 0, sizeof(plan));
    switch (state) {
    case FSM_STATE_IDLE:
    case FSM_STATE_COOLDOWN:
        plan.kind = (state == FSM_STATE_IDLE) ? LED_FX_EYES_PLAN_IDLE
                                              : LED_FX_EYES_PLAN_COOLDOWN;
        plan.idle_r = cfg->eyes_idle_r;
        plan.idle_g = cfg->eyes_idle_g;
        plan.idle_b = cfg->eyes_idle_b;
        plan.idle_brightness = eyes_pct_to_255(cfg->eyes_idle_brightness_pct);
        {
            const char *name = idle_effect_name(cfg);
            memcpy(plan.idle_effect, name, strlen(name) + 1);
        }
        break;
    case FSM_STATE_TRIGGERED:
        plan.kind = LED_FX_EYES_PLAN_TRIGGERED;
        plan.scare_r = cfg->eyes_scare_r;
        plan.scare_g = cfg->eyes_scare_g;
        plan.scare_b = cfg->eyes_scare_b;
        plan.scare_brightness = eyes_pct_to_255(cfg->eyes_scare_brightness_pct);
        plan.duration_ms = (uint32_t)cfg->trigger_duration_s * 1000u;
        break;
    case FSM_STATE_OFF:
        plan.kind = LED_FX_EYES_PLAN_OFF; /* resto azzerato dal memset */
        break;
    default:
        return false; /* stato non gestito: *out resta intatto */
    }

    *out = plan;
    return true;
}
