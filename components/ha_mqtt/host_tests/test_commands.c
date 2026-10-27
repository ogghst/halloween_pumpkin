/* Test host parsing comandi HA. */
#include <string.h>
#include "unity.h"
#include "ha_commands.h"

static const char *const k_anims[] = {
    "solid", "candle", "breathe", "rainbow", "flicker", "scare",
};

/* I 5 effetti occhi persistenti (come li passa ha_entities: oneshot esclusi). */
static const char *const k_eyes[] = {
    "off", "solid", "breathe", "color_cycle", "blink",
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

/* Come P ma con le opzioni occhi (persistenti). */
static ha_cmd_result_t PE(const char *key, const char *payload)
{
    return ha_cmd_parse(key, payload, strlen(payload), k_eyes, 5,
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

static void test_eyes_light(void)
{
    /* Stesso schema JSON di light, opzioni occhi. */
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes",
        "{\"state\":\"ON\",\"brightness\":200,\"color\":{\"r\":255,\"g\":40,\"b\":0},"
        "\"effect\":\"color_cycle\"}"));
    TEST_ASSERT_TRUE(s_cmd.light_on);
    TEST_ASSERT_TRUE(s_cmd.has_brightness);
    TEST_ASSERT_EQUAL_INT(200, s_cmd.brightness);
    TEST_ASSERT_TRUE(s_cmd.has_color);
    TEST_ASSERT_EQUAL_INT(255, s_cmd.r);
    TEST_ASSERT_EQUAL_INT(40, s_cmd.g);
    TEST_ASSERT_EQUAL_INT(0, s_cmd.b);
    TEST_ASSERT_TRUE(s_cmd.has_effect);
    TEST_ASSERT_EQUAL_STRING("color_cycle", s_cmd.effect);

    /* solo state */
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes", "{\"state\":\"OFF\"}"));
    TEST_ASSERT_FALSE(s_cmd.light_on);
    TEST_ASSERT_FALSE(s_cmd.has_brightness);
    TEST_ASSERT_FALSE(s_cmd.has_color);
    TEST_ASSERT_FALSE(s_cmd.has_effect);

    /* brightness fuori range */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes", "{\"state\":\"ON\",\"brightness\":256}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes", "{\"state\":\"ON\",\"brightness\":-1}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes", "{\"state\":\"ON\",\"brightness\":0}"));
    TEST_ASSERT_EQUAL_INT(0, s_cmd.brightness);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes", "{\"state\":\"ON\",\"brightness\":255}"));
    TEST_ASSERT_EQUAL_INT(255, s_cmd.brightness);

    /* effect fuori lista: oneshot ("flash") scartato come i nomi ignoti */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes", "{\"state\":\"ON\",\"effect\":\"flash\"}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes", "{\"state\":\"ON\",\"effect\":\"glare\"}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes", "{\"state\":\"ON\",\"effect\":\"candle\"}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes", "{\"state\":\"ON\",\"effect\":\"pippo\"}"));

    /* JSON malformato / non oggetto / senza state */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes", "{\"state\":"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes", "[]"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes", "{\"brightness\":5}"));
}

static void test_eyes_color_keys(void)
{
    /* state e color opzionali, anche insieme; "{}" = nessuna modifica. */
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes_idle_color", "{\"state\":\"OFF\"}"));
    TEST_ASSERT_TRUE(s_cmd.has_state);
    TEST_ASSERT_FALSE(s_cmd.onoff);
    TEST_ASSERT_FALSE(s_cmd.has_color);

    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes_scare_color",
        "{\"state\":\"ON\",\"color\":{\"r\":10,\"g\":20,\"b\":30}}"));
    TEST_ASSERT_TRUE(s_cmd.has_state);
    TEST_ASSERT_TRUE(s_cmd.onoff);
    TEST_ASSERT_TRUE(s_cmd.has_color);
    TEST_ASSERT_EQUAL_INT(10, s_cmd.r);
    TEST_ASSERT_EQUAL_INT(20, s_cmd.g);
    TEST_ASSERT_EQUAL_INT(30, s_cmd.b);

    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes_idle_color", "{\"color\":{\"r\":0,\"g\":0,\"b\":0}}"));
    TEST_ASSERT_FALSE(s_cmd.has_state);
    TEST_ASSERT_TRUE(s_cmd.has_color);

    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes_scare_color", "{}"));
    TEST_ASSERT_FALSE(s_cmd.has_state);
    TEST_ASSERT_FALSE(s_cmd.has_color);

    /* scarti */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes_idle_color", "{\"color\":{\"r\":256,\"g\":0,\"b\":0}}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes_idle_color", "{\"color\":{\"r\":1,\"g\":2}}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes_idle_color", "{\"state\":\"on\"}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes_scare_color", "{\"state\":\"MAYBE\"}"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes_scare_color", ""));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, PE("eyes_scare_color", "ON"));
}

static void test_eyes_switches(void)
{
    /* Stesso parse di auto: payload "ON"/"OFF" esatti. */
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("eyes_enabled", "ON"));
    TEST_ASSERT_TRUE(s_cmd.onoff);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("eyes_enabled", "OFF"));
    TEST_ASSERT_FALSE(s_cmd.onoff);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("eyes_follow_fsm", "ON"));
    TEST_ASSERT_TRUE(s_cmd.onoff);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("eyes_follow_fsm", "OFF"));
    TEST_ASSERT_FALSE(s_cmd.onoff);
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("eyes_enabled", "on"));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("eyes_follow_fsm", "ON "));
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("eyes_enabled", ""));
    TEST_ASSERT_NOT_EQUAL(0, s_err[0]);
}

static void test_target_dispatch(void)
{
    /* Il dispatch di ha_entities commuta su cmd.target: deve essere impostato
     * per OGNI chiave valida, altrimenti il comando finisce nel default
     * silenzioso (bug di banco 2026-10-26: light muta da Home Assistant). */
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("light", "{\"state\":\"ON\"}"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_LIGHT, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("auto", "ON"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_AUTO, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("trigger_mm", "1200"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_TRIGGER_MM, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("cooldown_s", "30"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_COOLDOWN_S, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("trigger_anim", "flicker"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_TRIGGER_ANIM, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("scare", "PRESS"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_SCARE, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes", "{\"state\":\"ON\"}"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_EYES, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes_idle_color", "{\"color\":{\"r\":1,\"g\":2,\"b\":3}}"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_EYES_IDLE_COLOR, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, PE("eyes_scare_color", "{\"state\":\"OFF\"}"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_EYES_SCARE_COLOR, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("eyes_enabled", "ON"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_EYES_ENABLED, s_cmd.target);
    TEST_ASSERT_EQUAL_INT(HA_CMD_OK, P("eyes_follow_fsm", "OFF"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_EYES_FOLLOW, s_cmd.target);

    /* Su IGNORED il chiamante non legge target (contratto: significativo solo
     * su HA_CMD_OK); per chiavi sconosciute resta NONE. */
    TEST_ASSERT_EQUAL_INT(HA_CMD_IGNORED, P("foo", "ON"));
    TEST_ASSERT_EQUAL_INT(HA_TARGET_NONE, s_cmd.target);
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
    RUN_TEST(test_eyes_light);
    RUN_TEST(test_eyes_color_keys);
    RUN_TEST(test_eyes_switches);
    RUN_TEST(test_target_dispatch);
    RUN_TEST(test_keys_and_errors);
    RUN_TEST(test_truncated_len);
    return UNITY_END();
}
