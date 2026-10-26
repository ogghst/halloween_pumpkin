/* Byte-wise frame parser for the HLK-LD2420 protocol (see radar_protocol.h).
 *
 * Three stream formats share one FSM:
 *  - command/ACK frames  FD FC FB FA | len:2 | cmd:2 + value | 04 03 02 01,
 *  - energy frames       F4 F3 F2 F1 | 0x0023 | presence:1 + range:2 +
 *                        16x energy:2 | F8 F7 F6 F5 (45 B total),
 *  - running-mode text lines (fw <= 1.5.3): printable ASCII accumulated up
 *    to RADAR_TEXT_LINE_MAX bytes, emitted as a REPORT on CRLF.
 *
 * Pure C: no allocation, no clock, total robustness on arbitrary input.
 * Resynchronization rule: when a candidate frame fails validation (length
 * or tail), the buffered bytes are re-scanned from offset +1 and fed through
 * the FSM again, so a valid frame that starts inside a corrupted one is
 * still recovered. Tail bytes are validated while streaming (they are
 * protocol constants) so the buffer only ever holds header + length +
 * payload <= 6 + 76 bytes.
 *
 * Counters: frames_ok counts validated binary frames (energy frames and
 * ACKs), lines_parsed counts emitted text lines; frames_bad/bytes_dropped
 * cover rejected candidates and dropped bytes (noise, aborted or overlong
 * lines).
 */
#include "radar_protocol.h"

#include <string.h>

/* Energy frame layout: 4 header + len 2 + payload 35 + tail 4. */
#define ENERGY_PAYLOAD_LEN (RADAR_ENERGY_FRAME_TOTAL - 10u) /* 0x0023 */
_Static_assert(ENERGY_PAYLOAD_LEN == 0x0023u, "energy frame layout drift");

/* FSM states (radar_parser_t::state). */
enum {
    ST_HEAD = 0,  /* scanning for a header byte or the start of a text line */
    ST_H2,        /* second header byte */
    ST_H3,        /* third header byte */
    ST_H4,        /* fourth header byte */
    ST_LEN1,      /* declared intra-frame length, low byte */
    ST_LEN2,      /* ... high byte; declared length validated here */
    ST_PAYLOAD,   /* intra-frame data, counted by need */
    ST_TAIL,      /* end-of-frame bytes, need = remaining (4..1) */
    ST_LINE,      /* accumulating a running-mode text line */
    ST_LINE_CR,   /* CR seen inside a text line, waiting for LF */
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

/* Printable ASCII run inside a text line; CR/LF are handled by the FSM. */
static bool line_is_text(uint8_t b)
{
    return b >= 0x20 && b <= 0x7E;
}

void radar_parser_init(radar_parser_t *p)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p)); /* ST_HEAD == 0, len/need/line cleared */
}

/* Declared intra-frame length limits:
 *  - command/ACK: >= 2, frame must fit the buffer (len <= max - 8), and the
 *    value after the 2 status bytes must fit the ACK value storage, i.e.
 *    len <= 4 + RADAR_ACK_VALUE_MAX. ACKs shorter than 4 carry no status
 *    word and are rejected later at payload validation (frames_bad).
 *  - energy frame: exactly ENERGY_PAYLOAD_LEN; any other declared length on
 *    the report header is an unknown binary format and is rejected. */
static bool parser_len_ok(bool is_cmd, uint16_t len)
{
    if (len > (uint16_t)(RADAR_FRAME_MAX_BYTES - 8))
        return false;
    if (is_cmd)
        return len >= 2 && len <= 4u + RADAR_ACK_VALUE_MAX;
    return len == ENERGY_PAYLOAD_LEN;
}

/* Payload validation once the declared number of bytes is buffered. Energy
 * frames carry no marker bytes: the pinned length plus the exact 4-byte
 * tail are the integrity check. */
static bool parser_payload_ok(const radar_parser_t *p)
{
    if (!p->is_cmd)
        return true;

    const uint16_t flen = (uint16_t)(p->buf[4] | ((uint16_t)p->buf[5] << 8));
    return flen >= 4; /* ACK word + status word must both be present */
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

    /* Energy frame: [presence:1] [range:2 cm] [16x energy:2], all LE. */
    const uint8_t *q = &p->buf[6];
    radar_data_frame_t *d = &out->data;

    out->kind = RADAR_FRAME_REPORT;
    d->gates_valid = true;
    d->raw_state = q[0];
    d->presence = q[0] != 0;
    d->distance_mm = parser_cm_to_mm((uint16_t)(q[1] | ((uint16_t)q[2] << 8)));
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        d->gate_energy[g] =
            (uint16_t)(q[3u + 2u * g] | ((uint16_t)q[4u + 2u * g] << 8));
    /* timestamp_us stays 0: the caller owns the clock. */
}

/* True when the token occurs anywhere in the line window. */
static bool line_contains(const uint8_t *line, uint8_t len, const char *tok)
{
    const size_t n = strlen(tok);
    if ((size_t)len < n)
        return false;
    for (size_t i = 0; i + n <= (size_t)len; i++) {
        if (memcmp(&line[i], tok, n) == 0)
            return true;
    }
    return false;
}

/* Decode one finished text line into a REPORT (running mode, fw <= 1.5.3):
 * presence from the "ON"/"OFF" tokens, distance from the first digit run
 * (cm on the wire, converted to mm; 0 when the line carries none). */
static void line_decode(const radar_parser_t *p, radar_frame_t *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));

    out->kind = RADAR_FRAME_REPORT;
    out->data.gates_valid = false;

    /* "OFF" is searched before "ON": a line carrying both resolves to
     * absence (running-mode token rule). */
    const bool off = line_contains(p->line, p->line_len, "OFF");
    const bool on = line_contains(p->line, p->line_len, "ON");
    out->data.presence = on && !off;
    out->data.raw_state = out->data.presence ? 1u : 0u;

    uint8_t i = 0;
    while (i < p->line_len && (p->line[i] < '0' || p->line[i] > '9'))
        i++;
    uint32_t cm = 0;
    for (; i < p->line_len && p->line[i] >= '0' && p->line[i] <= '9'; i++) {
        cm = cm * 10u + (uint32_t)(p->line[i] - '0');
        if (cm > 0xFFFFu)
            cm = 0xFFFFu; /* saturate; the mm conversion caps anyway */
    }
    out->data.distance_mm = parser_cm_to_mm((uint16_t)cm);
}

/* Close the current text line on CRLF: emit it as a REPORT, or account for
 * the buffered bytes when the line had outgrown the buffer. is_cmd doubles
 * as the overlong-line flag while a line is accumulating (no frame
 * candidate is active in the line states). */
static int line_finish(radar_parser_t *p, radar_frame_t *out)
{
    const bool over = p->is_cmd;

    p->state = ST_HEAD;
    p->is_cmd = false;
    if (over) {
        p->bytes_dropped += p->line_len; /* the overflow bytes were already counted */
    } else {
        p->lines_parsed++;
        line_decode(p, out);
    }
    p->line_len = 0;
    return over ? STEP_MORE : STEP_FRAME;
}

/* One byte in line context: printable characters extend the line, a frame
 * header aborts the line (its bytes are dropped, the byte starts a frame
 * candidate), anything else is dropped noise that keeps the line alive.
 * Never delivers a frame, so no output slot is needed. */
static int line_byte(radar_parser_t *p, uint8_t b)
{
    if (b == k_hdr_cmd[0] || b == k_hdr_rep[0]) {
        p->bytes_dropped += p->line_len;
        p->line_len = 0;
        p->is_cmd = (b == k_hdr_cmd[0]);
        p->buf[0] = b;
        p->len = 1;
        p->state = ST_H2;
        return STEP_MORE;
    }
    if (line_is_text(b)) {
        if (p->line_len < RADAR_TEXT_LINE_MAX) {
            p->line[p->line_len++] = b;
        } else {
            p->is_cmd = true; /* line overgrown: dropped at CRLF */
            p->bytes_dropped++;
        }
        return STEP_MORE;
    }
    p->bytes_dropped++;
    return STEP_MORE;
}

/* Consume one byte against the current FSM state. The byte is appended to
 * the candidate buffer only when accepted; STEP_FAIL_* leaves the candidate
 * in p->buf[0..p->len) for the resynchronization scan. */
static int parser_step(radar_parser_t *p, uint8_t b, radar_frame_t *out)
{
    switch (p->state) {
    case ST_HEAD:
        if (b == k_hdr_cmd[0] || b == k_hdr_rep[0]) {
            p->is_cmd = (b == k_hdr_cmd[0]);
            p->buf[0] = b;
            p->len = 1;
            p->state = ST_H2;
        } else if (line_is_text(b)) {
            p->line[0] = b; /* running-mode text line starts */
            p->line_len = 1;
            p->is_cmd = false;
            p->state = ST_LINE;
        } else {
            p->bytes_dropped++;
        }
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
            return STEP_FAIL_CLEAN; /* rejects 0, oversize, wrong energy len */
        p->buf[p->len++] = b;
        p->need = flen;
        p->state = ST_PAYLOAD;
        return STEP_MORE;
    }

    case ST_PAYLOAD:
        p->buf[p->len++] = b; /* buf cap: 6 + 76 <= RADAR_FRAME_MAX_BYTES */
        if (--p->need > 0)
            return STEP_MORE;
        if (!parser_payload_ok(p))
            return STEP_FAIL_BUF;
        p->need = 4;
        p->state = ST_TAIL;
        return STEP_MORE;

    case ST_LINE:
        if (b == 0x0D) {
            p->state = ST_LINE_CR;
            return STEP_MORE;
        }
        return line_byte(p, b);

    case ST_LINE_CR:
        if (b == 0x0A)
            return line_finish(p, out);
        if (b == 0x0D) { /* repeated CR: the new one becomes pending */
            p->bytes_dropped++;
            return STEP_MORE;
        }
        p->bytes_dropped++; /* the pending CR was stray */
        p->state = ST_LINE;
        return line_byte(p, b);

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
