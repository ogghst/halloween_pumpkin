/* Presence trigger logic for the HLK-LD2420: pure, clock-injected,
 * host-testable layer.
 *
 * Consumes decoded data frames (radar_data_frame_t) and produces edge
 * events for PRESENCE_ON / PRESENCE_OFF / DISTANCE_UPDATED.
 *
 * Semantics (Fase 2 spec, adapted to the LD2420):
 *  - The LD2420 reports a plain presence flag plus a distance in cm; it
 *    does NOT distinguish moving/static targets (present_static is not
 *    modelled). Per-gate energy gating is the module's own job (trigger
 *    and keep thresholds per gate).
 *  - A "qualifying" frame has presence true with a nonzero distance
 *    <= cfg.trigger_mm.
 *  - PRESENCE_ON fires once after cfg.confirm_frames consecutive
 *    qualifying frames (rising edge of a confirmed approaching target).
 *    Any non-qualifying frame resets the confirmation run.
 *  - While presence is ON, any frame with presence true refreshes the
 *    hold timer (a person who stands still is still there).
 *  - PRESENCE_OFF fires when no presence is seen for cfg.hold_ms, or
 *    immediately when presence is true but distance is beyond
 *    cfg.trigger_mm + cfg.hysteresis_mm.
 *  - distance_updated is set when distance_mm changes.
 */
#ifndef RADAR_TRIGGER_H
#define RADAR_TRIGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "radar_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t trigger_mm;     /* fire when distance <= trigger_mm */
    uint8_t confirm_frames;  /* consecutive qualifying frames to fire (>=1) */
    uint32_t hold_ms;        /* presence hold without any target */
    uint16_t hysteresis_mm;  /* exit margin beyond trigger_mm */
} radar_trigger_config_t;

typedef struct {
    bool presence;       /* latched trigger state */
    uint8_t confirm_run; /* consecutive qualifying frames so far */
} radar_trigger_state_t;

typedef struct {
    bool presence_on;        /* rising edge this frame */
    bool presence_off;       /* falling edge this frame */
    bool distance_updated;   /* distance_mm changed this frame */
    radar_trigger_state_t st;
} radar_trigger_events_t;

/* Internal state; fields are private to the component. */
typedef struct {
    radar_trigger_config_t cfg;
    radar_trigger_state_t st;
    int64_t last_present_us;  /* last frame with presence true */
    uint16_t last_distance_mm;
    bool have_distance;
} radar_trigger_t;

void radar_trigger_init(radar_trigger_t *t, const radar_trigger_config_t *cfg);

/* Feed one decoded report frame stamped with the receive time. `out`
 * (nullable) receives the edge flags for this frame. */
void radar_trigger_feed(radar_trigger_t *t, const radar_data_frame_t *frame,
                        int64_t now_us, radar_trigger_events_t *out);

/* Runtime update of the trigger distance (also validates/clamps). */
void radar_trigger_set_trigger_mm(radar_trigger_t *t, uint16_t trigger_mm);

#ifdef __cplusplus
}
#endif
#endif /* RADAR_TRIGGER_H */
