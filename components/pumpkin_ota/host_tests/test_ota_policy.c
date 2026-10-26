/* Test host politica OTA (pura): gate URL/batteria/LED/heap, confronto
 * versioni, evento JSON. Nessuna dipendenza ESP-IDF. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "ota_policy.h"

#define MIN_HEAP 65536u

void setUp(void)
{
}

void tearDown(void)
{
}

/* Input nominale che passa tutti i gate. */
static pumpkin_ota_policy_input_t nominal(void)
{
    pumpkin_ota_policy_input_t in = {
        .url = "https://example.com/pumpkin.bin",
        .battery_pct = 80,
        .led_current_ma = 100,
        .free_heap = 200000,
        .max_block = 160000,
        .min_free_heap = MIN_HEAP,
        .max_led_ma = 500,
        .url_max = PUMPKIN_OTA_URL_MAX,
        .warn_battery_unknown = false,
    };
    return in;
}

static void test_nominal_allow(void)
{
    pumpkin_ota_policy_input_t in = nominal();
    char reason[24] = "?";
    TEST_ASSERT_EQUAL_INT(0, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
    TEST_ASSERT_FALSE(in.warn_battery_unknown);
}

static void test_url_gate(void)
{
    char reason[24];
    pumpkin_ota_policy_input_t in;

    /* http:// rifiutato */
    in = nominal();
    in.url = "http://example.com/pumpkin.bin";
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("bad_url", reason);

    /* URL assente */
    in = nominal();
    in.url = NULL;
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("bad_url", reason);

    /* URL vuota */
    in = nominal();
    in.url = "";
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));

    /* URL troppo lunga (url_max include il NUL) */
    static char long_url[PUMPKIN_OTA_URL_MAX + 1];
    memset(long_url, 'a', sizeof long_url - 1);
    memcpy(long_url, "https://", 8);
    long_url[sizeof long_url - 1] = '\0';
    in = nominal();
    in.url = long_url;
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));

    /* esattamente al limite: consentita */
    static char max_url[PUMPKIN_OTA_URL_MAX];
    memset(max_url, 'a', sizeof max_url - 1);
    memcpy(max_url, "https://", 8);
    max_url[sizeof max_url - 1] = '\0';
    in = nominal();
    in.url = max_url;
    TEST_ASSERT_EQUAL_INT(0, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
}

static void test_battery_gate(void)
{
    char reason[24];
    pumpkin_ota_policy_input_t in;

    /* 39% -> rifiuto */
    in = nominal();
    in.battery_pct = 39;
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("battery_low", reason);

    /* 40% -> consentito */
    in = nominal();
    in.battery_pct = 40;
    TEST_ASSERT_EQUAL_INT(0, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));

    /* sconosciuta -> consentita ma segnalata */
    in = nominal();
    in.battery_pct = -1;
    TEST_ASSERT_EQUAL_INT(0, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
    TEST_ASSERT_TRUE(in.warn_battery_unknown);
}

static void test_led_gate(void)
{
    char reason[24];
    pumpkin_ota_policy_input_t in;

    /* oltre il limite -> rifiuto */
    in = nominal();
    in.led_current_ma = 501;
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("led_full_light", reason);

    /* boundary: esattamente al limite -> consentito */
    in = nominal();
    in.led_current_ma = 500;
    TEST_ASSERT_EQUAL_INT(0, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
}

static void test_heap_gate(void)
{
    char reason[24];
    pumpkin_ota_policy_input_t in;

    /* heap libero sotto soglia */
    in = nominal();
    in.free_heap = MIN_HEAP - 1;
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("heap_low", reason);

    /* largest block sotto soglia */
    in = nominal();
    in.max_block = MIN_HEAP - 1;
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("heap_low", reason);

    /* entrambi esattamente alla soglia -> consentito */
    in = nominal();
    in.free_heap = MIN_HEAP;
    in.max_block = MIN_HEAP;
    TEST_ASSERT_EQUAL_INT(0, pumpkin_ota_policy_check(&in, reason, sizeof(reason)));
}

static void test_version_changed(void)
{
    /* diverse -> 0 */
    TEST_ASSERT_EQUAL_INT(0, pumpkin_ota_policy_version_changed("1.0.0", "1.0.1"));
    /* uguali -> -1 */
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_version_changed("1.0.0", "1.0.0"));
    /* prefisso v ignorato in entrambe */
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_version_changed("v1.0.0", "1.0.0"));
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_version_changed("v1.0.0", "v1.0.0"));
    TEST_ASSERT_EQUAL_INT(0, pumpkin_ota_policy_version_changed("v1.0.0", "v1.0.1"));
    /* NULL -> -1 */
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_version_changed(NULL, "1.0.0"));
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_version_changed("1.0.0", NULL));
    TEST_ASSERT_EQUAL_INT(-1, pumpkin_ota_policy_version_changed(NULL, NULL));
}

static void test_event_json_started_unknown_battery(void)
{
    char buf[512];
    pumpkin_ota_event_t ev = {
        .event_str = "started",
        .detail = NULL,
        .installed = NULL,
        .free_heap = 123456,
        .max_block = 100000,
        .progress = 0,
        .battery_unknown = true,
    };
    size_t n = pumpkin_ota_policy_event_json(buf, sizeof(buf), &ev);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_LESS_THAN_UINT(512, n);
    TEST_ASSERT_EQUAL_STRING(
        "{\"event\":\"started\",\"free_heap\":123456,\"max_block\":100000,"
        "\"progress\":0,\"battery\":\"unknown\"}",
        buf);
}

static void test_event_json_failed_and_rejected(void)
{
    char buf[512];

    pumpkin_ota_event_t ev = {
        .event_str = "failed",
        .detail = "ESP_ERR_HTTPS_OTA_IN_PROGRESS",
        .installed = NULL,
        .free_heap = 1,
        .max_block = 2,
        .progress = 0,
        .battery_unknown = false,
    };
    size_t n = pumpkin_ota_policy_event_json(buf, sizeof(buf), &ev);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(strstr(buf, "{\"event\":\"failed\"") == buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"detail\":\"ESP_ERR_HTTPS_OTA_IN_PROGRESS\""));
    TEST_ASSERT_NULL(strstr(buf, "\"battery\""));

    pumpkin_ota_event_t rej = {
        .event_str = "rejected",
        .detail = "heap_low",
        .installed = NULL,
        .free_heap = 3,
        .max_block = 4,
        .progress = 0,
        .battery_unknown = false,
    };
    n = pumpkin_ota_policy_event_json(buf, sizeof(buf), &rej);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"detail\":\"heap_low\""));

    pumpkin_ota_event_t ok = {
        .event_str = "success",
        .detail = NULL,
        .installed = "v2.0.0",
        .free_heap = 5,
        .max_block = 6,
        .progress = 100,
        .battery_unknown = false,
    };
    n = pumpkin_ota_policy_event_json(buf, sizeof(buf), &ok);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"installed\":\"v2.0.0\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"progress\":100"));
}

static void test_event_json_invalid(void)
{
    char buf[512];
    pumpkin_ota_event_t ev = {
        .event_str = "progress", .detail = NULL, .installed = NULL,
        .free_heap = 0, .max_block = 0, .progress = 30,
    };
    TEST_ASSERT_EQUAL_UINT(0, pumpkin_ota_policy_event_json(buf, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL_UINT(0, pumpkin_ota_policy_event_json(NULL, sizeof(buf), &ev));
    TEST_ASSERT_EQUAL_UINT(0, pumpkin_ota_policy_event_json(buf, 0, &ev));
    /* cap insufficiente: nessun output parziale */
    char tiny[16];
    TEST_ASSERT_EQUAL_UINT(0, pumpkin_ota_policy_event_json(tiny, sizeof(tiny), &ev));
    /* cap esattamente sufficiente: ritorna la lunghezza */
    char exact[64];
    size_t n = pumpkin_ota_policy_event_json(exact, sizeof(exact), &ev);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT(n, strlen(exact));
    char tight[64];
    TEST_ASSERT_EQUAL_UINT(n, pumpkin_ota_policy_event_json(tight, n + 1, &ev));
    TEST_ASSERT_EQUAL_UINT(0, pumpkin_ota_policy_event_json(tight, n, &ev));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nominal_allow);
    RUN_TEST(test_url_gate);
    RUN_TEST(test_battery_gate);
    RUN_TEST(test_led_gate);
    RUN_TEST(test_heap_gate);
    RUN_TEST(test_version_changed);
    RUN_TEST(test_event_json_started_unknown_battery);
    RUN_TEST(test_event_json_failed_and_rejected);
    RUN_TEST(test_event_json_invalid);
    return UNITY_END();
}
