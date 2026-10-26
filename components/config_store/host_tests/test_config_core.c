/* Host unit tests for config_store_core.c (Unity, native build).
 * Only the pure core is compiled here: the NVS/debounce IDF layer is
 * exercised on target. See CMakeLists.txt in this directory. */
#include <string.h>

#include "unity.h"

#include "config_store.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* Full, valid v1 configuration (the Kconfig defaults). */
static pumpkin_config_t mk_full(void)
{
    pumpkin_config_t c = {
        .version = PUMPKIN_CONFIG_VERSION,
        .trigger_mm = 1200,
        .cooldown_s = 30,
        .trigger_duration_s = 8,
        .idle_brightness_pct = 10,
        .max_brightness_pct = 60,
        .auto_mode = true,
        .color_r = 255,
        .color_g = 120,
        .color_b = 0,
        .brightness_pct = 100,
        .power_on = true,
    };
    snprintf(c.trigger_anim, sizeof(c.trigger_anim), "scare");
    snprintf(c.idle_anim, sizeof(c.idle_anim), "candle");
    return c;
}

/* 1a. Every numeric field outside its range gets clamped and the fix is
 * reported with ESP_ERR_INVALID_STATE; anim names stay intact. */
void test_sanitize_clamps_out_of_range_fields(void)
{
    pumpkin_config_t c = mk_full();
    c.trigger_mm = 50;         /* < 100 */
    c.cooldown_s = 4000;       /* > 3600 */
    c.trigger_duration_s = 0;  /* < 1 */
    c.idle_brightness_pct = 200; /* > 100 */
    c.max_brightness_pct = 3;  /* < 5 */
    c.brightness_pct = 255;    /* > 100 */

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, config_sanitize(&c));
    TEST_ASSERT_EQUAL_UINT16(100, c.trigger_mm);
    TEST_ASSERT_EQUAL_UINT16(3600, c.cooldown_s);
    TEST_ASSERT_EQUAL_UINT16(1, c.trigger_duration_s);
    TEST_ASSERT_EQUAL_UINT8(100, c.idle_brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(5, c.max_brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(100, c.brightness_pct);
    TEST_ASSERT_EQUAL_STRING("scare", c.trigger_anim);
    TEST_ASSERT_EQUAL_STRING("candle", c.idle_anim);
}

/* 1b. Upper-bound clamps on the same fields. */
void test_sanitize_clamps_upper_bounds(void)
{
    pumpkin_config_t c = mk_full();
    c.trigger_mm = 60000;          /* > 10000 */
    c.idle_brightness_pct = 101;   /* > 100 */
    c.max_brightness_pct = 101;    /* > 100 */

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, config_sanitize(&c));
    TEST_ASSERT_EQUAL_UINT16(10000, c.trigger_mm);
    TEST_ASSERT_EQUAL_UINT8(100, c.idle_brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(100, c.max_brightness_pct);
}

/* 1c. An already-valid config is left alone: ESP_OK, values untouched. */
void test_sanitize_valid_config_is_ok(void)
{
    pumpkin_config_t c = mk_full();
    pumpkin_config_t before = c;

    TEST_ASSERT_EQUAL(ESP_OK, config_sanitize(&c));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof(c));
}

/* 2a. A too-long anim name is truncated to 15 chars with a guaranteed
 * NUL; the other anim is untouched. */
void test_sanitize_truncates_too_long_anim(void)
{
    pumpkin_config_t c = mk_full();
    memset(c.trigger_anim, 'a', sizeof(c.trigger_anim)); /* no NUL at all */

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, config_sanitize(&c));
    TEST_ASSERT_EQUAL_UINT8(15, strnlen(c.trigger_anim, sizeof(c.trigger_anim)));
    TEST_ASSERT_EQUAL('\0', c.trigger_anim[15]);
    TEST_ASSERT_EQUAL_MEMORY("aaaaaaaaaaaaaaa", c.trigger_anim, 15);
    TEST_ASSERT_EQUAL_STRING("candle", c.idle_anim);
}

/* 2b. A name containing a non-printable byte is cut at the first one. */
void test_sanitize_cuts_anim_at_non_printable(void)
{
    pumpkin_config_t c = mk_full();
    strcpy(c.trigger_anim, "scare\tredux");

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, config_sanitize(&c));
    TEST_ASSERT_EQUAL_STRING("scare", c.trigger_anim);
}

/* 2c. An empty anim name is left empty (the IDF layer substitutes the
 * default) and alone does not count as a correction: ESP_OK. */
void test_sanitize_leaves_empty_anim_alone(void)
{
    pumpkin_config_t c = mk_full();
    c.idle_anim[0] = '\0';

    TEST_ASSERT_EQUAL(ESP_OK, config_sanitize(&c));
    TEST_ASSERT_EQUAL_UINT8(0, c.idle_anim[0]);
    TEST_ASSERT_EQUAL_STRING("scare", c.trigger_anim);
}

/* 3a. A current-version config needs no migration. */
void test_migrate_current_version_ok(void)
{
    pumpkin_config_t c = mk_full();
    pumpkin_config_t before = c;

    TEST_ASSERT_EQUAL(ESP_OK, config_migrate(&c));
    TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof(c));
}

/* 3b. A version from the future is rejected: the caller falls back to
 * the defaults. */
void test_migrate_future_version_not_supported(void)
{
    pumpkin_config_t c = mk_full();
    c.version = 99;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, config_migrate(&c));
    TEST_ASSERT_EQUAL_UINT8(99, c.version); /* untouched */
}

/* 3c. v0 (pre-versioning blob) steps through the chain to the current
 * version without touching the v1 fields. */
void test_migrate_v0_steps_to_current(void)
{
    pumpkin_config_t c = mk_full();
    c.version = 0;

    TEST_ASSERT_EQUAL(ESP_OK, config_migrate(&c));
    TEST_ASSERT_EQUAL_UINT8(PUMPKIN_CONFIG_VERSION, c.version);
    TEST_ASSERT_EQUAL_UINT16(1200, c.trigger_mm);
    TEST_ASSERT_EQUAL_STRING("scare", c.trigger_anim);
}

/* 4. Serialize -> deserialize roundtrip on a full config is exact, and
 * the version field travels outside the blob. */
void test_serialize_deserialize_roundtrip(void)
{
    pumpkin_config_t c = mk_full();
    size_t sz = config_serialized_size();
    TEST_ASSERT_EQUAL(46, sz);

    uint8_t buf[64];
    config_serialize(&c, buf, sizeof(buf));

    pumpkin_config_t out;
    memset(&out, 0xA5, sizeof(out)); /* poison: deserialize must fill all */
    out.version = 0x77;              /* not part of the blob */
    TEST_ASSERT_EQUAL(ESP_OK, config_deserialize(&out, buf, sz));
    TEST_ASSERT_EQUAL_UINT8(0x77, out.version);

    /* field-by-field: the struct has padding, memcmp would read it */
    TEST_ASSERT_EQUAL_UINT16(c.trigger_mm, out.trigger_mm);
    TEST_ASSERT_EQUAL_UINT16(c.cooldown_s, out.cooldown_s);
    TEST_ASSERT_EQUAL_UINT16(c.trigger_duration_s, out.trigger_duration_s);
    TEST_ASSERT_EQUAL_MEMORY(c.trigger_anim, out.trigger_anim, 16);
    TEST_ASSERT_EQUAL_MEMORY(c.idle_anim, out.idle_anim, 16);
    TEST_ASSERT_EQUAL_UINT8(c.idle_brightness_pct, out.idle_brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(c.max_brightness_pct, out.max_brightness_pct);
    TEST_ASSERT_EQUAL(c.auto_mode, out.auto_mode);
    TEST_ASSERT_EQUAL_UINT8(c.color_r, out.color_r);
    TEST_ASSERT_EQUAL_UINT8(c.color_g, out.color_g);
    TEST_ASSERT_EQUAL_UINT8(c.color_b, out.color_b);
    TEST_ASSERT_EQUAL_UINT8(c.brightness_pct, out.brightness_pct);
    TEST_ASSERT_EQUAL(c.power_on, out.power_on);

    /* re-serializing the copy gives the identical byte blob */
    uint8_t buf2[64];
    config_serialize(&out, buf2, sizeof(buf2));
    TEST_ASSERT_EQUAL_MEMORY(buf, buf2, sz);
}

/* 5. A blob of the wrong size is rejected with ESP_ERR_INVALID_SIZE and
 * leaves the destination struct untouched. */
void test_deserialize_wrong_size_rejects(void)
{
    pumpkin_config_t c = mk_full();
    uint8_t buf[64];
    config_serialize(&c, buf, sizeof(buf));

    pumpkin_config_t out = c;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      config_deserialize(&out, buf, config_serialized_size() - 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      config_deserialize(&out, buf, config_serialized_size() + 1));
    TEST_ASSERT_EQUAL_MEMORY(&c, &out, sizeof(c));
}

/* 6. Layout: known bytes at known offsets (little-endian u16s, anims at
 * fixed offsets, trailing u8 fields in v1 order). */
void test_serialize_layout_known_bytes(void)
{
    pumpkin_config_t c = mk_full();
    uint8_t buf[64];
    config_serialize(&c, buf, sizeof(buf));

    /* trigger_mm = 1200 = 0x04B0 little-endian */
    TEST_ASSERT_EQUAL_HEX8(0xB0, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[1]);
    /* cooldown_s = 30 = 0x001E */
    TEST_ASSERT_EQUAL_HEX8(0x1E, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[3]);
    /* trigger_duration_s = 8 */
    TEST_ASSERT_EQUAL_HEX8(0x08, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[5]);
    /* trigger_anim at 6, idle_anim at 22 */
    TEST_ASSERT_EQUAL_MEMORY("scare", &buf[6], 6); /* NUL included */
    TEST_ASSERT_EQUAL_MEMORY("candle", &buf[22], 7);
    for (int i = 13; i < 16; i++)
        TEST_ASSERT_EQUAL_UINT8(0, buf[6 + i]); /* NUL padding */
    /* trailing u8 fields in v1 order */
    TEST_ASSERT_EQUAL_UINT8(10, buf[38]);  /* idle_brightness_pct */
    TEST_ASSERT_EQUAL_UINT8(60, buf[39]);  /* max_brightness_pct */
    TEST_ASSERT_EQUAL_UINT8(1, buf[40]);   /* auto_mode */
    TEST_ASSERT_EQUAL_UINT8(255, buf[41]); /* color_r */
    TEST_ASSERT_EQUAL_UINT8(120, buf[42]); /* color_g */
    TEST_ASSERT_EQUAL_UINT8(0, buf[43]);   /* color_b */
    TEST_ASSERT_EQUAL_UINT8(100, buf[44]); /* brightness_pct */
    TEST_ASSERT_EQUAL_UINT8(1, buf[45]);   /* power_on */
}

/* 7. Robustness: NULL arguments are safe rejections. */
void test_null_args_rejected(void)
{
    pumpkin_config_t c = mk_full();
    uint8_t buf[64];
    config_serialize(&c, buf, sizeof(buf));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_sanitize(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_migrate(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_deserialize(NULL, buf, 46));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      config_deserialize(&c, NULL, 46));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sanitize_clamps_out_of_range_fields);
    RUN_TEST(test_sanitize_clamps_upper_bounds);
    RUN_TEST(test_sanitize_valid_config_is_ok);
    RUN_TEST(test_sanitize_truncates_too_long_anim);
    RUN_TEST(test_sanitize_cuts_anim_at_non_printable);
    RUN_TEST(test_sanitize_leaves_empty_anim_alone);
    RUN_TEST(test_migrate_current_version_ok);
    RUN_TEST(test_migrate_future_version_not_supported);
    RUN_TEST(test_migrate_v0_steps_to_current);
    RUN_TEST(test_serialize_deserialize_roundtrip);
    RUN_TEST(test_deserialize_wrong_size_rejects);
    RUN_TEST(test_serialize_layout_known_bytes);
    RUN_TEST(test_null_args_rejected);
    return UNITY_END();
}
