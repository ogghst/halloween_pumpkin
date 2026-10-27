/* Host unit tests for config_store (Unity, native build). The pure core
 * runs directly; the NVS/debounce IDF layer (config_store.c) is compiled
 * too against the stubs in stub/ (NVS absent -> RAM-only mode, fake
 * event loop with post counters), so the typed setters are testable on
 * host as well. The real NVS write path stays exercised on target. See
 * CMakeLists.txt in this directory. */
#include <string.h>

#include "unity.h"

#include "config_store.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* Full, valid v3 configuration (Kconfig defaults + placeholder MQTT
 * broker values + zona occhi con valori distinguibili; synthetic
 * strings, never real credentials). */
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
        .mqtt = {
            .host = "broker.example",
            .port = 8883,
            .user = "usr",
            .pass = "pwd",
            .tls = true,
        },
        .eyes_enabled = true,
        .eyes_idle_r = 1,
        .eyes_idle_g = 2,
        .eyes_idle_b = 3,
        .eyes_scare_r = 4,
        .eyes_scare_g = 5,
        .eyes_scare_b = 6,
        .eyes_idle_brightness_pct = 33,
        .eyes_scare_brightness_pct = 90,
        .eyes_follow_fsm = true,
    };
    snprintf(c.trigger_anim, sizeof(c.trigger_anim), "scare");
    snprintf(c.idle_anim, sizeof(c.idle_anim), "candle");
    snprintf(c.eyes_idle_effect, sizeof(c.eyes_idle_effect), "rainbow");
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

/* 3c. v0 (pre-versioning blob) steps through the whole chain to the
 * current version: v1 fields untouched, then the v1 -> v2 step fills the
 * mqtt defaults. */
void test_migrate_v0_steps_to_current(void)
{
    pumpkin_config_t c = mk_full();
    c.version = 0;

    TEST_ASSERT_EQUAL(ESP_OK, config_migrate(&c));
    TEST_ASSERT_EQUAL_UINT8(PUMPKIN_CONFIG_VERSION, c.version);
    TEST_ASSERT_EQUAL_UINT16(1200, c.trigger_mm);
    TEST_ASSERT_EQUAL_STRING("scare", c.trigger_anim);
    TEST_ASSERT_EQUAL_UINT16(1883, c.mqtt.port);
}

/* 4. Serialize -> deserialize roundtrip on a full config is exact (v3,
 * mqtt and eyes blocks included), and the version field travels outside
 * the blob. */
void test_serialize_deserialize_roundtrip(void)
{
    pumpkin_config_t c = mk_full();
    size_t sz = config_serialized_size();
    TEST_ASSERT_EQUAL(235, sz); /* 46 v1 + 163 mqtt + 26 occhi */

    uint8_t buf[256];
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
    TEST_ASSERT_EQUAL_MEMORY(c.mqtt.host, out.mqtt.host, CONFIG_MQTT_HOST_LEN);
    TEST_ASSERT_EQUAL_UINT16(c.mqtt.port, out.mqtt.port);
    TEST_ASSERT_EQUAL_MEMORY(c.mqtt.user, out.mqtt.user, CONFIG_MQTT_USER_LEN);
    TEST_ASSERT_EQUAL_MEMORY(c.mqtt.pass, out.mqtt.pass, CONFIG_MQTT_PASS_LEN);
    TEST_ASSERT_EQUAL(c.mqtt.tls, out.mqtt.tls);
    TEST_ASSERT_EQUAL(c.eyes_enabled, out.eyes_enabled);
    TEST_ASSERT_EQUAL_UINT8(c.eyes_idle_r, out.eyes_idle_r);
    TEST_ASSERT_EQUAL_UINT8(c.eyes_idle_g, out.eyes_idle_g);
    TEST_ASSERT_EQUAL_UINT8(c.eyes_idle_b, out.eyes_idle_b);
    TEST_ASSERT_EQUAL_UINT8(c.eyes_scare_r, out.eyes_scare_r);
    TEST_ASSERT_EQUAL_UINT8(c.eyes_scare_g, out.eyes_scare_g);
    TEST_ASSERT_EQUAL_UINT8(c.eyes_scare_b, out.eyes_scare_b);
    TEST_ASSERT_EQUAL_MEMORY(c.eyes_idle_effect, out.eyes_idle_effect,
                             CONFIG_ANIM_NAME_LEN);
    TEST_ASSERT_EQUAL_UINT8(c.eyes_idle_brightness_pct,
                            out.eyes_idle_brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(c.eyes_scare_brightness_pct,
                            out.eyes_scare_brightness_pct);
    TEST_ASSERT_EQUAL(c.eyes_follow_fsm, out.eyes_follow_fsm);

    /* re-serializing the copy gives the identical byte blob */
    uint8_t buf2[256];
    config_serialize(&out, buf2, sizeof(buf2));
    TEST_ASSERT_EQUAL_MEMORY(buf, buf2, sz);
}

/* 5. A blob of the wrong size is rejected with ESP_ERR_INVALID_SIZE and
 * leaves the destination struct untouched. */
void test_deserialize_wrong_size_rejects(void)
{
    pumpkin_config_t c = mk_full();
    uint8_t buf[256];
    config_serialize(&c, buf, sizeof(buf));

    pumpkin_config_t out = c;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      config_deserialize(&out, buf, config_serialized_size() - 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      config_deserialize(&out, buf, config_serialized_size() + 1));
    TEST_ASSERT_EQUAL_MEMORY(&c, &out, sizeof(c));
}

/* 6. Layout of the v1 HEAD (bytes 0..45, unchanged by v2): known bytes at
 * known offsets (little-endian u16s, anims at fixed offsets, trailing u8
 * fields in v1 order). */
void test_serialize_layout_known_bytes(void)
{
    pumpkin_config_t c = mk_full();
    uint8_t buf[256];
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

/* 8. A legacy 46-byte v1 blob still deserializes (mqtt block zeroed) and
 * migrates to v2: v1 fields preserved, fixed mqtt defaults filled. */
void test_legacy_v1_blob_deserialize_then_migrate(void)
{
    pumpkin_config_t c = mk_full();
    uint8_t v2buf[256];
    config_serialize(&c, v2buf, sizeof(v2buf));
    uint8_t v1buf[46];
    memcpy(v1buf, v2buf, sizeof(v1buf)); /* v1 head is byte-identical */

    pumpkin_config_t out;
    memset(&out, 0xA5, sizeof(out));
    out.version = 1; /* as read from the "ver" key */
    TEST_ASSERT_EQUAL(ESP_OK, config_deserialize(&out, v1buf, sizeof(v1buf)));
    TEST_ASSERT_EQUAL_UINT8(1, out.version); /* untouched by deserialize */
    TEST_ASSERT_EQUAL_UINT16(1200, out.trigger_mm);
    TEST_ASSERT_EQUAL_UINT16(30, out.cooldown_s);
    TEST_ASSERT_EQUAL_UINT16(8, out.trigger_duration_s);
    TEST_ASSERT_EQUAL_STRING("scare", out.trigger_anim);
    TEST_ASSERT_EQUAL_STRING("candle", out.idle_anim);
    TEST_ASSERT_EQUAL_UINT8(10, out.idle_brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(60, out.max_brightness_pct);
    TEST_ASSERT_EQUAL(out.auto_mode, true);
    TEST_ASSERT_EQUAL_UINT8(255, out.color_r);
    TEST_ASSERT_EQUAL_UINT8(120, out.color_g);
    TEST_ASSERT_EQUAL_UINT8(0, out.color_b);
    TEST_ASSERT_EQUAL_UINT8(100, out.brightness_pct);
    TEST_ASSERT_EQUAL(out.power_on, true);
    /* no mqtt block in the blob: deterministic zeros, not poison */
    TEST_ASSERT_EQUAL_UINT16(0, out.mqtt.port);
    TEST_ASSERT_EQUAL_UINT8(0, out.mqtt.host[0]);
    TEST_ASSERT_EQUAL_UINT8(0, out.mqtt.user[0]);
    TEST_ASSERT_EQUAL_UINT8(0, out.mqtt.pass[0]);
    TEST_ASSERT_EQUAL(out.mqtt.tls, false);

    /* migrate 1 -> 2: fixed defaults, v1 fields preserved */
    TEST_ASSERT_EQUAL(ESP_OK, config_migrate(&out));
    TEST_ASSERT_EQUAL_UINT8(PUMPKIN_CONFIG_VERSION, out.version);
    TEST_ASSERT_EQUAL_UINT16(1883, out.mqtt.port);
    TEST_ASSERT_EQUAL_STRING("", out.mqtt.host);
    TEST_ASSERT_EQUAL_STRING("", out.mqtt.user);
    TEST_ASSERT_EQUAL_STRING("", out.mqtt.pass);
    TEST_ASSERT_EQUAL(out.mqtt.tls, false);
    TEST_ASSERT_EQUAL_UINT16(1200, out.trigger_mm);
    TEST_ASSERT_EQUAL_STRING("scare", out.trigger_anim);
    TEST_ASSERT_EQUAL_STRING("candle", out.idle_anim);
}

/* 9. Struct-level 1 -> 2 migration zeroes the WHOLE mqtt block before
 * filling the defaults (no leftover bytes in the string tails); the
 * chain then continues 2 -> 3 with the eyes defaults. */
void test_migrate_v1_fills_mqtt_defaults_block(void)
{
    pumpkin_config_t c = mk_full();
    memset(&c.mqtt, 0x55, sizeof(c.mqtt)); /* a v1 boot had no block */
    c.version = 1;

    TEST_ASSERT_EQUAL(ESP_OK, config_migrate(&c));
    TEST_ASSERT_EQUAL_UINT8(PUMPKIN_CONFIG_VERSION, c.version);
    static uint8_t zeros[CONFIG_MQTT_HOST_LEN];
    TEST_ASSERT_EQUAL_MEMORY(zeros, c.mqtt.host, CONFIG_MQTT_HOST_LEN);
    TEST_ASSERT_EQUAL_MEMORY(zeros, c.mqtt.user, CONFIG_MQTT_USER_LEN);
    TEST_ASSERT_EQUAL_MEMORY(zeros, c.mqtt.pass, CONFIG_MQTT_PASS_LEN);
    TEST_ASSERT_EQUAL_UINT16(1883, c.mqtt.port);
    TEST_ASSERT_EQUAL(c.mqtt.tls, false);
    /* the chained v2 -> v3 step filled the eyes defaults */
    TEST_ASSERT_EQUAL(c.eyes_enabled, true);
    TEST_ASSERT_EQUAL_UINT8(255, c.eyes_idle_r);
    TEST_ASSERT_EQUAL_UINT8(120, c.eyes_idle_g);
    TEST_ASSERT_EQUAL_UINT8(0, c.eyes_idle_b);
    TEST_ASSERT_EQUAL_STRING("breathe", c.eyes_idle_effect);
}

/* 10. v2 tail: mqtt block at the documented offsets, and the 46-byte v1
 * head is independent of the mqtt content. */
void test_serialize_v2_tail_offsets_and_head_stability(void)
{
    pumpkin_config_t a = mk_full();
    pumpkin_config_t b = a;
    memset(&b.mqtt, 0, sizeof(b.mqtt)); /* same v1 fields, mqtt off */

    uint8_t bufa[256];
    uint8_t bufb[256];
    config_serialize(&a, bufa, sizeof(bufa));
    config_serialize(&b, bufb, sizeof(bufb));

    /* the v1 head does not move when mqtt changes */
    TEST_ASSERT_EQUAL_MEMORY(bufa, bufb, 46);
    /* b's tail is the all-zero mqtt block, tls included (bytes 46..208;
     * the v3 eyes tail after byte 209 follows mk_full in both copies) */
    for (size_t i = 46; i < 209; i++)
        TEST_ASSERT_EQUAL_UINT8(0, bufb[i]);

    /* host[64] at 46 ("broker.example" + NUL), port u16 LE at 110 */
    TEST_ASSERT_EQUAL_MEMORY("broker.example", &bufa[46], 15);
    TEST_ASSERT_EQUAL_UINT8(0, bufa[61]); /* NUL tail of host */
    TEST_ASSERT_EQUAL_HEX8(0xB3, bufa[110]); /* 8883 = 0x22B3 LE */
    TEST_ASSERT_EQUAL_HEX8(0x22, bufa[111]);
    /* user[32] at 112 ("usr" + NUL), pass[64] at 144 ("pwd" + NUL) */
    TEST_ASSERT_EQUAL_MEMORY("usr", &bufa[112], 4);
    TEST_ASSERT_EQUAL_MEMORY("pwd", &bufa[144], 4);
    TEST_ASSERT_EQUAL_UINT8(0, bufa[112 + 31]);
    TEST_ASSERT_EQUAL_UINT8(0, bufa[144 + 63]);
    /* tls u8 at 208 */
    TEST_ASSERT_EQUAL_UINT8(1, bufa[208]);
}

/* 11. Sanitize repairs the mqtt block: port 0 -> 1883, unterminated or
 * non-printable strings cut like the anims. A fully valid block alone is
 * not a correction (covered by the ESP_OK paths of tests 1c/2c). */
void test_sanitize_mqtt_fields(void)
{
    pumpkin_config_t c = mk_full();
    c.mqtt.port = 0;                                   /* out of 1..65535 */
    memset(c.mqtt.host, 'h', sizeof(c.mqtt.host));     /* no NUL at all */
    strcpy(c.mqtt.user, "usr\ttab");                   /* non-printable */
    strcpy(c.mqtt.pass, "still-fine");

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, config_sanitize(&c));
    TEST_ASSERT_EQUAL_UINT16(1883, c.mqtt.port);
    TEST_ASSERT_EQUAL_UINT8(63, strnlen(c.mqtt.host, sizeof(c.mqtt.host)));
    TEST_ASSERT_EQUAL('\0', c.mqtt.host[63]);
    TEST_ASSERT_EQUAL_STRING("usr", c.mqtt.user);
    TEST_ASSERT_EQUAL_STRING("still-fine", c.mqtt.pass);
}

/* 12. tls normalizes to a clean 0/1 (raw byte 0 -> false, garbage ->
 * true, reported as a fix); sanitize on an already-clean tls is silent. */
void test_sanitize_mqtt_tls_normalizes(void)
{
    pumpkin_config_t c = mk_full();
    uint8_t garbage = 0xA5;
    memcpy(&c.mqtt.tls, &garbage, 1); /* raw byte, as read from a blob */

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, config_sanitize(&c));
    TEST_ASSERT_EQUAL(c.mqtt.tls, true);

    pumpkin_config_t d = mk_full();
    uint8_t zero = 0;
    memcpy(&d.mqtt.tls, &zero, 1);
    TEST_ASSERT_EQUAL(ESP_OK, config_sanitize(&d));
    TEST_ASSERT_EQUAL(d.mqtt.tls, false);
}

/* 13. Coda occhi v3: offset byte documentati nell'header ([209] enabled,
 * [210..212] idle rgb, [213..215] scare rgb, [216..231] effect[16],
 * [232] idle pct, [233] scare pct, [234] follow) e i primi 209 byte
 * (v1 head + mqtt) restano byte-identici al variare dei soli occhi. */
void test_serialize_v3_tail_offsets_and_head_stability(void)
{
    pumpkin_config_t a = mk_full();
    pumpkin_config_t b = a;
    b.eyes_enabled = false;
    b.eyes_idle_r = 0;
    b.eyes_idle_g = 0;
    b.eyes_idle_b = 0;
    b.eyes_scare_r = 0;
    b.eyes_scare_g = 0;
    b.eyes_scare_b = 0;
    memset(b.eyes_idle_effect, 0, sizeof(b.eyes_idle_effect));
    b.eyes_idle_brightness_pct = 0;
    b.eyes_scare_brightness_pct = 0;
    b.eyes_follow_fsm = false;

    uint8_t bufa[256];
    uint8_t bufb[256];
    config_serialize(&a, bufa, sizeof(bufa));
    config_serialize(&b, bufb, sizeof(bufb));

    /* la parte v2 non si muove quando cambiano i soli occhi */
    TEST_ASSERT_EQUAL_MEMORY(bufa, bufb, 209);
    /* b con occhi azzerati: coda tutta a zero */
    for (size_t i = 209; i < 235; i++)
        TEST_ASSERT_EQUAL_UINT8(0, bufb[i]);

    /* offset della coda occhi su `a` */
    TEST_ASSERT_EQUAL_UINT8(1, bufa[209]); /* eyes_enabled */
    TEST_ASSERT_EQUAL_UINT8(1, bufa[210]); /* idle r */
    TEST_ASSERT_EQUAL_UINT8(2, bufa[211]); /* idle g */
    TEST_ASSERT_EQUAL_UINT8(3, bufa[212]); /* idle b */
    TEST_ASSERT_EQUAL_UINT8(4, bufa[213]); /* scare r */
    TEST_ASSERT_EQUAL_UINT8(5, bufa[214]); /* scare g */
    TEST_ASSERT_EQUAL_UINT8(6, bufa[215]); /* scare b */
    TEST_ASSERT_EQUAL_MEMORY("rainbow", &bufa[216], 8); /* NUL incluso */
    for (size_t i = 224; i < 232; i++)
        TEST_ASSERT_EQUAL_UINT8(0, bufa[i]); /* coda NUL dell'effetto */
    TEST_ASSERT_EQUAL_UINT8(33, bufa[232]); /* idle pct */
    TEST_ASSERT_EQUAL_UINT8(90, bufa[233]); /* scare pct */
    TEST_ASSERT_EQUAL_UINT8(1, bufa[234]);  /* follow_fsm */
}

/* 14. Un blob v2 sintetico di 209 byte (prefisso del blob v3) si
 * deserializza con la coda occhi azzerata (nessun avanzo dallo stato
 * del chiamante) e la migrate v2 -> v3 riempie i default occhi esatti
 * lasciando il mqtt letto dal blob intatto. */
void test_v2_blob_deserialize_then_migrate_fills_eyes_defaults(void)
{
    pumpkin_config_t full = mk_full();
    uint8_t v3buf[256];
    config_serialize(&full, v3buf, sizeof(v3buf));
    uint8_t v2buf[209];
    memcpy(v2buf, v3buf, sizeof(v2buf)); /* il blob v2 e' il prefisso v3 */

    pumpkin_config_t out;
    memset(&out, 0xA5, sizeof(out)); /* poison: deserialize riempie tutto */
    out.version = 2; /* come letto dalla chiave "ver" */
    TEST_ASSERT_EQUAL(ESP_OK, config_deserialize(&out, v2buf, sizeof(v2buf)));
    TEST_ASSERT_EQUAL_UINT8(2, out.version); /* non toccato */

    /* mqtt letto dal blob */
    TEST_ASSERT_EQUAL_STRING("broker.example", out.mqtt.host);
    TEST_ASSERT_EQUAL_UINT16(8883, out.mqtt.port);
    TEST_ASSERT_EQUAL_STRING("usr", out.mqtt.user);
    TEST_ASSERT_EQUAL_STRING("pwd", out.mqtt.pass);
    TEST_ASSERT_EQUAL(out.mqtt.tls, true);

    /* coda occhi azzerata, nessun 0xA5 superstite */
    TEST_ASSERT_EQUAL(out.eyes_enabled, false);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_idle_r);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_idle_g);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_idle_b);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_scare_r);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_scare_g);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_scare_b);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_idle_effect[0]);
    TEST_ASSERT_EQUAL_UINT8(0,
                            out.eyes_idle_effect[CONFIG_ANIM_NAME_LEN - 1]);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_idle_brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_scare_brightness_pct);
    TEST_ASSERT_EQUAL(out.eyes_follow_fsm, false);

    /* migrate v2 -> v3: default occhi esatti (UGUALI ai Kconfig
     * PUMPKIN_EYES_*), mqtt intatto */
    TEST_ASSERT_EQUAL(ESP_OK, config_migrate(&out));
    TEST_ASSERT_EQUAL_UINT8(PUMPKIN_CONFIG_VERSION, out.version);
    TEST_ASSERT_EQUAL(out.eyes_enabled, true);
    TEST_ASSERT_EQUAL_UINT8(255, out.eyes_idle_r);
    TEST_ASSERT_EQUAL_UINT8(120, out.eyes_idle_g);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_idle_b);
    TEST_ASSERT_EQUAL_UINT8(255, out.eyes_scare_r);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_scare_g);
    TEST_ASSERT_EQUAL_UINT8(0, out.eyes_scare_b);
    TEST_ASSERT_EQUAL_STRING("breathe", out.eyes_idle_effect);
    TEST_ASSERT_EQUAL_UINT8(40, out.eyes_idle_brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(100, out.eyes_scare_brightness_pct);
    TEST_ASSERT_EQUAL(out.eyes_follow_fsm, true);
    TEST_ASSERT_EQUAL_STRING("broker.example", out.mqtt.host);
    TEST_ASSERT_EQUAL_UINT16(8883, out.mqtt.port);
    TEST_ASSERT_EQUAL(out.mqtt.tls, true);
}

/* 15. Sanitize ripara la zona occhi: pct clampate 0..100, effect tagliato
 * al primo byte non stampabile come le anim, bool normalizzati 0/1. */
void test_sanitize_eyes_fields(void)
{
    pumpkin_config_t c = mk_full();
    c.eyes_idle_brightness_pct = 200;            /* > 100 */
    c.eyes_scare_brightness_pct = 255;           /* > 100 */
    strcpy(c.eyes_idle_effect, "rainbow\ttwo");  /* non stampabile */
    uint8_t garbage = 0xA5;
    memcpy(&c.eyes_enabled, &garbage, 1);        /* raw byte da un blob */
    memcpy(&c.eyes_follow_fsm, &garbage, 1);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, config_sanitize(&c));
    TEST_ASSERT_EQUAL_UINT8(100, c.eyes_idle_brightness_pct);
    TEST_ASSERT_EQUAL_UINT8(100, c.eyes_scare_brightness_pct);
    TEST_ASSERT_EQUAL_STRING("rainbow", c.eyes_idle_effect);
    TEST_ASSERT_EQUAL(c.eyes_enabled, true);
    TEST_ASSERT_EQUAL(c.eyes_follow_fsm, true);

    pumpkin_config_t d = mk_full();
    d.eyes_idle_brightness_pct = 255;
    uint8_t zero = 0;
    memcpy(&d.eyes_enabled, &zero, 1); /* bool gia' pulito: non e' una fix */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, config_sanitize(&d));
    TEST_ASSERT_EQUAL_UINT8(100, d.eyes_idle_brightness_pct);
    TEST_ASSERT_EQUAL(d.eyes_enabled, false);
    TEST_ASSERT_EQUAL(d.eyes_follow_fsm, true);
}

/* 16. set_color_param (layer IDF su host: NVS assente -> RAM only):
 * accetta i tre id colore, rifiuta gli altri con ESP_ERR_INVALID_ARG,
 * posta un evento per ogni id cambiato ed e' no-op silenziosa a parita'
 * di valore; il wrapper set_color resta valido per CFG_PARAM_COLOR. */
void test_set_color_param_ids_noop_and_events(void)
{
    stub_event_post_count = 0;
    stub_event_last_id = -1;

    /* prima dell'init il negozio non e' pronto */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      config_store_set_color_param(CFG_PARAM_COLOR, 1, 2, 3));

    /* init con i default di mk_full (NVS stub assente: nessuna scrittura) */
    pumpkin_config_t defaults = mk_full();
    TEST_ASSERT_EQUAL(ESP_OK, config_store_init(&defaults));

    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_color_param(
                                  CFG_PARAM_COLOR, 11, 22, 33));
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_color_param(
                                  CFG_PARAM_EYES_IDLE_COLOR, 44, 55, 66));
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_color_param(
                                  CFG_PARAM_EYES_SCARE_COLOR, 77, 88, 99));

    pumpkin_config_t c;
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get(&c));
    TEST_ASSERT_EQUAL_UINT8(11, c.color_r);
    TEST_ASSERT_EQUAL_UINT8(22, c.color_g);
    TEST_ASSERT_EQUAL_UINT8(33, c.color_b);
    TEST_ASSERT_EQUAL_UINT8(44, c.eyes_idle_r);
    TEST_ASSERT_EQUAL_UINT8(55, c.eyes_idle_g);
    TEST_ASSERT_EQUAL_UINT8(66, c.eyes_idle_b);
    TEST_ASSERT_EQUAL_UINT8(77, c.eyes_scare_r);
    TEST_ASSERT_EQUAL_UINT8(88, c.eyes_scare_g);
    TEST_ASSERT_EQUAL_UINT8(99, c.eyes_scare_b);

    /* tre eventi, ultimo con l'id dello scare color */
    TEST_ASSERT_EQUAL_INT(3, stub_event_post_count);
    TEST_ASSERT_EQUAL_INT(CFG_PARAM_EYES_SCARE_COLOR, stub_event_last_id);

    /* id non colore: rifiutati, nessun evento */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_store_set_color_param(
                                               CFG_PARAM_TRIGGER_MM, 1, 2, 3));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_store_set_color_param(
                                               CFG_PARAM_MQTT, 1, 2, 3));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, config_store_set_color_param(
                                               CFG_PARAM_EYES_ENABLED, 1, 2, 3));
    TEST_ASSERT_EQUAL_INT(3, stub_event_post_count);

    /* stesso valore: no-op senza evento */
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_color_param(
                                  CFG_PARAM_EYES_IDLE_COLOR, 44, 55, 66));
    TEST_ASSERT_EQUAL_INT(3, stub_event_post_count);

    /* wrapper storico: posta CFG_PARAM_COLOR e aggiorna il colore corpo */
    TEST_ASSERT_EQUAL(ESP_OK, config_store_set_color(254, 120, 0));
    TEST_ASSERT_EQUAL_INT(4, stub_event_post_count);
    TEST_ASSERT_EQUAL_INT(CFG_PARAM_COLOR, stub_event_last_id);
    TEST_ASSERT_EQUAL(ESP_OK, config_store_get(&c));
    TEST_ASSERT_EQUAL_UINT8(254, c.color_r);
}

/* 7. Robustness: NULL arguments are safe rejections. */
void test_null_args_rejected(void)
{
    pumpkin_config_t c = mk_full();
    uint8_t buf[256];
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
    RUN_TEST(test_legacy_v1_blob_deserialize_then_migrate);
    RUN_TEST(test_migrate_v1_fills_mqtt_defaults_block);
    RUN_TEST(test_serialize_v2_tail_offsets_and_head_stability);
    RUN_TEST(test_sanitize_mqtt_fields);
    RUN_TEST(test_sanitize_mqtt_tls_normalizes);
    RUN_TEST(test_serialize_v3_tail_offsets_and_head_stability);
    RUN_TEST(test_v2_blob_deserialize_then_migrate_fills_eyes_defaults);
    RUN_TEST(test_sanitize_eyes_fields);
    RUN_TEST(test_set_color_param_ids_noop_and_events);
    RUN_TEST(test_null_args_rejected);
    return UNITY_END();
}
