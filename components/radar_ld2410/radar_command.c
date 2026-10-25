/* HLK-LD2410B command codec and synchronous command transactions.
 *
 * Pure C: no allocation, no clock. Time is injected through
 * radar_command_io_t (the transport owns real time); the transaction polls
 * it and bounds its own waiting with the io timeout budget.
 */
#include "radar_protocol.h"

#include <stdio.h>
#include <string.h>

int radar_cmd_build(uint8_t *out, size_t out_size, uint16_t command,
                    const uint8_t *value, size_t value_len)
{
    if (value_len > RADAR_CMD_VALUE_MAX)
        return -1;

    /* FD FC FB FA | len:2 | command:2 + value:N | 04 03 02 01 */
    const size_t total = 12 + value_len;
    if (out == NULL || out_size < total)
        return -1;

    const uint16_t flen = (uint16_t)(2u + value_len);
    out[0] = 0xFD;
    out[1] = 0xFC;
    out[2] = 0xFB;
    out[3] = 0xFA;
    out[4] = (uint8_t)(flen & 0xFFu);
    out[5] = (uint8_t)(flen >> 8);
    out[6] = (uint8_t)(command & 0xFFu);
    out[7] = (uint8_t)(command >> 8);
    if (value_len > 0)
        memcpy(&out[8], value, value_len);
    out[8u + value_len] = 0x04;
    out[9u + value_len] = 0x03;
    out[10u + value_len] = 0x02;
    out[11u + value_len] = 0x01;
    return (int)total;
}

bool radar_cmd_is_ack_of(const radar_frame_t *frame, uint16_t command)
{
    return frame != NULL && frame->kind == RADAR_FRAME_ACK &&
           frame->ack.command == (uint16_t)(command | 0x0100u);
}

int radar_command_transact(const radar_command_io_t *io, radar_parser_t *parser,
                           uint16_t command, const uint8_t *value,
                           size_t value_len, radar_frame_t *ack_out)
{
    if (io == NULL || parser == NULL || io->write == NULL || io->read == NULL)
        return RADAR_CMD_ERR_ARG;

    uint8_t frame[RADAR_FRAME_MAX_BYTES];
    const int n = radar_cmd_build(frame, sizeof(frame), command, value, value_len);
    if (n < 0)
        return RADAR_CMD_ERR_ARG;
    if (io->write(io->ctx, frame, (size_t)n) < 0)
        return RADAR_CMD_ERR_IO;

    /* Poll budget: one transport read per millisecond of timeout_ms. Only
     * empty reads consume the budget, so a stream that keeps flowing never
     * starves the transaction. No real clock is touched here. */
    uint32_t idle_polls = 0;
    radar_frame_t f;
    bool found = false;   /* wanted ACK seen (result held in `result`) */
    int result = RADAR_CMD_OK;

    for (;;) {
        uint8_t rbuf[32];
        const int got = io->read(io->ctx, rbuf, sizeof(rbuf));
        if (got < 0)
            return RADAR_CMD_ERR_IO;
        if (got == 0) {
            if (found)
                return result; /* ACK already in hand, silence is fine */
            idle_polls++;
            if (idle_polls >= io->timeout_ms)
                return RADAR_CMD_ERR_TIMEOUT; /* timeout_ms empty polls */
            continue;
        }
        idle_polls = 0;
        for (int i = 0; i < got; i++) {
            /* Feed every byte that was read: the transport read is
             * destructive, dropping any would lose stream data. Spurious
             * reports and ACKs of other commands are consumed and skipped. */
            if (radar_parser_feed(parser, rbuf[i], &f) &&
                !found && radar_cmd_is_ack_of(&f, command)) {
                if (ack_out != NULL)
                    *ack_out = f;
                result = (f.ack.status == 0) ? RADAR_CMD_OK : RADAR_CMD_ERR_STATUS;
                found = true;
            }
        }
    }
}

int radar_command_session(const radar_command_io_t *io, radar_parser_t *parser,
                          radar_command_step_t step, void *ctx)
{
    static const uint8_t k_enter_value[2] = { 0x01, 0x00 }; /* §2.2.1 */

    const int enter_rc = radar_command_transact(io, parser, RADAR_CMD_ENTER_CONFIG,
                                                k_enter_value, sizeof(k_enter_value),
                                                NULL);
    if (enter_rc != RADAR_CMD_OK)
        return enter_rc; /* no config-mode ACK: any further command is invalid */

    const int step_rc = (step != NULL) ? step(io, parser, ctx) : RADAR_CMD_OK;

    /* Leave config mode no matter how the step went (§2.4.1); the module
     * must return to working mode. The exit result never masks the step
     * result. */
    (void)radar_command_transact(io, parser, RADAR_CMD_EXIT_CONFIG, NULL, 0, NULL);
    return step_rc;
}

int radar_params_parse(const uint8_t *value, size_t len, radar_module_params_t *out)
{
    if (value == NULL || out == NULL || len < 4)
        return -1;

    const uint8_t n = value[1]; /* header byte N: gates 0..N reported */
    if (value[0] != 0xAA || n > RADAR_MAX_GATES)
        return -1;

    /* AA | N | maxmov | maxst | mov[0..N] (N+1) | st[0..N] (N+1) | dur:2 */
    const size_t expect = 8u + 2u * n;
    if (len != expect)
        return -1;

    out->max_gates = n;
    out->max_moving_gate = value[2];
    out->max_static_gate = value[3];
    memcpy(out->motion_sensitivity, &value[4], (size_t)n + 1u);
    memcpy(out->static_sensitivity, &value[5u + n], (size_t)n + 1u);
    out->unmanned_duration_s =
        (uint16_t)(value[6u + 2u * n] | ((uint16_t)value[7u + 2u * n] << 8));
    return 0;
}

/* (word:2, u32:4) little-endian parameter pair, protocol §2.2.3 / §2.2.7. */
static void params_put_pair(uint8_t *out, uint16_t word, uint32_t v)
{
    out[0] = (uint8_t)(word & 0xFFu);
    out[1] = (uint8_t)(word >> 8);
    out[2] = (uint8_t)(v & 0xFFu);
    out[3] = (uint8_t)((v >> 8) & 0xFFu);
    out[4] = (uint8_t)((v >> 16) & 0xFFu);
    out[5] = (uint8_t)((v >> 24) & 0xFFu);
}

int radar_params_build_limits(uint16_t max_moving_gate, uint16_t max_static_gate,
                              uint16_t unmanned_duration_s,
                              uint8_t *out, size_t out_size)
{
    /* §2.2.3: gates in 2..8, duration 0..65535 s (uint16_t by type). */
    if (out == NULL || out_size < 18)
        return -1;
    if (max_moving_gate < 2 || max_moving_gate > RADAR_MAX_GATES)
        return -1;
    if (max_static_gate < 2 || max_static_gate > RADAR_MAX_GATES)
        return -1;

    params_put_pair(&out[0], 0x0000u, max_moving_gate);
    params_put_pair(&out[6], 0x0001u, max_static_gate);
    params_put_pair(&out[12], 0x0002u, unmanned_duration_s);
    return 18;
}

int radar_params_build_sensitivity(uint16_t gate, uint8_t motion,
                                   uint8_t static_sens,
                                   uint8_t *out, size_t out_size)
{
    /* §2.2.7: gate 0..8 or 0xFFFF (all gates); sensitivity 0..100. */
    if (out == NULL || out_size < 18)
        return -1;
    if (gate != RADAR_ALL_GATES && gate > RADAR_MAX_GATES)
        return -1;
    if (motion > 100 || static_sens > 100)
        return -1;

    params_put_pair(&out[0], 0x0000u, gate);
    params_put_pair(&out[6], 0x0001u, motion);
    params_put_pair(&out[12], 0x0002u, static_sens);
    return 18;
}

int radar_fw_version_parse(const uint8_t *value, size_t len,
                           radar_fw_version_t *out)
{
    if (value == NULL || out == NULL || len != 8)
        return -1;

    /* §2.2.8: type:2 | major:2 | minor:4. Example 00 00 | 02 01 | 16 24 06 22
     * displays as "V1.02.22062416": major is high.low bytes in decimal, the
     * minor field is printed as 8 hex digits (it encodes the build date). */
    out->fw_type = (uint16_t)(value[0] | ((uint16_t)value[1] << 8));
    out->major = (uint32_t)(value[2] | ((uint32_t)value[3] << 8));
    out->minor = (uint32_t)value[4] | ((uint32_t)value[5] << 8) |
                 ((uint32_t)value[6] << 16) | ((uint32_t)value[7] << 24);
    (void)snprintf(out->text, sizeof(out->text), "V%u.%02u.%08lX",
                   (unsigned)(out->major >> 8), (unsigned)(out->major & 0xFFu),
                   (unsigned long)out->minor);
    return 0;
}

int radar_resolution_parse(const uint8_t *value, size_t len, uint16_t *index_out)
{
    if (value == NULL || index_out == NULL || len != 2)
        return -1;
    *index_out = (uint16_t)(value[0] | ((uint16_t)value[1] << 8));
    return 0;
}

uint16_t radar_gate_mm(uint16_t resolution_index)
{
    /* Table 8: 0x0001 selects 0.2 m per gate; every other index (including
     * the factory default 0x0000 and unknown values) is 0.75 m. */
    return resolution_index == RADAR_RESOLUTION_02 ? 200u : 750u;
}
