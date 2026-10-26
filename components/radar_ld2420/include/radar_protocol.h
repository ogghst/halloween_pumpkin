/* HLK-LD2420 serial protocol: frame parser and command codec.
 *
 * Pure C, no ESP-IDF dependency: unit-testable on host.
 * Reference: Hi-Link HLK-LD2420 protocol (translated document, extracted
 * 2026-10-25), cross-checked against the ESPHome ld2420 component
 * (esphome/esphome, consulted 2026-10-25). All multi-byte fields are
 * little-endian.
 *
 * Command frame : FD FC FB FA | len:2 | cmd:2 + value:N | 04 03 02 01
 * ACK frame     : FD FC FB FA | len:2 | (cmd|0x0100):2 + status:2 + data | 04 03 02 01
 * Energy frame  : F4 F3 F2 F1 | len:2 (=0x0023) | presence:1 + range:2 + 16x energy:2 | F8 F7 F6 F5
 * Running mode  (fw <= 1.5.3, no energy frames): ASCII lines terminated by
 *                 CRLF containing "ON"/"OFF" and "Range XXXX" (cm).
 * Firmware "v1.5.34" -> numeric 1534 (digits concatenated); energy frames
 * require numeric >= 154.
 */
#ifndef RADAR_PROTOCOL_H
#define RADAR_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_MAX_GATES        16u
/* Largest frames: write-all-thresholds command (122 B total) and a
 * read-all-thresholds ACK (76 B total). */
#define RADAR_FRAME_MAX_BYTES  128u
#define RADAR_ACK_VALUE_MAX    72u  /* status:2 + 16x u32 = 66 B */
#define RADAR_CMD_VALUE_MAX    112u /* 16x (name:2 + u32:4) = 96 B + margin */
#define RADAR_ENERGY_FRAME_TOTAL 45u /* header 4 + len 2 + payload 35 + tail 4 */
#define RADAR_ALL_GATES        0xFFFFu
#define RADAR_TEXT_LINE_MAX    64u

/* Command words (verified: protocol doc samples + ESPHome ld2420.cpp). */
#define RADAR_CMD_ENTER_CONFIG     0x00FFu /* value 0x0001; ACK adds proto ver:2 + buffer:2 */
#define RADAR_CMD_EXIT_CONFIG      0x00FEu
#define RADAR_CMD_READ_VERSION     0x0000u /* ACK: status + len:2 + ASCII text */
#define RADAR_CMD_RESTART          0x0068u
#define RADAR_CMD_READ_PARAMS      0x0008u /* value: N x name:2; ACK: N x u32 (same order) */
#define RADAR_CMD_WRITE_PARAMS     0x0007u /* value: N x (name:2 + u32:4) */
#define RADAR_CMD_WRITE_SYS_PARAM  0x0012u /* value: name 0x0000 + u32 mode */

/* Module parameter names (READ_PARAMS/WRITE_PARAMS). */
#define RADAR_PARAM_MIN_GATE       0x0000u /* 0..15 */
#define RADAR_PARAM_MAX_GATE       0x0001u /* 0..15 */
#define RADAR_PARAM_TIMEOUT_S      0x0004u /* presence hold, seconds 0..255 */
#define RADAR_PARAM_MOVE_THRESH(g) ((uint16_t)(0x0010u + (g))) /* trigger threshold */
#define RADAR_PARAM_STILL_THRESH(g) ((uint16_t)(0x0020u + (g))) /* keep threshold */

/* System mode values (WRITE_SYS_PARAM, name 0x0000). */
#define RADAR_SYSMODE_DEBUG   0x00u
#define RADAR_SYSMODE_ENERGY  0x04u
#define RADAR_SYSMODE_RUNNING 0x64u

/* Decoded report. Energy-mode frames carry the 16 gate energies; running
 * mode text frames set gates_valid = false. The wire distance unit is cm
 * (protocol running-mode spec); converted to mm here. */
typedef struct {
    bool presence;         /* presence byte != 0 */
    uint8_t raw_state;     /* presence byte as reported (0 = nobody) */
    uint16_t distance_mm;  /* range word x10; 0 when the module reports none */
    bool gates_valid;
    uint16_t gate_energy[RADAR_MAX_GATES];
    int64_t timestamp_us;  /* filled by the caller: the parser has no clock */
} radar_data_frame_t;

typedef enum {
    RADAR_FRAME_NONE = 0,
    RADAR_FRAME_REPORT,    /* energy frame or running-mode text line */
    RADAR_FRAME_ACK,
} radar_frame_kind_t;

typedef struct {
    radar_frame_kind_t kind;
    radar_data_frame_t data;  /* valid when kind == RADAR_FRAME_REPORT */
    struct {                  /* valid when kind == RADAR_FRAME_ACK */
        uint16_t command;     /* command word | 0x0100 */
        uint16_t status;      /* 0 success, anything else failure */
        uint8_t value_len;
        uint8_t value[RADAR_ACK_VALUE_MAX];
    } ack;
} radar_frame_t;

/* Parser state, owned by the caller: no dynamic allocation anywhere.
 * The fields below are internal; do not rely on them outside the component. */
typedef struct {
    uint8_t buf[RADAR_FRAME_MAX_BYTES];
    uint8_t line[RADAR_TEXT_LINE_MAX];
    uint16_t len;
    uint16_t need;
    uint8_t line_len;
    uint8_t state;
    bool is_cmd;
    /* diagnostics */
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t bytes_dropped;
    uint32_t lines_parsed;
} radar_parser_t;

void radar_parser_init(radar_parser_t *p);

/* Feed one byte. Returns true exactly once per complete frame: a validated
 * energy frame (header, length 0x0023, tail), a complete running-mode text
 * line (CRLF, decoded into presence/distance), or a validated ACK. Corrupt
 * data triggers resynchronization: no crash, no overflow on arbitrary
 * input. */
bool radar_parser_feed(radar_parser_t *p, uint8_t byte, radar_frame_t *out);

/* --- command codec (pure) --- */

/* Build a command frame into out. Returns the total byte count written,
 * or -1 when out_size is insufficient or value_len > RADAR_CMD_VALUE_MAX. */
int radar_cmd_build(uint8_t *out, size_t out_size, uint16_t command,
                    const uint8_t *value, size_t value_len);

/* True when `frame` is an ACK frame for `command` (word | 0x0100). */
bool radar_cmd_is_ack_of(const radar_frame_t *frame, uint16_t command);

/* --- synchronous command transactions (transport injected) --- */

enum {
    RADAR_CMD_OK = 0,
    RADAR_CMD_ERR_STATUS = -1,
    RADAR_CMD_ERR_TIMEOUT = -2,
    RADAR_CMD_ERR_IO = -3,
    RADAR_CMD_ERR_ARG = -4,
};

/* Injected transport so the transaction logic is host-testable.
 * write: 0 on success, <0 on error. read: bytes read (0 = nothing now),
 * <0 on error. timeout_ms is the overall budget for one transaction. */
typedef struct {
    int (*write)(void *ctx, const uint8_t *buf, size_t len);
    int (*read)(void *ctx, uint8_t *buf, size_t len);
    void *ctx;
    uint32_t timeout_ms;
} radar_command_io_t;

/* Send one command and consume bytes until its ACK arrives, skipping
 * unrelated REPORT frames. Returns RADAR_CMD_OK when the ACK status is 0. */
int radar_command_transact(const radar_command_io_t *io, radar_parser_t *parser,
                           uint16_t command, const uint8_t *value,
                           size_t value_len, radar_frame_t *ack_out);

typedef int (*radar_command_step_t)(const radar_command_io_t *io,
                                    radar_parser_t *parser, void *ctx);

/* ENTER_CONFIG -> step -> ALWAYS EXIT_CONFIG (even when step failed or
 * timed out). If ENTER_CONFIG itself fails, no step runs and no EXIT is
 * sent (the module ignores commands outside config mode anyway) and the
 * enter error is returned. */
int radar_command_session(const radar_command_io_t *io, radar_parser_t *parser,
                          radar_command_step_t step, void *ctx);

/* --- parameter payload codecs --- */

/* Build READ_PARAMS value: `count` little-endian names from `names`.
 * Returns value length or -1 on error (count > RADAR_MAX_GATES). */
int radar_params_build_read(const uint16_t *names, size_t count,
                            uint8_t *out, size_t out_size);

/* Parse READ_PARAMS ACK payload (after the status word): `count` u32
 * values in request order. Returns 0 or -1 when truncated. */
int radar_params_parse_values(const uint8_t *value, size_t len,
                              uint32_t *values, size_t count);

/* Build WRITE_PARAMS value from (name, u32) pairs. Returns value length
 * or -1 on error. */
int radar_params_build_write(const uint16_t *names, const uint32_t *values,
                             size_t count, uint8_t *out, size_t out_size);

/* Convenience: build the 3-pair write value for (min gate, max gate,
 * timeout seconds). Validated: min <= max <= 15, timeout <= 255. */
int radar_params_build_gate_limits(uint16_t min_gate, uint16_t max_gate,
                                   uint16_t timeout_s,
                                   uint8_t *out, size_t out_size);

/* Convenience: build the 32-pair write value for all move/still gate
 * thresholds. Returns value length (192: 32 pairs x 6 B) or -1. NOTE: 192 B
 * exceeds RADAR_CMD_VALUE_MAX: send it as TWO 16-pair WRITE_PARAMS commands
 * (move bank 0x0010..0x001F, then still bank 0x0020..0x002F). */
int radar_params_build_all_thresholds(const uint32_t move[RADAR_MAX_GATES],
                                      const uint32_t still[RADAR_MAX_GATES],
                                      uint8_t *out, size_t out_size);

/* Build WRITE_SYS_PARAM value for a system mode (RADAR_SYSMODE_*). */
int radar_params_build_sysmode(uint8_t mode, uint8_t *out, size_t out_size);

/* Firmware version from the READ_VERSION ACK payload: status already
 * stripped; value = len:2 + ASCII text ("v1.5.4", doc sample; "v1.5.34"
 * on newer modules). `numeric` concatenates the digits (154 / 1534);
 * `energy_capable` is numeric >= 154 (fw >= 1.5.4 streams energy frames,
 * older fw only running-mode text). Returns 0 or -1 on malformed payload. */
typedef struct {
    char text[16];
    int32_t numeric;
    bool energy_capable;
} radar_fw_version_t;

int radar_fw_version_parse(const uint8_t *value, size_t len,
                           radar_fw_version_t *out);

#ifdef __cplusplus
}
#endif
#endif /* RADAR_PROTOCOL_H */
