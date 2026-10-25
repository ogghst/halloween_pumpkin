/* Host unit tests for radar_trigger.c (Unity, native build).
 * Test functions are non-static: the file that owns main() declares and
 * runs them (see test_radar_parser.c, led_effects convention). */
#include <string.h>

#include "radar_trigger.h"
#include "unity.h"

/* Baseline configuration: trigger at 1 m, energy 30, 3 confirm frames,
 * 1 s hold, one hysteresis gate of 750 mm, static targets do not trigger. */
static radar_trigger_t *mk_default(radar_trigger_t *t)
{
    radar_trigger_config_t cfg = {
        .trigger_mm = 1000,
        .trigger_energy = 30,
        .confirm_frames = 3,
        .hold_ms = 1000,
        .hysteresis_gates = 1,
        .gate_mm = 750,
        .static_trigger = false,
    };
    radar_trigger_init(t, &cfg);
    return t;
}

static radar_data_frame_t mk_frame(radar_target_state_t state, uint16_t mov_mm,
                                   uint8_t mov_e, uint16_t st_mm, uint8_t st_e,
                                   uint16_t det_mm)
{
    radar_data_frame_t f;
    memset(&f, 0, sizeof(f));
    f.target_state = state;
    f.moving_distance_mm = mov_mm;
    f.moving_energy = mov_e;
    f.static_distance_mm = st_mm;
    f.static_energy = st_e;
    f.detection_distance_mm = det_mm;
    return f;
}

/* Moving target well inside the trigger window. */
static radar_data_frame_t mk_moving_ok(void)
{
    return mk_frame(RADAR_TARGET_MOVING, 800, 40, 0, 0, 800);
}

static void feed(radar_trigger_t *t, const radar_data_frame_t *f, int64_t ts,
                 radar_trigger_events_t *ev)
{
    radar_trigger_feed(t, f, ts, ev);
}

/* 1. Confirmation: PRESENCE_ON fires exactly on the third consecutive
 *    qualifying frame, and only once (no re-fire while present). */
void test_trigger_confirm_exact_third_frame(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t f = mk_moving_ok();
    int64_t ts = 1000000;

    mk_default(&t);
    feed(&t, &f, ts, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_FALSE(t.st.presence);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run);

    feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_EQUAL_UINT8(2, t.st.confirm_run);

    feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_on);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_TRUE(t.st.presence);

    /* Edge is single: a further qualifying frame does not re-fire. */
    feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_TRUE(t.st.presence);
    TEST_ASSERT_EQUAL_UINT8(3, t.st.confirm_run); /* saturated, no overflow */
}

/* 2. A non-qualifying frame in the middle resets the confirmation run and
 *    slides the confirmation: ON needs three fresh consecutive frames. */
void test_trigger_confirm_run_reset_on_gap(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_moving_ok();
    radar_data_frame_t gap = mk_frame(RADAR_TARGET_MOVING, 2000, 40, 0, 0, 2000);
    int64_t ts = 1000000;

    mk_default(&t);
    feed(&t, &ok, ts, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(2, t.st.confirm_run);

    feed(&t, &gap, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run);
    TEST_ASSERT_FALSE(t.st.presence);

    feed(&t, &ok, ts += 100000, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on); /* fourth frame overall: not yet */
    TEST_ASSERT_FALSE(t.st.presence);

    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_on); /* third consecutive after the gap */
    TEST_ASSERT_TRUE(t.st.presence);
}

/* 3. Hold: after ON, targetless frames keep presence for hold_ms and drop
 *    it strictly after: still ON at exactly +hold_ms, OFF at +1 us. */
void test_trigger_hold_expiry_only_after_hold_ms(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_moving_ok();
    radar_data_frame_t none = mk_frame(RADAR_TARGET_NONE, 0, 0, 0, 0, 0);
    int64_t ts = 1000000, last;

    mk_default(&t);
    feed(&t, &ok, ts, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);
    last = ts; /* last target at 1200000 */

    feed(&t, &none, last + 999999, &ev);
    TEST_ASSERT_TRUE(t.st.presence);
    TEST_ASSERT_FALSE(ev.presence_off);

    feed(&t, &none, last + 1000000, &ev); /* exactly hold_ms: not yet */
    TEST_ASSERT_TRUE(t.st.presence);
    TEST_ASSERT_FALSE(ev.presence_off);

    feed(&t, &none, last + 1000001, &ev); /* one us past: OFF */
    TEST_ASSERT_TRUE(ev.presence_off);
    TEST_ASSERT_FALSE(t.st.presence);

    feed(&t, &none, last + 2000000, &ev); /* edge is single */
    TEST_ASSERT_FALSE(ev.presence_off);
}

/* 4. Hysteresis: a MOVING target beyond trigger_mm + gates*gate_mm leaves
 *    immediately (no hold), while the band itself only refreshes hold. */
void test_trigger_hysteresis_immediate_exit(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_moving_ok();
    /* Exit threshold: 1000 + 1 * 750 = 1750 mm. */
    radar_data_frame_t band = mk_frame(RADAR_TARGET_MOVING, 1750, 40, 0, 0, 1750);
    radar_data_frame_t out = mk_frame(RADAR_TARGET_MOVING, 1751, 40, 0, 0, 1751);
    int64_t ts = 1000000;

    mk_default(&t);
    feed(&t, &ok, ts, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);

    /* Distance == threshold: non-qualifying but inside the band -> held. */
    feed(&t, &band, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);
    TEST_ASSERT_FALSE(ev.presence_off);

    /* 1 mm further, 1 us later: immediate OFF, far before hold_ms. */
    feed(&t, &out, ts + 1, &ev);
    TEST_ASSERT_TRUE(ev.presence_off);
    TEST_ASSERT_FALSE(t.st.presence);
    TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run);
}

/* 5. Static-only target without static_trigger: never arms nor fires, but
 *    present_static follows the frames; NONE clears it, no OFF (was OFF). */
void test_trigger_static_only_no_on_present_static(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t still =
        mk_frame(RADAR_TARGET_STATIC, 0, 0, 500, 90, 500);
    radar_data_frame_t none = mk_frame(RADAR_TARGET_NONE, 0, 0, 0, 0, 0);
    int64_t ts = 1000000;

    mk_default(&t);
    for (int i = 0; i < 5; i++) {
        feed(&t, &still, ts += 100000, &ev);
        TEST_ASSERT_FALSE(ev.presence_on);
        TEST_ASSERT_FALSE(t.st.presence);
        TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run); /* resets every frame */
        TEST_ASSERT_TRUE(t.st.present_static);
    }

    feed(&t, &none, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_off); /* presence was never ON */
    TEST_ASSERT_FALSE(t.st.present_static);
}

/* 6. A person who stands still after ON keeps presence: STATIC frames
 *    refresh the hold (they are targets), OFF only when frames go NONE. */
void test_trigger_static_frames_hold_presence(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_moving_ok();
    radar_data_frame_t still =
        mk_frame(RADAR_TARGET_STATIC, 0, 0, 600, 70, 600);
    radar_data_frame_t none = mk_frame(RADAR_TARGET_NONE, 0, 0, 0, 0, 0);
    int64_t ts = 1000000, last;

    mk_default(&t);
    feed(&t, &ok, ts, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);

    /* 10 s of static frames with 300 ms spacing: without the refresh the
     * 1 s hold would have expired three frames in. */
    for (int i = 0; i < 10; i++) {
        feed(&t, &still, ts += 300000, &ev);
        TEST_ASSERT_TRUE(t.st.presence);
        TEST_ASSERT_TRUE(t.st.present_static);
        TEST_ASSERT_FALSE(ev.presence_off);
    }
    last = ts;

    feed(&t, &none, last + 1000000, &ev); /* hold not yet expired */
    TEST_ASSERT_TRUE(t.st.presence);
    feed(&t, &none, last + 1000001, &ev);
    TEST_ASSERT_TRUE(ev.presence_off);
    TEST_ASSERT_FALSE(t.st.presence);
}

/* 7. Energy gate: a MOVING frame with energy trigger_energy - 1 does not
 *    qualify; exactly trigger_energy does (contract: >=). */
void test_trigger_energy_below_threshold_no_qualify(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t weak = mk_frame(RADAR_TARGET_MOVING, 800, 29, 0, 0, 800);
    radar_data_frame_t edge = mk_frame(RADAR_TARGET_MOVING, 800, 30, 0, 0, 800);
    int64_t ts = 1000000;

    mk_default(&t);
    for (int i = 0; i < 5; i++) {
        feed(&t, &weak, ts += 100000, &ev);
        TEST_ASSERT_FALSE(t.st.presence);
        TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run);
    }

    feed(&t, &edge, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run); /* qualifies at the edge */
}

/* 8. Leave and re-enter: after an OFF, a second ON needs a fresh count of
 *    confirm_frames qualifying frames. */
void test_trigger_leave_and_reenter_rearms(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_moving_ok();
    radar_data_frame_t far = mk_frame(RADAR_TARGET_MOVING, 2000, 40, 0, 0, 2000);
    int64_t ts = 1000000;

    mk_default(&t);
    feed(&t, &ok, ts, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);

    feed(&t, &far, ts += 100000, &ev); /* beyond hysteresis: immediate OFF */
    TEST_ASSERT_TRUE(ev.presence_off);
    TEST_ASSERT_FALSE(t.st.presence);

    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_on); /* second ON, third fresh frame */
    TEST_ASSERT_TRUE(t.st.presence);
}

/* 9. distance_updated only on change of detection_distance_mm; the very
 *    first frame counts as a change. */
void test_trigger_distance_updated_on_change(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    int64_t ts = 1000000;

    mk_default(&t);
    radar_data_frame_t f = mk_frame(RADAR_TARGET_MOVING, 800, 40, 0, 0, 800);
    feed(&t, &f, ts, &ev);
    TEST_ASSERT_TRUE(ev.distance_updated); /* first frame */

    feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.distance_updated); /* same value */

    f.detection_distance_mm = 801;
    feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.distance_updated);

    f.detection_distance_mm = 800;
    feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.distance_updated); /* going back is a change too */
}

/* 10. set_trigger_mm updates the window at runtime and clamps into the
 *     Kconfig range [200, 6000]. */
void test_trigger_set_trigger_mm_updates_and_clamps(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_moving_ok(); /* 800 mm, energy 40 */
    int64_t ts = 1000000;

    mk_default(&t);
    feed(&t, &ok, ts, &ev);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run);

    radar_trigger_set_trigger_mm(&t, 600); /* 800 mm is now outside */
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run);

    radar_data_frame_t near = mk_frame(RADAR_TARGET_MOVING, 600, 40, 0, 0, 600);
    feed(&t, &near, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run); /* new window applied */

    /* Low clamp: 50 becomes 200 -> 200 mm qualifies, 201 does not. */
    radar_trigger_set_trigger_mm(&t, 50);
    radar_data_frame_t at200 = mk_frame(RADAR_TARGET_MOVING, 200, 40, 0, 0, 200);
    radar_data_frame_t at201 = mk_frame(RADAR_TARGET_MOVING, 201, 40, 0, 0, 201);
    feed(&t, &at200, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(2, t.st.confirm_run); /* qualifies, extends run */
    feed(&t, &at201, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run);

    /* High clamp: 60000 becomes 6000 -> 6000 qualifies, 6001 does not. */
    radar_trigger_set_trigger_mm(&t, 60000);
    radar_data_frame_t at6000 = mk_frame(RADAR_TARGET_MOVING, 6000, 40, 0, 0, 6000);
    radar_data_frame_t at6001 = mk_frame(RADAR_TARGET_MOVING, 6001, 40, 0, 0, 6001);
    feed(&t, &at6000, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run);
    feed(&t, &at6001, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run);
}

/* 11. set_gate_mm feeds the hysteresis math: shrinking the gate shrinks
 *     the exit band and can evict a distance that previously stayed. */
void test_trigger_set_gate_mm_changes_hysteresis(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_moving_ok();
    /* With gate 750 the exit threshold is 1750 mm: 1700 stays. */
    radar_data_frame_t mid = mk_frame(RADAR_TARGET_MOVING, 1700, 40, 0, 0, 1700);
    int64_t ts = 1000000;

    mk_default(&t);
    feed(&t, &ok, ts, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);

    feed(&t, &mid, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);
    TEST_ASSERT_FALSE(ev.presence_off);

    radar_trigger_set_gate_mm(&t, 200); /* threshold drops to 1200 mm */
    feed(&t, &mid, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_off); /* same distance, now outside */
    TEST_ASSERT_FALSE(t.st.presence);
}

/* 12. static_trigger: a STATIC target qualifies on its own fields and
 *     fires the ON after confirm_frames. */
void test_trigger_static_trigger_mode_arms_on(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t still =
        mk_frame(RADAR_TARGET_STATIC, 0, 0, 800, 50, 800);
    int64_t ts = 1000000;

    radar_trigger_config_t cfg = {
        .trigger_mm = 1000,
        .trigger_energy = 30,
        .confirm_frames = 3,
        .hold_ms = 1000,
        .hysteresis_gates = 1,
        .gate_mm = 750,
        .static_trigger = true,
    };
    radar_trigger_init(&t, &cfg);

    feed(&t, &still, ts, &ev);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run);
    feed(&t, &still, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    feed(&t, &still, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_on); /* static side armed the trigger */
    TEST_ASSERT_TRUE(t.st.presence);
    TEST_ASSERT_TRUE(t.st.present_static);
}

/* 13. BOTH counts the moving side for qualification and shows up in
 *     present_static (the state contains STATIC). */
void test_trigger_both_counts_moving_side(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    /* Weak static side on purpose: only the moving side may qualify. */
    radar_data_frame_t both = mk_frame(RADAR_TARGET_BOTH, 900, 40, 1500, 5, 900);
    int64_t ts = 1000000;

    mk_default(&t);
    feed(&t, &both, ts, &ev);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run);
    feed(&t, &both, ts += 100000, &ev);
    feed(&t, &both, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_on);
    TEST_ASSERT_TRUE(t.st.presence);
    TEST_ASSERT_TRUE(t.st.present_static);
}
