/* Pure core of the pumpkin FSM (contract in fsm_core.h).
 *
 * Implementation notes:
 * - Time is read only through the injected clock; deadlines are absolute
 *   microseconds and fsm_core_poll() performs the TIMEOUT transition.
 * - Every side effect is described in fsm_output_t; a look change always
 *   carries effect + color + brightness (no delta tracking).
 * - timer_valid + timer_ms == 0 is the documented "cancel" encoding:
 *   entering an untimed state always emits it so the IDF layer never
 *   keeps a stale one-shot armed.
 * - Command-driven config fields are persisted through out->persist;
 *   the IDF layer maps the core param ids onto config_store writes.
 */
#include <string.h>

#include "fsm_core.h"

/* Bounded copy of an animation name; src is a fixed-size field and is
 * not guaranteed NUL-terminated. */
static void copy_anim(char dst[FSM_ANIM_NAME_LEN], const char *src)
{
    size_t i;

    memset(dst, 0, FSM_ANIM_NAME_LEN);
    for (i = 0; i < FSM_ANIM_NAME_LEN - 1u && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
}

static int64_t now_us(const fsm_core_t *f)
{
    return f->clock.now_us ? f->clock.now_us(f->clock.ctx) : 0;
}

/* Percent to the 0..255 scale the LED layer wants (floor rounding). */
static uint8_t pct_to_255(uint8_t pct)
{
    return (uint8_t)(((uint32_t)pct * 255u) / 100u);
}

static uint8_t cap_pct(const fsm_core_t *f, uint8_t pct)
{
    if (pct > f->cfg.max_brightness_pct) {
        return f->cfg.max_brightness_pct;
    }
    return pct;
}

/* A look change always carries effect + color + brightness. */
static void emit_look(const fsm_core_t *f, fsm_output_t *out, const char *anim,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t pct)
{
    out->do_effect = true;
    copy_anim(out->effect, anim);
    out->do_color = true;
    out->r = r;
    out->g = g;
    out->b = b;
    out->do_brightness = true;
    out->brightness_0_255 = pct_to_255(cap_pct(f, pct));
}

static void emit_idle_look(const fsm_core_t *f, fsm_output_t *out)
{
    emit_look(f, out, f->cfg.idle_anim, f->cfg.color_r, f->cfg.color_g,
              f->cfg.color_b, f->cfg.idle_brightness_pct);
}

static void emit_trigger_look(const fsm_core_t *f, fsm_output_t *out)
{
    emit_look(f, out, f->cfg.trigger_anim, f->cfg.color_r, f->cfg.color_g,
              f->cfg.color_b, f->cfg.brightness_pct);
}

static void emit_manual_look(const fsm_core_t *f, fsm_output_t *out)
{
    emit_look(f, out, f->manual.anim, f->manual.r, f->manual.g, f->manual.b,
              f->manual.brightness_pct);
}

static void set_state(fsm_core_t *f, fsm_output_t *out, fsm_state_t new_state)
{
    out->state_changed = true;
    out->prev_state = f->state;
    out->new_state = new_state;
    f->state = new_state;
}

/* Rearm the single deadline; durations arrive sanitized (>= 1 s). */
static void arm_timer_s(fsm_core_t *f, fsm_output_t *out, uint16_t seconds)
{
    out->timer_valid = true;
    out->timer_ms = (uint32_t)seconds * 1000u;
    f->deadline_us = now_us(f) + (int64_t)out->timer_ms * 1000;
    f->has_deadline = true;
}

static void cancel_timer(fsm_core_t *f, fsm_output_t *out)
{
    out->timer_valid = true;
    out->timer_ms = 0;
    f->has_deadline = false;
}

/* Untimed presentation state: idle look, no deadline. */
static void enter_idle(fsm_core_t *f, fsm_output_t *out)
{
    set_state(f, out, FSM_STATE_IDLE);
    f->return_state = FSM_STATE_IDLE;
    emit_idle_look(f, out);
    cancel_timer(f, out);
}

/* Entering MANUAL seeds the manual look with the look on display
 * (TRIGGERED shows the trigger look, everything else the idle look). */
static void seed_manual(fsm_core_t *f)
{
    if (f->state == FSM_STATE_TRIGGERED) {
        copy_anim(f->manual.anim, f->cfg.trigger_anim);
        f->manual.r = f->cfg.color_r;
        f->manual.g = f->cfg.color_g;
        f->manual.b = f->cfg.color_b;
        f->manual.brightness_pct = cap_pct(f, f->cfg.brightness_pct);
    } else {
        copy_anim(f->manual.anim, f->cfg.idle_anim);
        f->manual.r = f->cfg.color_r;
        f->manual.g = f->cfg.color_g;
        f->manual.b = f->cfg.color_b;
        f->manual.brightness_pct = cap_pct(f, f->cfg.idle_brightness_pct);
    }
}

void fsm_core_init(fsm_core_t *f, const fsm_config_t *cfg,
                   const fsm_clock_t *clock)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
    if (cfg != NULL) {
        f->cfg = *cfg;
    }
    if (clock != NULL) {
        f->clock = *clock;
    }
    f->state = f->cfg.power_on ? FSM_STATE_IDLE : FSM_STATE_OFF;
    f->return_state = FSM_STATE_IDLE;
    f->has_deadline = false;
    /* Valid manual look even if MANUAL is entered before any command. */
    seed_manual(f);
}

void fsm_core_boot(fsm_core_t *f, fsm_output_t *out)
{
    if (f == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (f->state == FSM_STATE_OFF) {
        out->do_power = true;
        out->power_on = false;
        return;
    }
    out->do_power = true;
    out->power_on = true;
    emit_idle_look(f, out);
}

void fsm_core_poll(fsm_core_t *f, fsm_output_t *out)
{
    if (f == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!f->has_deadline || now_us(f) < f->deadline_us) {
        return;
    }
    f->has_deadline = false;

    if (f->state == FSM_STATE_TRIGGERED) {
        if (f->return_state == FSM_STATE_MANUAL) {
            /* A scare from MANUAL returns to the manual look. */
            set_state(f, out, FSM_STATE_MANUAL);
            emit_manual_look(f, out);
            cancel_timer(f, out);
        } else if (f->cfg.cooldown_s > 0) {
            f->return_state = FSM_STATE_IDLE;
            set_state(f, out, FSM_STATE_COOLDOWN);
            emit_idle_look(f, out);
            arm_timer_s(f, out, f->cfg.cooldown_s);
        } else {
            /* Cooldown disabled: straight back to IDLE. */
            enter_idle(f, out);
        }
    } else if (f->state == FSM_STATE_COOLDOWN) {
        enter_idle(f, out);
    }
}

static void cmd_mode(fsm_core_t *f, fsm_input_t *in, fsm_output_t *out)
{
    switch (in->u.mode) {
    case FSM_MODE_OFF:
        if (f->state != FSM_STATE_OFF) {
            set_state(f, out, FSM_STATE_OFF);
            f->return_state = FSM_STATE_IDLE;
            out->do_power = true;
            out->power_on = false;
            cancel_timer(f, out);
        }
        break;

    case FSM_MODE_MANUAL:
        if (f->state != FSM_STATE_MANUAL) {
            seed_manual(f);
            set_state(f, out, FSM_STATE_MANUAL);
            f->return_state = FSM_STATE_MANUAL;
            cancel_timer(f, out);
            out->do_power = true;
            out->power_on = true;
            emit_manual_look(f, out);
        }
        break;

    case FSM_MODE_AUTO:
        /* IDLE is already auto: no-op. TRIGGERED/COOLDOWN keep running
         * with their deadline preserved; only MANUAL/OFF re-enter IDLE. */
        if (f->state == FSM_STATE_MANUAL || f->state == FSM_STATE_OFF) {
            if (f->state == FSM_STATE_OFF) {
                out->do_power = true;
                out->power_on = true;
            }
            enter_idle(f, out);
        }
        break;

    default:
        in->result = ESP_ERR_INVALID_ARG;
        break;
    }
}

static void cmd_light(fsm_core_t *f, fsm_input_t *in, fsm_output_t *out)
{
    uint8_t pct = in->u.light.brightness_pct;

    if (f->state == FSM_STATE_OFF) {
        in->result = ESP_ERR_INVALID_STATE;
        return;
    }
    if (pct > 100u) {
        pct = 100u;
    }

    if (f->state == FSM_STATE_MANUAL) {
        /* Manual look only: transient, nothing persisted. */
        f->manual.r = in->u.light.r;
        f->manual.g = in->u.light.g;
        f->manual.b = in->u.light.b;
        f->manual.brightness_pct = pct;
        emit_manual_look(f, out);
        return;
    }

    f->cfg.color_r = in->u.light.r;
    f->cfg.color_g = in->u.light.g;
    f->cfg.color_b = in->u.light.b;
    out->persist.color = true;
    out->persist.r = in->u.light.r;
    out->persist.g = in->u.light.g;
    out->persist.b = in->u.light.b;

    if (f->state == FSM_STATE_TRIGGERED) {
        f->cfg.brightness_pct = pct;
        out->persist.bright = true;
        out->persist.bright_param = FSM_PERSIST_BRIGHTNESS;
        out->persist.bright_pct = pct;
        emit_trigger_look(f, out);
    } else {
        /* IDLE and COOLDOWN share the idle look. */
        f->cfg.idle_brightness_pct = pct;
        out->persist.bright = true;
        out->persist.bright_param = FSM_PERSIST_IDLE_BRIGHTNESS;
        out->persist.bright_pct = pct;
        emit_idle_look(f, out);
    }
}

static void cmd_effect(fsm_core_t *f, fsm_input_t *in, fsm_output_t *out)
{
    if (f->state == FSM_STATE_OFF) {
        in->result = ESP_ERR_INVALID_STATE;
        return;
    }

    if (f->state == FSM_STATE_MANUAL) {
        copy_anim(f->manual.anim, in->u.anim);
        emit_manual_look(f, out);
        return;
    }

    if (f->state == FSM_STATE_TRIGGERED) {
        copy_anim(f->cfg.trigger_anim, in->u.anim);
        out->persist.anim = true;
        out->persist.anim_param = FSM_PERSIST_TRIGGER_ANIM;
        copy_anim(out->persist.anim_name, in->u.anim);
        emit_trigger_look(f, out);
        return;
    }

    /* IDLE and COOLDOWN shape the idle look. */
    copy_anim(f->cfg.idle_anim, in->u.anim);
    out->persist.anim = true;
    out->persist.anim_param = FSM_PERSIST_IDLE_ANIM;
    copy_anim(out->persist.anim_name, in->u.anim);
    emit_idle_look(f, out);
}

static void cmd_scare(fsm_core_t *f, fsm_input_t *in, fsm_output_t *out)
{
    if (f->state == FSM_STATE_OFF) {
        in->result = ESP_ERR_INVALID_STATE;
        return;
    }

    if (f->state == FSM_STATE_TRIGGERED) {
        /* Restart the duration; look and return target unchanged. */
        arm_timer_s(f, out, f->cfg.trigger_duration_s);
        return;
    }

    f->return_state = (f->state == FSM_STATE_MANUAL) ? FSM_STATE_MANUAL
                                                     : FSM_STATE_IDLE;
    set_state(f, out, FSM_STATE_TRIGGERED);
    emit_trigger_look(f, out);
    arm_timer_s(f, out, f->cfg.trigger_duration_s);
}

static void apply_config(fsm_core_t *f, fsm_input_t *in, fsm_output_t *out)
{
    f->cfg = in->u.cfg;

    /* power_on is a boot policy only: it is never read here. */
    switch (f->state) {
    case FSM_STATE_IDLE:
    case FSM_STATE_COOLDOWN:
        emit_idle_look(f, out);
        break;
    case FSM_STATE_TRIGGERED:
        /* Look refreshed, deadline untouched. */
        emit_trigger_look(f, out);
        break;
    case FSM_STATE_MANUAL:
        emit_manual_look(f, out);
        break;
    case FSM_STATE_OFF:
    default:
        /* Rail off: no light output at all. */
        break;
    }
}

void fsm_core_process(fsm_core_t *f, fsm_input_t *in, fsm_output_t *out)
{
    if (f == NULL || in == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    in->result = ESP_OK;

    switch (in->type) {
    case FSM_IN_PRESENCE_ON:
        /* Only a fresh IDLE entry triggers; MANUAL/OFF/COOLDOWN and an
         * already-TRIGGERED state ignore presence. */
        if (f->state == FSM_STATE_IDLE && f->cfg.auto_mode) {
            f->return_state = FSM_STATE_IDLE;
            set_state(f, out, FSM_STATE_TRIGGERED);
            emit_trigger_look(f, out);
            arm_timer_s(f, out, f->cfg.trigger_duration_s);
        }
        break;

    case FSM_IN_PRESENCE_OFF:
        /* Presence never shortens the fixed trigger duration. */
        break;

    case FSM_IN_POLL:
        fsm_core_poll(f, out);
        break;

    case FSM_IN_CMD_MODE:
        cmd_mode(f, in, out);
        break;

    case FSM_IN_CMD_LIGHT:
        cmd_light(f, in, out);
        break;

    case FSM_IN_CMD_EFFECT:
        cmd_effect(f, in, out);
        break;

    case FSM_IN_CMD_SCARE:
        cmd_scare(f, in, out);
        break;

    case FSM_IN_CONFIG_CHANGED:
        apply_config(f, in, out);
        break;

    default:
        break;
    }
}
