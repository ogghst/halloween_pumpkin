/* Host unit tests for the wifi_prov pure core (Unity, native build).
 *
 * Coverage, per the Fase 4 contract:
 * - prov_pop_from_mac: fixed MAC -> "pump-010203", NUL at index 12,
 *   uppercase hex, degenerate inputs left untouched
 * - prov_mqtt_encode/decode roundtrip with every field set
 * - single-field payload: has_* set only for the present field
 * - unknown record type skipped, later valid records still accepted
 * - malformed inputs -> ESP_ERR_INVALID_ARG (value past buffer end,
 *   truncated trailing record, wrong version, port len != 2, tls len != 1,
 *   tls value > 1)
 * - strings always NUL-terminated, safe truncation at the CONFIG_MQTT_*_LEN
 *   limits, encode without the over-limit/absent fields, ESP_ERR_NO_MEM on
 *   a buffer smaller than the encoded size
 */
#include <string.h>

#include "unity.h"

#include "prov_mqtt_codec.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* --- prov_pop_from_mac --- */

static void test_pop_fixed_mac(void)
{
    const uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03 };
    char pop[13] = { 0 };
    prov_pop_from_mac(mac, pop);
    TEST_ASSERT_EQUAL_STRING("pump-010203", pop);
    TEST_ASSERT_EQUAL_HEX8('0', pop[5]);
    TEST_ASSERT_EQUAL_CHAR('\0', pop[12]); /* NUL exactly at 12 */
}

static void test_pop_uppercase_and_full_byte(void)
{
    const uint8_t mac[6] = { 0x11, 0x22, 0x33, 0xde, 0xad, 0xbe };
    char pop[13] = { 0 };
    prov_pop_from_mac(mac, pop);
    TEST_ASSERT_EQUAL_STRING("pump-DEADBE", pop);

    const uint8_t mac_ff[6] = { 0, 0, 0, 0xFF, 0xFF, 0xFF };
    prov_pop_from_mac(mac_ff, pop);
    TEST_ASSERT_EQUAL_STRING("pump-FFFFFF", pop);
}

static void test_pop_null_args_untouched(void)
{
    char pop[13];
    memset(pop, 'x', sizeof(pop));
    const uint8_t mac[6] = { 0, 0, 0, 1, 2, 3 };
    prov_pop_from_mac(NULL, pop);
    prov_pop_from_mac(mac, NULL);
    for (size_t i = 0; i < sizeof(pop); i++) {
        TEST_ASSERT_EQUAL_CHAR('x', pop[i]);
    }
}

/* --- decode/encode helpers --- */

static prov_mqtt_msg_t full_msg(void)
{
    prov_mqtt_msg_t m = { 0 };
    m.has_host = true;
    m.has_port = true;
    m.has_user = true;
    m.has_pass = true;
    m.has_tls = true;
    strcpy(m.host, "broker.casa.lan");
    m.port = 8883;
    strcpy(m.user, "zucca");
    strcpy(m.pass, "s3cret-pass");
    m.tls = true;
    return m;
}

static void test_roundtrip_full(void)
{
    prov_mqtt_msg_t in = full_msg();
    uint8_t buf[PROV_MQTT_TLV_MAX_LEN];
    size_t len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_encode(&in, buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE(len > 1u && len <= PROV_MQTT_TLV_MAX_LEN);
    TEST_ASSERT_EQUAL_HEX8(PROV_MQTT_TLV_VERSION, buf[0]);

    prov_mqtt_msg_t out;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_decode(buf, len, &out));
    TEST_ASSERT_TRUE(out.has_host && out.has_port && out.has_user &&
                     out.has_pass && out.has_tls);
    TEST_ASSERT_EQUAL_STRING("broker.casa.lan", out.host);
    TEST_ASSERT_EQUAL_UINT16(8883, out.port);
    TEST_ASSERT_EQUAL_STRING("zucca", out.user);
    TEST_ASSERT_EQUAL_STRING("s3cret-pass", out.pass);
    TEST_ASSERT_TRUE(out.tls);
}

static void test_roundtrip_port_little_endian_and_tls_false(void)
{
    prov_mqtt_msg_t in = { 0 };
    in.has_port = true;
    in.has_tls = true;
    in.port = 0x1234;
    in.tls = false;

    uint8_t buf[PROV_MQTT_TLV_MAX_LEN];
    size_t len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_encode(&in, buf, sizeof(buf), &len));

    /* port record: type 2, len 2 LE, value u16 LE */
    TEST_ASSERT_EQUAL_HEX8(PROV_MQTT_TLV_PORT, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[3]);
    TEST_ASSERT_EQUAL_HEX8(0x34, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0x12, buf[5]);

    prov_mqtt_msg_t out;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_UINT16(0x1234, out.port);
    TEST_ASSERT_FALSE(out.tls);
    TEST_ASSERT_TRUE(out.has_port && out.has_tls);
    TEST_ASSERT_FALSE(out.has_host || out.has_user || out.has_pass);
}

static void test_single_field_only_has_flag(void)
{
    prov_mqtt_msg_t in = { 0 };
    in.has_host = true;
    strcpy(in.host, "h");

    uint8_t buf[PROV_MQTT_TLV_MAX_LEN];
    size_t len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_encode(&in, buf, sizeof(buf), &len));

    prov_mqtt_msg_t out;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_decode(buf, len, &out));
    TEST_ASSERT_TRUE(out.has_host);
    TEST_ASSERT_FALSE(out.has_port || out.has_user || out.has_pass ||
                      out.has_tls);
    TEST_ASSERT_EQUAL_STRING("h", out.host);
}

static void test_unknown_type_skipped_but_next_record_kept(void)
{
    /* version, host record, unknown type 0x7F with 2-byte value, port */
    uint8_t buf[] = {
        0x01,
        PROV_MQTT_TLV_HOST, 0x02, 0x00, 'n', '1',
        0x7F, 0x02, 0x00, 0xEE, 0xEE, /* unknown: discarded */
        PROV_MQTT_TLV_PORT, 0x02, 0x00, 0x39, 0x05, /* 1337 */
    };
    prov_mqtt_msg_t out;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_decode(buf, sizeof(buf), &out));
    TEST_ASSERT_TRUE(out.has_host);
    TEST_ASSERT_EQUAL_STRING("n1", out.host);
    TEST_ASSERT_EQUAL_UINT16(1337, out.port);
    TEST_ASSERT_FALSE(out.has_user || out.has_pass || out.has_tls);
}

static void test_decode_invalid_cases(void)
{
    prov_mqtt_msg_t out;

    /* NULL args and empty buffer */
    uint8_t one[] = { 0x01 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, prov_mqtt_decode(NULL, 1, &out));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, prov_mqtt_decode(one, 1, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, prov_mqtt_decode(one, 0, &out));

    /* wrong version */
    uint8_t bad_ver[] = { 0x02 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      prov_mqtt_decode(bad_ver, sizeof(bad_ver), &out));

    /* record length runs past the buffer */
    uint8_t over[] = { 0x01, PROV_MQTT_TLV_HOST, 0x10, 0x00, 'a', 'b' };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      prov_mqtt_decode(over, sizeof(over), &out));

    /* truncated trailing record header (2 bytes) */
    uint8_t trunc[] = { 0x01, 0x03, 0x00 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      prov_mqtt_decode(trunc, sizeof(trunc), &out));

    /* port with len 1 */
    uint8_t port1[] = { 0x01, PROV_MQTT_TLV_PORT, 0x01, 0x00, 0x9C };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      prov_mqtt_decode(port1, sizeof(port1), &out));

    /* tls with len 2 */
    uint8_t tls2[] = { 0x01, PROV_MQTT_TLV_TLS, 0x02, 0x00, 0x01, 0x01 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      prov_mqtt_decode(tls2, sizeof(tls2), &out));

    /* tls value 2 */
    uint8_t tls_bad[] = { 0x01, PROV_MQTT_TLV_TLS, 0x01, 0x00, 0x02 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      prov_mqtt_decode(tls_bad, sizeof(tls_bad), &out));
}

static void test_encode_invalid_cases(void)
{
    prov_mqtt_msg_t in = full_msg();
    uint8_t buf[PROV_MQTT_TLV_MAX_LEN];
    size_t len = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, prov_mqtt_encode(NULL, buf,
                                                            sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, prov_mqtt_encode(&in, NULL,
                                                            sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, prov_mqtt_encode(&in, buf,
                                                            sizeof(buf), NULL));

    /* not NUL-terminated string within the buffer */
    prov_mqtt_msg_t unterminated = { 0 };
    unterminated.has_host = true;
    memset(unterminated.host, 'A', sizeof(unterminated.host));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      prov_mqtt_encode(&unterminated, buf, sizeof(buf), &len));

    /* buffer too small: version alone does not fit */
    uint8_t tiny[1];
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      prov_mqtt_encode(&in, tiny, sizeof(tiny), &len));

    /* buffer large enough for the version but not for the records */
    uint8_t small[4];
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      prov_mqtt_encode(&in, small, sizeof(small), &len));
}

static void test_strings_nul_guaranteed_and_safe_truncation(void)
{
    /* 100-byte host value: truncated to CONFIG_MQTT_HOST_LEN-1 chars + NUL */
    uint8_t buf[1u + 3u + 100u];
    size_t pos = 0;
    buf[pos++] = PROV_MQTT_TLV_VERSION;
    buf[pos++] = PROV_MQTT_TLV_HOST;
    buf[pos++] = 100u & 0xFFu;
    buf[pos++] = 0u;
    for (size_t i = 0; i < 100u; i++) {
        buf[pos++] = (uint8_t)('a' + (i % 26u));
    }
    prov_mqtt_msg_t out;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_decode(buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_CHAR('\0', out.host[CONFIG_MQTT_HOST_LEN - 1u]);
    TEST_ASSERT_EQUAL_UINT8('a', (uint8_t)out.host[0]);
    for (size_t i = 0; i < CONFIG_MQTT_HOST_LEN - 1u; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)('a' + (i % 26u)),
                                (uint8_t)out.host[i]);
    }

    /* exactly full-length strings survive the roundtrip unchanged */
    prov_mqtt_msg_t in = { 0 };
    in.has_host = true;
    in.has_user = true;
    in.has_pass = true;
    memset(in.host, 'H', CONFIG_MQTT_HOST_LEN - 1u);
    in.host[CONFIG_MQTT_HOST_LEN - 1u] = '\0';
    memset(in.user, 'U', CONFIG_MQTT_USER_LEN - 1u);
    in.user[CONFIG_MQTT_USER_LEN - 1u] = '\0';
    memset(in.pass, 'P', CONFIG_MQTT_PASS_LEN - 1u);
    in.pass[CONFIG_MQTT_PASS_LEN - 1u] = '\0';

    uint8_t enc[PROV_MQTT_TLV_MAX_LEN];
    size_t len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_encode(&in, enc, sizeof(enc), &len));

    prov_mqtt_msg_t out2;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_decode(enc, len, &out2));
    TEST_ASSERT_EQUAL_MEMORY(in.host, out2.host, CONFIG_MQTT_HOST_LEN);
    TEST_ASSERT_EQUAL_MEMORY(in.user, out2.user, CONFIG_MQTT_USER_LEN);
    TEST_ASSERT_EQUAL_MEMORY(in.pass, out2.pass, CONFIG_MQTT_PASS_LEN);
}

static void test_decode_zeroes_absent_fields(void)
{
    uint8_t buf[] = { PROV_MQTT_TLV_VERSION };
    prov_mqtt_msg_t out;
    memset(&out, 0xAA, sizeof(out)); /* decode must zero first */
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_decode(buf, sizeof(buf), &out));
    TEST_ASSERT_FALSE(out.has_host || out.has_port || out.has_user ||
                      out.has_pass || out.has_tls);
    TEST_ASSERT_EQUAL_UINT16(0, out.port);
    TEST_ASSERT_EQUAL_CHAR('\0', out.host[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', out.user[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', out.pass[0]);
    TEST_ASSERT_FALSE(out.tls);
}

static void test_last_duplicate_record_wins(void)
{
    uint8_t buf[] = {
        0x01,
        PROV_MQTT_TLV_PORT, 0x02, 0x00, 0x01, 0x00, /* 1 */
        PROV_MQTT_TLV_PORT, 0x02, 0x00, 0x02, 0x00, /* 2 (last wins) */
    };
    prov_mqtt_msg_t out;
    TEST_ASSERT_EQUAL(ESP_OK, prov_mqtt_decode(buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_UINT16(2, out.port);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pop_fixed_mac);
    RUN_TEST(test_pop_uppercase_and_full_byte);
    RUN_TEST(test_pop_null_args_untouched);
    RUN_TEST(test_roundtrip_full);
    RUN_TEST(test_roundtrip_port_little_endian_and_tls_false);
    RUN_TEST(test_single_field_only_has_flag);
    RUN_TEST(test_unknown_type_skipped_but_next_record_kept);
    RUN_TEST(test_decode_invalid_cases);
    RUN_TEST(test_encode_invalid_cases);
    RUN_TEST(test_strings_nul_guaranteed_and_safe_truncation);
    RUN_TEST(test_decode_zeroes_absent_fields);
    RUN_TEST(test_last_duplicate_record_wins);
    return UNITY_END();
}
