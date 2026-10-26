/* Host unit tests for radar_trigger.c (Unity, native build).
 * Test functions are non-static: the file that owns main() declares and
 * runs them (see test_radar_parser.c, led_effects convention).
 *
 * LD2420 semantics: a frame qualifies when presence is set with a
 * nonzero distance no farther than trigger_mm; ON needs confirm_frames
 * consecutive qualifying frames; while ON any presence frame refreshes
 * the hold, presence beyond trigger_mm + hysteresis_mm leaves at once,
 * and the hold expires strictly after hold_ms without presence. */
#include <string.h>

#include "unity.h"
#include "radar_trigger.h"

/* Baseline configuration: trigger at 1 m, 3 confirm frames, 1 s hold,
 * 500 mm of exit hysteresis. */
static radar_trigger_t *mk_default(radar_trigger_t *t)
{
    radar_trigger_config_t cfg = {
        .trigger_mm = 1000,
        .confirm_frames = 3,
        .hold_ms = 1000,
        .hysteresis_mm = 500,
    };
    radar_trigger_init(t, &cfg);
    return t;
}

static radar_data_frame_t mk_frame(bool presence, uint16_t distance_mm)
{
    radar_data_frame_t f;
    memset(&f, 0, sizeof(f));
    f.presence = presence;
    f.distance_mm = distance_mm;
    return f;
}

/* Target well inside the trigger window. */
static radar_data_frame_t mk_ok(void)
{
    return mk_frame(true, 800);
}

/* Presence-less frame: the module streams these when nobody is seen. */
static radar_data_frame_t mk_none(void)
{
    return mk_frame(false, 0);
}

/* 1. Confirmation: PRESENCE_ON fires exactly on the third consecutive
 *    qualifying frame, and only once (no re-fire while present). */
void test_trigger_confirm_exact_third_frame(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t f = mk_ok();
    int64_t ts = 1000000;

    mk_default(&t);
    radar_trigger_feed(&t, &f, ts, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_FALSE(t.st.presence);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run);

    radar_trigger_feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_EQUAL_UINT8(2, t.st.confirm_run);

    radar_trigger_feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_on);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_TRUE(t.st.presence);

    /* Edge is single: a further qualifying frame does not re-fire. */
    radar_trigger_feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_TRUE(t.st.presence);
    TEST_ASSERT_EQUAL_UINT8(3, t.st.confirm_run); /* saturated, no overflow */
}

/* 2. A non-qualifying frame in the middle (presence far beyond the
 *    window) resets the confirmation run and slides the confirmation:
 *    ON needs three fresh consecutive frames. */
void test_trigger_confirm_run_reset_on_gap(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_ok();
    radar_data_frame_t gap = mk_frame(true, 2000);
    int64_t ts = 1000000;

    mk_default(&t);
    radar_trigger_feed(&t, &ok, ts, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(2, t.st.confirm_run);

    radar_trigger_feed(&t, &gap, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run);
    TEST_ASSERT_FALSE(t.st.presence);

    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on); /* fourth frame overall: not yet */
    TEST_ASSERT_FALSE(t.st.presence);

    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_on); /* third consecutive after the gap */
    TEST_ASSERT_TRUE(t.st.presence);
}

/* 3. Hold: after ON, presence-less frames keep presence for hold_ms and
 *    drop it strictly after: still ON at exactly +hold_ms, OFF at
 *    +1 us. */
void test_trigger_hold_expiry_only_after_hold_ms(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_ok();
    radar_data_frame_t none = mk_none();
    int64_t ts = 1000000, on;

    mk_default(&t);
    radar_trigger_feed(&t, &ok, ts, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);
    on = ts;

    /* Presence-less frames inside the window: no OFF. */
    radar_trigger_feed(&t, &none, ts += 999999, &ev);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_TRUE(t.st.presence);

    /* Exactly hold_ms after the last present frame: still ON. */
    radar_trigger_feed(&t, &none, ts += 1, &ev);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_TRUE(t.st.presence);

    /* One microsecond later: OFF. */
    radar_trigger_feed(&t, &none, ts += 1, &ev);
    TEST_ASSERT_TRUE(ev.presence_off);
    TEST_ASSERT_FALSE(t.st.presence);
    /* Strictly after: OFF lands hold_ms + 1 us past the last present
     * frame. */
    TEST_ASSERT_EQUAL_INT64(1000001, ts - on);
}

/* 4. Hysteresis: presence beyond trigger_mm + hysteresis_mm (1500 here)
 *    leaves immediately, without waiting for the hold to expire. */
void test_trigger_hysteresis_immediate_exit(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_ok();
    radar_data_frame_t far_ = mk_frame(true, 1501);
    int64_t ts = 1000000;

    mk_default(&t);
    radar_trigger_feed(&t, &ok, ts, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);

    radar_trigger_feed(&t, &far_, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_off); /* immediate, same frame */
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_FALSE(t.st.presence);
    TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run); /* run reset for re-entry */

    /* Immediate means before the hold: still inside the 1 s window. */
    TEST_ASSERT_TRUE(ts < 1000000 + 3 * 100000 + 1000000);
}

/* 5. Presence within the band (trigger, trigger + hysteresis] neither
 *    leaves nor fires: it stays ON and refreshes the hold. */
void test_trigger_hysteresis_band_refreshes_hold(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_ok();
    radar_data_frame_t band = mk_frame(true, 1200);
    radar_data_frame_t none = mk_none();
    int64_t ts = 1000000, refresh;

    mk_default(&t);
    radar_trigger_feed(&t, &ok, ts, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);

    /* Band frame: no edge, hold refreshed from here. */
    ts += 900000;
    radar_trigger_feed(&t, &band, ts, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_TRUE(t.st.presence);
    refresh = ts;

    /* Still ON exactly hold_ms after the band frame... */
    radar_trigger_feed(&t, &none, ts += 1000000, &ev);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_TRUE(t.st.presence);

    /* ...and OFF one microsecond later: the band frame refreshed. */
    radar_trigger_feed(&t, &none, ts += 1, &ev);
    TEST_ASSERT_TRUE(ev.presence_off);
    TEST_ASSERT_EQUAL_INT64(1000001, ts - refresh);
}

/* 6. distance_updated only on change of distance_mm; the very first
 *    frame counts as a change. */
void test_trigger_distance_updated_on_change(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t f;
    int64_t ts = 1000000;

    mk_default(&t);
    f = mk_frame(true, 800);
    radar_trigger_feed(&t, &f, ts, &ev);
    TEST_ASSERT_TRUE(ev.distance_updated); /* first frame */

    radar_trigger_feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.distance_updated); /* same distance */

    f = mk_frame(true, 600);
    radar_trigger_feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.distance_updated);

    f = mk_none();
    radar_trigger_feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.distance_updated); /* back to 0 = change */

    radar_trigger_feed(&t, &f, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.distance_updated);
}

/* 7. Leave and re-enter: after an OFF, a second ON needs a fresh count
 *    of confirm_frames qualifying frames. */
void test_trigger_leave_and_reenter_rearms(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_ok();
    radar_data_frame_t far_ = mk_frame(true, 1600);
    int64_t ts = 1000000;

    mk_default(&t);
    radar_trigger_feed(&t, &ok, ts, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);

    radar_trigger_feed(&t, &far_, ts += 100000, &ev); /* immediate exit */
    TEST_ASSERT_TRUE(ev.presence_off);

    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on); /* re-arming, only 2 frames so far */

    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(ev.presence_on); /* second edge, fresh count */
    TEST_ASSERT_TRUE(t.st.presence);
}

/* 8. A frame with presence but distance 0 (module reports no range)
 *    never qualifies and resets the confirmation run; while ON it still
 *    refreshes the hold (presence is presence). */
void test_trigger_presence_zero_distance_not_qualifying(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t ok = mk_ok();
    radar_data_frame_t zero = mk_frame(true, 0);
    int64_t ts = 1000000;

    mk_default(&t);

    /* Arming side: does not count, resets the run. */
    radar_trigger_feed(&t, &ok, ts, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(2, t.st.confirm_run);
    radar_trigger_feed(&t, &zero, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run);
    TEST_ASSERT_FALSE(t.st.presence);

    /* ON side: refreshes the hold. */
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    radar_trigger_feed(&t, &ok, ts += 100000, &ev);
    TEST_ASSERT_TRUE(t.st.presence);
    radar_trigger_feed(&t, &zero, ts += 500000, &ev);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_TRUE(t.st.presence);

    /* Still ON hold_ms after the zero-distance refresh, OFF after. */
    radar_data_frame_t none = mk_none();
    radar_trigger_feed(&t, &none, ts += 1000000, &ev);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_TRUE(t.st.presence);
    radar_trigger_feed(&t, &none, ts += 1, &ev);
    TEST_ASSERT_TRUE(ev.presence_off);
}

/* 9. set_trigger_mm updates the window at runtime and clamps into the
 *    Kconfig range [200, 8000]. */
void test_trigger_set_trigger_mm_clamps_runtime(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_data_frame_t edge = mk_frame(true, 1200);
    int64_t ts = 1000000;

    mk_default(&t);

    /* Runtime effect: 1200 mm does not qualify at the default 1000 mm. */
    radar_trigger_feed(&t, &edge, ts, &ev);
    TEST_ASSERT_EQUAL_UINT8(0, t.st.confirm_run);

    /* Widen the window at runtime: the same distance now qualifies. */
    radar_trigger_set_trigger_mm(&t, 4000);
    TEST_ASSERT_EQUAL_UINT16(4000, t.cfg.trigger_mm); /* in range kept */
    radar_trigger_feed(&t, &edge, ts += 100000, &ev);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run);

    /* Clamps. */
    radar_trigger_set_trigger_mm(&t, 100);
    TEST_ASSERT_EQUAL_UINT16(200, t.cfg.trigger_mm);  /* low clamp */
    radar_trigger_set_trigger_mm(&t, 9999);
    TEST_ASSERT_EQUAL_UINT16(8000, t.cfg.trigger_mm); /* high clamp */
}

/* 10. confirm_frames = 1: the very first qualifying frame fires ON. */
void test_trigger_confirm_one_fires_on_first_frame(void)
{
    radar_trigger_t t;
    radar_trigger_events_t ev;
    radar_trigger_config_t cfg = {
        .trigger_mm = 1000,
        .confirm_frames = 1,
        .hold_ms = 1000,
        .hysteresis_mm = 500,
    };
    int64_t ts = 1000000;

    radar_trigger_init(&t, &cfg);
    radar_data_frame_t ok = mk_ok();
    radar_trigger_feed(&t, &ok, ts, &ev);
    TEST_ASSERT_TRUE(ev.presence_on);
    TEST_ASSERT_TRUE(t.st.presence);
    TEST_ASSERT_EQUAL_UINT8(1, t.st.confirm_run);

    /* confirm_frames = 0 in the config is contracted to mean 1. */
    cfg.confirm_frames = 0;
    radar_trigger_init(&t, &cfg);
    TEST_ASSERT_EQUAL_UINT8(1, t.cfg.confirm_frames);
}

/* 11. Robustness: NULL triggers/frames/out are safe no-ops. */
void test_trigger_null_args_noop(void)
{
    radar_trigger_events_t ev;
    radar_data_frame_t f = mk_ok();

    radar_trigger_init(NULL, NULL);
    radar_trigger_set_trigger_mm(NULL, 500);

    memset(&ev, 0xAA, sizeof(ev));
    radar_trigger_feed(NULL, &f, 0, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_FALSE(ev.distance_updated);

    radar_trigger_t t;
    mk_default(&t);
    memset(&ev, 0xAA, sizeof(ev));
    radar_trigger_feed(&t, NULL, 1000000, &ev);
    TEST_ASSERT_FALSE(ev.presence_on);
    TEST_ASSERT_FALSE(ev.presence_off);
    TEST_ASSERT_FALSE(ev.distance_updated);
    TEST_ASSERT_FALSE(t.st.presence);

    radar_trigger_feed(&t, &f, 1000000, NULL); /* out is nullable */
}
