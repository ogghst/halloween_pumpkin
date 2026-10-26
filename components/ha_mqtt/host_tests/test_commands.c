/* Test host parsing comandi HA. */
#include <string.h>
#include "unity.h"
#include "ha_commands.h"

static const char *const k_anims[] = {
    "solid", "candle", "breathe", "rainbow", "flicker", "scare",
};

static ha_cmd_t s_cmd;
static char s_err[128];

void setUp(void)
{
    memset(&s_cmd, 0, sizeof s_cmd);
    s_err[0] = '\0';
}

void tearDown(void)
{
}

/* Parse con payload NUL-terminato (len = strlen). */
static ha_cmd_result_t P(const char *key, const char *payload)
{
    return ha_cmd_parse(key, payload, strlen(payload), k_anims, 6,
                        &s_cmd, s_err, sizeof s_err);
}

static void test_light_full(void)
{
    ha_cmd_result_t r = P("light",
        "{\"state\":\"ON\",\"brightness\":128,\"color\":{\"r\":255,\"g\":120,\"b\":0},"
        "\"effect\":\"candle\",\"unknown\":123}");
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, r);
    TEST_ASSERT_TRUE(s_cmd.light_on);
    TEST_ASSERT_TRUE(s_cmd.has_brightness);
    TEST_ASSERT_EQUAL_INT(128, s_cmd.brightness);
    TEST_ASSERT_TRUE(s_cmd.has_color);
    TEST_ASSERT_EQUAL_INT(255, s_cmd.r);
    TEST_ASSERT_EQUAL_INT(120, s_cmd.g);
    TEST_ASSERT_EQUAL_INT(0, s_cmd.b);
    TEST_ASSERT_TRUE(s_cmd.has_effect);
    TEST_ASSERT_EQUAL_STRING("candle", s_cmd.effect);
    TEST_ASSERT_EQUAL_INT(0, s_err[0]); /* nessun motivo su OK */
}

static void test_light_state_only(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("light", "{\"state\":\"OFF\"}"));
    TEST_ASSERT_FALSE(s_cmd.light_on);
    TEST_ASSERT_FALSE(s_cmd.has_brightness);
    TEST_ASSERT_FALSE(s_cmd.has_color);
    TEST_ASSERT_FALSE(s_cmd.has_effect);
}

static void test_light_malformed(void)
{
    /* JSON malformato */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", "{\"state\":"));
    TEST_ASSERT_NOT_EQUAL(0, s_err[0]);
    /* non-oggetto (array) */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", "[\"state\"]"));
    /* senza state */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", "{\"brightness\":5}"));
    /* state minuscolo (case sensitive) */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", "{\"state\":\"on\"}"));
    /* payload vuoto */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", ""));
    /* numerico puro: JSON valido ma non oggetto */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", "1200"));
}

static void test_light_brightness(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", "{\"state\":\"ON\",\"brightness\":256}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", "{\"state\":\"ON\",\"brightness\":-1}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", "{\"state\":\"ON\",\"brightness\":12.5}"));

    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("light", "{\"state\":\"ON\",\"brightness\":0}"));
    TEST_ASSERT_TRUE(s_cmd.has_brightness);
    TEST_ASSERT_EQUAL_INT(0, s_cmd.brightness);

    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("light", "{\"state\":\"ON\",\"brightness\":255}"));
    TEST_ASSERT_EQUAL_INT(255, s_cmd.brightness);
}

static void test_light_color(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED,
        P("light", "{\"state\":\"ON\",\"color\":{\"r\":300,\"g\":0,\"b\":0}}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED,
        P("light", "{\"state\":\"ON\",\"color\":{\"r\":1,\"g\":2}}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK,
        P("light", "{\"state\":\"ON\",\"color\":{\"r\":0,\"g\":128,\"b\":255}}"));
    TEST_ASSERT_TRUE(s_cmd.has_color);
    TEST_ASSERT_EQUAL_INT(0, s_cmd.r);
    TEST_ASSERT_EQUAL_INT(128, s_cmd.g);
    TEST_ASSERT_EQUAL_INT(255, s_cmd.b);
}

static void test_light_effect(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("light", "{\"state\":\"ON\",\"effect\":\"pippo\"}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("light", "{\"state\":\"ON\",\"effect\":\"scare\"}"));
    TEST_ASSERT_TRUE(s_cmd.has_effect);
    TEST_ASSERT_EQUAL_STRING("scare", s_cmd.effect);
}

static void test_auto(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("auto", "ON"));
    TEST_ASSERT_TRUE(s_cmd.onoff);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("auto", "OFF"));
    TEST_ASSERT_FALSE(s_cmd.onoff);
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("auto", "on"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("auto", ""));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("auto", "ON "));
    TEST_ASSERT_NOT_EQUAL(0, s_err[0]);
}

static void test_trigger_mm(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("trigger_mm", "1200"));
    TEST_ASSERT_EQUAL_INT(1200, s_cmd.number);
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("trigger_mm", "99"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("trigger_mm", "10001"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("trigger_mm", "100"));
    TEST_ASSERT_EQUAL_INT(100, s_cmd.number);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("trigger_mm", "10000"));
    TEST_ASSERT_EQUAL_INT(10000, s_cmd.number);
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("trigger_mm", "abc"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("trigger_mm", "12.5"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("trigger_mm", "99999999999"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("trigger_mm", ""));
    /* messaggio di range con i valori */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("trigger_mm", "42"));
    TEST_ASSERT_NOT_EQUAL(0, s_err[0]);
}

static void test_cooldown_s(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("cooldown_s", "0"));
    TEST_ASSERT_EQUAL_INT(0, s_cmd.number);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("cooldown_s", "3600"));
    TEST_ASSERT_EQUAL_INT(3600, s_cmd.number);
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("cooldown_s", "3601"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("cooldown_s", "-1"));
}

static void test_trigger_anim(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("trigger_anim", "flicker"));
    TEST_ASSERT_EQUAL_STRING("flicker", s_cmd.anim);
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("trigger_anim", "nope"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("trigger_anim", ""));
}

static void test_scare(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("scare", "PRESS"));
    TEST_ASSERT_TRUE(s_cmd.onoff);
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("scare", "press"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("scare", "PRESSS"));
}

static void test_keys_and_errors(void)
{
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("foo", "ON"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_ERROR,
        ha_cmd_parse(NULL, "ON", 2, k_anims, 6, &s_cmd, s_err, sizeof s_err));
    TEST_ASSERT_EQUAL_INT(HA_CMD_ERROR,
        ha_cmd_parse("auto", NULL, 2, k_anims, 6, &s_cmd, s_err, sizeof s_err));
    /* out NULL: argomento invalido */
    TEST_ASSERT_EQUAL_INT(HA_CMD_ERROR,
        ha_cmd_parse("auto", "ON", 2, k_anims, 6, NULL, s_err, sizeof s_err));
}

static void test_truncated_len(void)
{
    /* il parse usa len, non assume NUL: "1200garbage" con len 4 -> 1200 */
    ha_cmd_result_t r = ha_cmd_parse("trigger_mm", "1200garbage", 4, k_anims, 6,
                                     &s_cmd, s_err, sizeof s_err);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, r);
    TEST_ASSERT_EQUAL_INT(1200, s_cmd.number);

    /* stessa stringa con len pieno -> garbage in coda -> IGNORED */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED,
        ha_cmd_parse("trigger_mm", "1200garbage", 11, k_anims, 6,
                     &s_cmd, s_err, sizeof s_err));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_light_full);
    RUN_TEST(test_light_state_only);
    RUN_TEST(test_light_malformed);
    RUN_TEST(test_light_brightness);
    RUN_TEST(test_light_color);
    RUN_TEST(test_light_effect);
    RUN_TEST(test_auto);
    RUN_TEST(test_trigger_mm);
    RUN_TEST(test_cooldown_s);
    RUN_TEST(test_trigger_anim);
    RUN_TEST(test_scare);
    RUN_TEST(test_keys_and_errors);
    RUN_TEST(test_truncated_len);
    return UNITY_END();
}
