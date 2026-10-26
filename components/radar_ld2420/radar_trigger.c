/* Presence trigger: pure decision layer over decoded LD2420 reports.
 *
 * Turns decoded report frames into edge events (contract in
 * radar_trigger.h). No clock and no allocation: time is injected as
 * now_us and all state lives in the caller-owned radar_trigger_t.
 *
 * Design notes:
 *  - The LD2420 reports a plain presence flag plus a distance (cm on the
 *    wire, mm here); it has no moving/static distinction and no
 *    per-frame energies: per-gate energy gating is the module's own job
 *    (trigger and keep thresholds per gate).
 *  - A frame qualifies when presence is set with a nonzero distance no
 *    farther than trigger_mm. Any other frame resets the confirmation
 *    run — including presence with distance 0 (the wire value when the
 *    module reports no range).
 *  - While presence is ON, ANY frame with presence true refreshes the
 *    hold (a person who stands still is still there), including frames
 *    with distance 0. A frame with presence beyond
 *    trigger_mm + hysteresis_mm leaves immediately, without waiting
 *    for the hold to expire.
 *  - The hold expires strictly AFTER hold_ms without presence: a
 *    presence-less frame stamped exactly hold_ms after the last present
 *    frame still finds presence ON; one microsecond later it does not.
 *  - confirm_run saturates at confirm_frames instead of wrapping, so a
 *    long run of qualifying frames cannot overflow the counter.
 */
#include <string.h>

#include "radar_trigger.h"

/* Same window as CONFIG_RADAR_TRIGGER_MM in Kconfig (this pure layer does
 * not read sdkconfig): radar_trigger_set_trigger_mm clamps into it. */
#define TRIGGER_MM_MIN 200u
#define TRIGGER_MM_MAX 8000u

/* A frame qualifies when a target is seen no farther than trigger_mm.
 * distance_mm == 0 means the module reported no range: never qualifies. */
static bool frame_qualifies(const radar_trigger_config_t *cfg,
                            const radar_data_frame_t *f)
{
    return f->presence && f->distance_mm > 0 &&
           f->distance_mm <= cfg->trigger_mm;
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

void radar_trigger_feed(radar_trigger_t *t, const radar_data_frame_t *frame,
                        int64_t now_us, radar_trigger_events_t *out)
{
    radar_trigger_events_t ev = { false, false, false, { false, 0 } };

    if (t == NULL || frame == NULL) {
        if (out != NULL)
            *out = ev;
        return;
    }

    const radar_trigger_config_t *cfg = &t->cfg;

    /* distance_mm changes are reported on every frame; the very first
     * frame counts as a change too. */
    if (!t->have_distance || frame->distance_mm != t->last_distance_mm) {
        ev.distance_updated = true;
        t->last_distance_mm = frame->distance_mm;
        t->have_distance = true;
    }

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
                           (uint32_t)cfg->hysteresis_mm;
        int64_t hold_us = (int64_t)cfg->hold_ms * 1000; /* now_us is us */
        if (frame->presence && frame->distance_mm > exit_mm) {
            /* Target beyond the hysteresis band: leave at once,
             * without waiting for the hold to expire. */
            t->st.presence = false;
            ev.presence_off = true;
        } else if (frame->presence) {
            t->last_present_us = now_us; /* any presence refreshes hold */
        } else if (now_us - t->last_present_us > hold_us) {
            t->st.presence = false;
            ev.presence_off = true;
        }
    }

    /* Non-qualifying frames reset the confirmation run even while
     * present, so re-entering always requires a fresh count. */
    if (!qualifying)
        t->st.confirm_run = 0;

    ev.st = t->st;
    if (out != NULL)
        *out = ev;
}
