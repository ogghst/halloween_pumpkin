/* Host unit tests per eyes_ctrl_core.c (Unity, build nativa).
 * Solo il core puro: il layer IDF (eventi, led_fx) si verifica sul target.
 * pumpkin_fsm.h e config_store.h sono gli header REALI del repo; led_fx.h
 * e' la doppia di test in stub/ (il contratto occhi non e' ancora
 * atterrato in led_effects). */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "eyes_ctrl_core.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* Configurazione occhi valida e completa (tutti i gate aperti). */
static pumpkin_config_t mk_cfg(void)
{
    pumpkin_config_t c;
    memset(&c, 0, sizeof(c));
    c.trigger_duration_s = 8;
    c.eyes_enabled = true;
    c.eyes_follow_fsm = true;
    c.eyes_idle_r = 255;
    c.eyes_idle_g = 64;
    c.eyes_idle_b = 0;
    memcpy(c.eyes_idle_effect, "candle", 7);
    c.eyes_idle_brightness_pct = 40;
    c.eyes_scare_r = 0;
    c.eyes_scare_g = 255;
    c.eyes_scare_b = 0;
    c.eyes_scare_brightness_pct = 100;
    return c;
}

/* Riempie il piano con un pattern sentinella: dopo un rifiuto il contenuto
 * deve restare identico byte per byte ("non toccare gli occhi"). */
static void fill_sentinel(led_fx_eyes_plan_t *p)
{
    memset(p, 0xA5, sizeof(*p));
}

static bool plan_is_sentinel(const led_fx_eyes_plan_t *p)
{
    led_fx_eyes_plan_t ref;
    fill_sentinel(&ref);
    return memcmp(&ref, p, sizeof(*p)) == 0;
}

/* 1. pct -> 0..255 con arrotondamento (10 % -> 26 come da contratto). */
void test_pct_to_255_rounding(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, eyes_pct_to_255(0));
    TEST_ASSERT_EQUAL_UINT8(3, eyes_pct_to_255(1));
    TEST_ASSERT_EQUAL_UINT8(26, eyes_pct_to_255(10));
    TEST_ASSERT_EQUAL_UINT8(102, eyes_pct_to_255(40));
    TEST_ASSERT_EQUAL_UINT8(128, eyes_pct_to_255(50));
    TEST_ASSERT_EQUAL_UINT8(252, eyes_pct_to_255(99));
    TEST_ASSERT_EQUAL_UINT8(255, eyes_pct_to_255(100));
}

/* 2. Gate "segui la FSM": tabella di verita' completa. */
void test_follow_allowed_truth_table(void)
{
    pumpkin_config_t c = mk_cfg();

    TEST_ASSERT_FALSE(eyes_follow_allowed(NULL));

    c.eyes_enabled = false;
    TEST_ASSERT_FALSE(eyes_follow_allowed(&c));

    c = mk_cfg();
    c.eyes_follow_fsm = false;
    TEST_ASSERT_FALSE(eyes_follow_allowed(&c));

    c = mk_cfg();
    TEST_ASSERT_TRUE(eyes_follow_allowed(&c));
}

/* 3. IDLE: look di riposo completo, nessun campo scare/duration. */
void test_plan_idle_uses_idle_look(void)
{
    pumpkin_config_t c = mk_cfg();
    led_fx_eyes_plan_t plan;

    fill_sentinel(&plan);
    TEST_ASSERT_TRUE(eyes_plan_build(FSM_STATE_IDLE, &c, &plan));
    TEST_ASSERT_EQUAL_INT(LED_FX_EYES_PLAN_IDLE, plan.kind);
    TEST_ASSERT_EQUAL_UINT8(255, plan.idle_r);
    TEST_ASSERT_EQUAL_UINT8(64, plan.idle_g);
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_b);
    TEST_ASSERT_EQUAL_STRING("candle", plan.idle_effect);
    TEST_ASSERT_EQUAL_UINT8(102, plan.idle_brightness); /* 40 % -> 102 */
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_r);
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_g);
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_b);
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_brightness);
    TEST_ASSERT_EQUAL_UINT32(0, plan.duration_ms);
}

/* 4. TRIGGERED: look di spavento e duration_ms da trigger_duration_s. */
void test_plan_triggered_scare_and_duration(void)
{
    pumpkin_config_t c = mk_cfg();
    led_fx_eyes_plan_t plan;

    TEST_ASSERT_TRUE(eyes_plan_build(FSM_STATE_TRIGGERED, &c, &plan));
    TEST_ASSERT_EQUAL_INT(LED_FX_EYES_PLAN_TRIGGERED, plan.kind);
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_r);
    TEST_ASSERT_EQUAL_UINT8(255, plan.scare_g);
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_b);
    TEST_ASSERT_EQUAL_UINT8(255, plan.scare_brightness); /* 100 % -> 255 */
    TEST_ASSERT_EQUAL_UINT32(8000, plan.duration_ms);    /* 8 s -> 8000 ms */
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_r);
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_g);
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_b);
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_brightness);
}

/* 5. COOLDOWN: stesso look di riposo dell'IDLE, kind dedicato. */
void test_plan_cooldown_same_look_as_idle(void)
{
    pumpkin_config_t c = mk_cfg();
    led_fx_eyes_plan_t idle, cooldown;

    TEST_ASSERT_TRUE(eyes_plan_build(FSM_STATE_IDLE, &c, &idle));
    TEST_ASSERT_TRUE(eyes_plan_build(FSM_STATE_COOLDOWN, &c, &cooldown));
    TEST_ASSERT_EQUAL_INT(LED_FX_EYES_PLAN_COOLDOWN, cooldown.kind);
    TEST_ASSERT_EQUAL_UINT8(idle.idle_r, cooldown.idle_r);
    TEST_ASSERT_EQUAL_UINT8(idle.idle_g, cooldown.idle_g);
    TEST_ASSERT_EQUAL_UINT8(idle.idle_b, cooldown.idle_b);
    TEST_ASSERT_EQUAL_UINT8(idle.idle_brightness, cooldown.idle_brightness);
    TEST_ASSERT_EQUAL_STRING(idle.idle_effect, cooldown.idle_effect);
    TEST_ASSERT_EQUAL_UINT32(0, cooldown.duration_ms);
}

/* 6. OFF: piano azzerato, solo il kind e' significativo. */
void test_plan_off_is_zeroed(void)
{
    pumpkin_config_t c = mk_cfg();
    led_fx_eyes_plan_t plan;

    fill_sentinel(&plan);
    TEST_ASSERT_TRUE(eyes_plan_build(FSM_STATE_OFF, &c, &plan));
    TEST_ASSERT_EQUAL_INT(LED_FX_EYES_PLAN_OFF, plan.kind);
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_r);
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_g);
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_b);
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_brightness);
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_r);
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_g);
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_b);
    TEST_ASSERT_EQUAL_UINT8(0, plan.scare_brightness);
    TEST_ASSERT_EQUAL_UINT32(0, plan.duration_ms);
    TEST_ASSERT_EQUAL_UINT8(0, plan.idle_effect[0]);
}

/* 7. Rifiuti: false e *out byte per byte invariato. */
void test_plan_rejected_leaves_out_untouched(void)
{
    pumpkin_config_t c = mk_cfg();
    led_fx_eyes_plan_t plan;

    fill_sentinel(&plan);
    TEST_ASSERT_FALSE(eyes_plan_build(FSM_STATE_MANUAL, &c, &plan));
    TEST_ASSERT_TRUE(plan_is_sentinel(&plan));

    c = mk_cfg();
    c.eyes_enabled = false;
    TEST_ASSERT_FALSE(eyes_plan_build(FSM_STATE_IDLE, &c, &plan));
    TEST_ASSERT_TRUE(plan_is_sentinel(&plan));

    c = mk_cfg();
    c.eyes_follow_fsm = false;
    TEST_ASSERT_FALSE(eyes_plan_build(FSM_STATE_TRIGGERED, &c, &plan));
    TEST_ASSERT_TRUE(plan_is_sentinel(&plan));

    TEST_ASSERT_FALSE(eyes_plan_build((fsm_state_t)99, &c, &plan));
    TEST_ASSERT_TRUE(plan_is_sentinel(&plan));

    TEST_ASSERT_FALSE(eyes_plan_build(FSM_STATE_IDLE, NULL, &plan));
    TEST_ASSERT_TRUE(plan_is_sentinel(&plan));

    TEST_ASSERT_FALSE(eyes_plan_build(FSM_STATE_IDLE, &c, NULL));
}

/* 8. Effetto di riposo sanitizzato: vuoto o >= 16 caratteri -> "breathe";
 * 15 caratteri esatti resta tale, con NUL garantito in coda. */
void test_idle_effect_sanitized(void)
{
    pumpkin_config_t c = mk_cfg();
    led_fx_eyes_plan_t plan;

    c.eyes_idle_effect[0] = '\0'; /* vuoto */
    TEST_ASSERT_TRUE(eyes_plan_build(FSM_STATE_IDLE, &c, &plan));
    TEST_ASSERT_EQUAL_STRING("breathe", plan.idle_effect);

    memcpy(c.eyes_idle_effect, "abcdefghijklmnop",
           sizeof(c.eyes_idle_effect)); /* 16 char, buffer pieno */
    TEST_ASSERT_TRUE(eyes_plan_build(FSM_STATE_IDLE, &c, &plan));
    TEST_ASSERT_EQUAL_STRING("breathe", plan.idle_effect);

    memset(c.eyes_idle_effect, 'x', sizeof(c.eyes_idle_effect)); /* non terminato */
    TEST_ASSERT_TRUE(eyes_plan_build(FSM_STATE_IDLE, &c, &plan));
    TEST_ASSERT_EQUAL_STRING("breathe", plan.idle_effect);

    memcpy(c.eyes_idle_effect, "abcdefghijklmno", 16); /* 15 char + NUL */
    TEST_ASSERT_TRUE(eyes_plan_build(FSM_STATE_IDLE, &c, &plan));
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmno", plan.idle_effect);
    TEST_ASSERT_EQUAL_UINT8(15, strlen(plan.idle_effect));
    TEST_ASSERT_EQUAL_UINT8('\0', plan.idle_effect[15]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pct_to_255_rounding);
    RUN_TEST(test_follow_allowed_truth_table);
    RUN_TEST(test_plan_idle_uses_idle_look);
    RUN_TEST(test_plan_triggered_scare_and_duration);
    RUN_TEST(test_plan_cooldown_same_look_as_idle);
    RUN_TEST(test_plan_off_is_zeroed);
    RUN_TEST(test_plan_rejected_leaves_out_untouched);
    RUN_TEST(test_idle_effect_sanitized);
    return UNITY_END();
}
