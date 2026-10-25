/* Byte-wise frame parser for the HLK-LD2410B protocol (see radar_protocol.h).
 *
 * Pure C: no allocation, no clock, total robustness on arbitrary input.
 * Resynchronization rule: when a candidate frame fails validation (length,
 * payload markers or tail), the buffered bytes are re-scanned from offset +1
 * and fed through the FSM again, so a valid frame that starts inside a
 * corrupted one is still recovered. Tail bytes are validated while streaming
 * (they are protocol constants) so the buffer only ever holds
 * header + length + payload <= 6 + 56 bytes.
 */
#include "radar_protocol.h"

#include <string.h>

/* FSM states (radar_parser_t::state). */
enum {
    ST_HEAD = 0,  /* scanning for a header byte (0xFD command/ACK, 0xF4 report) */
    ST_H2,        /* second header byte */
    ST_H3,        /* third header byte */
    ST_H4,        /* fourth header byte */
    ST_LEN1,      /* declared intra-frame length, low byte */
    ST_LEN2,      /* ... high byte; declared length validated here */
    ST_PAYLOAD,   /* intra-frame data, counted by need */
    ST_TAIL,      /* end-of-frame bytes, need = remaining (4..1) */
};

/* Step results (internal). FAIL tells whether the rejected byte was already
 * consumed into the candidate buffer (STEP_FAIL_BUF) or not (STEP_FAIL_CLEAN),
 * so the resynchronization replay can decide whether to re-inject it. */
enum {
    STEP_MORE = 0,   /* byte consumed, frame still incomplete */
    STEP_FRAME,      /* byte consumed, complete frame delivered via *out */
    STEP_FAIL_CLEAN, /* candidate invalid, byte NOT consumed */
    STEP_FAIL_BUF,   /* candidate invalid, byte already in p->buf */
};

static const uint8_t k_hdr_cmd[4] = { 0xFD, 0xFC, 0xFB, 0xFA };
static const uint8_t k_hdr_rep[4] = { 0xF4, 0xF3, 0xF2, 0xF1 };
static const uint8_t k_tail_cmd[4] = { 0x04, 0x03, 0x02, 0x01 };
static const uint8_t k_tail_rep[4] = { 0xF8, 0xF7, 0xF6, 0xF5 };

void radar_parser_init(radar_parser_t *p)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p)); /* ST_HEAD == 0, len/need 0, counters cleared */
}

/* Declared intra-frame length limits (protocol v1.07):
 *  - command/ACK: >= 2, frame must fit the buffer (len <= max - 8), and the
 *    value after the 2 status bytes must fit the ACK value storage, i.e.
 *    len <= 4 + RADAR_ACK_VALUE_MAX. ACKs shorter than 4 carry no status
 *    word and are rejected later at payload validation (frames_bad).
 *  - report: 13 (basic) .. max - 8 (engineering is 35). */
static bool parser_len_ok(bool is_cmd, uint16_t len)
{
    if (len > (uint16_t)(RADAR_FRAME_MAX_BYTES - 8))
        return false;
    if (is_cmd)
        return len >= 2 && len <= 4u + RADAR_ACK_VALUE_MAX;
    return len >= 13;
}

/* Payload markers once the declared number of bytes is buffered. */
static bool parser_payload_ok(const radar_parser_t *p)
{
    const uint16_t flen = (uint16_t)(p->buf[4] | ((uint16_t)p->buf[5] << 8));

    if (p->is_cmd)
        return flen >= 4; /* ACK word + status word must both be present */

    /* report: [type] [0xAA] data... [0x55] [0x00] */
    if (p->buf[7] != 0xAA)
        return false;
    if (p->buf[6u + flen - 2u] != 0x55 || p->buf[6u + flen - 1u] != 0x00)
        return false;
    if (p->buf[6] == 0x02)
        return flen == 13;           /* Table 13: basic information */
    if (p->buf[6] == 0x01)
        return flen == 13u + 22u;    /* Table 15: + 2 gates, 2x9 energies, light, OUT */
    return false;                    /* unknown data type */
}

/* Wire cm -> mm, saturating: corrupted data must never wrap into a small
 * (triggerable) distance. */
static uint16_t parser_cm_to_mm(uint16_t cm)
{
    return cm > 0xFFFFu / 10u ? 0xFFFFu : (uint16_t)(cm * 10u);
}

static void parser_decode(const radar_parser_t *p, radar_frame_t *out)
{
    const uint16_t flen = (uint16_t)(p->buf[4] | ((uint16_t)p->buf[5] << 8));

    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));

    if (p->is_cmd) {
        /* ACK: [command | 0x0100] [status:2] [value...] */
        out->kind = RADAR_FRAME_ACK;
        out->ack.command = (uint16_t)(p->buf[6] | ((uint16_t)p->buf[7] << 8));
        out->ack.status = (uint16_t)(p->buf[8] | ((uint16_t)p->buf[9] << 8));
        out->ack.value_len = (uint8_t)(flen - 4);
        memcpy(out->ack.value, &p->buf[10], flen - 4);
        return;
    }

    /* report: type and 0xAA stripped; target data at buf[8]. */
    const uint8_t *q = &p->buf[8];
    radar_data_frame_t *d = &out->data;
    const bool engineering = (p->buf[6] == 0x01);

    out->kind = RADAR_FRAME_REPORT;
    d->target_state = (radar_target_state_t)q[0];
    d->moving_distance_mm = parser_cm_to_mm((uint16_t)(q[1] | ((uint16_t)q[2] << 8)));
    d->moving_energy = q[3];
    d->static_distance_mm = parser_cm_to_mm((uint16_t)(q[4] | ((uint16_t)q[5] << 8)));
    d->static_energy = q[6];
    d->detection_distance_mm = parser_cm_to_mm((uint16_t)(q[7] | ((uint16_t)q[8] << 8)));

    if (engineering) {
        d->engineering = true;
        d->max_moving_gate = q[9];
        d->max_static_gate = q[10];
        memcpy(d->moving_gate_energy, &q[11], RADAR_MAX_GATES + 1);
        memcpy(d->static_gate_energy, &q[20], RADAR_MAX_GATES + 1);
        d->light_level = q[29];
        d->out_level = q[30];
    }
    /* timestamp_us stays 0: the caller owns the clock. */
}

/* Consume one byte against the current FSM state. The byte is appended to
 * the candidate buffer only when accepted; STEP_FAIL_* leaves the candidate
 * in p->buf[0..p->len) for the resynchronization scan. */
static int parser_step(radar_parser_t *p, uint8_t b, radar_frame_t *out)
{
    switch (p->state) {
    case ST_HEAD:
        if (b != k_hdr_cmd[0] && b != k_hdr_rep[0]) {
            p->bytes_dropped++;
            return STEP_MORE;
        }
        p->is_cmd = (b == k_hdr_cmd[0]);
        p->buf[0] = b;
        p->len = 1;
        p->state = ST_H2;
        return STEP_MORE;

    case ST_H2:
    case ST_H3:
    case ST_H4: {
        const uint8_t *hdr = p->is_cmd ? k_hdr_cmd : k_hdr_rep;
        if (b != hdr[p->state - ST_H2 + 1])
            return STEP_FAIL_CLEAN;
        p->buf[p->len++] = b;
        p->state = (p->state == ST_H4) ? ST_LEN1 : (uint8_t)(p->state + 1);
        return STEP_MORE;
    }

    case ST_LEN1:
        p->buf[p->len++] = b;
        p->state = ST_LEN2;
        return STEP_MORE;

    case ST_LEN2: {
        const uint16_t flen = (uint16_t)(p->buf[4] | ((uint16_t)b << 8));
        if (!parser_len_ok(p->is_cmd, flen))
            return STEP_FAIL_CLEAN; /* rejects len 0, oversize, value overflow */
        p->buf[p->len++] = b;
        p->need = flen;
        p->state = ST_PAYLOAD;
        return STEP_MORE;
    }

    case ST_PAYLOAD:
        p->buf[p->len++] = b; /* buf cap: 6 + 56 <= RADAR_FRAME_MAX_BYTES */
        if (--p->need > 0)
            return STEP_MORE;
        if (!parser_payload_ok(p))
            return STEP_FAIL_BUF;
        p->need = 4;
        p->state = ST_TAIL;
        return STEP_MORE;

    default: /* ST_TAIL */
    {
        const uint8_t *tail = p->is_cmd ? k_tail_cmd : k_tail_rep;
        if (b != tail[4u - p->need])
            return STEP_FAIL_CLEAN;
        if (--p->need > 0)
            return STEP_MORE;
        p->frames_ok++;
        parser_decode(p, out);
        p->state = ST_HEAD;
        p->len = 0;
        p->need = 0;
        return STEP_FRAME;
    }
    }
}

bool radar_parser_feed(radar_parser_t *p, uint8_t byte, radar_frame_t *out)
{
    if (p == NULL)
        return false;

    /* Replay queue for resynchronization: bytes that belonged to a failed
     * candidate and must go through the FSM again. Invariant: queue bytes
     * were recycled from p->buf (<= RADAR_FRAME_MAX_BYTES) plus at most the
     * one byte rejected without being buffered, so the queue fits. */
    uint8_t pend[RADAR_FRAME_MAX_BYTES + 1];
    size_t qh = 0;
    size_t qt = 0;
    uint8_t b = byte;

    for (;;) {
        const int r = parser_step(p, b, out);
        if (r == STEP_FRAME)
            return true;
        if (r == STEP_MORE) {
            if (qh < qt) {
                b = pend[qh++];
                continue;
            }
            return false;
        }

        /* Candidate failed: scan the buffered bytes from offset +1 for the
         * next header start; everything before it is dropped noise. */
        p->frames_bad++;
        size_t i = 1;
        while (i < p->len && p->buf[i] != k_hdr_cmd[0] && p->buf[i] != k_hdr_rep[0])
            i++;
        p->bytes_dropped += (uint32_t)i;

        const size_t rem = (size_t)p->len - i;
        const size_t queued = qt - qh;
        if (queued > 0)
            memmove(pend, pend + qh, queued);
        if (rem > 0)
            memcpy(pend + queued, &p->buf[i], rem);
        qh = 0;
        qt = queued + rem;
        if (r == STEP_FAIL_CLEAN) {
            if (b == k_hdr_cmd[0] || b == k_hdr_rep[0])
                pend[qt++] = b;     /* rejected byte starts another candidate */
            else
                p->bytes_dropped++; /* rejected byte was pure noise */
        }

        p->state = ST_HEAD;
        p->len = 0;
        p->need = 0;

        if (qh == qt)
            return false;
        b = pend[qh++];
    }
}
