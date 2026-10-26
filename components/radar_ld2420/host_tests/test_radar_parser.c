/* Host unit tests for radar_parser.c (Unity, native build).
   Also runs the radar_command.c tests (test_radar_command.c) and the
   radar_trigger.c tests (test_radar_trigger.c): a single main() lives here.
   Vectors are the exact byte examples of the Hi-Link HLK-LD2420 protocol
   document, cross-checked with the ESPHome ld2420 component.
   See CMakeLists.txt in this directory. */
#include <string.h>
#include "unity.h"
#include "radar_protocol.h"

/* Tests defined in test_radar_command.c. */
void test_cmd_build_enter_config_matches_doc(void);
void test_cmd_build_read_version(void);
void test_cmd_build_restart(void);
void test_gate_limits_build_and_validation(void);
void test_build_all_thresholds_layout(void);
void test_build_read_multi_names(void);
void test_parse_values_six_vector(void);
void test_parse_values_truncated(void);
void test_build_write_single_pair(void);
void test_build_sysmode_energy(void);
void test_transact_happy_path(void);
void test_transact_ack_status_one(void);
void test_transact_timeout_budget(void);
void test_transact_ignores_reports_and_foreign_acks(void);
void test_session_sends_exit_after_step_failure(void);
void test_session_enter_failure_no_step(void);
void test_fw_version_parse_vectors(void);
void test_fw_version_via_transact(void);
void test_cmd_is_ack_of(void);

/* Tests defined in test_radar_trigger.c. */
void test_trigger_confirm_exact_third_frame(void);
void test_trigger_confirm_run_reset_on_gap(void);
void test_trigger_hold_expiry_only_after_hold_ms(void);
void test_trigger_hysteresis_immediate_exit(void);
void test_trigger_hysteresis_band_refreshes_hold(void);
void test_trigger_distance_updated_on_change(void);
void test_trigger_leave_and_reenter_rearms(void);
void test_trigger_presence_zero_distance_not_qualifying(void);
void test_trigger_set_trigger_mm_clamps_runtime(void);
void test_trigger_confirm_one_fires_on_first_frame(void);
void test_trigger_null_args_noop(void);

void setUp(void)
{
}

void tearDown(void)
{
}

/* --- LD2420 example vectors (byte-exact) --- */

/* Enter-config ACK: status 0, proto ver 0x0002, buffer 0x0020. */
static const uint8_t k_ack_enter[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0xFF, 0x01, 0x00, 0x00,
    0x02, 0x00, 0x20, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* Exit-config ACK (status 0). */
static const uint8_t k_ack_exit[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFE, 0x01, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* Read-version ACK: status 0, len 6, "v1.5.34". */
static const uint8_t k_ack_version[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x0C, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x06, 0x00, 0x76, 0x31, 0x2E, 0x35, 0x2E, 0x34, 0x04, 0x03, 0x02, 0x01
};

/* Energy frame: presence 1, range 0x001E = 30 cm, energies 100, 50, then
 * 14 filler values; 45 bytes total. */
static const uint8_t k_energy_frame[] = {
    0xF4, 0xF3, 0xF2, 0xF1, 0x23, 0x00, 0x01, 0x1E, 0x00,
    0x64, 0x00, 0x32, 0x00, 0x00, 0x10, 0x00, 0x02, 0x07, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x01, 0x01, 0x00,
    0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00,
    0x07, 0x00, 0xF8, 0xF7, 0xF6, 0xF5
};

static const uint16_t k_energy_expect[RADAR_MAX_GATES] = {
    100, 50, 4096, 512, 7, 0xFFFF, 0, 255, 256, 1, 2, 3, 4, 5, 6, 7
};

static void feed_all(radar_parser_t *p, const uint8_t *bytes, size_t n,
                     radar_frame_t *out, bool *delivered)
{
    for (size_t i = 0; i < n; i++) {
        if (radar_parser_feed(p, bytes[i], out)) {
            if (delivered != NULL)
                *delivered = true;
        }
    }
}

/* Build a syntactically valid energy frame with arbitrary content. */
static size_t build_energy_frame(uint8_t *out, uint8_t presence,
                                 uint16_t range_cm, const uint16_t *energies)
{
    size_t i = 0;
    out[i++] = 0xF4;
    out[i++] = 0xF3;
    out[i++] = 0xF2;
    out[i++] = 0xF1;
    out[i++] = 0x23;
    out[i++] = 0x00;
    out[i++] = presence;
    out[i++] = (uint8_t)(range_cm & 0xFFu);
    out[i++] = (uint8_t)(range_cm >> 8);
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
        out[i++] = (uint8_t)(energies[g] & 0xFFu);
        out[i++] = (uint8_t)(energies[g] >> 8);
    }
    out[i++] = 0xF8;
    out[i++] = 0xF7;
    out[i++] = 0xF6;
    out[i++] = 0xF5;
    TEST_ASSERT_EQUAL(RADAR_ENERGY_FRAME_TOTAL, i);
    return i;
}

/* Check a decoded energy report against the expected content. */
static void assert_energy_report(const radar_frame_t *f, bool presence,
                                 uint16_t range_cm, const uint16_t *energies)
{
    TEST_ASSERT_EQUAL(RADAR_FRAME_REPORT, f->kind);
    TEST_ASSERT_TRUE(f->data.gates_valid);
    TEST_ASSERT_TRUE_MESSAGE(f->data.presence == presence, "presence");
    TEST_ASSERT_EQUAL_UINT16((uint16_t)range_cm * 10u, f->data.distance_mm);
    TEST_ASSERT_EQUAL_UINT8(presence ? 1u : 0u, f->data.raw_state);
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        TEST_ASSERT_EQUAL_UINT16(energies[g], f->data.gate_energy[g]);
}

/* 1. Valid energy frame decodes every field (doc vector). */
void test_parser_energy_frame_full(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);
    memset(&f, 0xA5, sizeof(f));

    bool got = false;
    feed_all(&p, k_energy_frame, sizeof(k_energy_frame), &f, &got);

    TEST_ASSERT_TRUE(got);
    assert_energy_report(&f, true, 30, k_energy_expect);
    TEST_ASSERT_TRUE(f.data.timestamp_us == 0); /* caller owns the clock */
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_bad);
    TEST_ASSERT_EQUAL_UINT32(0, p.bytes_dropped);
    TEST_ASSERT_EQUAL_UINT32(0, p.lines_parsed);
}

/* 2. Energy frame with presence 0 and range 0: nobody, distance 0. */
void test_parser_energy_frame_absent(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t frame[RADAR_ENERGY_FRAME_TOTAL];
    static const uint16_t energies[RADAR_MAX_GATES] = { 0 };
    radar_parser_init(&p);

    build_energy_frame(frame, 0, 0, energies);
    bool got = false;
    feed_all(&p, frame, sizeof(frame), &f, &got);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(RADAR_FRAME_REPORT, f.kind);
    TEST_ASSERT_FALSE(f.data.presence);
    TEST_ASSERT_EQUAL_UINT8(0, f.data.raw_state);
    TEST_ASSERT_EQUAL_UINT16(0, f.data.distance_mm);
    TEST_ASSERT_TRUE(f.data.gates_valid);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
}

/* 3. Declared length != 0x0023: rejected as an unknown binary format, the
 * parser resynchronizes on the next frame. */
void test_parser_energy_len_rejected(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t bad[6 + 34 + 4]; /* as if len were 0x0022 */
    radar_parser_init(&p);

    memcpy(bad, k_energy_frame, 5); /* F4 F3 F2 F1 22 */
    bad[4] = 0x22;
    bad[5] = 0x00;
    memset(&bad[6], 0x55, 34);
    memcpy(&bad[40], (const uint8_t[]) { 0xF8, 0xF7, 0xF6, 0xF5 }, 4);

    bool got = false;
    feed_all(&p, bad, sizeof(bad), &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);
    /* 5 candidate bytes rescanned + rejected length byte + the frame's
     * F8 F7 F6 F5 tail as line noise (the 34 'U' bytes are still pending). */
    TEST_ASSERT_EQUAL_UINT32(10, p.bytes_dropped);

    got = false;
    feed_all(&p, k_energy_frame, sizeof(k_energy_frame), &f, &got);
    TEST_ASSERT_TRUE(got);
    assert_energy_report(&f, true, 30, k_energy_expect);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);
    TEST_ASSERT_EQUAL_UINT32(44, p.bytes_dropped); /* + 34 'U' line abandoned */
}

/* 4. Corrupted energy tail rejects the frame and resynchronizes. */
void test_parser_energy_bad_tail_resync(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t corrupt[sizeof(k_energy_frame)];
    radar_parser_init(&p);

    memcpy(corrupt, k_energy_frame, sizeof(corrupt));
    corrupt[sizeof(corrupt) - 1] = 0xF6; /* expected 0xF5 */

    bool got = false;
    feed_all(&p, corrupt, sizeof(corrupt), &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);

    got = false;
    feed_all(&p, k_ack_exit, sizeof(k_ack_exit), &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_HEX16(0x01FE, f.ack.command);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);
}

/* 5. Running-mode line "presence ON, Range 250": presence true, 2500 mm,
 * no gate energies, counted in lines_parsed. */
void test_parser_line_presence_on_range(void)
{
    radar_parser_t p;
    radar_frame_t f;
    static const char k_line[] = "presence ON, Range 250\r\n";
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, (const uint8_t *)k_line, sizeof(k_line) - 1, &f, &got);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(RADAR_FRAME_REPORT, f.kind);
    TEST_ASSERT_TRUE(f.data.presence);
    TEST_ASSERT_EQUAL_UINT8(1, f.data.raw_state);
    TEST_ASSERT_EQUAL_UINT16(2500, f.data.distance_mm);
    TEST_ASSERT_FALSE(f.data.gates_valid);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.lines_parsed);
    TEST_ASSERT_EQUAL_UINT32(0, p.bytes_dropped);
}

/* 6. "OFF" wins over "ON" (searched first), no digits means distance 0. */
void test_parser_line_off(void)
{
    radar_parser_t p;
    radar_frame_t f;
    static const char k_off[] = "presence OFF, Range 0000\r\n";
    static const char k_both[] = "trigger ON OFF\r\n";
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, (const uint8_t *)k_off, sizeof(k_off) - 1, &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_FALSE(f.data.presence);
    TEST_ASSERT_EQUAL_UINT8(0, f.data.raw_state);
    TEST_ASSERT_EQUAL_UINT16(0, f.data.distance_mm);

    got = false;
    feed_all(&p, (const uint8_t *)k_both, sizeof(k_both) - 1, &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_FALSE(f.data.presence); /* OFF beats ON */

    TEST_ASSERT_EQUAL_UINT32(2, p.lines_parsed);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_ok);
}

/* 7. A line without any digits still reports presence with distance 0. */
void test_parser_line_no_digits(void)
{
    radar_parser_t p;
    radar_frame_t f;
    static const char k_line[] = "presence ON\r\n";
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, (const uint8_t *)k_line, sizeof(k_line) - 1, &f, &got);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_TRUE(f.data.presence);
    TEST_ASSERT_EQUAL_UINT16(0, f.data.distance_mm);
    TEST_ASSERT_FALSE(f.data.gates_valid);
    TEST_ASSERT_EQUAL_UINT32(1, p.lines_parsed);
}

/* 8. A line beyond RADAR_TEXT_LINE_MAX is discarded (bytes_dropped). */
void test_parser_line_overlong_dropped(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t line[RADAR_TEXT_LINE_MAX + 8];
    radar_parser_init(&p);

    memset(line, 'A', sizeof(line));           /* 72 printable bytes */
    line[sizeof(line) - 2] = 0x0D;             /* CRLF terminator */
    line[sizeof(line) - 1] = 0x0A;

    bool got = false;
    feed_all(&p, line, sizeof(line), &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(0, p.lines_parsed);
    /* 70 printable: 64 buffered (dropped at CRLF) + 6 overflow dropped
     * as they arrived; CR/LF close the line without extra drops. */
    TEST_ASSERT_EQUAL_UINT32(sizeof(line) - 2, p.bytes_dropped);

    /* The parser still accepts a frame afterwards. */
    got = false;
    feed_all(&p, k_energy_frame, sizeof(k_energy_frame), &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
}

/* 9. A line of exactly RADAR_TEXT_LINE_MAX bytes is still valid. */
void test_parser_line_max_len_ok(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t line[RADAR_TEXT_LINE_MAX + 2];
    radar_parser_init(&p);

    memset(line, 'B', RADAR_TEXT_LINE_MAX);
    line[RADAR_TEXT_LINE_MAX] = 0x0D;
    line[RADAR_TEXT_LINE_MAX + 1] = 0x0A;

    bool got = false;
    feed_all(&p, line, sizeof(line), &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(RADAR_FRAME_REPORT, f.kind);
    TEST_ASSERT_FALSE(f.data.presence); /* no tokens in the line */
    TEST_ASSERT_EQUAL_UINT16(0, f.data.distance_mm);
    TEST_ASSERT_EQUAL_UINT32(1, p.lines_parsed);
    TEST_ASSERT_EQUAL_UINT32(0, p.bytes_dropped);
}

/* 10. Energy frame + printable noise (no CRLF) + energy frame: both frames
 * decode; the abandoned line bytes are counted as dropped. */
void test_parser_energy_noise_energy(void)
{
    radar_parser_t p;
    radar_frame_t f;
    static const char k_noise[] = "garbage text here";
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, k_energy_frame, sizeof(k_energy_frame), &f, &got);
    TEST_ASSERT_TRUE(got);

    got = false;
    feed_all(&p, (const uint8_t *)k_noise, sizeof(k_noise) - 1, &f, &got);
    TEST_ASSERT_FALSE(got);

    got = false;
    feed_all(&p, k_energy_frame, sizeof(k_energy_frame), &f, &got);
    TEST_ASSERT_TRUE(got);
    assert_energy_report(&f, true, 30, k_energy_expect);

    TEST_ASSERT_EQUAL_UINT32(2, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_bad);
    TEST_ASSERT_EQUAL_UINT32(sizeof(k_noise) - 1, p.bytes_dropped);
    TEST_ASSERT_EQUAL_UINT32(0, p.lines_parsed);
}

/* 11. Energy frame and a text line back to back on the same UART. */
void test_parser_energy_and_line_mixed(void)
{
    radar_parser_t p;
    static const char k_line[] = "presence OFF, Range 0075\r\n";
    uint8_t stream[sizeof(k_energy_frame) + sizeof(k_line) - 1];
    radar_parser_init(&p);

    memcpy(stream, k_energy_frame, sizeof(k_energy_frame));
    memcpy(stream + sizeof(k_energy_frame), k_line, sizeof(k_line) - 1);

    unsigned reports = 0;
    bool line_seen = false;
    radar_frame_t fr;
    for (size_t i = 0; i < sizeof(stream); i++) {
        if (radar_parser_feed(&p, stream[i], &fr)) {
            reports++;
            if (!fr.data.gates_valid) {
                line_seen = true;
                TEST_ASSERT_FALSE(fr.data.presence);
                TEST_ASSERT_EQUAL_UINT16(750, fr.data.distance_mm);
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT8(2, reports);
    TEST_ASSERT_TRUE(line_seen);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.lines_parsed);
}

/* Deterministic PRNG (xorshift32) for the fuzz test. */
static uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* 12. 10k pseudo-random bytes with 100 valid energy frames woven in: no
 * crash, every inserted frame is decoded by the end (in order), random
 * CRLF pairs may only produce text reports, counters coherent. */
void test_parser_fuzz_10k_recovers_all_frames(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t frame[RADAR_ENERGY_FRAME_TOTAL];
    static const uint16_t energies[RADAR_MAX_GATES] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
    };
    uint8_t exp_presence[100];
    uint16_t exp_range[100];
    size_t n_ins = 0;
    size_t n_del = 0;
    uint32_t spurious_lines = 0;
    radar_parser_init(&p);

    uint32_t seed = 0x12345678u;
    for (int round = 0; round < 100; round++) {
        /* random-length noise burst, arbitrary bytes (headers included) */
        const size_t noise = 60 + (xorshift32(&seed) % 60);
        for (size_t i = 0; i < noise; i++) {
            const uint8_t nb = (uint8_t)(xorshift32(&seed) >> 13);
            if (radar_parser_feed(&p, nb, &f)) {
                /* only a random CRLF line can complete out of the noise */
                if (f.kind == RADAR_FRAME_REPORT && !f.data.gates_valid)
                    spurious_lines++;
                else
                    TEST_FAIL_MESSAGE("noise produced a binary frame");
            }
        }

        const uint8_t presence = (uint8_t)(round % 2);
        const uint16_t range = (uint16_t)((uint32_t)round * 13u % 500u);
        exp_presence[n_ins] = presence;
        exp_range[n_ins] = range;
        n_ins++;

        build_energy_frame(frame, presence, range, energies);
        for (size_t i = 0; i < sizeof(frame); i++) {
            if (!radar_parser_feed(&p, frame[i], &f))
                continue;
            TEST_ASSERT_EQUAL(RADAR_FRAME_REPORT, f.kind);
            TEST_ASSERT_TRUE(f.data.gates_valid);
            TEST_ASSERT_TRUE(n_del < n_ins);
            TEST_ASSERT_EQUAL_UINT8(exp_presence[n_del], f.data.presence ? 1u : 0u);
            TEST_ASSERT_EQUAL_UINT16((uint16_t)exp_range[n_del] * 10u,
                                     f.data.distance_mm);
            n_del++;
        }
    }

    TEST_ASSERT_EQUAL_UINT32(100, n_del); /* every inserted frame decoded */
    TEST_ASSERT_EQUAL_UINT32(100, p.frames_ok);
    TEST_ASSERT_TRUE(p.frames_bad > 0);  /* noise produced rejections */
    TEST_ASSERT_TRUE(p.bytes_dropped > 0);
    TEST_ASSERT_TRUE(p.lines_parsed == spurious_lines);
}

/* 13. ACK status != 0 is propagated to the caller. */
void test_parser_ack_status_one_propagated(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    /* Exit-config ACK with status 1 (failure). */
    bool got = false;
    feed_all(&p, (const uint8_t[]) {
        0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFE, 0x01, 0x01, 0x00,
        0x04, 0x03, 0x02, 0x01 }, 14, &f, &got);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(RADAR_FRAME_ACK, f.kind);
    TEST_ASSERT_EQUAL_HEX16(0x01FE, f.ack.command);
    TEST_ASSERT_EQUAL_HEX16(1, f.ack.status);
}

/* 14. ACK value larger than RADAR_ACK_VALUE_MAX: rejected by the length
 * gate (len would need 4 + 73 bytes of intra-frame data). */
void test_parser_ack_value_over_max_rejected(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t big[4 + 2 + (4 + RADAR_ACK_VALUE_MAX + 1) + 4];
    radar_parser_init(&p);

    const uint16_t flen = 4 + RADAR_ACK_VALUE_MAX + 1; /* 77: value 73 bytes */
    memset(big, 0, sizeof(big));
    big[0] = 0xFD; big[1] = 0xFC; big[2] = 0xFB; big[3] = 0xFA;
    big[4] = (uint8_t)(flen & 0xFF);
    big[5] = (uint8_t)(flen >> 8);
    big[6] = 0xFE; big[7] = 0x01; big[8] = 0x00; big[9] = 0x00;
    big[sizeof(big) - 4] = 0x04; big[sizeof(big) - 3] = 0x03;
    big[sizeof(big) - 2] = 0x02; big[sizeof(big) - 1] = 0x01;

    bool got = false;
    feed_all(&p, big, sizeof(big), &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);

    got = false;
    feed_all(&p, k_ack_enter, sizeof(k_ack_enter), &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_HEX16(0x01FF, f.ack.command);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
}

/* 15. The read-version ACK vector decodes with its 8-byte value. */
void test_parser_ack_version_vector(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, k_ack_version, sizeof(k_ack_version), &f, &got);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(RADAR_FRAME_ACK, f.kind);
    TEST_ASSERT_EQUAL_HEX16(0x0100, f.ack.command);
    TEST_ASSERT_EQUAL_HEX16(0, f.ack.status);
    TEST_ASSERT_EQUAL_UINT8(8, f.ack.value_len);
    static const uint8_t k_value[8] = { 0x06, 0x00, 'v', '1', '.', '5', '.', '4' };
    TEST_ASSERT_EQUAL_MEMORY(k_value, f.ack.value, sizeof(k_value));
    TEST_ASSERT_TRUE(radar_cmd_is_ack_of(&f, RADAR_CMD_READ_VERSION));
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
}

/* 16. Two energy frames back to back with no gap: both decode in order. */
void test_parser_energy_back_to_back(void)
{
    radar_parser_t p;
    radar_parser_init(&p);

    uint8_t both[2 * sizeof(k_energy_frame)];
    memcpy(both, k_energy_frame, sizeof(k_energy_frame));
    memcpy(both + sizeof(k_energy_frame), k_energy_frame, sizeof(k_energy_frame));

    unsigned n = 0;
    radar_frame_t fr;
    for (size_t i = 0; i < sizeof(both); i++) {
        if (radar_parser_feed(&p, both[i], &fr)) {
            assert_energy_report(&fr, true, 30, k_energy_expect);
            n++;
        }
    }

    TEST_ASSERT_EQUAL_UINT8(2, n);
    TEST_ASSERT_EQUAL_UINT32(2, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_bad);
    TEST_ASSERT_EQUAL_UINT32(0, p.bytes_dropped);
}

/* 17. A frame header inside a text line abandons the line and starts the
 * frame: the line bytes are dropped, the frame decodes. */
void test_parser_line_aborted_by_header(void)
{
    radar_parser_t p;
    radar_frame_t f;
    static const char k_line[] = "presence ON, Range 2"; /* 20 bytes, no CRLF */
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, (const uint8_t *)k_line, sizeof(k_line) - 1, &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(0, p.lines_parsed);

    got = false;
    feed_all(&p, k_energy_frame, sizeof(k_energy_frame), &f, &got);
    TEST_ASSERT_TRUE(got);
    assert_energy_report(&f, true, 30, k_energy_expect);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.lines_parsed);
    TEST_ASSERT_EQUAL_UINT32(sizeof(k_line) - 1, p.bytes_dropped);
}

/* 18. A frame truncated halfway produces nothing until the rest arrives. */
void test_parser_truncated_frame_silent(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, k_energy_frame, sizeof(k_energy_frame) / 2, &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_bad); /* not failed yet: still waiting */

    feed_all(&p, k_energy_frame + sizeof(k_energy_frame) / 2,
             sizeof(k_energy_frame) - sizeof(k_energy_frame) / 2, &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
}

/* 19. Valid header + consistent length but corrupted tail, immediately
 * followed by a valid frame that starts inside the first frame's extent:
 * the second frame is recovered. */
void test_parser_resync_second_frame_inside_corrupted(void)
{
    radar_parser_t p;
    radar_parser_init(&p);

    /* Exit-ACK-shaped frame whose tail bytes are replaced by the energy
     * frame's header: the report starts at offset 10, inside the 14-byte
     * extent of the first frame. */
    uint8_t stream[10 + sizeof(k_energy_frame)];
    memcpy(stream, k_ack_exit, 10); /* FD FC FB FA 04 00 FE 01 00 00 */
    memcpy(stream + 10, k_energy_frame, sizeof(k_energy_frame));

    bool got = false;
    radar_frame_t fr;
    for (size_t i = 0; i < sizeof(stream); i++) {
        if (radar_parser_feed(&p, stream[i], &fr))
            got = true;
    }

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);
    TEST_ASSERT_EQUAL_UINT32(10, p.bytes_dropped);
}

/* 20. A complete valid energy frame starting exactly at a corrupted
 * candidate's first tail byte is recovered: the rejected byte is the
 * embedded frame's header, so the resync re-queues it and nothing is lost. */
void test_parser_resync_frame_embedded_in_corrupted_payload(void)
{
    radar_parser_t p;
    radar_parser_init(&p);

    /* Fake frame: valid header, declared len 0x0023, 35 filler payload
     * bytes, then the energy frame where the tail should be. */
    uint8_t stream[6 + 35 + sizeof(k_energy_frame) + 4];
    memcpy(stream, k_energy_frame, 6); /* F4 F3 F2 F1 23 00 */
    memset(&stream[6], 0x77, 35);
    memcpy(&stream[41], k_energy_frame, sizeof(k_energy_frame));
    memcpy(&stream[41 + sizeof(k_energy_frame)],
           (const uint8_t[]) { 0xF8, 0xF7, 0xF6, 0xF5 }, 4);

    bool got = false;
    radar_frame_t fr;
    for (size_t i = 0; i < sizeof(stream); i++) {
        if (radar_parser_feed(&p, stream[i], &fr)) {
            got = true;
            assert_energy_report(&fr, true, 30, k_energy_expect);
        }
    }

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);
    /* 41 filler bytes rescanned away + the fake frame's tail as noise. */
    TEST_ASSERT_EQUAL_UINT32(45, p.bytes_dropped);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parser_energy_frame_full);
    RUN_TEST(test_parser_energy_frame_absent);
    RUN_TEST(test_parser_energy_len_rejected);
    RUN_TEST(test_parser_energy_bad_tail_resync);
    RUN_TEST(test_parser_line_presence_on_range);
    RUN_TEST(test_parser_line_off);
    RUN_TEST(test_parser_line_no_digits);
    RUN_TEST(test_parser_line_overlong_dropped);
    RUN_TEST(test_parser_line_max_len_ok);
    RUN_TEST(test_parser_energy_noise_energy);
    RUN_TEST(test_parser_energy_and_line_mixed);
    RUN_TEST(test_parser_fuzz_10k_recovers_all_frames);
    RUN_TEST(test_parser_ack_status_one_propagated);
    RUN_TEST(test_parser_ack_value_over_max_rejected);
    RUN_TEST(test_parser_ack_version_vector);
    RUN_TEST(test_parser_energy_back_to_back);
    RUN_TEST(test_parser_line_aborted_by_header);
    RUN_TEST(test_parser_truncated_frame_silent);
    RUN_TEST(test_parser_resync_second_frame_inside_corrupted);
    RUN_TEST(test_parser_resync_frame_embedded_in_corrupted_payload);
    RUN_TEST(test_cmd_build_enter_config_matches_doc);
    RUN_TEST(test_cmd_build_read_version);
    RUN_TEST(test_cmd_build_restart);
    RUN_TEST(test_gate_limits_build_and_validation);
    RUN_TEST(test_build_all_thresholds_layout);
    RUN_TEST(test_build_read_multi_names);
    RUN_TEST(test_parse_values_six_vector);
    RUN_TEST(test_parse_values_truncated);
    RUN_TEST(test_build_write_single_pair);
    RUN_TEST(test_build_sysmode_energy);
    RUN_TEST(test_transact_happy_path);
    RUN_TEST(test_transact_ack_status_one);
    RUN_TEST(test_transact_timeout_budget);
    RUN_TEST(test_transact_ignores_reports_and_foreign_acks);
    RUN_TEST(test_session_sends_exit_after_step_failure);
    RUN_TEST(test_session_enter_failure_no_step);
    RUN_TEST(test_fw_version_parse_vectors);
    RUN_TEST(test_fw_version_via_transact);
    RUN_TEST(test_cmd_is_ack_of);
    RUN_TEST(test_trigger_confirm_exact_third_frame);
    RUN_TEST(test_trigger_confirm_run_reset_on_gap);
    RUN_TEST(test_trigger_hold_expiry_only_after_hold_ms);
    RUN_TEST(test_trigger_hysteresis_immediate_exit);
    RUN_TEST(test_trigger_hysteresis_band_refreshes_hold);
    RUN_TEST(test_trigger_distance_updated_on_change);
    RUN_TEST(test_trigger_leave_and_reenter_rearms);
    RUN_TEST(test_trigger_presence_zero_distance_not_qualifying);
    RUN_TEST(test_trigger_set_trigger_mm_clamps_runtime);
    RUN_TEST(test_trigger_confirm_one_fires_on_first_frame);
    RUN_TEST(test_trigger_null_args_noop);
    return UNITY_END();
}
