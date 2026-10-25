/* Presence trigger: pure decision layer over decoded radar reports.
 *
 * Turns target reports into edge events (contract in radar_trigger.h).
 * No clock and no allocation: time is injected as now_us and all state
 * lives in the caller-owned radar_trigger_t.
 *
 * Design notes:
 *  - present_static mirrors the frames verbatim (true iff the frame state
 *    contains STATIC). It is an occupancy hint, not a latch: no extra hold
 *    is applied here because holding occupancy is the job of the app/HA
 *    layer, which already owns the presence latch.
 *  - The hold expires strictly AFTER hold_ms without targets: a targetless
 *    frame stamped exactly hold_ms after the last target still finds the
 *    presence ON; one microsecond later it does not.
 *  - confirm_run saturates at confirm_frames instead of wrapping, so a
 *    long run of qualifying frames cannot overflow the counter.
 */
#include <string.h>

#include "radar_trigger.h"

/* Same window as CONFIG_RADAR_TRIGGER_MM in Kconfig (this pure layer does
 * not read sdkconfig): radar_trigger_set_trigger_mm clamps into it. */
#define TRIGGER_MM_MIN 200u
#define TRIGGER_MM_MAX 6000u

static bool state_has_moving(radar_target_state_t s)
{
    return s == RADAR_TARGET_MOVING || s == RADAR_TARGET_BOTH;
}

static bool state_has_static(radar_target_state_t s)
{
    return s == RADAR_TARGET_STATIC || s == RADAR_TARGET_BOTH;
}

/* A frame qualifies when the moving side (the only side counted for BOTH)
 * is within trigger_mm with enough energy. A STATIC target qualifies only
 * in static_trigger mode, judged on its own distance/energy fields. */
static bool frame_qualifies(const radar_trigger_config_t *cfg,
                            const radar_data_frame_t *f)
{
    if (state_has_moving(f->target_state))
        return f->moving_distance_mm <= cfg->trigger_mm &&
               f->moving_energy >= cfg->trigger_energy;
    if (cfg->static_trigger && f->target_state == RADAR_TARGET_STATIC)
        return f->static_distance_mm <= cfg->trigger_mm &&
               f->static_energy >= cfg->trigger_energy;
    return false;
}

void radar_trigger_init(radar_trigger_t *t, const radar_trigger_config_t *cfg)
{
    if (t == NULL)
        return;
    memset(t, 0, sizeof(*t));
    if (cfg != NULL)
        t->cfg = *cfg;
    if (t->cfg.confirm_frames == 0) /* contract: >= 1 */
        t->cfg.confirm_frames = 1;
}

void radar_trigger_set_trigger_mm(radar_trigger_t *t, uint16_t trigger_mm)
{
    if (t == NULL)
        return;
    if (trigger_mm < TRIGGER_MM_MIN)
        trigger_mm = TRIGGER_MM_MIN;
    else if (trigger_mm > TRIGGER_MM_MAX)
        trigger_mm = TRIGGER_MM_MAX;
    t->cfg.trigger_mm = trigger_mm;
}

void radar_trigger_set_gate_mm(radar_trigger_t *t, uint16_t gate_mm)
{
    if (t == NULL)
        return;
    /* No clamp: gate_mm 0 simply collapses the hysteresis band (exit
     * threshold == trigger_mm). */
    t->cfg.gate_mm = gate_mm;
}

void radar_trigger_feed(radar_trigger_t *t, const radar_data_frame_t *frame,
                        int64_t now_us, radar_trigger_events_t *out)
{
    radar_trigger_events_t ev = { false, false, false, { false, false, 0 } };

    if (t == NULL || frame == NULL) {
        if (out != NULL)
            *out = ev;
        return;
    }

    const radar_trigger_config_t *cfg = &t->cfg;

    /* detection_distance_mm changes are reported on every frame. */
    if (!t->have_distance || frame->detection_distance_mm != t->last_distance_mm) {
        ev.distance_updated = true;
        t->last_distance_mm = frame->detection_distance_mm;
        t->have_distance = true;
    }

    /* present_static follows the frames, no hold of its own. */
    t->st.present_static = state_has_static(frame->target_state);

    bool qualifying = frame_qualifies(cfg, frame);

    if (!t->st.presence) {
        /* Arming: only consecutive qualifying frames count. */
        if (qualifying) {
            if (t->st.confirm_run < cfg->confirm_frames)
                t->st.confirm_run++;
            if (t->st.confirm_run >= cfg->confirm_frames) {
                t->st.presence = true;
                ev.presence_on = true;
            }
        } else {
            t->st.confirm_run = 0;
        }
    }

    if (t->st.presence) {
        uint32_t exit_mm = (uint32_t)cfg->trigger_mm +
                           (uint32_t)cfg->hysteresis_gates * cfg->gate_mm;
        int64_t hold_us = (int64_t)cfg->hold_ms * 1000; /* now_us is us */
        if (state_has_moving(frame->target_state) &&
            frame->moving_distance_mm > exit_mm) {
            /* Moving target beyond the hysteresis band: leave at once,
             * without waiting for the hold to expire. */
            t->st.presence = false;
            ev.presence_off = true;
        } else if (frame->target_state != RADAR_TARGET_NONE) {
            t->last_target_us = now_us; /* any visible target refreshes hold */
        } else if (now_us - t->last_target_us > hold_us) {
            t->st.presence = false;
            ev.presence_off = true;
        }
    }

    /* Non-qualifying frames reset the confirmation run even while present,
     * so re-entering always requires a fresh confirmation count. */
    if (!qualifying)
        t->st.confirm_run = 0;

    ev.st = t->st;
    if (out != NULL)
        *out = ev;
}
