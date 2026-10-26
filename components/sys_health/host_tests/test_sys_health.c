/* Test host del nucleo puro di sys_health: matrice delle 16 combinazioni di
 * flag per la conferma rollback, mapping dei motivi di reset, limiti di
 * dimensione dei JSON (400/512) e troncamento che non sfora mai il cap. */
#include <string.h>

#include "unity.h"

#include "sys_health.h"
#include "sys_health_core.h"

static const sys_health_flags_t f_ok = { true, true, true, true };

void setUp(void)
{
}

void tearDown(void)
{
}

/* 16 combinazioni: solo "tutte e quattro vere" conferma (immagine pending). */
static void test_should_confirm_matrix(void)
{
    for (unsigned mask = 0; mask < 16; mask++) {
        sys_health_flags_t f = {
            .wifi_up = mask & 1u,
            .mqtt_connected = mask & 2u,
            .sensor_ok = mask & 4u,
            .fsm_ok = mask & 8u,
        };
        char msg[64];
        snprintf(msg, sizeof(msg), "mask=%u", mask);
        TEST_ASSERT_EQUAL_MESSAGE(mask == 15u,
                                  sys_health_core_should_confirm(&f, true),
                                  msg);
    }
}

static void test_should_confirm_requires_pending(void)
{
    TEST_ASSERT_FALSE(sys_health_core_should_confirm(&f_ok, false));
    TEST_ASSERT_FALSE(sys_health_core_should_confirm(NULL, true));
    TEST_ASSERT_FALSE(sys_health_core_should_confirm(NULL, false));
}

static void test_reset_reason_str(void)
{
    TEST_ASSERT_EQUAL_STRING("POWERON",
                             sys_health_core_reset_reason_str(1));
    TEST_ASSERT_EQUAL_STRING("SW", sys_health_core_reset_reason_str(3));
    TEST_ASSERT_EQUAL_STRING("PANIC", sys_health_core_reset_reason_str(4));
    TEST_ASSERT_EQUAL_STRING("INT_WDT", sys_health_core_reset_reason_str(5));
    TEST_ASSERT_EQUAL_STRING("TASK_WDT", sys_health_core_reset_reason_str(6));
    TEST_ASSERT_EQUAL_STRING("WDT", sys_health_core_reset_reason_str(7));
    TEST_ASSERT_EQUAL_STRING("DEEPSLEEP", sys_health_core_reset_reason_str(8));
    TEST_ASSERT_EQUAL_STRING("BROWNOUT", sys_health_core_reset_reason_str(9));
    /* fuori dai motivi mappati: sempre UNKNOWN */
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", sys_health_core_reset_reason_str(0));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", sys_health_core_reset_reason_str(2));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", sys_health_core_reset_reason_str(10));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", sys_health_core_reset_reason_str(15));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", sys_health_core_reset_reason_str(-1));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", sys_health_core_reset_reason_str(999));
}

static void test_stability_json_shape(void)
{
    char buf[512];
    const sys_health_sample_t s = {
        .uptime_s = 3600,
        .free_heap = 123456,
        .min_heap = 100000,
        .max_block = 98304,
        .wifi_reconn = 2,
        .mqtt_reconn = 1,
        .resets = 5,
        .version = "v1.0.0",
    };
    size_t len = sys_health_core_stability_json(buf, sizeof(buf), &s);
    TEST_ASSERT_EQUAL(strlen(buf), len);
    TEST_ASSERT_EQUAL_STRING(
        "{\"up\":3600,\"free\":123456,\"min\":100000,\"maxblk\":98304,"
        "\"wr\":2,\"mr\":1,\"rst\":5,\"v\":\"v1.0.0\"}",
        buf);
}

static void test_stability_json_under_400(void)
{
    char buf[512];
    /* input tipici e input estremi: entrambi sotto il limite di progetto */
    const sys_health_sample_t tip = {
        .uptime_s = 86400,
        .free_heap = 123456,
        .min_heap = 100000,
        .max_block = 98304,
        .wifi_reconn = 2,
        .mqtt_reconn = 1,
        .resets = 5,
        .version = "v1.0.0",
    };
    size_t len = sys_health_core_stability_json(buf, sizeof(buf), &tip);
    TEST_ASSERT_LESS_THAN(400u, len);

    const sys_health_sample_t ext = {
        .uptime_s = 4294967295u,
        .free_heap = 4294967295u,
        .min_heap = 4294967295u,
        .max_block = 4294967295u,
        .wifi_reconn = 4294967295u,
        .mqtt_reconn = 4294967295u,
        .resets = 4294967295u,
        .version = "v1.0.0-2-g1234567-dirty",
    };
    len = sys_health_core_stability_json(buf, sizeof(buf), &ext);
    TEST_ASSERT_EQUAL(strlen(buf), len);
    TEST_ASSERT_LESS_THAN(400u, len);
}

static void test_stability_json_args_and_truncation(void)
{
    char buf[512];
    const sys_health_sample_t s = {
        .uptime_s = 1,
        .free_heap = 2,
        .min_heap = 3,
        .max_block = 4,
        .wifi_reconn = 5,
        .mqtt_reconn = 6,
        .resets = 7,
        .version = "v9.9.9-2-gabcdef0-dirty",
    };
    TEST_ASSERT_EQUAL(0, sys_health_core_stability_json(NULL, sizeof(buf), &s));
    TEST_ASSERT_EQUAL(0, sys_health_core_stability_json(buf, 0, &s));
    TEST_ASSERT_EQUAL(0, sys_health_core_stability_json(buf, sizeof(buf), NULL));

    /* troncamento: mai oltre cap, NUL finale, nulla scritto oltre */
    memset(buf, 'X', sizeof(buf));
    size_t len = sys_health_core_stability_json(buf, 64, &s);
    TEST_ASSERT_EQUAL(63u, len);
    TEST_ASSERT_EQUAL('\0', buf[63]);
    for (size_t i = len + 1; i < sizeof(buf); i++)
        TEST_ASSERT_EQUAL('X', buf[i]);
}

static void test_tasks_json_twelve_under_512(void)
{
    char buf[512];
    static const char *names[12] = { "IDLE0", "IDLE1",  "anim",   "fsm",
                                     "radar", "ha_mqtt", "sys_health", "tiT",
                                     "ipc0",  "ipc1",   "esp_timer", "wifi" };
    sys_health_task_hwm_t t[12];
    for (int i = 0; i < 12; i++) {
        t[i].name = names[i];
        t[i].hwm = (uint32_t)100 * (i + 1);
    }
    size_t len = sys_health_core_tasks_json(buf, sizeof(buf), t, 12);
    TEST_ASSERT_EQUAL(strlen(buf), len);
    TEST_ASSERT_LESS_THAN(512u, len);
    TEST_ASSERT_TRUE(strstr(buf, "{\"t\":[") == buf);
    TEST_ASSERT_TRUE(strncmp(&buf[len - 2], "]}", 2) == 0);
    for (int i = 0; i < 12; i++)
        TEST_ASSERT_NOT_NULL(strstr(buf, names[i]));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"h\":1200"));
}

static void test_tasks_json_empty_and_args(void)
{
    char buf[64];
    sys_health_task_hwm_t t[2] = { { "a", 1 }, { "b", 2 } };

    size_t len = sys_health_core_tasks_json(buf, sizeof(buf), t, 0);
    TEST_ASSERT_EQUAL_STRING("{\"t\":[]}", buf);
    TEST_ASSERT_EQUAL(8u, len);

    len = sys_health_core_tasks_json(buf, sizeof(buf), NULL, 3);
    TEST_ASSERT_EQUAL_STRING("{\"t\":[]}", buf);

    TEST_ASSERT_EQUAL(0, sys_health_core_tasks_json(NULL, sizeof(buf), t, 2));
    TEST_ASSERT_EQUAL(0, sys_health_core_tasks_json(buf, 0, t, 2));
}

static void test_tasks_json_truncates_but_closes(void)
{
    char buf[512];
    sys_health_task_hwm_t t[4] = {
        { "t0", 10 }, { "t1", 20 }, { "t2", 30 }, { "t3", 40 },
    };
    /* cap 61: header + 2 elementi + chiusura ci stanno, il terzo no
     * ({"t":[ = 6, {"n":"t0","h":10} = 17, +",..." = 18 a elemento) */
    size_t len = sys_health_core_tasks_json(buf, 61, t, 4);
    TEST_ASSERT_EQUAL(strlen(buf), len);
    TEST_ASSERT_LESS_THAN(61u, len);
    TEST_ASSERT_TRUE(strncmp(&buf[len - 2], "]}", 2) == 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "t0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "t1"));
    TEST_ASSERT_NULL(strstr(buf, "t2"));
    TEST_ASSERT_NULL(strstr(buf, "t3"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_should_confirm_matrix);
    RUN_TEST(test_should_confirm_requires_pending);
    RUN_TEST(test_reset_reason_str);
    RUN_TEST(test_stability_json_shape);
    RUN_TEST(test_stability_json_under_400);
    RUN_TEST(test_stability_json_args_and_truncation);
    RUN_TEST(test_tasks_json_twelve_under_512);
    RUN_TEST(test_tasks_json_empty_and_args);
    RUN_TEST(test_tasks_json_truncates_but_closes);
    return UNITY_END();
}
