/* Host unit tests for fsm_core.c (Unity, native build).
 *
 * The core is pure: a fake clock (struct + ctx) drives every deadline and
 * outputs are plain structs, so each transition is checked field by field
 * against the authoritative table:
 *
 * - boot: power_on ? IDLE (power + idle look) : OFF (power off only)
 * - IDLE + PRESENCE_ON: auto_mode ? TRIGGERED (trigger look + duration) : noop
 * - TRIGGERED ignores presence (fixed duration); COOLDOWN ignores presence
 * - poll at the deadline: TRIGGERED -> COOLDOWN (cooldown_s == 0 -> IDLE),
 *   COOLDOWN -> IDLE; scare from MANUAL returns to the manual look
 * - CMD_MODE OFF/MANUAL/AUTO, CMD_LIGHT/CMD_EFFECT/CMD_SCARE and
 *   CONFIG_CHANGED per state, with persistence flags and timer handling.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "fsm_core.h"

#define B(pct) ((uint8_t)(((uint32_t)(pct) * 255u) / 100u))

#define TRIGGER_MS 5000u
#define COOLDOWN_MS 2000u
#define T0 1000000LL /* arbitrary nonzero start of the fake clock */

void setUp(void)
{
}

void tearDown(void)
{
}

typedef struct {
    int64_t now_us;
} fake_clock_t;

static int64_t fake_now(void *ctx)
{
    return ((fake_clock_t *)ctx)->now_us;
}

/* Baseline configuration: 5 s trigger, 2 s cooldown, auto mode on,
 * idle "candle" at 30 %, trigger "scare" at 80 %, no cap, warm white. */
static fsm_config_t mk_cfg(bool power_on)
{
    fsm_config_t cfg = {
        .trigger_duration_s = 5,
        .cooldown_s = 2,
        .auto_mode = true,
        .idle_anim = "candle",
        .trigger_anim = "scare",
        .idle_brightness_pct = 30,
        .brightness_pct = 80,
        .max_brightness_pct = 100,
        .color_r = 255,
        .color_g = 60,
        .color_b = 0,
        .power_on = power_on,
    };
    return cfg;
}

/* Alternative configuration used by the CONFIG_CHANGED tests. */
static fsm_config_t mk_cfg2(void)
{
    fsm_config_t cfg = mk_cfg(true);

    strncpy(cfg.idle_anim, "rainbow", sizeof(cfg.idle_anim) - 1);
    strncpy(cfg.trigger_anim, "flicker", sizeof(cfg.trigger_anim) - 1);
    cfg.idle_brightness_pct = 50;
    cfg.brightness_pct = 90;
    cfg.color_r = 0;
    cfg.color_g = 255;
    cfg.color_b = 0;
    return cfg;
}

static fsm_clock_t clk_of(fake_clock_t *fk)
{
    fsm_clock_t clk = { .ctx = fk, .now_us = fake_now };
    return clk;
}

static fsm_core_t mk_core(fake_clock_t *fk, bool power_on)
{
    fsm_core_t f;
    fsm_config_t cfg = mk_cfg(power_on);
    fsm_clock_t clk = clk_of(fk);

    fsm_core_init(&f, &cfg, &clk);
    return f;
}

static fsm_input_t in_presence(bool on)
{
    fsm_input_t in;
    memset(&in, 0, sizeof(in));
    in.type = on ? FSM_IN_PRESENCE_ON : FSM_IN_PRESENCE_OFF;
    return in;
}

static fsm_input_t in_mode(fsm_mode_t mode)
{
    fsm_input_t in;
    memset(&in, 0, sizeof(in));
    in.type = FSM_IN_CMD_MODE;
    in.u.mode = mode;
    return in;
}

static fsm_input_t in_light(uint8_t r, uint8_t g, uint8_t b, uint8_t pct)
{
    fsm_input_t in;
    memset(&in, 0, sizeof(in));
    in.type = FSM_IN_CMD_LIGHT;
    in.u.light.r = r;
    in.u.light.g = g;
    in.u.light.b = b;
    in.u.light.brightness_pct = pct;
    return in;
}

static fsm_input_t in_effect(const char *name)
{
    fsm_input_t in;
    memset(&in, 0, sizeof(in));
    in.type = FSM_IN_CMD_EFFECT;
    snprintf(in.u.anim, sizeof(in.u.anim), "%s", name);
    return in;
}

static fsm_input_t in_scare(void)
{
    fsm_input_t in;
    memset(&in, 0, sizeof(in));
    in.type = FSM_IN_CMD_SCARE;
    return in;
}

static fsm_input_t in_config(const fsm_config_t *cfg)
{
    fsm_input_t in;
    memset(&in, 0, sizeof(in));
    in.type = FSM_IN_CONFIG_CHANGED;
    in.u.cfg = *cfg;
    return in;
}

/* A complete look emission: effect + color + brightness, nothing else. */
static void expect_look(const fsm_output_t *o, const char *anim, uint8_t r,
                        uint8_t g, uint8_t b, uint8_t pct)
{
    TEST_ASSERT_TRUE(o->do_effect);
    TEST_ASSERT_EQUAL_STRING(anim, o->effect);
    TEST_ASSERT_TRUE(o->do_color);
    TEST_ASSERT_EQUAL_UINT8(r, o->r);
    TEST_ASSERT_EQUAL_UINT8(g, o->g);
    TEST_ASSERT_EQUAL_UINT8(b, o->b);
    TEST_ASSERT_TRUE(o->do_brightness);
    TEST_ASSERT_EQUAL_UINT8(B(pct), o->brightness_0_255);
}

/* No side effect at all (no-op input, rejected command, untouched poll). */
static void expect_untouched(const fsm_output_t *o)
{
    TEST_ASSERT_FALSE(o->state_changed);
    TEST_ASSERT_FALSE(o->do_effect);
    TEST_ASSERT_FALSE(o->do_color);
    TEST_ASSERT_FALSE(o->do_brightness);
    TEST_ASSERT_FALSE(o->do_power);
    TEST_ASSERT_FALSE(o->timer_valid);
    TEST_ASSERT_FALSE(o->persist.anim);
    TEST_ASSERT_FALSE(o->persist.bright);
    TEST_ASSERT_FALSE(o->persist.color);
}

/* Drive a booted-on core into the requested state. */
static void go_state(fsm_core_t *f, fake_clock_t *fk, fsm_state_t target,
                     fsm_output_t *out)
{
    fsm_input_t in;

    switch (target) {
    case FSM_STATE_TRIGGERED:
        in = in_presence(true);
        fsm_core_process(f, &in, out);
        break;
    case FSM_STATE_COOLDOWN:
        in = in_presence(true);
        fsm_core_process(f, &in, out);
        fk->now_us += (int64_t)TRIGGER_MS * 1000;
        fsm_core_poll(f, out);
        break;
    case FSM_STATE_MANUAL:
        in = in_mode(FSM_MODE_MANUAL);
        fsm_core_process(f, &in, out);
        break;
    case FSM_STATE_OFF:
        in = in_mode(FSM_MODE_OFF);
        fsm_core_process(f, &in, out);
        break;
    case FSM_STATE_IDLE:
    default:
        break;
    }
}

/* 1. Boot with power_on: rail on + full idle look, no timer, no state
 *    change event (the boot state comes from fsm_core_init). */
static void test_boot_on_emits_power_and_idle_look(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;

    fsm_core_boot(&f, &out);
    TEST_ASSERT_TRUE(out.do_power);
    TEST_ASSERT_TRUE(out.power_on);
    expect_look(&out, "candle", 255, 60, 0, 30);
    TEST_ASSERT_FALSE(out.state_changed);
    TEST_ASSERT_FALSE(out.timer_valid);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.state);
    TEST_ASSERT_FALSE(f.has_deadline);
}

/* 2. Boot with power_on false: rail off and nothing else. */
static void test_boot_off_rail_only(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, false);
    fsm_output_t out;

    fsm_core_boot(&f, &out);
    TEST_ASSERT_TRUE(out.do_power);
    TEST_ASSERT_FALSE(out.power_on);
    TEST_ASSERT_FALSE(out.do_effect);
    TEST_ASSERT_FALSE(out.do_color);
    TEST_ASSERT_FALSE(out.do_brightness);
    TEST_ASSERT_FALSE(out.timer_valid);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_OFF, f.state);
}

/* 3. IDLE + PRESENCE_ON with auto_mode: TRIGGERED, trigger look, duration
 *    timer armed from the current clock instant. */
static void test_presence_on_triggers_look_and_timer(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    fsm_core_boot(&f, &out);
    in = in_presence(true);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, out.prev_state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, out.new_state);
    expect_look(&out, "scare", 255, 60, 0, 80);
    TEST_ASSERT_TRUE(out.timer_valid);
    TEST_ASSERT_EQUAL_UINT32(TRIGGER_MS, out.timer_ms);
    TEST_ASSERT_TRUE(f.has_deadline);
    TEST_ASSERT_EQUAL_INT64(T0 + (int64_t)TRIGGER_MS * 1000, f.deadline_us);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.return_state);
}

/* 4. IDLE + PRESENCE_ON with auto_mode false: no-op. */
static void test_presence_on_ignored_without_auto_mode(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    f.cfg.auto_mode = false;
    fsm_core_boot(&f, &out);
    in = in_presence(true);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.state);
}

/* 5. PRESENCE_OFF is a no-op in every state and never shortens a running
 *    trigger: the COOLDOWN still starts at the original deadline. */
static void test_presence_off_is_noop_and_never_shortens(void)
{
    static const fsm_state_t states[] = {
        FSM_STATE_IDLE, FSM_STATE_TRIGGERED, FSM_STATE_COOLDOWN,
        FSM_STATE_MANUAL, FSM_STATE_OFF,
    };
    size_t i;

    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        fake_clock_t fk = { .now_us = T0 };
        fsm_core_t f = mk_core(&fk, true);
        fsm_output_t out;
        fsm_input_t in;

        fsm_core_boot(&f, &out);
        go_state(&f, &fk, states[i], &out);
        TEST_ASSERT_EQUAL_INT(states[i], f.state);

        in = in_presence(false);
        fsm_core_process(&f, &in, &out);
        TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
        expect_untouched(&out);
        TEST_ASSERT_EQUAL_INT(states[i], f.state);
    }

    /* The running trigger is not shortened: fresh core, presence at T0,
     * a PRESENCE_OFF at T0+1 s, COOLDOWN still only at T0+5 s. */
    {
        fake_clock_t fk = { .now_us = T0 };
        fsm_core_t f = mk_core(&fk, true);
        fsm_output_t out;
        fsm_input_t in = in_presence(true);

        fsm_core_boot(&f, &out);
        fsm_core_process(&f, &in, &out);
        fk.now_us += 1000000;
        in = in_presence(false);
        fsm_core_process(&f, &in, &out);
        expect_untouched(&out);
        fk.now_us += 4000000; /* T0+5 s exactly */
        fsm_core_poll(&f, &out);
        TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, f.state);
    }
}

/* 6. Presence is ignored in MANUAL, OFF and COOLDOWN (no double trigger). */
static void test_presence_ignored_in_manual_off_and_cooldown(void)
{
    static const fsm_state_t states[] = {
        FSM_STATE_MANUAL, FSM_STATE_COOLDOWN, FSM_STATE_OFF,
    };
    size_t i;

    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        fake_clock_t fk = { .now_us = T0 };
        fsm_core_t f = mk_core(&fk, true);
        fsm_output_t out;
        fsm_input_t in;

        fsm_core_boot(&f, &out);
        go_state(&f, &fk, states[i], &out);
        TEST_ASSERT_EQUAL_INT(states[i], f.state);

        in = in_presence(true);
        fsm_core_process(&f, &in, &out);
        TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
        expect_untouched(&out);
        TEST_ASSERT_EQUAL_INT(states[i], f.state);
    }

    /* COOLDOWN specifically: still armed, the deadline is untouched. */
    {
        fake_clock_t fk = { .now_us = T0 };
        fsm_core_t f = mk_core(&fk, true);
        fsm_output_t out;
        int64_t deadline;

        fsm_core_boot(&f, &out);
        go_state(&f, &fk, FSM_STATE_COOLDOWN, &out);
        deadline = f.deadline_us;
        fsm_input_t in = in_presence(true);
        fsm_core_process(&f, &in, &out);
        expect_untouched(&out);
        TEST_ASSERT_TRUE(f.has_deadline);
        TEST_ASSERT_EQUAL_INT64(deadline, f.deadline_us);
    }
}

/* 7. The poll fires strictly at the deadline: one microsecond early is a
 *    no-op, exactly at the deadline TRIGGERED -> COOLDOWN with the
 *    cooldown armed from the same instant. */
static void test_poll_fires_only_at_deadline(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;

    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_TRIGGERED, &out);

    fk.now_us += (int64_t)TRIGGER_MS * 1000 - 1;
    fsm_core_poll(&f, &out);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, f.state);
    TEST_ASSERT_TRUE(f.has_deadline);

    fk.now_us += 1;
    fsm_core_poll(&f, &out);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, out.prev_state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, out.new_state);
    expect_look(&out, "candle", 255, 60, 0, 30);
    TEST_ASSERT_TRUE(out.timer_valid);
    TEST_ASSERT_EQUAL_UINT32(COOLDOWN_MS, out.timer_ms);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.return_state);
    TEST_ASSERT_EQUAL_INT64(T0 + ((int64_t)TRIGGER_MS + COOLDOWN_MS) * 1000,
                            f.deadline_us);
}

/* 8. Cooldown: PRESENCE_ON is ignored, IDLE is reached only at the end,
 *    and a new cycle triggers normally afterwards. */
static void test_cooldown_blocks_retrigger_until_expiry(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_COOLDOWN, &out);

    in = in_presence(true); /* ignored inside the cooldown */
    fsm_core_process(&f, &in, &out);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, f.state);

    fk.now_us += (int64_t)COOLDOWN_MS * 1000 - 1;
    fsm_core_poll(&f, &out);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, f.state);

    fk.now_us += 1;
    fsm_core_poll(&f, &out);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, out.prev_state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, out.new_state);
    expect_look(&out, "candle", 255, 60, 0, 30);
    TEST_ASSERT_TRUE(out.timer_valid);
    TEST_ASSERT_EQUAL_UINT32(0, out.timer_ms); /* cancel */
    TEST_ASSERT_FALSE(f.has_deadline);

    in = in_presence(true); /* fresh cycle */
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, f.state);
    TEST_ASSERT_EQUAL_UINT32(TRIGGER_MS, out.timer_ms);
}

/* 9. cooldown_s == 0: the trigger timeout goes straight to IDLE with a
 *    single state change and no deadline. */
static void test_cooldown_zero_skips_cooldown_state(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;

    f.cfg.cooldown_s = 0;
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_TRIGGERED, &out);

    fk.now_us += (int64_t)TRIGGER_MS * 1000;
    fsm_core_poll(&f, &out);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, out.prev_state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, out.new_state); /* direct */
    expect_look(&out, "candle", 255, 60, 0, 30);
    TEST_ASSERT_TRUE(out.timer_valid);
    TEST_ASSERT_EQUAL_UINT32(0, out.timer_ms);
    TEST_ASSERT_FALSE(f.has_deadline);

    fsm_core_poll(&f, &out); /* stable afterwards */
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.state);
}

/* 10. CMD_MODE OFF from any state: rail off, timer cancelled, deadline
 *     dropped, and afterwards polls never transition. */
static void test_mode_off_from_every_state_cancels_timer(void)
{
    static const fsm_state_t states[] = {
        FSM_STATE_IDLE, FSM_STATE_TRIGGERED, FSM_STATE_COOLDOWN,
        FSM_STATE_MANUAL,
    };
    size_t i;

    for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        fake_clock_t fk = { .now_us = T0 };
        fsm_core_t f = mk_core(&fk, true);
        fsm_output_t out;
        fsm_input_t in;

        fsm_core_boot(&f, &out);
        go_state(&f, &fk, states[i], &out);
        TEST_ASSERT_EQUAL_INT(states[i], f.state);

        in = in_mode(FSM_MODE_OFF);
        fsm_core_process(&f, &in, &out);
        TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
        TEST_ASSERT_TRUE(out.state_changed);
        TEST_ASSERT_EQUAL_INT(states[i], out.prev_state);
        TEST_ASSERT_EQUAL_INT(FSM_STATE_OFF, out.new_state);
        TEST_ASSERT_TRUE(out.do_power);
        TEST_ASSERT_FALSE(out.power_on);
        TEST_ASSERT_TRUE(out.timer_valid);
        TEST_ASSERT_EQUAL_UINT32(0, out.timer_ms);
        TEST_ASSERT_FALSE(f.has_deadline);

        fk.now_us += 60000000; /* long past any deadline */
        fsm_core_poll(&f, &out);
        expect_untouched(&out);
        TEST_ASSERT_EQUAL_INT(FSM_STATE_OFF, f.state);
    }
}

/* 11. OFF twice is a no-op; an unknown mode value is rejected. */
static void test_mode_off_repeated_is_noop(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, false);
    fsm_output_t out;
    fsm_input_t in;

    fsm_core_boot(&f, &out);
    in = in_mode(FSM_MODE_OFF);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_OFF, f.state);

    in = in_mode((fsm_mode_t)99);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, in.result);
    expect_untouched(&out);
}

/* 12. Entering MANUAL seeds the manual look with the look on display:
 *     idle look from IDLE/COOLDOWN/OFF, trigger look from TRIGGERED. */
static void test_mode_manual_seeds_look_of_current_state(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    /* From IDLE */
    fsm_core_boot(&f, &out);
    in = in_mode(FSM_MODE_MANUAL);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, out.prev_state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_MANUAL, out.new_state);
    expect_look(&out, "candle", 255, 60, 0, 30);
    TEST_ASSERT_TRUE(out.do_power);
    TEST_ASSERT_TRUE(out.power_on);
    TEST_ASSERT_TRUE(out.timer_valid);
    TEST_ASSERT_EQUAL_UINT32(0, out.timer_ms);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_MANUAL, f.return_state);
    TEST_ASSERT_EQUAL_STRING("candle", f.manual.anim);
    TEST_ASSERT_EQUAL_UINT8(30, f.manual.brightness_pct);

    /* From TRIGGERED: trigger look is preserved into manual. */
    f = mk_core(&fk, true);
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_TRIGGERED, &out);
    in = in_mode(FSM_MODE_MANUAL);
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "scare", 255, 60, 0, 80);
    TEST_ASSERT_FALSE(f.has_deadline); /* trigger timer cancelled */
    TEST_ASSERT_EQUAL_STRING("scare", f.manual.anim);
    TEST_ASSERT_EQUAL_UINT8(80, f.manual.brightness_pct);

    /* From COOLDOWN */
    f = mk_core(&fk, true);
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_COOLDOWN, &out);
    in = in_mode(FSM_MODE_MANUAL);
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "candle", 255, 60, 0, 30);

    /* From OFF: idle look and rail back on. */
    f = mk_core(&fk, false);
    fsm_core_boot(&f, &out);
    in = in_mode(FSM_MODE_MANUAL);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_OFF, out.prev_state);
    expect_look(&out, "candle", 255, 60, 0, 30);
    TEST_ASSERT_TRUE(out.do_power);
    TEST_ASSERT_TRUE(out.power_on);
}

/* 13. AUTO re-enters IDLE from MANUAL (rail untouched) and from OFF
 *     (rail back on), always with the idle look and no deadline. */
static void test_mode_auto_reenters_idle_from_manual_and_off(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    /* From MANUAL */
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_MANUAL, &out);
    in = in_mode(FSM_MODE_AUTO);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_MANUAL, out.prev_state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, out.new_state);
    expect_look(&out, "candle", 255, 60, 0, 30);
    TEST_ASSERT_FALSE(out.do_power); /* rail was already on */
    TEST_ASSERT_TRUE(out.timer_valid);
    TEST_ASSERT_EQUAL_UINT32(0, out.timer_ms);
    TEST_ASSERT_FALSE(f.has_deadline);

    /* From OFF */
    f = mk_core(&fk, false);
    fsm_core_boot(&f, &out);
    in = in_mode(FSM_MODE_AUTO);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_OFF, out.prev_state);
    expect_look(&out, "candle", 255, 60, 0, 30);
    TEST_ASSERT_TRUE(out.do_power);
    TEST_ASSERT_TRUE(out.power_on);
    TEST_ASSERT_FALSE(f.has_deadline);
}

/* 14. AUTO inside the auto cycle is a no-op and never touches deadlines:
 *     IDLE stays idle, TRIGGERED/COOLDOWN keep running their timer. */
static void test_mode_auto_noop_inside_auto_cycle(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;
    int64_t deadline;

    /* From IDLE */
    fsm_core_boot(&f, &out);
    in = in_mode(FSM_MODE_AUTO);
    fsm_core_process(&f, &in, &out);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.state);

    /* From TRIGGERED: deadline preserved, transition still happens. */
    go_state(&f, &fk, FSM_STATE_TRIGGERED, &out);
    deadline = f.deadline_us;
    in = in_mode(FSM_MODE_AUTO);
    fsm_core_process(&f, &in, &out);
    expect_untouched(&out);
    TEST_ASSERT_TRUE(f.has_deadline);
    TEST_ASSERT_EQUAL_INT64(deadline, f.deadline_us);
    fk.now_us += (int64_t)TRIGGER_MS * 1000;
    fsm_core_poll(&f, &out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, f.state);

    /* From COOLDOWN: deadline preserved until expiry. */
    deadline = f.deadline_us;
    in = in_mode(FSM_MODE_AUTO);
    fsm_core_process(&f, &in, &out);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT64(deadline, f.deadline_us);
    fk.now_us += (int64_t)COOLDOWN_MS * 1000;
    fsm_core_poll(&f, &out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.state);
}

/* 15. Scare in OFF is rejected with no output. */
static void test_scare_rejected_in_off(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, false);
    fsm_output_t out;
    fsm_input_t in;

    fsm_core_boot(&f, &out);
    in = in_scare();
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, in.result);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_OFF, f.state);
    TEST_ASSERT_FALSE(f.has_deadline);
}

/* 16. Scare from IDLE returns to IDLE (via the normal COOLDOWN); scare
 *     from COOLDOWN replaces the cooldown timer with the duration. */
static void test_scare_from_idle_and_cooldown(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    /* From IDLE */
    fsm_core_boot(&f, &out);
    in = in_scare();
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, out.prev_state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, out.new_state);
    expect_look(&out, "scare", 255, 60, 0, 80);
    TEST_ASSERT_EQUAL_UINT32(TRIGGER_MS, out.timer_ms);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.return_state);
    fk.now_us += (int64_t)TRIGGER_MS * 1000;
    fsm_core_poll(&f, &out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, f.state); /* normal path */

    /* From COOLDOWN: the pending cooldown deadline is replaced. */
    f = mk_core(&fk, true);
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_COOLDOWN, &out); /* deadline T0+7 s */
    fk.now_us = T0 + 5500000;
    in = in_scare();
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, f.state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_IDLE, f.return_state);
    TEST_ASSERT_EQUAL_UINT32(TRIGGER_MS, out.timer_ms);
    TEST_ASSERT_EQUAL_INT64(T0 + 10500000, f.deadline_us);

    fk.now_us = T0 + 7000000; /* old cooldown end: nothing happens */
    fsm_core_poll(&f, &out);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, f.state);

    fk.now_us = T0 + 10500000; /* scare duration elapsed */
    fsm_core_poll(&f, &out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, f.state);
    TEST_ASSERT_EQUAL_UINT32(COOLDOWN_MS, out.timer_ms);
}

/* 17. Scare from MANUAL returns to the saved manual look after the
 *     trigger duration, with the timer cancelled. */
static void test_scare_from_manual_returns_to_manual_look(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_MANUAL, &out);
    in = in_light(10, 20, 30, 45); /* shape the manual look */
    fsm_core_process(&f, &in, &out);

    in = in_scare();
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, f.state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_MANUAL, f.return_state);
    expect_look(&out, "scare", 255, 60, 0, 80);
    TEST_ASSERT_EQUAL_UINT32(TRIGGER_MS, out.timer_ms);

    fk.now_us += (int64_t)TRIGGER_MS * 1000;
    fsm_core_poll(&f, &out);
    TEST_ASSERT_TRUE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, out.prev_state);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_MANUAL, out.new_state);
    expect_look(&out, "candle", 10, 20, 30, 45); /* manual look restored */
    TEST_ASSERT_TRUE(out.timer_valid);
    TEST_ASSERT_EQUAL_UINT32(0, out.timer_ms);
    TEST_ASSERT_FALSE(f.has_deadline);

    in = in_presence(true); /* still no auto logic in MANUAL */
    fsm_core_process(&f, &in, &out);
    expect_untouched(&out);
}

/* 18. Scare in TRIGGERED only rearms the duration: the look stays, the
 *     old deadline is forgotten and the new one governs the poll. */
static void test_scare_in_triggered_rearms_duration(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_TRIGGERED, &out); /* deadline T0+5 s */

    fk.now_us += 4900000;
    fsm_core_poll(&f, &out);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, f.state);

    in = in_scare(); /* restart at T0+4.9 s */
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    TEST_ASSERT_FALSE(out.state_changed);
    TEST_ASSERT_FALSE(out.do_effect); /* look untouched */
    TEST_ASSERT_TRUE(out.timer_valid);
    TEST_ASSERT_EQUAL_UINT32(TRIGGER_MS, out.timer_ms);
    TEST_ASSERT_EQUAL_INT64(T0 + 9900000, f.deadline_us);

    fk.now_us += 100000; /* T0+5 s: the old deadline is gone */
    fsm_core_poll(&f, &out);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_TRIGGERED, f.state);

    fk.now_us += 4900000; /* T0+9.9 s */
    fsm_core_poll(&f, &out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, f.state);
}

/* 19. CMD_LIGHT in MANUAL reshapes the manual look only: no persistence,
 *     no state change. */
static void test_light_in_manual_without_persist(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_MANUAL, &out);
    in = in_light(1, 2, 3, 50);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    expect_look(&out, "candle", 1, 2, 3, 50);
    TEST_ASSERT_FALSE(out.persist.anim);
    TEST_ASSERT_FALSE(out.persist.bright);
    TEST_ASSERT_FALSE(out.persist.color);
    TEST_ASSERT_FALSE(out.state_changed);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_MANUAL, f.state);
    TEST_ASSERT_EQUAL_UINT8(50, f.manual.brightness_pct);
}

/* 20. CMD_LIGHT in the auto cycle updates the core config, re-applies the
 *     current look and requests persistence with the state's param:
 *     IDLE/COOLDOWN -> idle brightness, TRIGGERED -> trigger brightness. */
static void test_light_in_auto_updates_cfg_and_persists(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;
    int64_t deadline;

    /* IDLE */
    fsm_core_boot(&f, &out);
    in = in_light(9, 8, 7, 40);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    expect_look(&out, "candle", 9, 8, 7, 40);
    TEST_ASSERT_TRUE(out.persist.color);
    TEST_ASSERT_EQUAL_UINT8(9, out.persist.r);
    TEST_ASSERT_EQUAL_UINT8(8, out.persist.g);
    TEST_ASSERT_EQUAL_UINT8(7, out.persist.b);
    TEST_ASSERT_TRUE(out.persist.bright);
    TEST_ASSERT_EQUAL_INT(FSM_PERSIST_IDLE_BRIGHTNESS, out.persist.bright_param);
    TEST_ASSERT_EQUAL_UINT8(40, out.persist.bright_pct);
    TEST_ASSERT_FALSE(out.persist.anim);
    TEST_ASSERT_FALSE(out.state_changed);
    TEST_ASSERT_FALSE(out.timer_valid);

    /* The new color follows the cycle: trigger look and cooldown look. */
    in = in_presence(true);
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "scare", 9, 8, 7, 80);
    fk.now_us += (int64_t)TRIGGER_MS * 1000;
    fsm_core_poll(&f, &out);
    expect_look(&out, "candle", 9, 8, 7, 40);

    /* COOLDOWN shares the idle param. */
    in = in_light(1, 1, 1, 25);
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "candle", 1, 1, 1, 25);
    TEST_ASSERT_EQUAL_INT(FSM_PERSIST_IDLE_BRIGHTNESS, out.persist.bright_param);

    /* TRIGGERED: trigger param, color persisted, deadline untouched. */
    f = mk_core(&fk, true);
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_TRIGGERED, &out);
    deadline = f.deadline_us;
    in = in_light(5, 5, 5, 70);
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "scare", 5, 5, 5, 70);
    TEST_ASSERT_TRUE(out.persist.bright);
    TEST_ASSERT_EQUAL_INT(FSM_PERSIST_BRIGHTNESS, out.persist.bright_param);
    TEST_ASSERT_EQUAL_UINT8(70, out.persist.bright_pct);
    TEST_ASSERT_FALSE(out.timer_valid);
    TEST_ASSERT_EQUAL_INT64(deadline, f.deadline_us);
}

/* 21. CMD_LIGHT in OFF is rejected. */
static void test_light_rejected_in_off(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, false);
    fsm_output_t out;
    fsm_input_t in;

    fsm_core_boot(&f, &out);
    in = in_light(1, 2, 3, 50);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, in.result);
    expect_untouched(&out);
}

/* 22. Percent clamping to [0,100] and the hard cap max_brightness_pct:
 *     the cap applies at emission (boot look included), the stored pct
 *     keeps the requested clamped value. */
static void test_light_pct_clamped_and_capped_by_max(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    /* Clamp above 100 before anything else. */
    fsm_core_boot(&f, &out);
    in = in_light(0, 0, 0, 200);
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_UINT8(B(100), out.brightness_0_255); /* 255 */
    TEST_ASSERT_EQUAL_UINT8(100, out.persist.bright_pct);

    /* Cap at boot: idle 80 % with max 60 % emits 60 %. */
    f = mk_core(&fk, true);
    f.cfg.idle_brightness_pct = 80;
    f.cfg.max_brightness_pct = 60;
    fsm_core_boot(&f, &out);
    expect_look(&out, "candle", 255, 60, 0, 60);
    TEST_ASSERT_EQUAL_UINT8(B(60), out.brightness_0_255);

    /* Cap on command: 80 % requested, 60 % emitted, 80 % persisted. */
    in = in_light(9, 9, 9, 80);
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "candle", 9, 9, 9, 60);
    TEST_ASSERT_EQUAL_UINT8(80, out.persist.bright_pct);

    /* The trigger look is capped too (color was updated to 9/9/9 above). */
    in = in_presence(true);
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "scare", 9, 9, 9, 60);
}

/* 23. CMD_EFFECT per state: MANUAL shapes the manual look (transient),
 *     TRIGGERED the trigger anim, IDLE/COOLDOWN the idle anim; OFF is
 *     rejected. The persisted anim is immediately visible in the cycle. */
static void test_effect_per_state_and_persist_params(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    /* MANUAL: no persistence. */
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_MANUAL, &out);
    in = in_effect("rainbow");
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    expect_look(&out, "rainbow", 255, 60, 0, 30);
    TEST_ASSERT_FALSE(out.persist.anim);
    TEST_ASSERT_EQUAL_STRING("rainbow", f.manual.anim);

    /* OFF: rejected. */
    f = mk_core(&fk, false);
    fsm_core_boot(&f, &out);
    in = in_effect("rainbow");
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, in.result);
    expect_untouched(&out);

    /* TRIGGERED: trigger anim persisted, deadline untouched. */
    f = mk_core(&fk, true);
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_TRIGGERED, &out);
    in = in_effect("flicker");
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "flicker", 255, 60, 0, 80);
    TEST_ASSERT_TRUE(out.persist.anim);
    TEST_ASSERT_EQUAL_INT(FSM_PERSIST_TRIGGER_ANIM, out.persist.anim_param);
    TEST_ASSERT_EQUAL_STRING("flicker", out.persist.anim_name);
    TEST_ASSERT_FALSE(out.persist.bright);
    TEST_ASSERT_FALSE(out.persist.color);
    TEST_ASSERT_TRUE(f.has_deadline);

    /* IDLE: idle anim persisted, then visible again in COOLDOWN. */
    f = mk_core(&fk, true);
    fsm_core_boot(&f, &out);
    in = in_effect("breathe");
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "breathe", 255, 60, 0, 30);
    TEST_ASSERT_EQUAL_INT(FSM_PERSIST_IDLE_ANIM, out.persist.anim_param);
    go_state(&f, &fk, FSM_STATE_COOLDOWN, &out);
    expect_look(&out, "breathe", 255, 60, 0, 30);
}

/* 24. CONFIG_CHANGED swaps the config and re-applies the current look per
 *     state; in MANUAL the manual look is kept; in OFF no light output
 *     and power_on is not read. */
static void test_config_changed_reapplies_look_per_state(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    /* IDLE */
    fsm_core_boot(&f, &out);
    {
        fsm_config_t cfg2 = mk_cfg2();
        in = in_config(&cfg2);
    }
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    expect_look(&out, "rainbow", 0, 255, 0, 50);
    TEST_ASSERT_FALSE(out.state_changed);
    TEST_ASSERT_FALSE(out.timer_valid);

    /* TRIGGERED: trigger look, timer untouched (checked in test 25). */
    f = mk_core(&fk, true);
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_TRIGGERED, &out);
    {
        fsm_config_t cfg2 = mk_cfg2();
        in = in_config(&cfg2);
    }
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "flicker", 0, 255, 0, 90);
    TEST_ASSERT_TRUE(f.has_deadline);

    /* COOLDOWN: idle look. */
    f = mk_core(&fk, true);
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_COOLDOWN, &out);
    {
        fsm_config_t cfg2 = mk_cfg2();
        in = in_config(&cfg2);
    }
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "rainbow", 0, 255, 0, 50);
    TEST_ASSERT_TRUE(f.has_deadline); /* cooldown keeps running */

    /* MANUAL: manual look preserved, not replaced by the config look. */
    f = mk_core(&fk, true);
    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_MANUAL, &out);
    in = in_light(1, 2, 3, 45);
    fsm_core_process(&f, &in, &out);
    {
        fsm_config_t cfg2 = mk_cfg2();
        in = in_config(&cfg2);
    }
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "candle", 1, 2, 3, 45);
    TEST_ASSERT_FALSE(out.persist.anim);

    /* OFF: nothing at all, power_on ignored. */
    f = mk_core(&fk, false);
    fsm_core_boot(&f, &out);
    {
        fsm_config_t cfg2 = mk_cfg2(); /* power_on true */
        in = in_config(&cfg2);
    }
    fsm_core_process(&f, &in, &out);
    expect_untouched(&out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_OFF, f.state);
}

/* 25. CONFIG_CHANGED in TRIGGERED never restarts the duration: the look is
 *     refreshed but the poll still fires at the original deadline. */
static void test_config_changed_in_triggered_keeps_deadline(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;
    int64_t deadline;
    fsm_config_t cfg2 = mk_cfg2();

    fsm_core_boot(&f, &out);
    go_state(&f, &fk, FSM_STATE_TRIGGERED, &out);
    deadline = f.deadline_us;

    fk.now_us += 2000000;
    in = in_config(&cfg2);
    fsm_core_process(&f, &in, &out);
    expect_look(&out, "flicker", 0, 255, 0, 90);
    TEST_ASSERT_FALSE(out.timer_valid); /* timer untouched */
    TEST_ASSERT_TRUE(f.has_deadline);
    TEST_ASSERT_EQUAL_INT64(deadline, f.deadline_us);

    fk.now_us += 3000000; /* original deadline T0+5 s */
    fsm_core_poll(&f, &out);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_COOLDOWN, f.state);

    /* The new config governs the following idle look as well. */
    fk.now_us += (int64_t)COOLDOWN_MS * 1000;
    fsm_core_poll(&f, &out);
    expect_look(&out, "rainbow", 0, 255, 0, 50);
}

/* 26. Poll without an armed deadline is a no-op, both via fsm_core_poll
 *     and via the FSM_IN_POLL input. */
static void test_poll_without_deadline_is_noop(void)
{
    fake_clock_t fk = { .now_us = T0 };
    fsm_core_t f = mk_core(&fk, true);
    fsm_output_t out;
    fsm_input_t in;

    fsm_core_boot(&f, &out);
    fsm_core_poll(&f, &out);
    expect_untouched(&out);

    in.type = FSM_IN_POLL;
    fsm_core_process(&f, &in, &out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, in.result);
    expect_untouched(&out);

    f = mk_core(&fk, false); /* OFF never has a deadline */
    fsm_core_boot(&f, &out);
    fsm_core_poll(&f, &out);
    expect_untouched(&out);
}

/* 27. Robustness: NULL arguments are safe no-ops; a core initialized
 *     without config/clock boots to OFF (zeroed config) and never
 *     dereferences the clock. */
static void test_null_args_and_missing_clock_are_safe(void)
{
    fsm_core_t f;
    fsm_output_t out;
    fsm_input_t in;
    fsm_config_t cfg = mk_cfg(true);
    fake_clock_t fk = { .now_us = T0 };
    fsm_clock_t clk = clk_of(&fk);

    fsm_core_init(NULL, &cfg, &clk);
    fsm_core_boot(NULL, &out);
    fsm_core_poll(NULL, &out);

    fsm_core_init(&f, NULL, NULL); /* zeroed config: power_on false */
    fsm_core_boot(&f, &out);
    TEST_ASSERT_TRUE(out.do_power);
    TEST_ASSERT_FALSE(out.power_on);
    TEST_ASSERT_EQUAL_INT(FSM_STATE_OFF, f.state);
    fsm_core_poll(&f, &out); /* clock.ctx NULL-guarded */
    expect_untouched(&out);

    in = in_presence(true);
    fsm_core_process(&f, &in, NULL); /* out NULL: no crash */
    fsm_core_process(NULL, &in, &out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_on_emits_power_and_idle_look);
    RUN_TEST(test_boot_off_rail_only);
    RUN_TEST(test_presence_on_triggers_look_and_timer);
    RUN_TEST(test_presence_on_ignored_without_auto_mode);
    RUN_TEST(test_presence_off_is_noop_and_never_shortens);
    RUN_TEST(test_presence_ignored_in_manual_off_and_cooldown);
    RUN_TEST(test_poll_fires_only_at_deadline);
    RUN_TEST(test_cooldown_blocks_retrigger_until_expiry);
    RUN_TEST(test_cooldown_zero_skips_cooldown_state);
    RUN_TEST(test_mode_off_from_every_state_cancels_timer);
    RUN_TEST(test_mode_off_repeated_is_noop);
    RUN_TEST(test_mode_manual_seeds_look_of_current_state);
    RUN_TEST(test_mode_auto_reenters_idle_from_manual_and_off);
    RUN_TEST(test_mode_auto_noop_inside_auto_cycle);
    RUN_TEST(test_scare_rejected_in_off);
    RUN_TEST(test_scare_from_idle_and_cooldown);
    RUN_TEST(test_scare_from_manual_returns_to_manual_look);
    RUN_TEST(test_scare_in_triggered_rearms_duration);
    RUN_TEST(test_light_in_manual_without_persist);
    RUN_TEST(test_light_in_auto_updates_cfg_and_persists);
    RUN_TEST(test_light_rejected_in_off);
    RUN_TEST(test_light_pct_clamped_and_capped_by_max);
    RUN_TEST(test_effect_per_state_and_persist_params);
    RUN_TEST(test_config_changed_reapplies_look_per_state);
    RUN_TEST(test_config_changed_in_triggered_keeps_deadline);
    RUN_TEST(test_poll_without_deadline_is_noop);
    RUN_TEST(test_null_args_and_missing_clock_are_safe);
    return UNITY_END();
}
