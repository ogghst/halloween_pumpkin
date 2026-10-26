/* Host unit tests for radar_command.c: command codec, synchronous
   transactions with an injected transport, and structured payload codecs
   (Unity, native build; main() lives in test_radar_parser.c).
   Vectors are the exact byte examples of the Hi-Link HLK-LD2420 protocol
   document, cross-checked with the ESPHome ld2420 component. */
#include <string.h>
#include "unity.h"
#include "radar_protocol.h"

/* --- LD2420 example vectors (byte-exact) --- */

/* Enter-config send frame: value = host protocol version 0x0001. */
static const uint8_t k_enter_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFF, 0x00, 0x01, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* Enter-config ACK: status 0, proto ver 0x0002, buffer 0x0020. */
static const uint8_t k_ack_enter[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0xFF, 0x01, 0x00, 0x00,
    0x02, 0x00, 0x20, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* Exit-config send frame (12 bytes, no value) and its ACK (status 0). */
static const uint8_t k_exit_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0xFE, 0x00, 0x04, 0x03, 0x02, 0x01
};
static const uint8_t k_ack_exit[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFE, 0x01, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* Exit-config ACK with status 1 (failure). */
static const uint8_t k_ack_exit_fail[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFE, 0x01, 0x01, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* Read-version send frame and ACK: status 0, len 6, "v1.5.34". */
static const uint8_t k_version_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
};
static const uint8_t k_ack_version[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x0C, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x06, 0x00, 0x76, 0x31, 0x2E, 0x35, 0x2E, 0x34, 0x04, 0x03, 0x02, 0x01
};
/* Read-version ACK value: status stripped, len 6, "v1.5.4" (the document
 * example; 154 is exactly the energy-capable boundary). */
static const uint8_t k_fw_value[] = {
    0x06, 0x00, 0x76, 0x31, 0x2E, 0x35, 0x2E, 0x34
};
/* Synthetic "v1.5.34" payload (len 7 + text). */
static const uint8_t k_fw_value_1534[] = {
    0x07, 0x00, 0x76, 0x31, 0x2E, 0x35, 0x2E, 0x33, 0x34
};

/* Restart send frame (no reply). */
static const uint8_t k_restart_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0x68, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* READ_PARAMS of trigger threshold g0 (name 0x0010) and its ACK (40000). */
static const uint8_t k_read_g0_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0x08, 0x00, 0x10, 0x00, 0x04, 0x03, 0x02, 0x01
};
static const uint8_t k_ack_read_g0[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0x08, 0x01, 0x00, 0x00,
    0x40, 0x9C, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* READ_PARAMS of trigger thresholds g0..g5: send (len 0x0E) and ACK with
 * 60000, 30000, 3000, 2000, 500, 400. */
static const uint8_t k_read_g0_5_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x0E, 0x00, 0x08, 0x00,
    0x10, 0x00, 0x11, 0x00, 0x12, 0x00, 0x13, 0x00, 0x14, 0x00, 0x15, 0x00,
    0x04, 0x03, 0x02, 0x01
};
static const uint8_t k_values_g0_5[] = {
    0x60, 0xEA, 0x00, 0x00, 0x30, 0x75, 0x00, 0x00,
    0xB8, 0x0B, 0x00, 0x00, 0xD0, 0x07, 0x00, 0x00,
    0xF4, 0x01, 0x00, 0x00, 0x90, 0x01, 0x00, 0x00
};

/* WRITE_PARAMS of min gate 0 (value 0): single pair, byte-exact. */
static const uint8_t k_write_min0_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0x07, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* WRITE_SYS_PARAM, system mode energy (0x04): byte-exact. */
static const uint8_t k_write_sysmode_energy_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0x12, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* READ_PARAMS of min gate (name 0x0000), used by the session step. */
static const uint8_t k_read_min_send[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* Valid energy frame (45 B) riding the same UART as the ACKs. */
static const uint8_t k_energy_frame[] = {
    0xF4, 0xF3, 0xF2, 0xF1, 0x23, 0x00, 0x01, 0x1E, 0x00,
    0x64, 0x00, 0x32, 0x00, 0x00, 0x10, 0x00, 0x02, 0x07, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x01, 0x01, 0x00,
    0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00,
    0x07, 0x00, 0xF8, 0xF7, 0xF6, 0xF5
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

/* 1. radar_cmd_build reproduces the enter-config send vector byte per byte. */
void test_cmd_build_enter_config_matches_doc(void)
{
    uint8_t out[32];
    static const uint8_t k_value[] = { 0x01, 0x00 };

    const int n = radar_cmd_build(out, sizeof(out), RADAR_CMD_ENTER_CONFIG,
                                  k_value, sizeof(k_value));
    TEST_ASSERT_EQUAL_INT((int)sizeof(k_enter_send), n);
    TEST_ASSERT_EQUAL_MEMORY(k_enter_send, out, sizeof(k_enter_send));
}

/* 2. Read-version send frame (command 0x0000, no value). */
void test_cmd_build_read_version(void)
{
    uint8_t out[32];

    const int n = radar_cmd_build(out, sizeof(out), RADAR_CMD_READ_VERSION,
                                  NULL, 0);
    TEST_ASSERT_EQUAL_INT((int)sizeof(k_version_send), n);
    TEST_ASSERT_EQUAL_MEMORY(k_version_send, out, sizeof(k_version_send));
}

/* 3. Restart send frame (command 0x0068, no value). */
void test_cmd_build_restart(void)
{
    uint8_t out[32];

    const int n = radar_cmd_build(out, sizeof(out), RADAR_CMD_RESTART, NULL, 0);
    TEST_ASSERT_EQUAL_INT((int)sizeof(k_restart_send), n);
    TEST_ASSERT_EQUAL_MEMORY(k_restart_send, out, sizeof(k_restart_send));
}

/* 4. Gate-limits value: 3 pairs (min gate, max gate, timeout) plus the
 * range validations of the LD2420 parameter table. */
void test_gate_limits_build_and_validation(void)
{
    uint8_t v[24] = { 0 };
    uint8_t frame[40];

    TEST_ASSERT_EQUAL_INT(18, radar_params_build_gate_limits(0, 12, 5, v, sizeof(v)));
    static const uint8_t k_expect[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* min gate 0 */
        0x01, 0x00, 0x0C, 0x00, 0x00, 0x00, /* max gate 12 (factory) */
        0x04, 0x00, 0x05, 0x00, 0x00, 0x00  /* timeout 5 s */
    };
    TEST_ASSERT_EQUAL_MEMORY(k_expect, v, sizeof(k_expect));

    const int n = radar_cmd_build(frame, sizeof(frame), RADAR_CMD_WRITE_PARAMS,
                                  v, 18);
    TEST_ASSERT_EQUAL_INT(30, n);
    TEST_ASSERT_EQUAL_HEX8(0x14, frame[4]); /* declared len 20 = 2 + 18 */
    TEST_ASSERT_EQUAL_HEX8(0x07, frame[6]); /* WRITE_PARAMS */
    TEST_ASSERT_EQUAL_HEX8(0x00, frame[7]);

    /* Boundary still valid; out-of-range rejected. */
    TEST_ASSERT_EQUAL_INT(18, radar_params_build_gate_limits(15, 15, 255, v, sizeof(v)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_gate_limits(5, 3, 5, v, sizeof(v)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_gate_limits(0, 16, 5, v, sizeof(v)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_gate_limits(0, 12, 256, v, sizeof(v)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_gate_limits(0, 12, 5, v, 17));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_gate_limits(0, 12, 5, NULL, 18));
}

/* 5. All-thresholds value: 32 pairs, names 0x0010..0x002F consecutive,
 * move bank first, then keep bank. */
void test_build_all_thresholds_layout(void)
{
    uint32_t move[RADAR_MAX_GATES];
    uint32_t still[RADAR_MAX_GATES];
    uint8_t v[2u * RADAR_MAX_GATES * 6u];

    for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
        move[g] = 100u + g;
        still[g] = 200u + g;
    }

    const int n = radar_params_build_all_thresholds(move, still, v, sizeof(v));
    TEST_ASSERT_EQUAL_INT((int)sizeof(v), n);

    for (unsigned i = 0; i < 2u * RADAR_MAX_GATES; i++) {
        const uint16_t name = (uint16_t)(v[6u * i] | ((uint16_t)v[6u * i + 1] << 8));
        TEST_ASSERT_EQUAL_HEX16(0x0010u + i, name); /* consecutive names */
        const uint32_t val = (uint32_t)v[6u * i + 2] |
                             ((uint32_t)v[6u * i + 3] << 8) |
                             ((uint32_t)v[6u * i + 4] << 16) |
                             ((uint32_t)v[6u * i + 5] << 24);
        TEST_ASSERT_EQUAL_UINT32(i < RADAR_MAX_GATES ? 100u + i
                                                     : 200u + i - RADAR_MAX_GATES,
                                 val);
    }
    TEST_ASSERT_EQUAL_HEX8(0x10, v[0]); /* first pair names move gate 0 */

    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_all_thresholds(move, still, v,
                                                                sizeof(v) - 1));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_all_thresholds(NULL, still, v,
                                                                sizeof(v)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_all_thresholds(move, NULL, v,
                                                                sizeof(v)));
}

/* 6. READ_PARAMS value and full frame for trigger thresholds g0..g5,
 * byte-exact vs the document example (declared len 0x0E). */
void test_build_read_multi_names(void)
{
    uint8_t v[24];
    uint8_t frame[40];
    const uint16_t names[6] = { 0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015 };

    TEST_ASSERT_EQUAL_INT(12, radar_params_build_read(names, 6, v, sizeof(v)));
    static const uint8_t k_expect[] = {
        0x10, 0x00, 0x11, 0x00, 0x12, 0x00, 0x13, 0x00, 0x14, 0x00, 0x15, 0x00
    };
    TEST_ASSERT_EQUAL_MEMORY(k_expect, v, sizeof(k_expect));

    const int n = radar_cmd_build(frame, sizeof(frame), RADAR_CMD_READ_PARAMS,
                                  v, 12);
    TEST_ASSERT_EQUAL_INT((int)sizeof(k_read_g0_5_send), n);
    TEST_ASSERT_EQUAL_MEMORY(k_read_g0_5_send, frame, sizeof(k_read_g0_5_send));

    /* Single name matches the g0 example; too many names rejected. */
    const uint16_t one = 0x0010;
    TEST_ASSERT_EQUAL_INT(2, radar_params_build_read(&one, 1, v, sizeof(v)));
    TEST_ASSERT_EQUAL_MEMORY(k_read_g0_send + 8, v, 2);
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_read(names, RADAR_MAX_GATES + 1,
                                                      v, sizeof(v)));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_read(names, 6, v, 11));
}

/* 7. READ_PARAMS ACK payload: six u32 values in request order. */
void test_parse_values_six_vector(void)
{
    uint32_t values[6] = { 0 };

    TEST_ASSERT_EQUAL_INT(0, radar_params_parse_values(k_values_g0_5,
                                                       sizeof(k_values_g0_5),
                                                       values, 6));
    TEST_ASSERT_EQUAL_UINT32(60000, values[0]);
    TEST_ASSERT_EQUAL_UINT32(30000, values[1]);
    TEST_ASSERT_EQUAL_UINT32(3000, values[2]);
    TEST_ASSERT_EQUAL_UINT32(2000, values[3]);
    TEST_ASSERT_EQUAL_UINT32(500, values[4]);
    TEST_ASSERT_EQUAL_UINT32(400, values[5]);

    /* Single value (40000 = 0x9C40) matches the g0 example. */
    uint32_t one = 0;
    TEST_ASSERT_EQUAL_INT(0, radar_params_parse_values(k_ack_read_g0 + 10, 4,
                                                       &one, 1));
    TEST_ASSERT_EQUAL_UINT32(40000, one);
}

/* 8. Truncated or implausible ACK payloads are rejected. */
void test_parse_values_truncated(void)
{
    uint32_t values[6];

    TEST_ASSERT_EQUAL_INT(-1, radar_params_parse_values(k_values_g0_5,
                                                        sizeof(k_values_g0_5) - 1,
                                                        values, 6));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_parse_values(NULL, 24, values, 6));
    TEST_ASSERT_EQUAL_INT(-1, radar_params_parse_values(k_values_g0_5, 24, NULL, 6));
    TEST_ASSERT_EQUAL_INT(0, radar_params_parse_values(NULL, 0, NULL, 0));
}

/* 9. WRITE_PARAMS of a single pair, byte-exact vs the min-gate-0 example. */
void test_build_write_single_pair(void)
{
    uint8_t v[12];
    uint8_t frame[40];
    const uint16_t names[1] = { RADAR_PARAM_MIN_GATE };
    const uint32_t values[1] = { 0 };

    TEST_ASSERT_EQUAL_INT(6, radar_params_build_write(names, values, 1, v, sizeof(v)));
    static const uint8_t k_expect[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_MEMORY(k_expect, v, sizeof(k_expect));

    const int n = radar_cmd_build(frame, sizeof(frame), RADAR_CMD_WRITE_PARAMS,
                                  v, 6);
    TEST_ASSERT_EQUAL_INT((int)sizeof(k_write_min0_send), n);
    TEST_ASSERT_EQUAL_MEMORY(k_write_min0_send, frame, sizeof(k_write_min0_send));

    /* More pairs than one frame can carry are rejected. */
    uint16_t names18[18];
    uint32_t values18[18];
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_write(names18, values18, 19,
                                                       v, sizeof(v)));
}

/* 10. WRITE_SYS_PARAM value and frame, byte-exact vs the energy-mode
 * example; running mode (0x64) and debug (0x00) build the same shape. */
void test_build_sysmode_energy(void)
{
    uint8_t v[12];
    uint8_t frame[40];

    TEST_ASSERT_EQUAL_INT(6, radar_params_build_sysmode(RADAR_SYSMODE_ENERGY,
                                                        v, sizeof(v)));
    static const uint8_t k_expect[] = { 0x00, 0x00, 0x04, 0x00, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_MEMORY(k_expect, v, sizeof(k_expect));

    const int n = radar_cmd_build(frame, sizeof(frame), RADAR_CMD_WRITE_SYS_PARAM,
                                  v, 6);
    TEST_ASSERT_EQUAL_INT((int)sizeof(k_write_sysmode_energy_send), n);
    TEST_ASSERT_EQUAL_MEMORY(k_write_sysmode_energy_send, frame,
                             sizeof(k_write_sysmode_energy_send));

    TEST_ASSERT_EQUAL_INT(6, radar_params_build_sysmode(RADAR_SYSMODE_RUNNING,
                                                        v, sizeof(v)));
    TEST_ASSERT_EQUAL_HEX8(0x64, v[2]);
    TEST_ASSERT_EQUAL_INT(-1, radar_params_build_sysmode(0x04, v, 5));
}

/* 11. Happy transaction: frame written, ACK consumed, status 0, ACK copied. */
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
    static const uint8_t k_value[4] = { 0x02, 0x00, 0x20, 0x00 };
    TEST_ASSERT_EQUAL_MEMORY(k_value, ack.ack.value, sizeof(k_value));
    TEST_ASSERT_EQUAL_UINT32(1, parser.frames_ok);
}

/* 12. ACK with status 1 maps to RADAR_CMD_ERR_STATUS (frame still copied). */
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

/* 13. Silent transport: ERR_TIMEOUT comes back within the poll budget. */
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

/* 14. Energy frames and foreign ACKs on the way are skipped; the right ACK
 * wins even with one byte per read. */
void test_transact_ignores_reports_and_foreign_acks(void)
{
    uint8_t stream[128];
    size_t off = 0;

    memcpy(stream + off, k_energy_frame, sizeof(k_energy_frame));
    off += sizeof(k_energy_frame);
    memcpy(stream + off, k_ack_read_g0, sizeof(k_ack_read_g0));
    off += sizeof(k_ack_read_g0);
    memcpy(stream + off, k_ack_enter, sizeof(k_ack_enter));
    off += sizeof(k_ack_enter);

    fake_io_t io;
    radar_parser_t parser;
    radar_frame_t ack;
    radar_parser_init(&parser);
    fake_io_init(&io, stream, off, 1, 200); /* one byte per read */

    const int rc = radar_command_transact(&io.io, &parser, RADAR_CMD_ENTER_CONFIG,
                                          k_enter_send + 8, 2, &ack);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(0x01FF, ack.ack.command);
    TEST_ASSERT_EQUAL_HEX16(0, ack.ack.status);
    TEST_ASSERT_EQUAL_UINT32(3, parser.frames_ok); /* energy + foreign + right ACK */
    TEST_ASSERT_EQUAL_UINT32(0, parser.frames_bad);
    TEST_ASSERT_EQUAL_UINT32(0, parser.lines_parsed); /* binary frames only */
}

/* Step helper: performs a real nested READ_PARAMS transaction. */
static int step_read_min(const radar_command_io_t *io, radar_parser_t *parser,
                         void *ctx)
{
    int *calls = ctx;
    (*calls)++;
    uint8_t value[2];
    TEST_ASSERT_EQUAL_INT(2, radar_params_build_read((const uint16_t[]) { 0x0000 },
                                                     1, value, sizeof(value)));
    return radar_command_transact(io, parser, RADAR_CMD_READ_PARAMS,
                                  value, sizeof(value), NULL);
}

/* 15. Step failure does not prevent EXIT_CONFIG; the step result is returned. */
void test_session_sends_exit_after_step_failure(void)
{
    /* enter ACK ok, then the step finds only the exit ACK (which it skips)
     * and times out; the session still sends EXIT_CONFIG. */
    uint8_t stream[64];
    memcpy(stream, k_ack_enter, sizeof(k_ack_enter));
    memcpy(stream + sizeof(k_ack_enter), k_ack_exit, sizeof(k_ack_exit));

    fake_io_t io;
    radar_parser_t parser;
    int calls = 0;
    radar_parser_init(&parser);
    fake_io_init(&io, stream, sizeof(k_ack_enter) + sizeof(k_ack_exit), 64, 10);

    const int rc = radar_command_session(&io.io, &parser, step_read_min, &calls);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_ERR_TIMEOUT, rc); /* the step error */
    TEST_ASSERT_EQUAL_INT(1, calls);                  /* step ran once */
    /* exactly three frames written: ENTER, step's READ_PARAMS, EXIT */
    TEST_ASSERT_EQUAL_UINT32(
        sizeof(k_enter_send) + sizeof(k_read_min_send) + sizeof(k_exit_send),
        (uint32_t)io.log_len);
    TEST_ASSERT_EQUAL_MEMORY(k_enter_send, &io.log[0], sizeof(k_enter_send));
    TEST_ASSERT_EQUAL_MEMORY(k_read_min_send, &io.log[sizeof(k_enter_send)],
                             sizeof(k_read_min_send));
    TEST_ASSERT_EQUAL_MEMORY(k_exit_send,
                             &io.log[sizeof(k_enter_send) + sizeof(k_read_min_send)],
                             sizeof(k_exit_send));
    TEST_ASSERT_EQUAL_UINT32(2, parser.frames_ok); /* enter + exit ACKs parsed */
}

/* 16. ENTER_CONFIG failure: no step runs and no exit is sent (without the
 * config-mode ACK any further command is invalid); the enter error is
 * reported. */
void test_session_enter_failure_no_step(void)
{
    fake_io_t io;
    radar_parser_t parser;
    int calls = 0;
    radar_parser_init(&parser);
    fake_io_init(&io, NULL, 0, 64, 8); /* no ACK at all */

    const int rc = radar_command_session(&io.io, &parser, step_read_min, &calls);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_ERR_TIMEOUT, rc);
    TEST_ASSERT_EQUAL_INT(0, calls);                     /* no step */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(sizeof(k_enter_send)), (uint32_t)(io.log_len));
    TEST_ASSERT_EQUAL_MEMORY(k_enter_send, io.log, sizeof(k_enter_send));
}

/* 17. Firmware-version payload: text, concatenated digits, energy flag. */
void test_fw_version_parse_vectors(void)
{
    radar_fw_version_t v;

    /* Document ACK value: len 6, "v1.5.4" -> 154, exactly the boundary. */
    memset(&v, 0, sizeof(v));
    TEST_ASSERT_EQUAL_INT(0, radar_fw_version_parse(k_fw_value, sizeof(k_fw_value), &v));
    TEST_ASSERT_EQUAL_STRING("v1.5.4", v.text);
    TEST_ASSERT_EQUAL_INT32(154, v.numeric);
    TEST_ASSERT_TRUE(v.energy_capable);

    /* "v1.5.34": digits concatenated -> 1534, energy frames available. */
    memset(&v, 0, sizeof(v));
    TEST_ASSERT_EQUAL_INT(0, radar_fw_version_parse(k_fw_value_1534,
                                                    sizeof(k_fw_value_1534), &v));
    TEST_ASSERT_EQUAL_STRING("v1.5.34", v.text);
    TEST_ASSERT_EQUAL_INT32(1534, v.numeric);
    TEST_ASSERT_TRUE(v.energy_capable);

    /* "v1.5.3": one digit less, no energy frames. */
    static const uint8_t k_153[] = { 0x06, 0x00, 'v', '1', '.', '5', '.', '3' };
    memset(&v, 0, sizeof(v));
    TEST_ASSERT_EQUAL_INT(0, radar_fw_version_parse(k_153, sizeof(k_153), &v));
    TEST_ASSERT_EQUAL_STRING("v1.5.3", v.text);
    TEST_ASSERT_EQUAL_INT32(153, v.numeric);
    TEST_ASSERT_FALSE(v.energy_capable);

    /* "1.4.9" without the leading v. */
    static const uint8_t k_149[] = { 0x05, 0x00, '1', '.', '4', '.', '9' };
    memset(&v, 0, sizeof(v));
    TEST_ASSERT_EQUAL_INT(0, radar_fw_version_parse(k_149, sizeof(k_149), &v));
    TEST_ASSERT_EQUAL_STRING("1.4.9", v.text);
    TEST_ASSERT_EQUAL_INT32(149, v.numeric);
    TEST_ASSERT_FALSE(v.energy_capable);

    /* Truncated: declared length exceeds the available bytes. */
    TEST_ASSERT_EQUAL_INT(-1, radar_fw_version_parse(k_fw_value, 7, &v));
    TEST_ASSERT_EQUAL_INT(-1, radar_fw_version_parse(k_fw_value, 1, &v));
    TEST_ASSERT_EQUAL_INT(-1, radar_fw_version_parse(NULL, sizeof(k_fw_value), &v));

    /* Text longer than the storage is cut at 15 chars (v and dots count);
     * the remaining digits (123456789012) exceed int32 and saturate. */
    static const uint8_t k_long[] = { 0x12, 0x00, 'v', '1', '.', '2', '.',
                                      '3', '4', '5', '6', '7', '8', '9',
                                      '0', '1', '2', '3', '4', '5' };
    memset(&v, 0, sizeof(v));
    TEST_ASSERT_EQUAL_INT(0, radar_fw_version_parse(k_long, sizeof(k_long), &v));
    TEST_ASSERT_EQUAL_UINT8(15, (uint8_t)strlen(v.text));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, v.numeric);
}

/* 18. The full read-version transaction feeds radar_fw_version_parse. */
void test_fw_version_via_transact(void)
{
    fake_io_t io;
    radar_parser_t parser;
    radar_frame_t ack;
    radar_fw_version_t fw;
    radar_parser_init(&parser);
    fake_io_init(&io, k_ack_version, sizeof(k_ack_version), 64, 50);

    TEST_ASSERT_EQUAL_INT(RADAR_CMD_OK,
                          radar_command_transact(&io.io, &parser,
                                                 RADAR_CMD_READ_VERSION,
                                                 NULL, 0, &ack));
    TEST_ASSERT_EQUAL_MEMORY(k_version_send, io.log, sizeof(k_version_send));
    TEST_ASSERT_EQUAL_UINT8(8, ack.ack.value_len);
    TEST_ASSERT_EQUAL_MEMORY(k_fw_value, ack.ack.value, sizeof(k_fw_value));

    memset(&fw, 0, sizeof(fw));
    TEST_ASSERT_EQUAL_INT(0, radar_fw_version_parse(ack.ack.value,
                                                    ack.ack.value_len, &fw));
    TEST_ASSERT_EQUAL_STRING("v1.5.4", fw.text);
    TEST_ASSERT_EQUAL_INT32(154, fw.numeric);
    TEST_ASSERT_TRUE(fw.energy_capable);
}

/* 19. radar_cmd_is_ack_of matches only the masked command word. */
void test_cmd_is_ack_of(void)
{
    radar_parser_t parser;
    radar_frame_t f;
    radar_parser_init(&parser);

    bool got = false;
    for (size_t i = 0; i < sizeof(k_ack_version); i++)
        got = radar_parser_feed(&parser, k_ack_version[i], &f) || got;

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_TRUE(radar_cmd_is_ack_of(&f, RADAR_CMD_READ_VERSION));
    TEST_ASSERT_FALSE(radar_cmd_is_ack_of(&f, RADAR_CMD_ENTER_CONFIG));
    TEST_ASSERT_FALSE(radar_cmd_is_ack_of(NULL, RADAR_CMD_READ_VERSION));

    f.kind = RADAR_FRAME_REPORT;
    TEST_ASSERT_FALSE(radar_cmd_is_ack_of(&f, RADAR_CMD_READ_VERSION));
}
