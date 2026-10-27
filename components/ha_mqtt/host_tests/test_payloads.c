/* Test host payload HA: discovery, registry, light, event, scalari. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "ha_payloads.h"
#include "ha_registry.h"
#include "cJSON.h"

static const char *const k_anims[] = {
    "solid", "candle", "breathe", "rainbow", "flicker", "scare", "sparkling",
};

void setUp(void)
{
    ha_registry_reset();
}

void tearDown(void)
{
}

/* Helper di accesso: ritorna la stringa o NULL se assente. */
static const char *jstr(const cJSON *obj, const char *name)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(it) && it->valuestring ? it->valuestring : NULL;
}

static cJSON *jobj(const cJSON *obj, const char *name)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsObject(it) ? it : NULL;
}

/* Effect list occhi per il test (6 persistenti, oneshot esclusi). */
static const char *const k_eyes[] = {
    "off", "solid", "breathe", "color_cycle", "blink", "glow",
};

/* Registra le 17 entita' del contratto (12 di Fase 5-7 + 5 occhi). */
static void add_all_entities(void)
{
    ha_entity_desc_t d;
    memset(&d, 0, sizeof d);

    d = (ha_entity_desc_t){ .key = "light", .name = "Luci", .type = HA_ET_LIGHT,
        .retained = true, .has_command = true,
        .options = k_anims, .options_len = 7 };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "presence", .name = "Presenza",
        .type = HA_ET_BINARY_SENSOR };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "distance", .name = "Distanza", .type = HA_ET_SENSOR,
        .unit = "mm", .device_class = "distance" };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "battery_v", .name = "Batteria V", .type = HA_ET_SENSOR,
        .unit = "V", .device_class = "voltage", .category = HA_CAT_DIAGNOSTIC };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "battery_pct", .name = "Batteria %", .type = HA_ET_SENSOR,
        .unit = "%", .device_class = "battery", .category = HA_CAT_DIAGNOSTIC };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "rssi", .name = "WiFi RSSI", .type = HA_ET_SENSOR,
        .unit = "dBm", .device_class = "signal_strength", .category = HA_CAT_DIAGNOSTIC };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "auto", .name = "Auto", .type = HA_ET_SWITCH,
        .retained = true, .has_command = true };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "trigger_mm", .name = "Soglia mm", .type = HA_ET_NUMBER,
        .unit = "mm", .category = HA_CAT_CONFIG, .retained = true, .has_command = true,
        .num_min = 100, .num_max = 10000, .num_step = 50 };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "cooldown_s", .name = "Cooldown s", .type = HA_ET_NUMBER,
        .category = HA_CAT_CONFIG, .retained = true, .has_command = true,
        .num_min = 0, .num_max = 3600, .num_step = 1 };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "trigger_anim", .name = "Animazione trigger",
        .type = HA_ET_SELECT, .category = HA_CAT_CONFIG, .retained = true,
        .has_command = true, .options = k_anims, .options_len = 7 };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "scare", .name = "Spavento", .type = HA_ET_BUTTON,
        .has_command = true };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "update", .name = "Aggiornamento firmware",
        .type = HA_ET_UPDATE, .category = HA_CAT_CONFIG, .retained = true,
        .has_command = true };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "eyes", .name = "Occhi", .type = HA_ET_LIGHT,
        .retained = true, .has_command = true,
        .options = k_eyes, .options_len = 6 };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "eyes_idle_color", .name = "Occhi - colore riposo",
        .type = HA_ET_LIGHT_RGB, .category = HA_CAT_CONFIG,
        .retained = true, .has_command = true };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "eyes_scare_color", .name = "Occhi - colore spavento",
        .type = HA_ET_LIGHT_RGB, .category = HA_CAT_CONFIG,
        .retained = true, .has_command = true };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "eyes_enabled", .name = "Occhi abilitati",
        .type = HA_ET_SWITCH, .retained = true, .has_command = true };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    d = (ha_entity_desc_t){ .key = "eyes_follow_fsm", .name = "Occhi seguono animazione",
        .type = HA_ET_SWITCH, .retained = true, .has_command = true };
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    TEST_ASSERT_EQUAL_UINT(17, ha_registry_count());
}

static void test_discovery(void)
{
    add_all_entities();

    const ha_device_info_t dev = {
        .device_id = "a1b2c3",
        .name = "Zucca pumpkin-a1b2c3",
        .manufacturer = "halloween_pumpkin",
        .model = "ESP32 D1 mini",
        .sw_version = "0.5.0",
        .origin_name = "halloween_pumpkin",
        .origin_sw = "0.5.0",
    };
    char *payload = ha_payload_discovery(&dev, "pumpkin/a1b2c3",
                                         "pumpkin/a1b2c3/availability");
    TEST_ASSERT_NOT_NULL(payload);

    cJSON *root = cJSON_Parse(payload);
    TEST_ASSERT_NOT_NULL(root);

    /* root device / origin / availability */
    cJSON *device = jobj(root, "device");
    TEST_ASSERT_NOT_NULL(device);
    cJSON *identifiers = cJSON_GetObjectItemCaseSensitive(device, "identifiers");
    TEST_ASSERT_TRUE(cJSON_IsArray(identifiers));
    TEST_ASSERT_EQUAL_STRING("pumpkin_a1b2c3",
                             cJSON_GetArrayItem(identifiers, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("Zucca pumpkin-a1b2c3", jstr(device, "name"));
    TEST_ASSERT_EQUAL_STRING("halloween_pumpkin", jstr(device, "manufacturer"));
    TEST_ASSERT_EQUAL_STRING("ESP32 D1 mini", jstr(device, "model"));
    TEST_ASSERT_EQUAL_STRING("0.5.0", jstr(device, "sw_version"));

    cJSON *origin = jobj(root, "origin");
    TEST_ASSERT_NOT_NULL(origin);
    TEST_ASSERT_EQUAL_STRING("halloween_pumpkin", jstr(origin, "name"));
    TEST_ASSERT_EQUAL_STRING("0.5.0", jstr(origin, "sw"));

    cJSON *availability = cJSON_GetObjectItemCaseSensitive(root, "availability");
    TEST_ASSERT_TRUE(cJSON_IsArray(availability));
    cJSON *av0 = cJSON_GetArrayItem(availability, 0);
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/availability", jstr(av0, "topic"));

    /* components */
    cJSON *comps = jobj(root, "components");
    TEST_ASSERT_NOT_NULL(comps);
    TEST_ASSERT_EQUAL_UINT(17, cJSON_GetArraySize(comps));

    cJSON *light = jobj(comps, "light");
    TEST_ASSERT_NOT_NULL(light);
    TEST_ASSERT_EQUAL_STRING("light", jstr(light, "platform"));
    TEST_ASSERT_EQUAL_STRING("json", jstr(light, "schema"));
    TEST_ASSERT_TRUE(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(light, "brightness")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(light, "brightness")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(light, "rgb")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(light, "effect")));
    cJSON *effect_list = cJSON_GetObjectItemCaseSensitive(light, "effect_list");
    TEST_ASSERT_TRUE(cJSON_IsArray(effect_list));
    TEST_ASSERT_EQUAL_UINT(7, cJSON_GetArraySize(effect_list));
    TEST_ASSERT_EQUAL_STRING("Luci", jstr(light, "name"));
    TEST_ASSERT_EQUAL_STRING("solid", cJSON_GetArrayItem(effect_list, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("pumpkin_a1b2c3_light", jstr(light, "unique_id"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/state/light", jstr(light, "state_topic"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/set/light", jstr(light, "command_topic"));
    TEST_ASSERT_NULL(jstr(light, "entity_category"));

    cJSON *presence = jobj(comps, "presence");
    TEST_ASSERT_NOT_NULL(presence);
    TEST_ASSERT_EQUAL_STRING("binary_sensor", jstr(presence, "platform"));
    TEST_ASSERT_EQUAL_STRING("occupancy", jstr(presence, "device_class"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/state/presence", jstr(presence, "state_topic"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(presence, "command_topic"));
    TEST_ASSERT_NULL(jstr(presence, "entity_category"));

    cJSON *distance = jobj(comps, "distance");
    TEST_ASSERT_NOT_NULL(distance);
    TEST_ASSERT_EQUAL_STRING("sensor", jstr(distance, "platform"));
    TEST_ASSERT_EQUAL_STRING("mm", jstr(distance, "unit_of_measurement"));
    TEST_ASSERT_EQUAL_STRING("measurement", jstr(distance, "state_class"));
    TEST_ASSERT_EQUAL_STRING("distance", jstr(distance, "device_class"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(distance, "command_topic"));

    cJSON *battery_pct = jobj(comps, "battery_pct");
    TEST_ASSERT_NOT_NULL(battery_pct);
    TEST_ASSERT_EQUAL_STRING("diagnostic", jstr(battery_pct, "entity_category"));
    TEST_ASSERT_EQUAL_STRING("battery", jstr(battery_pct, "device_class"));

    cJSON *battery_v = jobj(comps, "battery_v");
    TEST_ASSERT_NOT_NULL(battery_v);
    TEST_ASSERT_EQUAL_STRING("voltage", jstr(battery_v, "device_class"));
    TEST_ASSERT_EQUAL_STRING("diagnostic", jstr(battery_v, "entity_category"));

    cJSON *rssi = jobj(comps, "rssi");
    TEST_ASSERT_NOT_NULL(rssi);
    TEST_ASSERT_EQUAL_STRING("signal_strength", jstr(rssi, "device_class"));
    TEST_ASSERT_EQUAL_STRING("diagnostic", jstr(rssi, "entity_category"));

    cJSON *auto_sw = jobj(comps, "auto");
    TEST_ASSERT_NOT_NULL(auto_sw);
    TEST_ASSERT_EQUAL_STRING("switch", jstr(auto_sw, "platform"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/state/auto", jstr(auto_sw, "state_topic"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/set/auto", jstr(auto_sw, "command_topic"));
    TEST_ASSERT_NULL(jstr(auto_sw, "entity_category"));

    cJSON *trigger_mm = jobj(comps, "trigger_mm");
    TEST_ASSERT_NOT_NULL(trigger_mm);
    TEST_ASSERT_EQUAL_STRING("number", jstr(trigger_mm, "platform"));
    TEST_ASSERT_EQUAL_INT(100, (int)cJSON_GetObjectItemCaseSensitive(trigger_mm, "min")->valuedouble);
    TEST_ASSERT_EQUAL_INT(10000, (int)cJSON_GetObjectItemCaseSensitive(trigger_mm, "max")->valuedouble);
    TEST_ASSERT_EQUAL_INT(50, (int)cJSON_GetObjectItemCaseSensitive(trigger_mm, "step")->valuedouble);
    TEST_ASSERT_EQUAL_STRING("box", jstr(trigger_mm, "mode"));
    TEST_ASSERT_EQUAL_STRING("config", jstr(trigger_mm, "entity_category"));
    TEST_ASSERT_EQUAL_STRING("mm", jstr(trigger_mm, "unit_of_measurement"));

    cJSON *cooldown_s = jobj(comps, "cooldown_s");
    TEST_ASSERT_NOT_NULL(cooldown_s);
    TEST_ASSERT_EQUAL_INT(0, (int)cJSON_GetObjectItemCaseSensitive(cooldown_s, "min")->valuedouble);
    TEST_ASSERT_EQUAL_INT(3600, (int)cJSON_GetObjectItemCaseSensitive(cooldown_s, "max")->valuedouble);
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItemCaseSensitive(cooldown_s, "step")->valuedouble);

    cJSON *trigger_anim = jobj(comps, "trigger_anim");
    TEST_ASSERT_NOT_NULL(trigger_anim);
    TEST_ASSERT_EQUAL_STRING("select", jstr(trigger_anim, "platform"));
    cJSON *options = cJSON_GetObjectItemCaseSensitive(trigger_anim, "options");
    TEST_ASSERT_TRUE(cJSON_IsArray(options));
    TEST_ASSERT_EQUAL_UINT(7, cJSON_GetArraySize(options));
    TEST_ASSERT_EQUAL_STRING("config", jstr(trigger_anim, "entity_category"));

    cJSON *scare = jobj(comps, "scare");
    TEST_ASSERT_NOT_NULL(scare);
    TEST_ASSERT_EQUAL_STRING("button", jstr(scare, "platform"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/set/scare", jstr(scare, "command_topic"));
    TEST_ASSERT_EQUAL_STRING("PRESS", jstr(scare, "payload_press"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(scare, "state_topic"));
    TEST_ASSERT_NULL(jstr(scare, "entity_category"));

    /* update: topic dedicati fuori dal pattern state/<key>. */
    cJSON *upd = jobj(comps, "update");
    TEST_ASSERT_NOT_NULL(upd);
    TEST_ASSERT_EQUAL_STRING("update", jstr(upd, "platform"));
    TEST_ASSERT_EQUAL_STRING("Firmware zucca", jstr(upd, "title"));
    TEST_ASSERT_EQUAL_STRING("config", jstr(upd, "entity_category"));
    TEST_ASSERT_EQUAL_STRING("pumpkin_a1b2c3_update", jstr(upd, "unique_id"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/update/state", jstr(upd, "state_topic"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/update/latest",
                             jstr(upd, "latest_version_topic"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/set/update", jstr(upd, "command_topic"));

    /* Fase 8: occhi. "eyes" e' una light completa con effect_list occhi. */
    cJSON *eyes = jobj(comps, "eyes");
    TEST_ASSERT_NOT_NULL(eyes);
    TEST_ASSERT_EQUAL_STRING("Occhi", jstr(eyes, "name"));
    TEST_ASSERT_EQUAL_STRING("light", jstr(eyes, "platform"));
    TEST_ASSERT_EQUAL_STRING("json", jstr(eyes, "schema"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(eyes, "brightness")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(eyes, "rgb")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(eyes, "effect")));
    cJSON *eyes_list = cJSON_GetObjectItemCaseSensitive(eyes, "effect_list");
    TEST_ASSERT_TRUE(cJSON_IsArray(eyes_list));
    TEST_ASSERT_EQUAL_UINT(6, cJSON_GetArraySize(eyes_list));
    TEST_ASSERT_EQUAL_STRING("off", cJSON_GetArrayItem(eyes_list, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("blink", cJSON_GetArrayItem(eyes_list, 4)->valuestring);
    TEST_ASSERT_EQUAL_STRING("glow", cJSON_GetArrayItem(eyes_list, 5)->valuestring);
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/state/eyes", jstr(eyes, "state_topic"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/set/eyes", jstr(eyes, "command_topic"));

    /* I due light colore: schema json + rgb, SENZA brightness/effect/list. */
    cJSON *idle_c = jobj(comps, "eyes_idle_color");
    TEST_ASSERT_NOT_NULL(idle_c);
    TEST_ASSERT_EQUAL_STRING("light", jstr(idle_c, "platform"));
    TEST_ASSERT_EQUAL_STRING("json", jstr(idle_c, "schema"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(idle_c, "rgb")));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(idle_c, "brightness"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(idle_c, "effect"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(idle_c, "effect_list"));
    /* Parametri di configurazione: entity_category config nel payload. */
    TEST_ASSERT_EQUAL_STRING("config", jstr(idle_c, "entity_category"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/state/eyes_idle_color",
                             jstr(idle_c, "state_topic"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/set/eyes_idle_color",
                             jstr(idle_c, "command_topic"));

    cJSON *scare_c = jobj(comps, "eyes_scare_color");
    TEST_ASSERT_NOT_NULL(scare_c);
    TEST_ASSERT_EQUAL_STRING("light", jstr(scare_c, "platform"));
    TEST_ASSERT_EQUAL_STRING("json", jstr(scare_c, "schema"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(scare_c, "rgb")));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(scare_c, "brightness"));
    TEST_ASSERT_EQUAL_STRING("config", jstr(scare_c, "entity_category"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(scare_c, "effect"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(scare_c, "effect_list"));

    cJSON *eyes_en = jobj(comps, "eyes_enabled");
    TEST_ASSERT_NOT_NULL(eyes_en);
    TEST_ASSERT_EQUAL_STRING("switch", jstr(eyes_en, "platform"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/state/eyes_enabled",
                             jstr(eyes_en, "state_topic"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/set/eyes_enabled",
                             jstr(eyes_en, "command_topic"));

    cJSON *eyes_fl = jobj(comps, "eyes_follow_fsm");
    TEST_ASSERT_NOT_NULL(eyes_fl);
    TEST_ASSERT_EQUAL_STRING("switch", jstr(eyes_fl, "platform"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/state/eyes_follow_fsm",
                             jstr(eyes_fl, "state_topic"));
    TEST_ASSERT_EQUAL_STRING("pumpkin/a1b2c3/set/eyes_follow_fsm",
                             jstr(eyes_fl, "command_topic"));

    cJSON_Delete(root);
    free(payload);
}

static void test_discovery_null_args(void)
{
    const ha_device_info_t dev = {
        .device_id = "a1b2c3", .name = "Zucca", .manufacturer = "m", .model = "mm",
        .sw_version = "0.5.0", .origin_name = "o", .origin_sw = "0.5.0",
    };
    TEST_ASSERT_NULL(ha_payload_discovery(NULL, "pumpkin/a1b2c3", "pumpkin/a1b2c3/availability"));
    TEST_ASSERT_NULL(ha_payload_discovery(&dev, NULL, "pumpkin/a1b2c3/availability"));
    TEST_ASSERT_NULL(ha_payload_discovery(&dev, "pumpkin/a1b2c3", NULL));
}

static void test_registry(void)
{
    ha_entity_desc_t d;
    memset(&d, 0, sizeof d);
    d.key = "k", d.name = "n", d.type = HA_ET_SENSOR;

    /* key NULL */
    d.key = NULL;
    TEST_ASSERT_EQUAL_INT(-1, ha_registry_add(&d));
    d.key = "k";
    /* name NULL */
    d.name = NULL;
    TEST_ASSERT_EQUAL_INT(-1, ha_registry_add(&d));
    d.name = "n";
    /* argomento funzione NULL */
    TEST_ASSERT_EQUAL_INT(-1, ha_registry_add(NULL));
    /* tipo fuori range */
    d.type = (ha_entity_type_t)99;
    TEST_ASSERT_EQUAL_INT(-1, ha_registry_add(&d));
    d.type = HA_ET_SENSOR;
    /* key troppo lunga */
    char long_key[HA_KEY_MAX + 2];
    memset(long_key, 'x', sizeof long_key - 1);
    long_key[sizeof long_key - 1] = '\0';
    d.key = long_key;
    TEST_ASSERT_EQUAL_INT(-1, ha_registry_add(&d));
    d.key = "k";
    TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));

    /* riempi fino al limite: 24 add -> la 25a fallisce (HA_ENTITY_MAX) */
    static char keys[HA_ENTITY_MAX + 1][8];
    for (int i = 0; i < (int)HA_ENTITY_MAX + 1; i++) {
        snprintf(keys[i], sizeof keys[i], "f%02d", i);
        d.key = keys[i];
        if (i < (int)HA_ENTITY_MAX - 1)
            TEST_ASSERT_EQUAL_INT(0, ha_registry_add(&d));
        else
            TEST_ASSERT_EQUAL_INT(-1, ha_registry_add(&d));
    }
    TEST_ASSERT_EQUAL_UINT(HA_ENTITY_MAX, ha_registry_count());
    TEST_ASSERT_NOT_NULL(ha_registry_at(HA_ENTITY_MAX - 1));
    TEST_ASSERT_NULL(ha_registry_at(HA_ENTITY_MAX));
    TEST_ASSERT_EQUAL_STRING("k", ha_registry_at(0)->key);

    /* reset */
    TEST_ASSERT_EQUAL_INT(0, ha_registry_reset());
    TEST_ASSERT_EQUAL_UINT(0, ha_registry_count());
    TEST_ASSERT_NULL(ha_registry_at(0));
}

static void test_light_payload(void)
{
    char *s = ha_payload_light(true, true, 128, true, 255, 120, 0, true, "candle");
    TEST_ASSERT_NOT_NULL(s);
    cJSON *o = cJSON_Parse(s);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_STRING("ON", jstr(o, "state"));
    cJSON *b = cJSON_GetObjectItemCaseSensitive(o, "brightness");
    TEST_ASSERT_TRUE(cJSON_IsNumber(b));
    TEST_ASSERT_EQUAL_INT(128, (int)b->valuedouble);
    cJSON *color = jobj(o, "color");
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_EQUAL_INT(255, (int)cJSON_GetObjectItemCaseSensitive(color, "r")->valuedouble);
    TEST_ASSERT_EQUAL_INT(120, (int)cJSON_GetObjectItemCaseSensitive(color, "g")->valuedouble);
    TEST_ASSERT_EQUAL_INT(0, (int)cJSON_GetObjectItemCaseSensitive(color, "b")->valuedouble);
    TEST_ASSERT_EQUAL_STRING("candle", jstr(o, "effect"));
    cJSON_Delete(o);
    free(s);

    /* solo state: payload esatto */
    s = ha_payload_light(false, false, 0, false, 0, 0, 0, false, NULL);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"OFF\"}", s);
    free(s);
}

static void test_light_escape(void)
{
    char *s = ha_payload_light(true, false, 0, false, 0, 0, 0, true, "a\"b\\c");
    TEST_ASSERT_NOT_NULL(s);
    cJSON *o = cJSON_Parse(s);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_STRING("a\"b\\c", jstr(o, "effect"));
    cJSON_Delete(o);
    free(s);
}

static void test_event_trigger(void)
{
    char *s = ha_payload_event_trigger(1, 1234567, 950, "scare");
    TEST_ASSERT_NOT_NULL(s);
    cJSON *o = cJSON_Parse(s);
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_INT(1, (int)cJSON_GetObjectItemCaseSensitive(o, "v")->valuedouble);
    TEST_ASSERT_EQUAL_INT64(1234567,
        (int64_t)cJSON_GetObjectItemCaseSensitive(o, "ts")->valuedouble);
    TEST_ASSERT_EQUAL_INT(950,
        (int)cJSON_GetObjectItemCaseSensitive(o, "distance_mm")->valuedouble);
    TEST_ASSERT_EQUAL_STRING("scare", jstr(o, "anim"));
    cJSON_Delete(o);
    free(s);
}

static void test_scalars(void)
{
    TEST_ASSERT_EQUAL_STRING("ON", ha_payload_onoff(true));
    TEST_ASSERT_EQUAL_STRING("OFF", ha_payload_onoff(false));

    char *s = ha_payload_i32(-42);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("-42", s);
    free(s);

    s = ha_payload_u32(4294967295u);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("4294967295", s);
    free(s);

    s = ha_payload_f32(3.97f, 2);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("3.97", s);
    free(s);

    s = ha_payload_text("x");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("x", s);
    free(s);

    TEST_ASSERT_NULL(ha_payload_text(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_discovery);
    RUN_TEST(test_discovery_null_args);
    RUN_TEST(test_registry);
    RUN_TEST(test_light_payload);
    RUN_TEST(test_light_escape);
    RUN_TEST(test_event_trigger);
    RUN_TEST(test_scalars);
    return UNITY_END();
}
