/* HLK-LD2410B serial protocol: frame parser and command codec.
 *
 * Pure C, no ESP-IDF dependency: unit-testable on host.
 * Reference: "HLK-LD2410B Human presence sensing module - Serial
 * communication protocol", V1.07, 2024-08-05, Shenzhen Hi-Link Electronic.
 * All multi-byte fields are little-endian.
 *
 * Frame formats (protocol v1.07):
 *   command/ACK : FD FC FB FA | len:2 | cmd:2 + value:N | 04 03 02 01
 *   data report : F4 F3 F2 F1 | len:2 | type + AA + data + 55 + 00 | F8 F7 F6 F5
 * Known doc errata (documented in docs/wiki/insidie): the prose for "close
 * engineering mode" says command 0x0062 but its own frame examples use
 * 0x0063; we follow the byte examples.
 */
#ifndef RADAR_PROTOCOL_H
#define RADAR_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol limits (v1.07): max 8 distance gates; largest valid frames are
 * the engineering-mode report (45 B total) and the read-params ACK (36 B). */
#define RADAR_MAX_GATES        8u
#define RADAR_FRAME_MAX_BYTES  64u
#define RADAR_ACK_VALUE_MAX    32u  /* largest ACK payload after status: 24 B */
#define RADAR_CMD_VALUE_MAX    20u  /* largest command value: 0x0060/0x0064 = 18 B */
#define RADAR_ALL_GATES        0xFFFFu /* gate word addressing every gate (§2.2.7) */

/* Distance-resolution indexes (§2.2.16, Table 8). Factory default is 0.75 m
 * per gate (Table 7). NOTE: v1.07 prose for the factory default contradicts
 * Table 8; Table 8 is authoritative (0x0000 -> 0.75 m, 0x0001 -> 0.2 m). */
#define RADAR_RESOLUTION_075   0x0000u
#define RADAR_RESOLUTION_02    0x0001u

/* Command words (§2.2). */
#define RADAR_CMD_ENTER_CONFIG     0x00FFu /* value 0x0001; ACK adds proto ver + buffer size */
#define RADAR_CMD_EXIT_CONFIG      0x00FEu
#define RADAR_CMD_LIMITS           0x0060u /* max moving/static gate + unmanned duration */
#define RADAR_CMD_READ_PARAMS      0x0061u
#define RADAR_CMD_ENG_ON           0x0062u
#define RADAR_CMD_ENG_OFF          0x0063u
#define RADAR_CMD_SET_SENSITIVITY  0x0064u
#define RADAR_CMD_FW_VERSION       0x00A0u
#define RADAR_CMD_SET_BAUD         0x00A1u /* not used at runtime (factory default 256000) */
#define RADAR_CMD_FACTORY_RESET    0x00A2u /* console-only, explicit confirmation */
#define RADAR_CMD_MODULE_RESET     0x00A3u
#define RADAR_CMD_QUERY_RESOLUTION 0x00ABu

/* Target state (§2.3.2, Table 14). */
typedef enum {
    RADAR_TARGET_NONE = 0x00,
    RADAR_TARGET_MOVING = 0x01,
    RADAR_TARGET_STATIC = 0x02,
    RADAR_TARGET_BOTH = 0x03,
    RADAR_TARGET_NOISE_CAL_RUNNING = 0x04, /* only during background-noise detection */
    RADAR_TARGET_NOISE_CAL_OK = 0x05,
    RADAR_TARGET_NOISE_CAL_FAIL = 0x06,
} radar_target_state_t;

/* Decoded data report (§2.3.2, Table 13 basic + Table 15 engineering).
 * Distances converted to mm (the wire format is cm). */
typedef struct {
    radar_target_state_t target_state;
    uint16_t moving_distance_mm;
    uint8_t moving_energy;       /* 0..100 */
    uint16_t static_distance_mm;
    uint8_t static_energy;       /* 0..100 */
    uint16_t detection_distance_mm;
    /* Engineering-mode extras, valid only when engineering is true. */
    bool engineering;
    uint8_t max_moving_gate;     /* 0..8 */
    uint8_t max_static_gate;
    uint8_t moving_gate_energy[RADAR_MAX_GATES + 1];
    uint8_t static_gate_energy[RADAR_MAX_GATES + 1];
    uint8_t light_level;         /* photosensitive value 0..255 */
    uint8_t out_level;           /* OUT pin state reported by the module, 0/1 */
    int64_t timestamp_us;        /* filled by the caller: the parser has no clock */
} radar_data_frame_t;

typedef enum {
    RADAR_FRAME_NONE = 0,
    RADAR_FRAME_REPORT,          /* F4..F1 header: radar -> host data */
    RADAR_FRAME_ACK,             /* FD..FA header: ACK of a command */
} radar_frame_kind_t;

typedef struct {
    radar_frame_kind_t kind;
    radar_data_frame_t data;     /* valid when kind == RADAR_FRAME_REPORT */
    struct {                     /* valid when kind == RADAR_FRAME_ACK */
        uint16_t command;        /* command word | 0x0100 */
        uint16_t status;         /* 0 success, 1 failure */
        uint8_t value_len;
        uint8_t value[RADAR_ACK_VALUE_MAX]; /* payload after the status word */
    } ack;
} radar_frame_t;

/* Parser state, owned by the caller: no dynamic allocation anywhere.
 * The fields below are internal; do not rely on them outside the component. */
typedef struct {
    uint8_t buf[RADAR_FRAME_MAX_BYTES];
    uint16_t len;             /* bytes buffered for the candidate frame */
    uint16_t need;            /* bytes still missing (0 while scanning) */
    uint8_t state;            /* FSM state */
    bool is_cmd;              /* candidate frame uses the FD..FA header */
    /* diagnostics */
    uint32_t frames_ok;
    uint32_t frames_bad;      /* rejected: length, tail or payload format */
    uint32_t bytes_dropped;   /* resynchronized noise bytes */
} radar_parser_t;

void radar_parser_init(radar_parser_t *p);

/* Feed one byte. Returns true exactly once per complete, well-formed frame
 * (header, length, payload markers and tail all validated) and fills *out.
 * Corrupted data triggers resynchronization: no crash, no overflow on
 * arbitrary input. Frames whose declared length exceeds the buffer or whose
 * mandatory payload markers are wrong are rejected (frames_bad). */
bool radar_parser_feed(radar_parser_t *p, uint8_t byte, radar_frame_t *out);

/* --- command codec (pure) --- */

/* Build a command frame into out. Returns the total byte count written
 * (8 + 2 + value_len), or -1 when out_size is insufficient or value_len
 * is implausible (> RADAR_CMD_VALUE_MAX). */
int radar_cmd_build(uint8_t *out, size_t out_size, uint16_t command,
                    const uint8_t *value, size_t value_len);

/* True when `frame` is an ACK frame for `command` (word | 0x0100). */
bool radar_cmd_is_ack_of(const radar_frame_t *frame, uint16_t command);

/* --- synchronous command transactions (transport injected) --- */

enum {
    RADAR_CMD_OK = 0,
    RADAR_CMD_ERR_STATUS = -1, /* ACK received with status != 0 */
    RADAR_CMD_ERR_TIMEOUT = -2,
    RADAR_CMD_ERR_IO = -3,
    RADAR_CMD_ERR_ARG = -4,
};

/* Injected transport so the transaction logic is host-testable.
 * write: return 0 on success, <0 on error. read: return the number of bytes
 * read into buf (0 = nothing available now), <0 on error. timeout_ms is the
 * overall budget for one transaction. */
typedef struct {
    int (*write)(void *ctx, const uint8_t *buf, size_t len);
    int (*read)(void *ctx, uint8_t *buf, size_t len);
    void *ctx;
    uint32_t timeout_ms;
} radar_command_io_t;

/* Send one command and consume bytes until its ACK arrives, skipping
 * unrelated REPORT frames on the way. Returns RADAR_CMD_OK when the ACK
 * status is 0. `ack_out` (nullable) receives the full ACK frame. */
int radar_command_transact(const radar_command_io_t *io, radar_parser_t *parser,
                           uint16_t command, const uint8_t *value,
                           size_t value_len, radar_frame_t *ack_out);

typedef int (*radar_command_step_t)(const radar_command_io_t *io,
                                    radar_parser_t *parser, void *ctx);

/* ENTER_CONFIG -> step -> ALWAYS EXIT_CONFIG. The exit command is sent even
 * when step fails, times out or errors (protocol requirement: leave the
 * module in working mode). Returns the step result, or its own error when
 * ENTER_CONFIG itself fails. */
int radar_command_session(const radar_command_io_t *io, radar_parser_t *parser,
                          radar_command_step_t step, void *ctx);

/* --- structured payload codecs --- */

/* Module parameters as returned by READ_PARAMS (§2.2.4). */
typedef struct {
    uint8_t max_gates;         /* header byte N (<= 8) reported by the module */
    uint8_t max_moving_gate;
    uint8_t max_static_gate;
    uint8_t motion_sensitivity[RADAR_MAX_GATES + 1];
    uint8_t static_sensitivity[RADAR_MAX_GATES + 1];
    uint16_t unmanned_duration_s;
} radar_module_params_t;

/* Parse the READ_PARAMS ACK payload (after the status word):
 * AA | N | maxmov | maxst | mov[0..N] | st[0..N] | duration:2.
 * Returns 0, or -1 on malformed payload (wrong marker or truncated). */
int radar_params_parse(const uint8_t *value, size_t len, radar_module_params_t *out);

/* Build the LIMITS (0x0060) command value: three (word, u32) pairs:
 * (0x0000, max moving gate) (0x0001, max static gate) (0x0002, duration s).
 * Protocol §2.2.3: gates 2..8, duration 0..65535. Returns value length or -1. */
int radar_params_build_limits(uint16_t max_moving_gate, uint16_t max_static_gate,
                              uint16_t unmanned_duration_s,
                              uint8_t *out, size_t out_size);

/* Build the SET_SENSITIVITY (0x0064) command value: (gate, motion, static)
 * each as (word:2, u32:4); gate == RADAR_ALL_GATES configures every gate.
 * Returns value length or -1. */
int radar_params_build_sensitivity(uint16_t gate, uint8_t motion,
                                   uint8_t static_sens,
                                   uint8_t *out, size_t out_size);

/* Firmware version from the FW_VERSION ACK payload (§2.2.8):
 * type:2 major:2 minor:4. Example bytes 00 00 | 00 00 | 02 01 | 16 24 06 22
 * decode to text "V1.02.22062416". Returns 0 or -1 on malformed payload. */
typedef struct {
    uint16_t fw_type;
    uint32_t major;   /* 0x0102 -> 1.02 */
    uint32_t minor;   /* 0x22062416 */
    char text[24];
} radar_fw_version_t;

int radar_fw_version_parse(const uint8_t *value, size_t len,
                           radar_fw_version_t *out);

/* Resolution-query ACK payload (§2.2.17): index:2 (Table 8).
 * Returns 0 or -1 on malformed payload. */
int radar_resolution_parse(const uint8_t *value, size_t len, uint16_t *index_out);

/* Width of one distance gate in mm for a resolution index. */
uint16_t radar_gate_mm(uint16_t resolution_index);

#ifdef __cplusplus
}
#endif
#endif /* RADAR_PROTOCOL_H */
