/* Presence trigger logic: pure, clock-injected, host-testable layer.
 *
 * Consumes decoded data frames (radar_data_frame_t) and produces edge
 * events for PRESENCE_ON / PRESENCE_OFF / DISTANCE_UPDATED.
 *
 * Semantics (Fase 2 spec):
 *  - A "qualifying" frame has a MOVING target (or a STATIC target when
 *    cfg.static_trigger is set) with distance <= cfg.trigger_mm and
 *    target energy >= cfg.trigger_energy.
 *  - PRESENCE_ON fires once after cfg.confirm_frames consecutive
 *    qualifying frames (rising edge of a confirmed approaching target).
 *    Any non-qualifying frame resets the confirmation run.
 *  - While presence is ON, any frame with target_state != NONE refreshes
 *    the hold timer (a person who stands still is still there).
 *  - PRESENCE_OFF fires when no target at all is seen for cfg.hold_ms,
 *    or immediately when a MOVING target is beyond
 *    cfg.trigger_mm + cfg.hysteresis_gates * cfg.gate_mm (hysteresis).
 *  - A static-only target never arms/fires ON (unless static_trigger)
 *    but keeps present_static true: occupancy exposed to HA without a
 *    new trigger. Leaving and re-entering re-arms normally.
 *  - distance_updated is set when detection_distance_mm changes.
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
    uint16_t trigger_mm;       /* fire when moving distance <= trigger_mm */
    uint8_t trigger_energy;    /* ... and moving energy >= trigger_energy */
    uint8_t confirm_frames;    /* consecutive qualifying frames to fire (>=1) */
    uint32_t hold_ms;          /* presence hold without any target */
    uint8_t hysteresis_gates;  /* exit margin in distance gates */
    uint16_t gate_mm;          /* current gate width (750 or 200) */
    bool static_trigger;       /* static-only targets also arm and fire */
} radar_trigger_config_t;

typedef struct {
    bool presence;         /* latched trigger state */
    bool present_static;   /* static-only occupancy, no trigger */
    uint8_t confirm_run;   /* consecutive qualifying frames so far */
} radar_trigger_state_t;

typedef struct {
    bool presence_on;        /* rising edge this frame */
    bool presence_off;       /* falling edge this frame */
    bool distance_updated;   /* detection_distance_mm changed this frame */
    radar_trigger_state_t st;
} radar_trigger_events_t;

/* Internal state; fields are private to the component. */
typedef struct {
    radar_trigger_config_t cfg;
    radar_trigger_state_t st;
    int64_t last_target_us;      /* last frame with target_state != NONE */
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

/* Update after a resolution change: hysteresis math depends on gate width. */
void radar_trigger_set_gate_mm(radar_trigger_t *t, uint16_t gate_mm);

#ifdef __cplusplus
}
#endif
#endif /* RADAR_TRIGGER_H */
