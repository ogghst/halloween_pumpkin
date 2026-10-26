/* HLK-LD2420 command codec and synchronous command transactions.
 *
 * Pure C: no allocation, no clock. Time is injected through
 * radar_command_io_t (the transport owns real time); the transaction polls
 * it and bounds its own waiting with the io timeout budget.
 *
 * Parameter payloads follow the Hi-Link LD2420 tables: module parameters
 * are (name:2 + value:4) little-endian pairs, READ_PARAMS answers with the
 * values only, in request order; system parameters take the single
 * (name 0x0000 + u32 mode) pair.
 */
#include "radar_protocol.h"

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
    /* Enter-config value: 2-byte host protocol version, 0x0001 per the
     * Hi-Link sample frame; the ACK adds proto ver + buffer size. */
    static const uint8_t k_enter_value[2] = { 0x01, 0x00 };

    const int enter_rc = radar_command_transact(io, parser, RADAR_CMD_ENTER_CONFIG,
                                                k_enter_value, sizeof(k_enter_value),
                                                NULL);
    if (enter_rc != RADAR_CMD_OK)
        return enter_rc; /* no config-mode ACK: any further command is invalid */

    const int step_rc = (step != NULL) ? step(io, parser, ctx) : RADAR_CMD_OK;

    /* Leave config mode no matter how the step went; the module must return
     * to running mode (and only then latches written parameters). The exit
     * result never masks the step result. */
    (void)radar_command_transact(io, parser, RADAR_CMD_EXIT_CONFIG, NULL, 0, NULL);
    return step_rc;
}

/* --- parameter payload codecs --- */

/* name:2 little-endian. */
static void params_put_name(uint8_t *out, uint16_t name)
{
    out[0] = (uint8_t)(name & 0xFFu);
    out[1] = (uint8_t)(name >> 8);
}

/* (name:2, value:4) little-endian parameter pair (protocol parameter table). */
static void params_put_pair(uint8_t *out, uint16_t name, uint32_t v)
{
    params_put_name(out, name);
    out[2] = (uint8_t)(v & 0xFFu);
    out[3] = (uint8_t)((v >> 8) & 0xFFu);
    out[4] = (uint8_t)((v >> 16) & 0xFFu);
    out[5] = (uint8_t)((v >> 24) & 0xFFu);
}

int radar_params_build_read(const uint16_t *names, size_t count,
                            uint8_t *out, size_t out_size)
{
    if (count > RADAR_MAX_GATES)
        return -1;
    if (names == NULL && count > 0)
        return -1;
    const size_t n = 2u * count;
    if (out == NULL || out_size < n)
        return -1;

    for (size_t i = 0; i < count; i++)
        params_put_name(&out[2u * i], names[i]);
    return (int)n;
}

int radar_params_parse_values(const uint8_t *value, size_t len,
                              uint32_t *values, size_t count)
{
    /* An ACK carries at most RADAR_ACK_VALUE_MAX bytes of value. */
    if (count > RADAR_ACK_VALUE_MAX / 4u)
        return -1;
    if ((value == NULL || values == NULL) && count > 0)
        return -1;
    if (len < 4u * count)
        return -1; /* truncated ACK payload */

    for (size_t i = 0; i < count; i++) {
        const uint8_t *q = &value[4u * i];
        values[i] = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                    ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
    }
    return 0;
}

int radar_params_build_write(const uint16_t *names, const uint32_t *values,
                             size_t count, uint8_t *out, size_t out_size)
{
    /* Cap at what one command frame can still carry. */
    if (count > RADAR_CMD_VALUE_MAX / 6u)
        return -1;
    if ((names == NULL || values == NULL) && count > 0)
        return -1;
    const size_t n = 6u * count;
    if (out == NULL || out_size < n)
        return -1;

    for (size_t i = 0; i < count; i++)
        params_put_pair(&out[6u * i], names[i], values[i]);
    return (int)n;
}

int radar_params_build_gate_limits(uint16_t min_gate, uint16_t max_gate,
                                   uint16_t timeout_s,
                                   uint8_t *out, size_t out_size)
{
    if (out == NULL || out_size < 18)
        return -1;
    /* Gate numbers are 0..RADAR_MAX_GATES-1; timeout is 0..255. */
    if (min_gate >= RADAR_MAX_GATES || max_gate >= RADAR_MAX_GATES)
        return -1;
    if (min_gate > max_gate)
        return -1;
    if (timeout_s > 0xFFu)
        return -1;

    params_put_pair(&out[0], RADAR_PARAM_MIN_GATE, min_gate);
    params_put_pair(&out[6], RADAR_PARAM_MAX_GATE, max_gate);
    params_put_pair(&out[12], RADAR_PARAM_TIMEOUT_S, timeout_s);
    return 18;
}

int radar_params_build_all_thresholds(const uint32_t move[RADAR_MAX_GATES],
                                      const uint32_t still[RADAR_MAX_GATES],
                                      uint8_t *out, size_t out_size)
{
    if (move == NULL || still == NULL || out == NULL)
        return -1;

    /* 32 (name:2 + u32:4) pairs = 192 bytes: move thresholds 0x0010..0x001F
     * then keep thresholds 0x0020..0x002F. Larger than one command frame can
     * carry: send it as two 16-pair WRITE_PARAMS commands (96 B each). */
    const size_t n = 2u * RADAR_MAX_GATES * 6u;
    if (out_size < n)
        return -1;

    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        params_put_pair(&out[6u * g], RADAR_PARAM_MOVE_THRESH(g), move[g]);
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        params_put_pair(&out[6u * (RADAR_MAX_GATES + g)],
                        RADAR_PARAM_STILL_THRESH(g), still[g]);
    return (int)n;
}

int radar_params_build_sysmode(uint8_t mode, uint8_t *out, size_t out_size)
{
    if (out == NULL || out_size < 6)
        return -1;

    /* System-parameter table: name 0x0000 (SystemMode), u32 mode. */
    params_put_pair(&out[0], 0x0000u, mode);
    return 6;
}

int radar_fw_version_parse(const uint8_t *value, size_t len,
                           radar_fw_version_t *out)
{
    if (value == NULL || out == NULL || len < 2)
        return -1;

    /* ACK value: [declared text length:2] [ASCII text...]. */
    const size_t declared = (size_t)value[0] | ((size_t)value[1] << 8);
    if (declared > len - 2u)
        return -1; /* truncated payload */

    const size_t copy = (declared < sizeof(out->text) - 1u)
                            ? declared
                            : sizeof(out->text) - 1u;
    memcpy(out->text, &value[2], copy);
    out->text[copy] = '\0';

    /* Numeric form: all digits of the text concatenated ("v1.5.34" -> 1534). */
    int32_t numeric = 0;
    for (size_t i = 0; i < copy; i++) {
        const char c = out->text[i];
        if (c < '0' || c > '9')
            continue;
        if (numeric > (INT32_MAX - (int32_t)(c - '0')) / 10) {
            numeric = INT32_MAX;
            break;
        }
        numeric = numeric * 10 + (int32_t)(c - '0');
    }
    out->numeric = numeric;
    out->energy_capable = numeric >= 154; /* fw >= 1.5.4 streams energy frames */
    return 0;
}
