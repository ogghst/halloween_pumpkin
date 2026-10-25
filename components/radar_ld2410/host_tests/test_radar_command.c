/* Host unit tests for radar_command.c: command codec, synchronous
   transactions with an injected transport, and structured payload codecs
   (Unity, native build; main() lives in test_radar_parser.c). */
#include <string.h>
#include "unity.h"
#include "radar_protocol.h"

/* --- protocol v1.07 vectors --- */

/* §2.2.1 enter-config send frame, byte-exact. */
static const uint8_t k_enter_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFF, 0x00, 0x01, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* §2.2.1 enter-config ACK (status 0, proto ver 0x0001, buffer 0x0040). */
static const uint8_t k_ack_enter[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0xFF, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x40, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* §2.2.2 exit-config ACK (status 0). */
static const uint8_t k_ack_exit[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFE, 0x01, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* §2.2.2 exit-config send frame (12 bytes, no value). */
static const uint8_t k_exit_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0xFE, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* §2.2.2 exit-config ACK with status 1 (failure). */
static const uint8_t k_ack_exit_fail[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFE, 0x01, 0x01, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* §2.2.8 firmware-version ACK payload (after the status word). */
static const uint8_t k_fw_value[] = { 0x00, 0x00, 0x02, 0x01, 0x16, 0x24, 0x06, 0x22 };

/* §2.2.4 read-params ACK payload: N=8, max gates 8/8, motion 20 (0x14) and
 * static 25 (0x19) on gates 0..8, unmanned duration 5 s. */
static const uint8_t k_params_value[] = {
    0xAA, 0x08, 0x08, 0x08,
    0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
    0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19, 0x19,
    0x05, 0x00
};

/* --- injected transport --- */

typedef struct {
    radar_command_io_t io;
    const uint8_t *stream; /* queued response bytes */
    size_t len;
    size_t pos;
    size_t chunk;          /* max bytes returned per read */
    int zero_reads;        /* reads that found nothing available */
    uint8_t log[128];      /* every byte ever written, in order */
    size_t log_len;
} fake_io_t;

static int fake_write(void *ctx, const uint8_t *buf, size_t n)
{
    fake_io_t *f = ctx;
    if (f->log_len + n > sizeof(f->log))
        return -1;
    memcpy(&f->log[f->log_len], buf, n);
    f->log_len += n;
    return 0;
}

static int fake_read(void *ctx, uint8_t *buf, size_t n)
{
    fake_io_t *f = ctx;
    if (f->pos >= f->len) {
        f->zero_reads++;
        return 0;
    }
    size_t got = f->len - f->pos;
    if (got > n)
        got = n;
    if (got > f->chunk)
        got = f->chunk;
    memcpy(buf, &f->stream[f->pos], got);
    f->pos += got;
    return (int)got;
}

static void fake_io_init(fake_io_t *f, const uint8_t *stream, size_t len,
                         size_t chunk, uint32_t timeout_ms)
{
    memset(f, 0, sizeof(*f));
    f->stream = stream;
    f->len = len;
    f->chunk = chunk;
    f->io.write = fake_write;
    f->io.read = fake_read;
    f->io.ctx = f;
    f->io.timeout_ms = timeout_ms;
}

/* 1. radar_cmd_build reproduces the §2.2.1 send vector byte per byte. */
void test_cmd_build_enter_config_matches_doc(void)
{
    uint8_t out[32];
    static const uint8_t k_value[] = { 0x01, 0x00 };

    const int n = radar_cmd_build(out, sizeof(out), RADAR_CMD_ENTER_CONFIG,
                                  k_value, sizeof(k_value));
    TEST_ASSERT_EQUAL_INT((int)sizeof(k_enter_send), n);
    TEST_ASSERT_EQUAL_MEMORY(k_enter_send, out, sizeof(k_enter_send));
}

/* 2. LIMITS value: 3 pairs, frame length field 0x14, byte-exact vs §2.2.3. */
void test_cmd_build_limits_frame(void)
{
    uint8_t v[24] = { 0 };
    uint8_t frame[40];

    TEST_ASSERT_EQUAL_INT(18, radar_params_build_limits(8, 8, 5, v, sizeof(v)));

    static const uint8_t k_expect[] = {
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x05, 0x00, 0x00, 0x00
    };
    TEST_ASSERT_EQUAL_MEMORY(k_expect, v, sizeof(k_expect));

    const int n = radar_cmd_build(frame, sizeof(frame), RADAR_CMD_LIMITS, v, 18);
    TEST_ASSERT_EQUAL_INT(30, n);
    TEST_ASSERT_EQUAL_HEX8(0x14, frame[4]); /* declared len 20 = 2 + 18 */
    TEST_ASSERT_EQUAL_HEX8(0x00, frame[5]);

    static const uint8_t k_doc[] = {
        0xFD, 0xFC, 0xFB, 0xFA, 0x14, 0x00, 0x60, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
    };
    TEST_ASSERT_EQUAL_MEMORY(k_doc, frame, sizeof(k_doc));
}

/* 3. Sensitivity value for gate 3 and for all gates, vs §2.2.7 vectors. */
void test_cmd_build_sensitivity_gate_and_all(void)
{
    uint8_t frame[40];
    uint8_t v[18] = { 0 };

    /* gate 3, motion 40 (0x28), static 40 */
    TEST_ASSERT_EQUAL_INT(18, radar_params_build_sensitivity(3, 40, 40, v, sizeof(v)));
    static const uint8_t k_v3[] = {
        0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x28, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x28, 0x00, 0x00, 0x00
    };
    TEST_ASSERT_EQUAL_MEMORY(k_v3, v, sizeof(k_v3));

    TEST_ASSERT_EQUAL_INT(30, radar_cmd_build(frame, sizeof(frame),
                                              RADAR_CMD_SET_SENSITIVITY, v, 18));
    static const uint8_t k_doc3[] = {
        0xFD, 0xFC, 0xFB, 0xFA, 0x14, 0x00, 0x64, 0x00,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x28, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x28, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
    };
    TEST_ASSERT_EQUAL_MEMORY(k_doc3, frame, sizeof(k_doc3));

    /* all gates (0xFFFF), same levels */
    TEST_ASSERT_EQUAL_INT(18, radar_params_build_sensitivity(RADAR_ALL_GATES, 40, 40,
                                                             v, sizeof(v)));
    v[2] = 0xFF; /* compare against the same value with the gate word patched */
    v[3] = 0xFF;
    static const uint8_t k_doc_all[] = {
        0xFD, 0xFC, 0xFB, 0xFA, 0x14, 0x00, 0x64, 0x00,
        0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x28, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x28, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
    };
    TEST_ASSERT_EQUAL_INT(30, radar_cmd_build(frame, sizeof(frame),
                                              RADAR_CMD_SET_SENSITIVITY, v, 18));
    TEST_ASSERT_EQUAL_MEMORY(k_doc_all, frame, sizeof(k_doc_all));
}

/* 4. Implausible arguments are rejected with -1. */
void test_cmd_build_rejects_bad_args(void)
{
    uint8_t out[32];
    static const uint8_t k_value[RADAR_CMD_VALUE_MAX + 1] = { 0 };

    TEST_ASSERT_EQUAL_INT(-1, radar_cmd_build(out, 4, RADAR_CMD_ENTER_CONFIG,
                                              k_value, 2)); /* short buffer */
    TEST_ASSERT_EQUAL_INT(-1, radar_cmd_build(NULL, sizeof(out),
                                              RADAR_CMD_ENTER_CONFIG, k_value, 2));
    TEST_ASSERT_EQUAL_INT(-1, radar_cmd_build(out, sizeof(out),
                                              RADAR_CMD_ENTER_CONFIG,
                                              k_value, sizeof(k_value))); /* too long */

    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_limits(1, 8, 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_limits(8, 9, 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_limits(8, 8, 5, out, 17));

    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_sensitivity(9, 40, 40, out,
                                                             sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_sensitivity(3, 101, 40, out,
                                                             sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_sensitivity(3, 40, 101, out,
                                                             sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_sensitivity(3, 40, 40, out, 10));
}

/* 5. Happy transaction: frame written, ACK consumed, status 0, ACK copied. */
void test_transact_happy_path(void)
{
    fake_io_t io;
    radar_parser_t parser;
    radar_frame_t ack;
    radar_parser_init(&parser);
    fake_io_init(&io, k_ack_enter, sizeof(k_ack_enter), 64, 50);

    const int rc = radar_command_transact(&io.io, &parser, RADAR_CMD_ENTER_CONFIG,
                                          k_enter_send + 8, 2, &ack);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_OK, rc);
    TEST_ASSERT_EQUAL_MEMORY(k_enter_send, io.log, sizeof(k_enter_send));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(sizeof(k_enter_send)), (uint32_t)(io.log_len));
    TEST_ASSERT_EQUAL(RADAR_FRAME_ACK, ack.kind);
    TEST_ASSERT_TRUE(radar_cmd_is_ack_of(&ack, RADAR_CMD_ENTER_CONFIG));
    TEST_ASSERT_EQUAL_HEX16(0, ack.ack.status);
    TEST_ASSERT_EQUAL_UINT8(4, ack.ack.value_len);
    static const uint8_t k_value[4] = { 0x01, 0x00, 0x40, 0x00 };
    TEST_ASSERT_EQUAL_MEMORY(k_value, ack.ack.value, sizeof(k_value));
    TEST_ASSERT_EQUAL_UINT32(1, parser.frames_ok);
}

/* 6. ACK with status 1 maps to RADAR_CMD_ERR_STATUS (frame still copied). */
void test_transact_ack_status_one(void)
{
    fake_io_t io;
    radar_parser_t parser;
    radar_frame_t ack;
    radar_parser_init(&parser);
    fake_io_init(&io, k_ack_exit_fail, sizeof(k_ack_exit_fail), 64, 50);

    const int rc = radar_command_transact(&io.io, &parser, RADAR_CMD_EXIT_CONFIG,
                                          NULL, 0, &ack);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_ERR_STATUS, rc);
    TEST_ASSERT_EQUAL_HEX16(0x01FE, ack.ack.command);
    TEST_ASSERT_EQUAL_HEX16(1, ack.ack.status);
    TEST_ASSERT_EQUAL_UINT8(0, ack.ack.value_len);
}

/* 7. Silent transport: ERR_TIMEOUT comes back within the poll budget. */
void test_transact_timeout_budget(void)
{
    fake_io_t io;
    radar_parser_t parser;
    radar_parser_init(&parser);
    fake_io_init(&io, NULL, 0, 64, 25); /* never answers, budget 25 polls */

    const int rc = radar_command_transact(&io.io, &parser, RADAR_CMD_EXIT_CONFIG,
                                          NULL, 0, NULL);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_ERR_TIMEOUT, rc);
    TEST_ASSERT_EQUAL_INT(25, io.zero_reads); /* bounded by timeout_ms */
    TEST_ASSERT_EQUAL_MEMORY(k_exit_send, io.log, sizeof(k_exit_send)); /* sent */
    TEST_ASSERT_EQUAL_UINT32(sizeof(k_exit_send), (uint32_t)io.log_len);
}

/* 8. Reports and foreign ACKs on the way are skipped; the right ACK wins. */
void test_transact_ignores_reports_and_foreign_acks(void)
{
    /* basic report + engineering-on ACK (0x0062) + the enter-config ACK */
    static const uint8_t k_rep[] = {
        0xF4, 0xF3, 0xF2, 0xF1, 0x0D, 0x00, 0x02, 0xAA, 0x02, 0x51, 0x00,
        0x00, 0x00, 0x00, 0x3B, 0x00, 0x00, 0x55, 0x00, 0xF8, 0xF7, 0xF6, 0xF5
    };
    static const uint8_t k_ack_eng[] = {
        0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0x62, 0x01, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
    };
    uint8_t stream[64];
    memcpy(stream, k_rep, sizeof(k_rep));
    memcpy(stream + sizeof(k_rep), k_ack_eng, sizeof(k_ack_eng));
    memcpy(stream + sizeof(k_rep) + sizeof(k_ack_eng), k_ack_enter,
           sizeof(k_ack_enter));

    fake_io_t io;
    radar_parser_t parser;
    radar_frame_t ack;
    radar_parser_init(&parser);
    fake_io_init(&io, stream, sizeof(k_rep) + sizeof(k_ack_eng) + sizeof(k_ack_enter),
                 1, 50); /* one byte per read: exercises partial reads */

    const int rc = radar_command_transact(&io.io, &parser, RADAR_CMD_ENTER_CONFIG,
                                          k_enter_send + 8, 2, &ack);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(0x01FF, ack.ack.command);
    TEST_ASSERT_EQUAL_HEX16(0, ack.ack.status);
    TEST_ASSERT_EQUAL_UINT32(3, parser.frames_ok); /* report + foreign + right ACK */
    TEST_ASSERT_EQUAL_UINT32(0, parser.frames_bad);
}

/* Step helper: performs a real nested READ_PARAMS transaction. */
static int step_read_params(const radar_command_io_t *io, radar_parser_t *parser,
                            void *ctx)
{
    int *calls = ctx;
    (*calls)++;
    radar_frame_t ack;
    const int rc = radar_command_transact(io, parser, RADAR_CMD_READ_PARAMS,
                                          NULL, 0, &ack);
    return rc == RADAR_CMD_OK ? 42 : rc;
}

/* 9. Step failure does not prevent EXIT_CONFIG; the step result is returned. */
void test_session_sends_exit_after_step_failure(void)
{
    /* enter ACK ok, then silence for the step (timeout); the exit ACK is
     * still queued and gets consumed by the session's own EXIT_CONFIG. */
    uint8_t stream[64];
    memcpy(stream, k_ack_enter, sizeof(k_ack_enter));
    memcpy(stream + sizeof(k_ack_enter), k_ack_exit, sizeof(k_ack_exit));

    /* the step's own command frame (§2.2.4, no value) */
    static const uint8_t k_readparams_send[] = {
        0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0x61, 0x00, 0x04, 0x03, 0x02, 0x01
    };

    fake_io_t io;
    radar_parser_t parser;
    int calls = 0;
    radar_parser_init(&parser);
    fake_io_init(&io, stream, sizeof(k_ack_enter) + sizeof(k_ack_exit), 64, 10);

    const int rc = radar_command_session(&io.io, &parser, step_read_params, &calls);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_ERR_TIMEOUT, rc); /* the step error */
    TEST_ASSERT_EQUAL_INT(1, calls);                  /* step ran once */
    /* exactly three frames written: ENTER, step's READ_PARAMS, EXIT */
    TEST_ASSERT_EQUAL_UINT32(
        sizeof(k_enter_send) + sizeof(k_readparams_send) + sizeof(k_exit_send),
        (uint32_t)io.log_len);
    TEST_ASSERT_EQUAL_MEMORY(k_enter_send, &io.log[0], sizeof(k_enter_send));
    TEST_ASSERT_EQUAL_MEMORY(k_readparams_send, &io.log[sizeof(k_enter_send)],
                             sizeof(k_readparams_send));
    TEST_ASSERT_EQUAL_MEMORY(k_exit_send,
                             &io.log[sizeof(k_enter_send) + sizeof(k_readparams_send)],
                             sizeof(k_exit_send));
    TEST_ASSERT_EQUAL_UINT32(2, parser.frames_ok); /* enter + exit ACKs parsed */
}

/* 10. ENTER_CONFIG failure: no step runs and no exit is sent (without the
 * config-mode ACK any further command is invalid, §2.4.1); the enter error
 * is reported. */
void test_session_enter_failure_no_step(void)
{
    fake_io_t io;
    radar_parser_t parser;
    int calls = 0;
    radar_parser_init(&parser);
    fake_io_init(&io, NULL, 0, 64, 8); /* no ACK at all */

    const int rc = radar_command_session(&io.io, &parser, step_read_params, &calls);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_ERR_TIMEOUT, rc);
    TEST_ASSERT_EQUAL_INT(0, calls);                              /* no step */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(sizeof(k_enter_send)), (uint32_t)(io.log_len));  /* no exit */
    TEST_ASSERT_EQUAL_MEMORY(k_enter_send, io.log, sizeof(k_enter_send));
}

/* 11. READ_PARAMS payload: N=8, sensitivities and duration as transmitted. */
void test_params_parse_read_params_vector(void)
{
    radar_module_params_t params;
    memset(&params, 0, sizeof(params));

    TEST_ASSERT_EQUAL_INT(0, radar_params_parse(k_params_value,
                                                sizeof(k_params_value), &params));
    TEST_ASSERT_EQUAL_UINT8(8, params.max_gates);
    TEST_ASSERT_EQUAL_UINT8(8, params.max_moving_gate);
    TEST_ASSERT_EQUAL_UINT8(8, params.max_static_gate);
    for (int i = 0; i <= 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(20, params.motion_sensitivity[i]);
        TEST_ASSERT_EQUAL_UINT8(25, params.static_sensitivity[i]);
    }
    TEST_ASSERT_EQUAL_UINT16(5, params.unmanned_duration_s);
}

/* 12. Malformed params payloads: wrong marker, truncation, bad N. */
void test_params_parse_malformed(void)
{
    radar_module_params_t params;
    uint8_t bad[sizeof(k_params_value)];

    memcpy(bad, k_params_value, sizeof(k_params_value));
    bad[0] = 0xAB; /* marker must be 0xAA */
    TEST_ASSERT_EQUAL_INT(-1, radar_params_parse(bad, sizeof(bad), &params));

    memcpy(bad, k_params_value, sizeof(k_params_value));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_parse(bad, sizeof(bad) - 1, &params));

    memcpy(bad, k_params_value, sizeof(k_params_value));
    bad[1] = 0x09; /* N above the 8-gate protocol maximum */
    TEST_ASSERT_EQUAL_INT(-1, radar_params_parse(bad, sizeof(bad), &params));

    TEST_ASSERT_EQUAL_INT(-1, radar_params_parse(k_params_value, 2, &params));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_parse(NULL, sizeof(k_params_value),
                                                 &params));
}

/* 13. Firmware-version payload decodes to the documented text. */
void test_fw_version_parse_doc_vector(void)
{
    radar_fw_version_t v;
    memset(&v, 0, sizeof(v));

    TEST_ASSERT_EQUAL_INT(0, radar_fw_version_parse(k_fw_value, sizeof(k_fw_value),
                                                    &v));
    TEST_ASSERT_EQUAL_HEX16(0x0000, v.fw_type);
    TEST_ASSERT_EQUAL_HEX32(0x0102, v.major);
    TEST_ASSERT_EQUAL_HEX32(0x22062416, v.minor);
    TEST_ASSERT_EQUAL_STRING("V1.02.22062416", v.text);

    TEST_ASSERT_EQUAL_INT(-1, radar_fw_version_parse(k_fw_value, 7, &v));
}

/* 14. Resolution payload and gate width mapping (Table 8). */
void test_resolution_parse_and_gate_mm(void)
{
    uint16_t idx = 0xFFFF;
    static const uint8_t k_res[] = { 0x01, 0x00 }; /* 0.2 m per gate */

    TEST_ASSERT_EQUAL_INT(0, radar_resolution_parse(k_res, sizeof(k_res), &idx));
    TEST_ASSERT_EQUAL_HEX16(1, idx);
    TEST_ASSERT_EQUAL_UINT16(200, radar_gate_mm(idx));
    TEST_ASSERT_EQUAL_UINT16(750, radar_gate_mm(0)); /* factory default */
    TEST_ASSERT_EQUAL_UINT16(750, radar_gate_mm(7)); /* unknown -> default */

    TEST_ASSERT_EQUAL_INT(-1, radar_resolution_parse(k_res, 1, &idx));
    TEST_ASSERT_EQUAL_INT(-1, radar_resolution_parse(k_res, sizeof(k_res), NULL));
}
