/* Host unit tests for radar_parser.c (Unity, native build).
   Also runs the radar_command.c tests (test_radar_command.c) and the
   radar_trigger.c tests (test_radar_trigger.c): a single main() lives here.
   Vectors are the exact byte examples of "HLK-LD2410B Serial communication
   protocol V1.07" (2024-08-05). See CMakeLists.txt in this directory. */
#include <string.h>
#include "unity.h"
#include "radar_protocol.h"

/* Tests defined in test_radar_command.c. */
void test_cmd_build_enter_config_matches_doc(void);
void test_cmd_build_limits_frame(void);
void test_cmd_build_sensitivity_gate_and_all(void);
void test_cmd_build_rejects_bad_args(void);
void test_transact_happy_path(void);
void test_transact_ack_status_one(void);
void test_transact_timeout_budget(void);
void test_transact_ignores_reports_and_foreign_acks(void);
void test_session_sends_exit_after_step_failure(void);
void test_session_enter_failure_no_step(void);
void test_params_parse_read_params_vector(void);
void test_params_parse_malformed(void);
void test_fw_version_parse_doc_vector(void);
void test_resolution_parse_and_gate_mm(void);

/* Tests defined in test_radar_trigger.c. */
void test_trigger_confirm_exact_third_frame(void);
void test_trigger_confirm_run_reset_on_gap(void);
void test_trigger_hold_expiry_only_after_hold_ms(void);
void test_trigger_hysteresis_immediate_exit(void);
void test_trigger_static_only_no_on_present_static(void);
void test_trigger_static_frames_hold_presence(void);
void test_trigger_energy_below_threshold_no_qualify(void);
void test_trigger_leave_and_reenter_rearms(void);
void test_trigger_distance_updated_on_change(void);
void test_trigger_set_trigger_mm_updates_and_clamps(void);
void test_trigger_set_gate_mm_changes_hysteresis(void);
void test_trigger_static_trigger_mode_arms_on(void);
void test_trigger_both_counts_moving_side(void);

void setUp(void)
{
}

void tearDown(void)
{
}

/* --- protocol v1.07 example vectors (byte-exact) --- */

/* §2.3.2 basic report: STATIC, moving 81 cm (energy 0), static 0 cm
 * (energy 0x3B = 59), detection 0 cm. */
static const uint8_t k_rep_basic[] = {
    0xF4, 0xF3, 0xF2, 0xF1, 0x0D, 0x00, 0x02, 0xAA, 0x02, 0x51, 0x00,
    0x00, 0x00, 0x00, 0x3B, 0x00, 0x00, 0x55, 0x00, 0xF8, 0xF7, 0xF6, 0xF5
};

/* §2.3.2 engineering report: BOTH, moving 30 cm / 0x3C, static 0 cm / 0x39,
 * detection 0 cm, max gates 8/8, 9 moving + 9 static energies, light 0x60,
 * OUT 1. */
static const uint8_t k_rep_engineering[] = {
    0xF4, 0xF3, 0xF2, 0xF1, 0x23, 0x00, 0x01, 0xAA, 0x03, 0x1E, 0x00,
    0x3C, 0x00, 0x00, 0x39, 0x00, 0x00, 0x08, 0x08,
    0x3C, 0x22, 0x05, 0x03, 0x03, 0x04, 0x03, 0x06, 0x05,
    0x00, 0x00, 0x39, 0x10, 0x13, 0x06, 0x06, 0x08, 0x04,
    0x60, 0x01, 0x55, 0x00, 0xF8, 0xF7, 0xF6, 0xF5
};

/* §2.2.1 enter-config ACK: status 0, protocol version 0x0001, buffer 0x0040. */
static const uint8_t k_ack_enter[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0xFF, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x40, 0x00, 0x04, 0x03, 0x02, 0x01
};

/* §2.2.2 exit-config ACK (status 0). */
static const uint8_t k_ack_exit[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFE, 0x01, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
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

/* 1. Basic report decodes fully (cm -> mm conversion, counters). */
void test_parser_report_basic(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);
    memset(&f, 0xA5, sizeof(f));

    bool got = false;
    feed_all(&p, k_rep_basic, sizeof(k_rep_basic), &f, &got);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(RADAR_FRAME_REPORT, f.kind);
    TEST_ASSERT_EQUAL(RADAR_TARGET_STATIC, f.data.target_state);
    TEST_ASSERT_EQUAL_UINT16(810, f.data.moving_distance_mm);
    TEST_ASSERT_EQUAL_UINT8(0, f.data.moving_energy);
    TEST_ASSERT_EQUAL_UINT16(0, f.data.static_distance_mm);
    TEST_ASSERT_EQUAL_UINT8(59, f.data.static_energy);
    TEST_ASSERT_EQUAL_UINT16(0, f.data.detection_distance_mm);
    TEST_ASSERT_FALSE(f.data.engineering);
    TEST_ASSERT_TRUE(f.data.timestamp_us == 0); /* caller owns the clock */
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_bad);
    TEST_ASSERT_EQUAL_UINT32(0, p.bytes_dropped);
}

/* 2. Engineering report decodes every field including the 9+9 gate energies. */
void test_parser_report_engineering(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, k_rep_engineering, sizeof(k_rep_engineering), &f, &got);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(RADAR_FRAME_REPORT, f.kind);
    TEST_ASSERT_TRUE(f.data.engineering);
    TEST_ASSERT_EQUAL(RADAR_TARGET_BOTH, f.data.target_state);
    TEST_ASSERT_EQUAL_UINT16(300, f.data.moving_distance_mm);
    TEST_ASSERT_EQUAL_UINT8(0x3C, f.data.moving_energy);
    TEST_ASSERT_EQUAL_UINT16(0, f.data.static_distance_mm);
    TEST_ASSERT_EQUAL_UINT8(0x39, f.data.static_energy);
    TEST_ASSERT_EQUAL_UINT16(0, f.data.detection_distance_mm);
    TEST_ASSERT_EQUAL_UINT8(8, f.data.max_moving_gate);
    TEST_ASSERT_EQUAL_UINT8(8, f.data.max_static_gate);

    static const uint8_t k_mov[] = { 0x3C, 0x22, 0x05, 0x03, 0x03, 0x04, 0x03, 0x06, 0x05 };
    static const uint8_t k_st[] = { 0x00, 0x00, 0x39, 0x10, 0x13, 0x06, 0x06, 0x08, 0x04 };
    TEST_ASSERT_EQUAL_MEMORY(k_mov, f.data.moving_gate_energy, sizeof(k_mov));
    TEST_ASSERT_EQUAL_MEMORY(k_st, f.data.static_gate_energy, sizeof(k_st));
    TEST_ASSERT_EQUAL_UINT8(0x60, f.data.light_level);
    TEST_ASSERT_EQUAL_UINT8(1, f.data.out_level);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_bad);
}

/* 3. Enter-config ACK: command word | 0x0100, status, value 01 00 40 00. */
void test_parser_ack_enter_config(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, k_ack_enter, sizeof(k_ack_enter), &f, &got);

    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(RADAR_FRAME_ACK, f.kind);
    TEST_ASSERT_EQUAL_HEX16(0x01FF, f.ack.command);
    TEST_ASSERT_EQUAL_HEX16(0, f.ack.status);
    TEST_ASSERT_EQUAL_UINT8(4, f.ack.value_len);
    static const uint8_t k_value[4] = { 0x01, 0x00, 0x40, 0x00 };
    TEST_ASSERT_EQUAL_MEMORY(k_value, f.ack.value, sizeof(k_value));
    TEST_ASSERT_TRUE(radar_cmd_is_ack_of(&f, RADAR_CMD_ENTER_CONFIG));
    TEST_ASSERT_FALSE(radar_cmd_is_ack_of(&f, RADAR_CMD_EXIT_CONFIG));
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_bad);
}

/* 4. A frame truncated halfway produces nothing until the rest arrives. */
void test_parser_truncated_frame_silent(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, k_rep_basic, sizeof(k_rep_basic) / 2, &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_bad); /* not failed yet: still waiting */

    feed_all(&p, k_rep_basic + sizeof(k_rep_basic) / 2,
             sizeof(k_rep_basic) - sizeof(k_rep_basic) / 2, &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
}

/* 5. Random noise between two valid frames: both frames still decode. */
void test_parser_noise_between_frames(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    static const uint8_t k_noise[] = { 0x00, 0xFD, 0x12, 0x34 };

    bool got = false;
    feed_all(&p, k_rep_basic, sizeof(k_rep_basic), &f, &got);
    TEST_ASSERT_TRUE(got);
    got = false;
    feed_all(&p, k_noise, sizeof(k_noise), &f, &got);
    TEST_ASSERT_FALSE(got);
    got = false;
    feed_all(&p, k_ack_exit, sizeof(k_ack_exit), &f, &got);
    TEST_ASSERT_TRUE(got);

    TEST_ASSERT_EQUAL(RADAR_FRAME_ACK, f.kind);
    TEST_ASSERT_EQUAL_HEX16(0x01FE, f.ack.command);
    TEST_ASSERT_EQUAL_UINT32(2, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad); /* the FD 12 candidate */
    TEST_ASSERT_EQUAL_UINT32(4, p.bytes_dropped);
}

/* 6. Corrupted tail rejects the frame and resynchronizes on the next one. */
void test_parser_bad_tail_resync(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t corrupt[sizeof(k_rep_basic)];
    radar_parser_init(&p);

    memcpy(corrupt, k_rep_basic, sizeof(k_rep_basic));
    corrupt[sizeof(corrupt) - 1] = 0xF6; /* expected 0xF5 */

    bool got = false;
    feed_all(&p, corrupt, sizeof(corrupt), &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);

    got = false;
    feed_all(&p, k_ack_enter, sizeof(k_ack_enter), &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_HEX16(0x01FF, f.ack.command);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);
}

/* 7. Declared length beyond the maximum: rejected, no overflow, parser
 * keeps working. */
void test_parser_oversize_length_rejected(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, (const uint8_t[]) { 0xFD, 0xFC, 0xFB, 0xFA, 0xFF, 0xFF }, 6, &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);
    TEST_ASSERT_EQUAL_UINT16(0, p.len); /* nothing left buffered */

    got = false;
    feed_all(&p, k_rep_basic, sizeof(k_rep_basic), &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
}

/* 8. Declared length 0: rejected. */
void test_parser_zero_length_rejected(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    bool got = false;
    feed_all(&p, (const uint8_t[]) { 0xFD, 0xFC, 0xFB, 0xFA, 0x00, 0x00 }, 6, &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);

    got = false;
    feed_all(&p, k_rep_basic, sizeof(k_rep_basic), &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
}

/* 9. Unknown report data type byte (0x07): rejected. */
void test_parser_unknown_report_type_rejected(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t bad[sizeof(k_rep_basic)];
    radar_parser_init(&p);

    memcpy(bad, k_rep_basic, sizeof(k_rep_basic));
    bad[6] = 0x07; /* data type: only 0x01 and 0x02 exist */

    bool got = false;
    feed_all(&p, bad, sizeof(bad), &f, &got);
    TEST_ASSERT_FALSE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);

    got = false;
    feed_all(&p, k_ack_exit, sizeof(k_ack_exit), &f, &got);
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
}

/* 10. ACK status != 0 is propagated to the caller. */
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

/* 11. ACK value larger than RADAR_ACK_VALUE_MAX: rejected by the length
 * gate (len would need 4 + 33 bytes of intra-frame data). */
void test_parser_ack_value_over_max_rejected(void)
{
    radar_parser_t p;
    radar_frame_t f;
    uint8_t big[4 + 2 + (4 + RADAR_ACK_VALUE_MAX + 1) + 4];
    radar_parser_init(&p);

    const uint16_t flen = 4 + RADAR_ACK_VALUE_MAX + 1; /* 37: value 33 bytes */
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
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
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

/* 12. 10k pseudo-random bytes with 100 valid frames woven in: no crash,
 * every complete inserted frame is decoded by the end, counters coherent. */
void test_parser_fuzz_10k_recovers_all_frames(void)
{
    radar_parser_t p;
    radar_frame_t f;
    radar_parser_init(&p);

    /* Frame descriptors, in feed / decode order. */
    enum { KIND_REPORT, KIND_ACK_ENTER, KIND_ACK_EXIT };
    uint8_t ins_kind[128];
    size_t n_ins = 0;
    uint8_t del_kind[128];
    size_t n_del = 0;

    uint32_t seed = 0x12345678u;
    for (int round = 0; round < 100; round++) {
        /* random-length noise burst, arbitrary bytes (headers included) */
        const size_t noise = 60 + (xorshift32(&seed) % 60);
        for (size_t i = 0; i < noise; i++) {
            const uint8_t nb = (uint8_t)(xorshift32(&seed) >> 13);
            (void)radar_parser_feed(&p, nb, &f);
        }

        /* insert one complete valid frame */
        const uint8_t *v;
        size_t vn;
        uint8_t kind;
        if (round % 3 == 0) {
            v = k_rep_basic;
            vn = sizeof(k_rep_basic);
            kind = KIND_REPORT;
        } else if (round % 3 == 1) {
            v = k_ack_enter;
            vn = sizeof(k_ack_enter);
            kind = KIND_ACK_ENTER;
        } else {
            v = k_ack_exit;
            vn = sizeof(k_ack_exit);
            kind = KIND_ACK_EXIT;
        }
        TEST_ASSERT_TRUE(n_ins < sizeof(ins_kind));
        ins_kind[n_ins++] = kind;

        for (size_t i = 0; i < vn; i++) {
            if (radar_parser_feed(&p, v[i], &f) && n_del < sizeof(del_kind)) {
                del_kind[n_del++] =
                    (uint8_t)(f.kind == RADAR_FRAME_ACK
                                  ? (f.ack.command == 0x01FF ? KIND_ACK_ENTER
                                                             : KIND_ACK_EXIT)
                                  : KIND_REPORT);
            }
        }
    }

    /* A frame may complete during a later noise burst (resync tail), but it
     * must always complete, and in insertion order. */
    TEST_ASSERT_EQUAL_UINT(n_ins, n_del);
    TEST_ASSERT_EQUAL_MEMORY(ins_kind, del_kind, n_ins);
    TEST_ASSERT_EQUAL_UINT32(n_ins, p.frames_ok);
    TEST_ASSERT_TRUE(p.frames_bad > 0);  /* noise produced rejections */
    TEST_ASSERT_TRUE(p.bytes_dropped > 0);
    TEST_ASSERT_EQUAL_UINT16(0, p.len);  /* nothing left half-buffered */
}

/* 13. Two frames back to back with no gap: both decode in order. */
void test_parser_back_to_back(void)
{
    radar_parser_t p;
    radar_parser_init(&p);

    uint8_t both[64];
    memcpy(both, k_ack_exit, sizeof(k_ack_exit));
    memcpy(both + sizeof(k_ack_exit), k_ack_enter, sizeof(k_ack_enter));

    uint16_t seen[2];
    size_t n = 0;
    radar_frame_t fr;
    const size_t total = sizeof(k_ack_exit) + sizeof(k_ack_enter);
    for (size_t i = 0; i < total; i++) {
        if (radar_parser_feed(&p, both[i], &fr)) {
            TEST_ASSERT_TRUE(n < 2);
            seen[n++] = (uint16_t)(fr.kind == RADAR_FRAME_ACK ? fr.ack.command : 0);
        }
    }

    TEST_ASSERT_EQUAL_UINT8(2, n);
    TEST_ASSERT_EQUAL_HEX16(0x01FE, seen[0]);
    TEST_ASSERT_EQUAL_HEX16(0x01FF, seen[1]);
    TEST_ASSERT_EQUAL_UINT32(2, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, p.frames_bad);
    TEST_ASSERT_EQUAL_UINT32(0, p.bytes_dropped);
}

/* 14. Valid header + consistent length but corrupted tail, immediately
 * followed by a valid frame that starts inside the first frame's extent:
 * the second frame is recovered. */
void test_parser_resync_second_frame_inside_corrupted(void)
{
    radar_parser_t p;
    radar_parser_init(&p);

    /* Exit-ACK-shaped frame whose tail bytes are replaced by the basic
     * report's header: the report starts at offset 10, inside the 14-byte
     * extent of the first frame. */
    uint8_t stream[10 + sizeof(k_rep_basic)];
    memcpy(stream, k_ack_exit, 10); /* FD FC FB FA 04 00 FE 01 00 00 */
    memcpy(stream + 10, k_rep_basic, sizeof(k_rep_basic));

    uint8_t kind = 0;
    radar_frame_t fr;
    for (size_t i = 0; i < sizeof(stream); i++) {
        if (radar_parser_feed(&p, stream[i], &fr))
            kind = (uint8_t)(fr.kind == RADAR_FRAME_ACK ? fr.ack.command
                                                        : fr.data.target_state);
    }

    TEST_ASSERT_EQUAL(RADAR_TARGET_STATIC, kind); /* the basic report decoded */
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);
    TEST_ASSERT_EQUAL_UINT32(10, p.bytes_dropped);
}

/* 15. A complete valid frame fully embedded in a corrupted candidate's
 * payload is recovered by the +1 rescan. */
void test_parser_resync_frame_embedded_in_corrupted_payload(void)
{
    radar_parser_t p;
    radar_parser_init(&p);

    /* Fake engineering report: valid header, declared len 35, but the 0xAA
     * payload marker is missing; a complete basic report hides inside its
     * payload window (payload offset 5). */
    uint8_t stream[6 + 35];
    memset(stream, 0x77, sizeof(stream));
    stream[0] = 0xF4; stream[1] = 0xF3; stream[2] = 0xF2; stream[3] = 0xF1;
    stream[4] = 0x23; stream[5] = 0x00;
    memcpy(stream + 6 + 5, k_rep_basic, sizeof(k_rep_basic)); /* 23 <= 30 */

    uint8_t state = 0xFF;
    radar_frame_t fr;
    for (size_t i = 0; i < sizeof(stream); i++) {
        if (radar_parser_feed(&p, stream[i], &fr))
            state = (uint8_t)fr.data.target_state;
    }

    TEST_ASSERT_EQUAL(RADAR_TARGET_STATIC, state);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.frames_bad);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parser_report_basic);
    RUN_TEST(test_parser_report_engineering);
    RUN_TEST(test_parser_ack_enter_config);
    RUN_TEST(test_parser_truncated_frame_silent);
    RUN_TEST(test_parser_noise_between_frames);
    RUN_TEST(test_parser_bad_tail_resync);
    RUN_TEST(test_parser_oversize_length_rejected);
    RUN_TEST(test_parser_zero_length_rejected);
    RUN_TEST(test_parser_unknown_report_type_rejected);
    RUN_TEST(test_parser_ack_status_one_propagated);
    RUN_TEST(test_parser_ack_value_over_max_rejected);
    RUN_TEST(test_parser_fuzz_10k_recovers_all_frames);
    RUN_TEST(test_parser_back_to_back);
    RUN_TEST(test_parser_resync_second_frame_inside_corrupted);
    RUN_TEST(test_parser_resync_frame_embedded_in_corrupted_payload);
    RUN_TEST(test_cmd_build_enter_config_matches_doc);
    RUN_TEST(test_cmd_build_limits_frame);
    RUN_TEST(test_cmd_build_sensitivity_gate_and_all);
    RUN_TEST(test_cmd_build_rejects_bad_args);
    RUN_TEST(test_transact_happy_path);
    RUN_TEST(test_transact_ack_status_one);
    RUN_TEST(test_transact_timeout_budget);
    RUN_TEST(test_transact_ignores_reports_and_foreign_acks);
    RUN_TEST(test_session_sends_exit_after_step_failure);
    RUN_TEST(test_session_enter_failure_no_step);
    RUN_TEST(test_params_parse_read_params_vector);
    RUN_TEST(test_params_parse_malformed);
    RUN_TEST(test_fw_version_parse_doc_vector);
    RUN_TEST(test_resolution_parse_and_gate_mm);
    RUN_TEST(test_trigger_confirm_exact_third_frame);
    RUN_TEST(test_trigger_confirm_run_reset_on_gap);
    RUN_TEST(test_trigger_hold_expiry_only_after_hold_ms);
    RUN_TEST(test_trigger_hysteresis_immediate_exit);
    RUN_TEST(test_trigger_static_only_no_on_present_static);
    RUN_TEST(test_trigger_static_frames_hold_presence);
    RUN_TEST(test_trigger_energy_below_threshold_no_qualify);
    RUN_TEST(test_trigger_leave_and_reenter_rearms);
    RUN_TEST(test_trigger_distance_updated_on_change);
    RUN_TEST(test_trigger_set_trigger_mm_updates_and_clamps);
    RUN_TEST(test_trigger_set_gate_mm_changes_hysteresis);
    RUN_TEST(test_trigger_static_trigger_mode_arms_on);
    RUN_TEST(test_trigger_both_counts_moving_side);
    return UNITY_END();
}
