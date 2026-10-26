/* Target integration of the radar_ld2420 component (ESP-IDF, Fase 2).
 *
 * Owns the UART and the parser, runs the trigger, posts presence events on
 * the default event loop, probes the module baud rate, selects the serial
 * output mode (energy frames on fw >= 1.5.4, running text otherwise),
 * keeps the module configuration in sync at boot (writes ONLY what
 * differs), detects sensor faults, degrades to the OUT pin and recovers
 * with cyclic attempts and exponential backoff.
 *
 * Design choices (Fase 2, LD2420):
 *  - everything runs in one "radar" task (priority 5, stack
 *    CONFIG_RADAR_TASK_STACK): the RX loop, all config sessions (console
 *    requests are routed through a one-slot queue so the UART and the
 *    parser never have two consumers) and the calibration sampling;
 *  - UART poll 20 ms; per-command ACK budget 250 ms; baud probe budget
 *    400 ms per candidate (CONFIG_RADAR_BAUD, or 115200 then 256000 when
 *    it is 0); console request round-trip budget 8 s (a full boot sync
 *    with threshold banks stays well under it);
 *  - absent module: the probe logs "modulo assente o muto" and the task
 *    keeps running; the fault monitor takes over and retries with
 *    exponential backoff (no crash, no block, console alive);
 *  - fault watchdog: the silence timer is refreshed ONLY by validated
 *    module traffic (decoded reports in the RX loop, ACKs/frames/text
 *    lines parsed during a config session, a successful baud probe);
 *    raw RX noise can never mask a mute sensor;
 *  - recovery: cyclic attempts (1) UART reinit, (2) RESTART session,
 *    (3) RADAR_EN toggle when wired, waiting 1 s * 2^n between attempts
 *    (cap 30 s, reset on the first valid frame);
 *  - degraded mode: while the fault is active the OUT pin level drives
 *    PRESENCE_ON/OFF events flagged degraded=true; the trigger hold keeps
 *    expiring on wall time via synthetic no-target frames;
 *  - threshold writes: the full table (32 pairs = 192 B of payload) does
 *    NOT fit one command, so it is sent as TWO 16-pair WRITE_PARAMS
 *    (move bank 0x0010..0x001F, still bank 0x0020..0x002F); a single gate
 *    is one WRITE_PARAMS with 2 pairs (move + still of that gate);
 *  - NVS namespace "radar": "trigger_mm" (u16) trigger distance,
 *    "min_gate"/"max_gate" (u8) + "unmanned_s" (u16) desired gate limits,
 *    "move_thresh"/"still_thresh" (64 B blobs = 16x u32) desired
 *    threshold banks (written by calibrate apply / threshold changes);
 *    the boot sync reads the module once and rewrites only what differs;
 *    the legacy LD2410 keys ("sens", "max_mov", "max_st") are erased at
 *    boot.
 *
 * Concurrency: the radar task is the only writer of the module-facing
 * state; console readers copy the snapshot under a spinlock. No dynamic
 * allocation in the RX path (single stack buffer, caller-owned parser).
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "radar_ld2420.h"

ESP_EVENT_DEFINE_BASE(PRESENCE_EVENT);

static const char *TAG = "radar";

/* --- timing choices (see file header) --- */
#define RADAR_RX_POLL_MS        20u
#define RADAR_CMD_TIMEOUT_MS    250u
#define RADAR_PROBE_TIMEOUT_MS  400u
#define RADAR_REQ_WAIT_MS       8000u
#define RADAR_REQ_WAIT_SLICE_MS 1000u /* passi di attesa per il feed del WDT */
#define RADAR_REBOOT_DELAY_MS   600u /* RESTART: the module reboots after the ACK */
#define RADAR_EN_PULSE_MS       100u
#define RADAR_BACKOFF_INIT_MS   1000u
#define RADAR_BACKOFF_CAP_MS    30000u
#define RADAR_BOOT_RETRY_MS     2000u /* boot re-sync retry when not faulted */
#define RADAR_TASK_PRIORITY     5

/* --- persistent state (namespace "radar") --- */
#define RADAR_NVS_NAMESPACE  "radar"
#define RADAR_NVS_TRIGGER    "trigger_mm"
#define RADAR_NVS_MIN_GATE   "min_gate"
#define RADAR_NVS_MAX_GATE   "max_gate"
#define RADAR_NVS_UNMANNED   "unmanned_s"
#define RADAR_NVS_MOVE_THR   "move_thresh"
#define RADAR_NVS_STILL_THR  "still_thresh"
#define RADAR_THRESH_BLOB_LEN (RADAR_MAX_GATES * sizeof(uint32_t)) /* 64 B */

/* Factory table (ESPHome ld2420 defaults): the LD2420 has no factory
 * reset command, radar_factory_reset rewrites these values. */
static const uint32_t k_factory_move[RADAR_MAX_GATES] = {
    60000, 30000, 400, 250, 250, 250, 250, 250,
    250, 250, 250, 250, 250, 250, 250, 250,
};
static const uint32_t k_factory_still[RADAR_MAX_GATES] = {
    40000, 20000, 200, 200, 200, 200, 200, 150,
    150, 100, 100, 100, 100, 100, 100, 100,
};
#define RADAR_FACTORY_MIN_GATE  1u
#define RADAR_FACTORY_MAX_GATE  12u
#define RADAR_FACTORY_TIMEOUT_S 120u

/* Desired gate limits (NVS when present, else Kconfig). */
typedef struct {
    uint8_t min_gate;
    uint8_t max_gate;
    uint16_t timeout_s;
} radar_desired_t;

typedef enum {
    REQ_FW_VERSION,
    REQ_READ_PARAMS,
    REQ_SET_LIMITS,
    REQ_SET_THRESHOLDS,
    REQ_SET_ENGINEERING,
    REQ_RESTART,
    REQ_FACTORY_RESET,
    REQ_CAL_START,
    REQ_CAL_APPLY,
} radar_req_kind_t;

typedef struct {
    radar_req_kind_t kind;
    /* arguments */
    uint16_t gate;
    uint32_t move;
    uint32_t still;
    uint16_t min_gate;
    uint16_t max_gate;
    uint16_t hold_s;
    bool eng_on;
    uint32_t seconds;
    uint32_t move_vals[RADAR_MAX_GATES];
    uint32_t still_vals[RADAR_MAX_GATES];
    /* outputs */
    radar_fw_version_t *fw_out;
    radar_module_params_t *params_out;
} radar_req_t;

/* --- static state --- */
static uart_port_t s_uart = (uart_port_t)CONFIG_RADAR_UART_NUM;
static radar_parser_t s_parser;
static radar_trigger_t s_trigger;

static radar_state_t s_state;  /* snapshot mirror, written only by the task */
static radar_desired_t s_des;  /* desired gate limits */
static radar_fw_version_t s_fw;
static bool s_fw_valid;
static bool s_baud_known;
static uint32_t s_baud;        /* probed module baud (0 until it answers) */
static uint32_t s_mod_move[RADAR_MAX_GATES];  /* module threshold mirror */
static uint32_t s_mod_still[RADAR_MAX_GATES];
static bool s_mod_valid;
static bool s_synced;          /* boot config sync done */
static uint8_t s_stage;        /* cyclic recovery stage 0..2 */
static uint32_t s_backoff_ms = RADAR_BACKOFF_INIT_MS;
static int64_t s_next_action_us;
static int64_t s_next_sync_us;

static volatile bool s_fault;
static bool s_degraded_presence; /* OUT-pin latch while faulted */
#if CONFIG_RADAR_OUT_ENABLE
static int s_out_level;
#endif

static int64_t s_cal_end_us;
static radar_calibration_t s_cal;
static uint16_t s_cal_peak[RADAR_MAX_GATES];

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t s_req_queue;
static SemaphoreHandle_t s_req_done;
static SemaphoreHandle_t s_api_lock; /* serializes console requests */
static esp_err_t s_req_result;
static TaskHandle_t s_task;

/* --- small helpers --- */

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static uint32_t initial_baud(void)
{
    return (CONFIG_RADAR_BAUD != 0) ? (uint32_t)CONFIG_RADAR_BAUD : 115200u;
}

static esp_err_t cmd_err_to_esp(int rc)
{
    switch (rc) {
    case RADAR_CMD_OK:
        return ESP_OK;
    case RADAR_CMD_ERR_STATUS:
        return ESP_ERR_INVALID_RESPONSE;
    case RADAR_CMD_ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case RADAR_CMD_ERR_ARG:
        return ESP_ERR_INVALID_ARG;
    default:
        return ESP_FAIL;
    }
}

static void post_event(radar_event_t id, radar_event_data_t *data)
{
    esp_err_t err = esp_event_post(PRESENCE_EVENT, id, data, sizeof(*data), 0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "event %d post failed (%s)", (int)id, esp_err_to_name(err));
}

static void post_presence(radar_event_t id, bool presence, bool degraded,
                          uint16_t distance_mm)
{
    radar_event_data_t ed = {
        .presence = presence,
        .degraded = degraded,
        .distance_mm = distance_mm,
    };
    post_event(id, &ed);
}

/* --- NVS persistence --- */

static esp_err_t nvs_blob_read_len(const char *key, void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RADAR_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;
    size_t real = len;
    err = nvs_get_blob(h, key, buf, &real);
    nvs_close(h);
    return (err == ESP_OK && real == len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t nvs_blob_write(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RADAR_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Legacy LD2410 keys of the old protocol: stale values, erase at boot. */
static void nvs_erase_legacy(void)
{
    nvs_handle_t h;
    if (nvs_open(RADAR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    (void)nvs_erase_key(h, "sens");
    (void)nvs_erase_key(h, "max_mov");
    (void)nvs_erase_key(h, "max_st");
    (void)nvs_commit(h);
    nvs_close(h);
}

/* Desired gate limits: NVS values when present, else the Kconfig defaults;
 * sanitized so a session write is always valid. */
static void desired_load(void)
{
    s_des.min_gate = (uint8_t)CONFIG_RADAR_MIN_GATE;
    s_des.max_gate = (uint8_t)CONFIG_RADAR_MAX_GATE;
    s_des.timeout_s = (uint16_t)CONFIG_RADAR_UNMANNED_S;

    nvs_handle_t h;
    if (nvs_open(RADAR_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t u8;
        uint16_t u16;
        if (nvs_get_u8(h, RADAR_NVS_MIN_GATE, &u8) == ESP_OK && u8 <= RADAR_MAX_GATES)
            s_des.min_gate = u8;
        if (nvs_get_u8(h, RADAR_NVS_MAX_GATE, &u8) == ESP_OK && u8 <= RADAR_MAX_GATES)
            s_des.max_gate = u8;
        if (nvs_get_u16(h, RADAR_NVS_UNMANNED, &u16) == ESP_OK && u16 <= 255u)
            s_des.timeout_s = u16;
        nvs_close(h);
    }
    if (s_des.min_gate > RADAR_MAX_GATES)
        s_des.min_gate = RADAR_MAX_GATES;
    if (s_des.max_gate > RADAR_MAX_GATES)
        s_des.max_gate = RADAR_MAX_GATES;
    if (s_des.min_gate > s_des.max_gate)
        s_des.min_gate = s_des.max_gate;
    if (s_des.timeout_s > 255u)
        s_des.timeout_s = 255u;
}

/* Desired threshold banks: true when both NVS blobs exist (64 B each). */
static bool thresholds_load(uint32_t move[RADAR_MAX_GATES],
                            uint32_t still[RADAR_MAX_GATES])
{
    return nvs_blob_read_len(RADAR_NVS_MOVE_THR, move, RADAR_THRESH_BLOB_LEN) == ESP_OK &&
           nvs_blob_read_len(RADAR_NVS_STILL_THR, still, RADAR_THRESH_BLOB_LEN) == ESP_OK;
}

static void thresholds_store_all(const uint32_t move[RADAR_MAX_GATES],
                                 const uint32_t still[RADAR_MAX_GATES])
{
    if (nvs_blob_write(RADAR_NVS_MOVE_THR, move, RADAR_THRESH_BLOB_LEN) != ESP_OK ||
        nvs_blob_write(RADAR_NVS_STILL_THR, still, RADAR_THRESH_BLOB_LEN) != ESP_OK)
        ESP_LOGW(TAG, "threshold blob persist failed");
}

/* Read-modify-write of the threshold blobs after a single-gate change:
 * missing blobs are seeded from the module mirror, else from the factory
 * table (they become the desired config the boot sync enforces). */
static void thresholds_store_gate(uint16_t gate, uint32_t move, uint32_t still)
{
    uint32_t mv[RADAR_MAX_GATES];
    uint32_t st[RADAR_MAX_GATES];
    if (!thresholds_load(mv, st)) {
        if (s_mod_valid) {
            memcpy(mv, s_mod_move, sizeof(mv));
            memcpy(st, s_mod_still, sizeof(st));
        } else {
            memcpy(mv, k_factory_move, sizeof(mv));
            memcpy(st, k_factory_still, sizeof(st));
        }
    }
    mv[gate] = move;
    st[gate] = still;
    thresholds_store_all(mv, st);
}

/* --- UART --- */

static esp_err_t uart_setup(uint32_t baud)
{
    esp_err_t err = uart_driver_install(s_uart, CONFIG_RADAR_RX_BUFFER, 0, 0, NULL, 0);
    if (err == ESP_ERR_INVALID_STATE) /* already installed (reinit path) */
        err = ESP_OK;
    if (err != ESP_OK)
        return err;

    const uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(s_uart, &cfg);
    if (err != ESP_OK)
        return err;
    return uart_set_pin(s_uart, CONFIG_RADAR_TX_GPIO, CONFIG_RADAR_RX_GPIO,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static int io_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    int n = uart_write_bytes(s_uart, buf, len);
    return (n == (int)len) ? 0 : -1;
}

static int io_read(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    /* Deliberately NO watchdog refresh here: raw RX bytes (noise or echo on
     * a floating line, e.g. module unpowered) must never mask a mute
     * sensor. Liveness is taken from VALIDATED frames only, see
     * run_session()/handle_frame(). */
    int n = uart_read_bytes(s_uart, buf, len, pdMS_TO_TICKS(1));
    return (n < 0) ? -1 : n;
}

static const radar_command_io_t s_cmd_io = {
    .write = io_write,
    .read = io_read,
    .ctx = NULL,
    .timeout_ms = RADAR_CMD_TIMEOUT_MS,
};

/* The fault watchdog counts only VALIDATED module traffic. A config session
 * snapshots the parser frame counters and refreshes the watchdog only when
 * the module produced valid frames during it (ACKs and energy frames in
 * frames_ok, running-mode text lines in lines_parsed). */
static void mark_session_liveness(uint32_t ok_before, uint32_t lines_before)
{
    if (s_parser.frames_ok == ok_before && s_parser.lines_parsed == lines_before)
        return;
    portENTER_CRITICAL(&s_mux);
    s_state.last_frame_us = now_us();
    portEXIT_CRITICAL(&s_mux);
}

/* radar_command_session with watchdog liveness taken into account. */
static int run_session(radar_command_step_t step, void *ctx)
{
    const uint32_t ok_before = s_parser.frames_ok;
    const uint32_t lines_before = s_parser.lines_parsed;
    int rc = radar_command_session(&s_cmd_io, &s_parser, step, ctx);
    mark_session_liveness(ok_before, lines_before);
    return rc;
}

/* --- generic session steps (run only in the radar task) --- */

/* Write (name, u32) pairs. The LD2420 config buffer is 32 bytes of
 * intra-frame data (ENTER_CONFIG ACK, observed on the bench): a 16-pair
 * command (98 B of value) is silently dropped and times out. Commands are
 * therefore chunked to RADAR_WRITE_MAX_PAIRS pairs per transaction. */
#define RADAR_WRITE_MAX_PAIRS 5u
static int write_params(const radar_command_io_t *io, radar_parser_t *p,
                        const uint16_t *names, const uint32_t *vals, size_t count)
{
    uint8_t val[RADAR_CMD_VALUE_MAX];
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > RADAR_WRITE_MAX_PAIRS)
            chunk = RADAR_WRITE_MAX_PAIRS;
        int n = radar_params_build_write(names + done, vals + done, chunk,
                                         val, sizeof(val));
        if (n < 0)
            return RADAR_CMD_ERR_ARG;
        int rc = radar_command_transact(io, p, RADAR_CMD_WRITE_PARAMS,
                                        val, (size_t)n, NULL);
        if (rc != RADAR_CMD_OK)
            return rc;
        done += chunk;
    }
    return RADAR_CMD_OK;
}

/* READ_PARAMS of (min gate, max gate, timeout seconds): ACK = 3x u32. */
static int read_gate_limits(const radar_command_io_t *io, radar_parser_t *p,
                            uint8_t *min_out, uint8_t *max_out, uint16_t *to_out)
{
    static const uint16_t names[3] = {
        RADAR_PARAM_MIN_GATE, RADAR_PARAM_MAX_GATE, RADAR_PARAM_TIMEOUT_S,
    };
    uint8_t val[RADAR_CMD_VALUE_MAX];
    int n = radar_params_build_read(names, 3, val, sizeof(val));
    if (n < 0)
        return RADAR_CMD_ERR_ARG;
    radar_frame_t ack;
    int rc = radar_command_transact(io, p, RADAR_CMD_READ_PARAMS, val, (size_t)n, &ack);
    if (rc != RADAR_CMD_OK)
        return rc;
    uint32_t v[3];
    if (radar_params_parse_values(ack.ack.value, ack.ack.value_len, v, 3) != 0)
        return RADAR_CMD_ERR_STATUS;
    *min_out = (uint8_t)v[0];
    *max_out = (uint8_t)v[1];
    *to_out = (uint16_t)v[2];
    return RADAR_CMD_OK;
}

/* READ_PARAMS of one 16-name threshold bank: ACK = 16x u32 = 64 B, which
 * fits RADAR_ACK_VALUE_MAX. */
static int read_threshold_bank(const radar_command_io_t *io, radar_parser_t *p,
                               bool move_bank, uint32_t out[RADAR_MAX_GATES])
{
    uint16_t names[RADAR_MAX_GATES];
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
        if (move_bank)
            names[g] = RADAR_PARAM_MOVE_THRESH((uint16_t)g);
        else
            names[g] = RADAR_PARAM_STILL_THRESH((uint16_t)g);
    }
    uint8_t val[RADAR_CMD_VALUE_MAX];
    int n = radar_params_build_read(names, RADAR_MAX_GATES, val, sizeof(val));
    if (n < 0)
        return RADAR_CMD_ERR_ARG;
    radar_frame_t ack;
    int rc = radar_command_transact(io, p, RADAR_CMD_READ_PARAMS, val, (size_t)n, &ack);
    if (rc != RADAR_CMD_OK)
        return rc;
    if (radar_params_parse_values(ack.ack.value, ack.ack.value_len, out,
                                  RADAR_MAX_GATES) != 0)
        return RADAR_CMD_ERR_STATUS;
    return RADAR_CMD_OK;
}

/* WRITE_SYS_PARAM with the serial output mode. */
static int step_apply_mode(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    const uint8_t mode = *(const uint8_t *)ctx;
    uint8_t val[8];
    int n = radar_params_build_sysmode(mode, val, sizeof(val));
    if (n < 0)
        return RADAR_CMD_ERR_ARG;
    return radar_command_transact(io, p, RADAR_CMD_WRITE_SYS_PARAM, val, (size_t)n, NULL);
}

static int step_fw_version(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    (void)ctx;
    radar_frame_t ack;
    int rc = radar_command_transact(io, p, RADAR_CMD_READ_VERSION, NULL, 0, &ack);
    if (rc != RADAR_CMD_OK)
        return rc;
    radar_fw_version_t fw;
    if (radar_fw_version_parse(ack.ack.value, ack.ack.value_len, &fw) != 0)
        return RADAR_CMD_ERR_STATUS;
    s_fw = fw;
    s_fw_valid = true;
    portENTER_CRITICAL(&s_mux);
    strcpy(s_state.fw_text, fw.text);
    portEXIT_CRITICAL(&s_mux);
    return RADAR_CMD_OK;
}

/* Full module read: gate limits + both threshold banks (also refreshes the
 * module mirror used to seed missing NVS blobs). */
static int step_read_module_params(const radar_command_io_t *io, radar_parser_t *p,
                                   void *ctx)
{
    radar_module_params_t tmp;
    int rc = read_gate_limits(io, p, &tmp.min_gate, &tmp.max_gate, &tmp.timeout_s);
    if (rc != RADAR_CMD_OK)
        return rc;
    rc = read_threshold_bank(io, p, true, tmp.move_thresh);
    if (rc != RADAR_CMD_OK)
        return rc;
    rc = read_threshold_bank(io, p, false, tmp.still_thresh);
    if (rc != RADAR_CMD_OK)
        return rc;
    memcpy(s_mod_move, tmp.move_thresh, sizeof(s_mod_move));
    memcpy(s_mod_still, tmp.still_thresh, sizeof(s_mod_still));
    s_mod_valid = true;
    if (ctx != NULL)
        *(radar_module_params_t *)ctx = tmp;
    return RADAR_CMD_OK;
}

/* Write the pairs of `want` that differ from `cur` (one WRITE_PARAMS, up
 * to 16 pairs). *wrote reports whether a write was issued. */
static int write_diff_bank(const radar_command_io_t *io, radar_parser_t *p,
                           bool move_bank, const uint32_t cur[RADAR_MAX_GATES],
                           const uint32_t want[RADAR_MAX_GATES], bool *wrote)
{
    uint16_t names[RADAR_MAX_GATES];
    uint32_t vals[RADAR_MAX_GATES];
    size_t n = 0;
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
        if (cur[g] == want[g])
            continue;
        if (move_bank)
            names[n] = RADAR_PARAM_MOVE_THRESH((uint16_t)g);
        else
            names[n] = RADAR_PARAM_STILL_THRESH((uint16_t)g);
        vals[n++] = want[g];
    }
    *wrote = false;
    if (n == 0)
        return RADAR_CMD_OK;
    int rc = write_params(io, p, names, vals, n);
    if (rc == RADAR_CMD_OK)
        *wrote = true;
    return rc;
}

/* Boot sync: read, compare with the desired configuration (gate limits
 * always; thresholds only when the NVS blobs exist), write ONLY what
 * differs, verify. Nothing is written when the module already matches. */
static int step_boot_sync(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    (void)ctx;
    uint8_t cur_min, cur_max;
    uint16_t cur_to;
    int rc = read_gate_limits(io, p, &cur_min, &cur_max, &cur_to);
    if (rc != RADAR_CMD_OK)
        return rc;

    uint16_t wn[3];
    uint32_t wv[3];
    size_t wc = 0;
    if (cur_min != s_des.min_gate) {
        wn[wc] = RADAR_PARAM_MIN_GATE;
        wv[wc++] = s_des.min_gate;
    }
    if (cur_max != s_des.max_gate) {
        wn[wc] = RADAR_PARAM_MAX_GATE;
        wv[wc++] = s_des.max_gate;
    }
    if (cur_to != s_des.timeout_s) {
        wn[wc] = RADAR_PARAM_TIMEOUT_S;
        wv[wc++] = s_des.timeout_s;
    }
    bool wrote_limits = false;
    if (wc > 0) {
        rc = write_params(io, p, wn, wv, wc);
        if (rc != RADAR_CMD_OK)
            return rc;
        wrote_limits = true;
        ESP_LOGI(TAG, "gate limits written: min=%u max=%u timeout=%us (module: %u/%u/%us)",
                 s_des.min_gate, s_des.max_gate, s_des.timeout_s,
                 cur_min, cur_max, cur_to);
    }

    uint32_t cur_move[RADAR_MAX_GATES];
    uint32_t cur_still[RADAR_MAX_GATES];
    uint32_t want_move[RADAR_MAX_GATES];
    uint32_t want_still[RADAR_MAX_GATES];
    bool wrote_move = false;
    bool wrote_still = false;
    if (thresholds_load(want_move, want_still)) {
        rc = read_threshold_bank(io, p, true, cur_move);
        if (rc != RADAR_CMD_OK)
            return rc;
        rc = read_threshold_bank(io, p, false, cur_still);
        if (rc != RADAR_CMD_OK)
            return rc;
        rc = write_diff_bank(io, p, true, cur_move, want_move, &wrote_move);
        if (rc != RADAR_CMD_OK)
            return rc;
        rc = write_diff_bank(io, p, false, cur_still, want_still, &wrote_still);
        if (rc != RADAR_CMD_OK)
            return rc;
        if (wrote_move || wrote_still)
            ESP_LOGI(TAG, "thresholds written (move bank %u, still bank %u)",
                     (unsigned)wrote_move, (unsigned)wrote_still);
        memcpy(s_mod_move, cur_move, sizeof(cur_move));
        memcpy(s_mod_still, cur_still, sizeof(cur_still));
        s_mod_valid = true;
    }

    if (wrote_limits || wrote_move || wrote_still) {
        bool ok = true;
        if (wrote_limits) {
            uint8_t vmin, vmax;
            uint16_t vto;
            ok = read_gate_limits(io, p, &vmin, &vmax, &vto) == RADAR_CMD_OK &&
                 vmin == s_des.min_gate && vmax == s_des.max_gate &&
                 vto == s_des.timeout_s;
        }
        if (ok && (wrote_move || wrote_still)) {
            uint32_t vmove[RADAR_MAX_GATES];
            uint32_t vstill[RADAR_MAX_GATES];
            if (read_threshold_bank(io, p, true, vmove) == RADAR_CMD_OK &&
                read_threshold_bank(io, p, false, vstill) == RADAR_CMD_OK) {
                if (wrote_move)
                    ok = memcmp(vmove, want_move, sizeof(vmove)) == 0;
                if (ok && wrote_still)
                    ok = memcmp(vstill, want_still, sizeof(vstill)) == 0;
                memcpy(s_mod_move, vmove, sizeof(vmove));
                memcpy(s_mod_still, vstill, sizeof(vstill));
                s_mod_valid = true;
            } else {
                ESP_LOGW(TAG, "sync verify read failed");
            }
        }
        ESP_LOGI(TAG, "sync verify: %s", ok ? "ok" : "MISMATCH");
    } else {
        ESP_LOGI(TAG, "module parameters already in sync, no writes");
    }
    return RADAR_CMD_OK;
}

/* radar_set_gate_limits: one WRITE_PARAMS with the 3 pairs, read back. */
static int step_set_limits(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    const radar_req_t *req = ctx;
    uint16_t names[3] = {
        RADAR_PARAM_MIN_GATE, RADAR_PARAM_MAX_GATE, RADAR_PARAM_TIMEOUT_S,
    };
    uint32_t vals[3] = { req->min_gate, req->max_gate, req->hold_s };
    int rc = write_params(io, p, names, vals, 3);
    if (rc != RADAR_CMD_OK)
        return rc;
    uint8_t vmin, vmax;
    uint16_t vto;
    if (read_gate_limits(io, p, &vmin, &vmax, &vto) == RADAR_CMD_OK) {
        bool ok = vmin == req->min_gate && vmax == req->max_gate &&
                  vto == req->hold_s;
        ESP_LOGI(TAG, "gate limits verify: %s", ok ? "ok" : "MISMATCH");
    }
    return RADAR_CMD_OK;
}

/* radar_set_gate_thresholds, single gate: one WRITE_PARAMS with the 2
 * pairs (move + still of that gate). */
static int step_set_thresh_one(const radar_command_io_t *io, radar_parser_t *p,
                               void *ctx)
{
    const radar_req_t *req = ctx;
    uint16_t names[2] = {
        RADAR_PARAM_MOVE_THRESH(req->gate), RADAR_PARAM_STILL_THRESH(req->gate),
    };
    uint32_t vals[2] = { req->move, req->still };
    return write_params(io, p, names, vals, 2);
}

/* radar_set_gate_thresholds, all gates: the 32-pair table (192 B of
 * payload) as TWO 16-pair WRITE_PARAMS, move bank then still bank. */
static int step_set_thresh_all(const radar_command_io_t *io, radar_parser_t *p,
                               void *ctx)
{
    const radar_req_t *req = ctx;
    uint16_t names[RADAR_MAX_GATES];
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        names[g] = RADAR_PARAM_MOVE_THRESH((uint16_t)g);
    int rc = write_params(io, p, names, req->move_vals, RADAR_MAX_GATES);
    if (rc != RADAR_CMD_OK)
        return rc;
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        names[g] = RADAR_PARAM_STILL_THRESH((uint16_t)g);
    return write_params(io, p, names, req->still_vals, RADAR_MAX_GATES);
}

static int step_restart(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    (void)ctx;
    return radar_command_transact(io, p, RADAR_CMD_RESTART, NULL, 0, NULL);
}

/* The LD2420 has no factory-reset command: rewrite the ESPHome factory
 * table (threshold banks + gate limits). */
static int step_factory_reset(const radar_command_io_t *io, radar_parser_t *p,
                              void *ctx)
{
    (void)ctx;
    uint16_t names[RADAR_MAX_GATES];
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        names[g] = RADAR_PARAM_MOVE_THRESH((uint16_t)g);
    int rc = write_params(io, p, names, k_factory_move, RADAR_MAX_GATES);
    if (rc != RADAR_CMD_OK)
        return rc;
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        names[g] = RADAR_PARAM_STILL_THRESH((uint16_t)g);
    rc = write_params(io, p, names, k_factory_still, RADAR_MAX_GATES);
    if (rc != RADAR_CMD_OK)
        return rc;
    uint16_t lnames[3] = {
        RADAR_PARAM_MIN_GATE, RADAR_PARAM_MAX_GATE, RADAR_PARAM_TIMEOUT_S,
    };
    uint32_t lvals[3] = {
        RADAR_FACTORY_MIN_GATE, RADAR_FACTORY_MAX_GATE, RADAR_FACTORY_TIMEOUT_S,
    };
    return write_params(io, p, lnames, lvals, 3);
}

/* Calibrate apply: the two 16-pair threshold writes from the proposal. */
static int step_cal_apply(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    (void)ctx;
    uint16_t names[RADAR_MAX_GATES];
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        names[g] = RADAR_PARAM_MOVE_THRESH((uint16_t)g);
    int rc = write_params(io, p, names, s_cal.move, RADAR_MAX_GATES);
    if (rc != RADAR_CMD_OK)
        return rc;
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++)
        names[g] = RADAR_PARAM_STILL_THRESH((uint16_t)g);
    return write_params(io, p, names, s_cal.still, RADAR_MAX_GATES);
}

/* --- baud probe and boot sequence --- */

/* One READ_VERSION per candidate baud; the first that answers wins. */
static bool probe_module(void)
{
    uint32_t candidates[2];
    size_t ncand = 0;
    if (CONFIG_RADAR_BAUD != 0) {
        candidates[ncand++] = (uint32_t)CONFIG_RADAR_BAUD;
    } else {
        candidates[ncand++] = 115200;
        candidates[ncand++] = 256000;
    }

    for (size_t i = 0; i < ncand; i++) {
        if (uart_set_baudrate(s_uart, candidates[i]) != ESP_OK)
            continue;
        radar_parser_t probe; /* fresh parser: probe noise is discarded */
        radar_parser_init(&probe);
        radar_command_io_t io = s_cmd_io;
        io.timeout_ms = RADAR_PROBE_TIMEOUT_MS;
        radar_frame_t ack;
        int rc = radar_command_transact(&io, &probe, RADAR_CMD_READ_VERSION,
                                        NULL, 0, &ack);
        if (rc != RADAR_CMD_OK)
            continue;

        radar_fw_version_t fw;
        if (radar_fw_version_parse(ack.ack.value, ack.ack.value_len, &fw) == 0) {
            s_fw = fw;
            s_fw_valid = true;
        } else {
            memset(&s_fw, 0, sizeof(s_fw));
            s_fw_valid = false;
        }
        s_baud = candidates[i];
        s_baud_known = true;
        portENTER_CRITICAL(&s_mux);
        strcpy(s_state.fw_text, s_fw_valid ? s_fw.text : "");
        s_state.baud = s_baud;
        s_state.last_frame_us = now_us(); /* the module answered: it is alive */
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "module answered at %lu baud: fw %s",
                 (unsigned long)s_baud, s_fw_valid ? s_fw.text : "(non decodificabile)");
        return true;
    }
    ESP_LOGW(TAG, "radar: modulo assente o muto (nessuna risposta al probe baud)");
    return false;
}

/* Probe (once) -> serial mode (ENERGY on fw >= 1.5.4, RUNNING otherwise)
 * -> configuration sync. Every failure is non-fatal: the caller retries. */
static bool boot_attempt(void)
{
    if (!s_baud_known && !probe_module())
        return false;

    uint8_t mode = (s_fw_valid && s_fw.energy_capable) ? RADAR_SYSMODE_ENERGY
                                                       : RADAR_SYSMODE_RUNNING;
    int rc = run_session(step_apply_mode, &mode);
    if (rc != RADAR_CMD_OK) {
        ESP_LOGD(TAG, "mode selection failed (rc=%d)", rc);
        return false;
    }
    s_state.mode = (mode == RADAR_SYSMODE_ENERGY) ? RADAR_MODULE_MODE_ENERGY
                                                  : RADAR_MODULE_MODE_RUNNING;
    ESP_LOGI(TAG, "serial mode: %s",
             (s_state.mode == RADAR_MODULE_MODE_ENERGY) ? "ENERGY" : "RUNNING");

    rc = run_session(step_boot_sync, NULL);
    if (rc != RADAR_CMD_OK) {
        ESP_LOGD(TAG, "boot sync failed (rc=%d)", rc);
        return false;
    }
    s_synced = true;
    ESP_LOGI(TAG, "module configuration in sync");
    return true;
}

/* --- recovery stages (cyclic) --- */

static void stage_uart_reinit(void)
{
    ESP_LOGW(TAG, "recovery 1/3: UART reinit");
    (void)uart_driver_delete(s_uart);
    uint32_t baud = s_baud_known ? s_baud : initial_baud();
    if (uart_setup(baud) != ESP_OK)
        ESP_LOGE(TAG, "UART reinit failed");
}

static void stage_module_reset(void)
{
    ESP_LOGW(TAG, "recovery 2/3: module restart");
    int rc = run_session(step_restart, NULL);
    if (rc != RADAR_CMD_OK)
        ESP_LOGD(TAG, "module restart failed (rc=%d)", rc);
    vTaskDelay(pdMS_TO_TICKS(RADAR_REBOOT_DELAY_MS));
}

static void stage_en_toggle(void)
{
    if (CONFIG_RADAR_EN_GPIO < 0) {
        ESP_LOGD(TAG, "recovery 3/3: RADAR_EN not wired, skipping");
        return;
    }
    ESP_LOGW(TAG, "recovery 3/3: RADAR_EN toggle (GPIO %d)", CONFIG_RADAR_EN_GPIO);
    (void)gpio_set_level(CONFIG_RADAR_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(RADAR_EN_PULSE_MS));
    (void)gpio_set_level(CONFIG_RADAR_EN_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(RADAR_EN_PULSE_MS));
}

static void recovery_attempt(void)
{
    switch (s_stage) {
    case 0:
        stage_uart_reinit();
        break;
    case 1:
        stage_module_reset();
        break;
    default:
        stage_en_toggle();
        break;
    }
    s_stage = (uint8_t)((s_stage + 1u) % 3u);
}

/* --- fault watch and degraded mode --- */

/* While faulted the trigger hold must still expire on wall time: feed it
 * synthetic no-target frames stamped with the current time. */
static void degraded_hold_feed(int64_t t)
{
    if (!s_trigger.st.presence)
        return;
    radar_data_frame_t none;
    memset(&none, 0, sizeof(none));
    none.timestamp_us = t;
    radar_trigger_events_t ev;
    radar_trigger_feed(&s_trigger, &none, t, &ev);
    if (ev.presence_off) {
        /* the OUT pin may still report presence: it wins while faulted */
        s_state.presence = s_degraded_presence;
        post_presence(RADAR_EVENT_PRESENCE_OFF, s_degraded_presence, true,
                      s_state.distance_mm);
    }
}

#if CONFIG_RADAR_OUT_ENABLE
static void degraded_out_poll(void)
{
    int lvl = gpio_get_level(CONFIG_RADAR_OUT_GPIO);
    if (lvl == s_out_level)
        return;
    s_out_level = lvl;
    s_degraded_presence = (lvl != 0);
    ESP_LOGW(TAG, "OUT pin %s -> degraded presence %s",
             (lvl != 0) ? "high" : "low", (lvl != 0) ? "ON" : "OFF");
    s_state.presence = s_degraded_presence;
    post_presence((lvl != 0) ? RADAR_EVENT_PRESENCE_ON : RADAR_EVENT_PRESENCE_OFF,
                  s_degraded_presence, true, s_state.distance_mm);
}
#endif

static void fault_watch(int64_t t)
{
    if (!s_fault) {
        int64_t silence_us = t - s_state.last_frame_us;
        if (silence_us > (int64_t)CONFIG_RADAR_FAULT_TIMEOUT_MS * 1000) {
            s_fault = true;
            s_state.fault = true;
            s_state.degraded = true; /* presence reporting moves to the OUT pin */
            ESP_LOGW(TAG, "no valid frame for %d ms -> FAULT",
                     CONFIG_RADAR_FAULT_TIMEOUT_MS);
            post_presence(RADAR_EVENT_RADAR_FAULT, s_state.presence, true,
                          s_state.distance_mm);
        }
        return;
    }

    /* fault active: degrade to the OUT pin and keep the hold expiring */
#if CONFIG_RADAR_OUT_ENABLE
    degraded_out_poll();
#endif
    degraded_hold_feed(t);

    if (t >= s_next_action_us) {
        ESP_LOGW(TAG, "recovery cycle (stage %u, next in %lu ms)",
                 s_stage, (unsigned long)s_backoff_ms);
        recovery_attempt();
        boot_attempt();
        s_next_action_us = now_us() + 1000 * (int64_t)s_backoff_ms /* backoff is ms, clock is us */;
        s_backoff_ms *= 2u;
        if (s_backoff_ms > RADAR_BACKOFF_CAP_MS)
            s_backoff_ms = RADAR_BACKOFF_CAP_MS;
    }
}

/* --- calibration --- */

static void cal_sample(const radar_data_frame_t *d)
{
    s_cal.samples++;
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
        if (d->gate_energy[g] > s_cal_peak[g])
            s_cal_peak[g] = d->gate_energy[g];
    }
}

static void cal_finish(void)
{
    s_state.calibrating = false;
    if (s_fault) {
        ESP_LOGW(TAG, "calibration aborted: sensor went mute (%lu samples)",
                 (unsigned long)s_cal.samples);
        s_cal.valid = false;
        return;
    }
    if (s_cal.samples == 0) {
        ESP_LOGW(TAG, "calibration aborted: no energy frames received");
        s_cal.valid = false;
        return;
    }
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
        uint32_t move = ((uint32_t)s_cal_peak[g] * (uint32_t)CONFIG_RADAR_CAL_MOVE_PCT) / 100u;
        uint32_t still = ((uint32_t)s_cal_peak[g] * (uint32_t)CONFIG_RADAR_CAL_STILL_PCT) / 100u;
        s_cal.peak[g] = s_cal_peak[g];
        s_cal.move[g] = (move > 65535u) ? 65535u : move;
        s_cal.still[g] = (still > 65535u) ? 65535u : still;
    }
    s_cal.valid = true;
    ESP_LOGI(TAG, "calibration done (%lu samples):", (unsigned long)s_cal.samples);
    ESP_LOGI(TAG,
             "  peak: %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
             s_cal.peak[0], s_cal.peak[1], s_cal.peak[2], s_cal.peak[3],
             s_cal.peak[4], s_cal.peak[5], s_cal.peak[6], s_cal.peak[7],
             s_cal.peak[8], s_cal.peak[9], s_cal.peak[10], s_cal.peak[11],
             s_cal.peak[12], s_cal.peak[13], s_cal.peak[14], s_cal.peak[15]);
    ESP_LOGI(TAG,
             "  move: %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
             (unsigned long)s_cal.move[0], (unsigned long)s_cal.move[1],
             (unsigned long)s_cal.move[2], (unsigned long)s_cal.move[3],
             (unsigned long)s_cal.move[4], (unsigned long)s_cal.move[5],
             (unsigned long)s_cal.move[6], (unsigned long)s_cal.move[7],
             (unsigned long)s_cal.move[8], (unsigned long)s_cal.move[9],
             (unsigned long)s_cal.move[10], (unsigned long)s_cal.move[11],
             (unsigned long)s_cal.move[12], (unsigned long)s_cal.move[13],
             (unsigned long)s_cal.move[14], (unsigned long)s_cal.move[15]);
    ESP_LOGI(TAG,
             "  still: %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
             (unsigned long)s_cal.still[0], (unsigned long)s_cal.still[1],
             (unsigned long)s_cal.still[2], (unsigned long)s_cal.still[3],
             (unsigned long)s_cal.still[4], (unsigned long)s_cal.still[5],
             (unsigned long)s_cal.still[6], (unsigned long)s_cal.still[7],
             (unsigned long)s_cal.still[8], (unsigned long)s_cal.still[9],
             (unsigned long)s_cal.still[10], (unsigned long)s_cal.still[11],
             (unsigned long)s_cal.still[12], (unsigned long)s_cal.still[13],
             (unsigned long)s_cal.still[14], (unsigned long)s_cal.still[15]);
}

/* --- frame handling --- */

static void handle_frame(const radar_data_frame_t *d, int64_t t)
{
    portENTER_CRITICAL(&s_mux);
    s_state.last_frame_us = t;
    s_state.raw_presence = d->presence;
    s_state.distance_mm = d->distance_mm;
    s_state.gates_valid = d->gates_valid;
    if (d->gates_valid)
        memcpy(s_state.gate_energy, d->gate_energy, sizeof(s_state.gate_energy));
    s_state.frames_ok = s_parser.frames_ok;
    s_state.frames_bad = s_parser.frames_bad;
    s_state.bytes_dropped = s_parser.bytes_dropped;
    portEXIT_CRITICAL(&s_mux);

    if (s_fault) {
        s_fault = false;
        s_state.fault = false;
        s_state.degraded = false;
        s_state.recoveries++;
        s_backoff_ms = RADAR_BACKOFF_INIT_MS;
        s_next_action_us = 0;
        ESP_LOGI(TAG, "frames flowing again (recoveries=%lu)",
                 (unsigned long)s_state.recoveries);
        post_presence(RADAR_EVENT_RADAR_OK, s_trigger.st.presence, false,
                      s_state.distance_mm);
        if (s_degraded_presence) {
            /* hand presence back to the trigger: it re-fires on its own */
            s_degraded_presence = false;
            post_presence(RADAR_EVENT_PRESENCE_OFF, false, false,
                          s_state.distance_mm);
        }
    }

    if (s_state.calibrating && d->gates_valid)
        cal_sample(d);

    radar_trigger_events_t ev;
    radar_trigger_feed(&s_trigger, d, t, &ev);

    s_state.presence = s_trigger.st.presence;

    radar_event_data_t ed = {
        .presence = s_trigger.st.presence,
        .degraded = false,
        .distance_mm = s_state.distance_mm,
    };
    if (ev.presence_on)
        post_event(RADAR_EVENT_PRESENCE_ON, &ed);
    if (ev.presence_off)
        post_event(RADAR_EVENT_PRESENCE_OFF, &ed);
    if (ev.distance_updated)
        post_event(RADAR_EVENT_DISTANCE_UPDATED, &ed);
}

/* --- console requests (single consumer: the radar task) --- */

static esp_err_t run_request(const radar_req_t *req)
{
    switch (req->kind) {
    case REQ_FW_VERSION: {
        int rc = run_session(step_fw_version, NULL);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        if (req->fw_out != NULL)
            *req->fw_out = s_fw;
        return ESP_OK;
    }
    case REQ_READ_PARAMS: {
        radar_module_params_t tmp;
        int rc = run_session(step_read_module_params, &tmp);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        if (req->params_out != NULL)
            *req->params_out = tmp;
        return ESP_OK;
    }
    case REQ_SET_LIMITS: {
        int rc = run_session(step_set_limits, (void *)req);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        s_des.min_gate = (uint8_t)req->min_gate;
        s_des.max_gate = (uint8_t)req->max_gate;
        s_des.timeout_s = (uint16_t)req->hold_s;
        nvs_handle_t h;
        esp_err_t err = nvs_open(RADAR_NVS_NAMESPACE, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            (void)nvs_set_u8(h, RADAR_NVS_MIN_GATE, s_des.min_gate);
            (void)nvs_set_u8(h, RADAR_NVS_MAX_GATE, s_des.max_gate);
            (void)nvs_set_u16(h, RADAR_NVS_UNMANNED, s_des.timeout_s);
            (void)nvs_commit(h);
            nvs_close(h);
        } else {
            ESP_LOGW(TAG, "gate limits persist failed (%s)", esp_err_to_name(err));
        }
        return ESP_OK;
    }
    case REQ_SET_THRESHOLDS: {
        radar_command_step_t step = (req->gate == RADAR_ALL_GATES)
                                        ? step_set_thresh_all
                                        : step_set_thresh_one;
        int rc = run_session(step, (void *)req);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        if (req->gate == RADAR_ALL_GATES) {
            thresholds_store_all(req->move_vals, req->still_vals);
            memcpy(s_mod_move, req->move_vals, sizeof(s_mod_move));
            memcpy(s_mod_still, req->still_vals, sizeof(s_mod_still));
            s_mod_valid = true;
        } else {
            thresholds_store_gate(req->gate, req->move, req->still);
            if (s_mod_valid) {
                s_mod_move[req->gate] = req->move;
                s_mod_still[req->gate] = req->still;
            }
        }
        return ESP_OK;
    }
    case REQ_SET_ENGINEERING: {
        if (req->eng_on && (!s_fw_valid || !s_fw.energy_capable))
            return ESP_ERR_NOT_SUPPORTED;
        uint8_t mode = req->eng_on ? RADAR_SYSMODE_ENERGY : RADAR_SYSMODE_RUNNING;
        int rc = run_session(step_apply_mode, &mode);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        s_state.mode = req->eng_on ? RADAR_MODULE_MODE_ENERGY
                                   : RADAR_MODULE_MODE_RUNNING;
        return ESP_OK;
    }
    case REQ_RESTART: {
        int rc = run_session(step_restart, NULL);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        vTaskDelay(pdMS_TO_TICKS(RADAR_REBOOT_DELAY_MS));
        s_synced = false; /* the task loop re-runs the boot sequence */
        s_next_sync_us = now_us() + 1000 * (int64_t)RADAR_BOOT_RETRY_MS /* retry is ms, clock is us */;
        return ESP_OK;
    }
    case REQ_FACTORY_RESET: {
        int rc = run_session(step_factory_reset, NULL);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        vTaskDelay(pdMS_TO_TICKS(RADAR_REBOOT_DELAY_MS));
        /* the module now holds the factory table: erase ALL our keys so the
         * next boot does not rewrite the user values over it */
        nvs_handle_t h;
        if (nvs_open(RADAR_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_erase_all(h);
            (void)nvs_commit(h);
            nvs_close(h);
        }
        desired_load(); /* NVS is empty now: back to the Kconfig defaults */
        s_mod_valid = false;
        s_cal.valid = false;
        s_synced = false;
        s_next_sync_us = now_us() + 1000 * (int64_t)RADAR_BOOT_RETRY_MS /* retry is ms, clock is us */;
        return ESP_OK;
    }
    case REQ_CAL_START: {
        if (s_state.calibrating)
            return ESP_ERR_INVALID_STATE;
        if (!s_fw_valid || !s_fw.energy_capable ||
            s_state.mode != RADAR_MODULE_MODE_ENERGY)
            return ESP_ERR_NOT_SUPPORTED;
        memset(&s_cal, 0, sizeof(s_cal));
        memset(s_cal_peak, 0, sizeof(s_cal_peak));
        s_cal_end_us = now_us() + (int64_t)req->seconds * 1000000;
        s_state.calibrating = true;
        ESP_LOGI(TAG, "calibration started (%lus, keep the room empty)",
                 (unsigned long)req->seconds);
        return ESP_OK;
    }
    case REQ_CAL_APPLY: {
        if (!s_cal.valid || s_state.calibrating)
            return ESP_ERR_INVALID_STATE;
        int rc = run_session(step_cal_apply, NULL);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        thresholds_store_all(s_cal.move, s_cal.still);
        memcpy(s_mod_move, s_cal.move, sizeof(s_mod_move));
        memcpy(s_mod_still, s_cal.still, sizeof(s_mod_still));
        s_mod_valid = true;
        ESP_LOGI(TAG, "calibration applied and persisted");
        return ESP_OK;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static void service_requests(void)
{
    radar_req_t req;
    if (xQueueReceive(s_req_queue, &req, 0) != pdTRUE)
        return;
    s_req_result = run_request(&req);
    xSemaphoreGive(s_req_done);
}

static esp_err_t post_request(radar_req_t *req)
{
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_api_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t res = ESP_ERR_TIMEOUT;
    if (xQueueSend(s_req_queue, req, 0) == pdTRUE) {
#if CONFIG_RADAR_TASK_WDT
        /* Attesa a passi da 1 s con feed del watchdog: RADAR_REQ_WAIT_MS
         * (8 s) supera il timeout del task watchdog, quindi una richiesta
         * senza ACK aspetta comunque l'intero periodo, alimentando il WDT. */
        uint32_t waited = 0;
        do {
            if (xSemaphoreTake(s_req_done,
                               pdMS_TO_TICKS(RADAR_REQ_WAIT_SLICE_MS)) == pdTRUE) {
                res = s_req_result;
                break;
            }
            waited += RADAR_REQ_WAIT_SLICE_MS;
            esp_task_wdt_reset();
        } while (waited < RADAR_REQ_WAIT_MS);
#else
        if (xSemaphoreTake(s_req_done, pdMS_TO_TICKS(RADAR_REQ_WAIT_MS)) == pdTRUE)
            res = s_req_result;
#endif
    }
    xSemaphoreGive(s_api_lock);
    return res;
}

/* --- task --- */

static void radar_task(void *arg)
{
    (void)arg;
    uint8_t rxbuf[64];
#if CONFIG_RADAR_TASK_WDT
    esp_err_t werr = esp_task_wdt_add(NULL);
    if (werr != ESP_OK && werr != ESP_ERR_INVALID_ARG)
        ESP_LOGW(TAG, "iscrizione task watchdog fallita: %s",
                 esp_err_to_name(werr));
#endif

    boot_attempt();
    s_next_sync_us = now_us() + 1000 * (int64_t)RADAR_BOOT_RETRY_MS /* retry is ms, clock is us */;
    s_next_action_us = now_us() + 1000 * (int64_t)s_backoff_ms /* backoff is ms, clock is us */;

    for (;;) {
#if CONFIG_RADAR_TASK_WDT
        esp_task_wdt_reset();
#endif
        service_requests();

        int n = uart_read_bytes(s_uart, rxbuf, sizeof(rxbuf),
                                pdMS_TO_TICKS(RADAR_RX_POLL_MS));
        if (n > 0) {
            radar_frame_t f;
            int64_t t = now_us();
            for (int i = 0; i < n; i++) {
                if (radar_parser_feed(&s_parser, rxbuf[i], &f) &&
                    f.kind == RADAR_FRAME_REPORT)
                    handle_frame(&f.data, t);
            }
        }

        /* fault first, with a FRESH timestamp: the latch must not be
         * delayed by the blocking boot attempt below, and the recovery
         * backoff must be measured from real time */
        fault_watch(now_us());

        /* boot re-sync (post-restart or late module): only while the fault
         * monitor is not already driving the retries itself */
        if (!s_synced && !s_fault && now_us() >= s_next_sync_us) {
            boot_attempt();
            s_next_sync_us = now_us() + 1000 * (int64_t)RADAR_BOOT_RETRY_MS /* retry is ms, clock is us */;
        }

        if (s_state.calibrating && now_us() >= s_cal_end_us)
            cal_finish();
    }
}

/* --- public API --- */

esp_err_t radar_init(void)
{
    if (s_task != NULL)
        return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init: %s, erasing", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK)
            err = nvs_flash_init();
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "NVS init failed (%s): no persistence", esp_err_to_name(err));

    s_req_queue = xQueueCreate(1, sizeof(radar_req_t));
    s_req_done = xSemaphoreCreateBinary();
    s_api_lock = xSemaphoreCreateMutex();
    if (s_req_queue == NULL || s_req_done == NULL || s_api_lock == NULL)
        return ESP_ERR_NO_MEM;

    nvs_erase_legacy();
    desired_load();

    radar_trigger_config_t tc = {
        .trigger_mm = (uint16_t)CONFIG_RADAR_TRIGGER_MM,
        .confirm_frames = (uint8_t)CONFIG_RADAR_CONFIRM_FRAMES,
        .hold_ms = (uint32_t)CONFIG_RADAR_HOLD_MS,
        .hysteresis_mm = (uint16_t)CONFIG_RADAR_HYSTERESIS_MM,
    };
    radar_trigger_init(&s_trigger, &tc);

    nvs_handle_t h;
    if (nvs_open(RADAR_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint16_t mm;
        if (nvs_get_u16(h, RADAR_NVS_TRIGGER, &mm) == ESP_OK)
            radar_trigger_set_trigger_mm(&s_trigger, mm); /* clamps */
        nvs_close(h);
    }

    err = uart_setup(initial_baud());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART setup failed (%s)", esp_err_to_name(err));
        return err;
    }

#if CONFIG_RADAR_OUT_ENABLE
    const gpio_config_t out_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_RADAR_OUT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    s_out_level = 0;
#endif
#if CONFIG_RADAR_EN_GPIO >= 0
    const gpio_config_t en_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_RADAR_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&en_cfg);
    (void)gpio_set_level(CONFIG_RADAR_EN_GPIO, 1); /* enabled */
#endif

    memset(&s_state, 0, sizeof(s_state));
    s_state.last_frame_us = now_us();

    if (xTaskCreate(radar_task, "radar", CONFIG_RADAR_TASK_STACK, NULL,
                    RADAR_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "radar task started (uart%d 8N1, tx=%d rx=%d out=%d, baud %s)",
             CONFIG_RADAR_UART_NUM, CONFIG_RADAR_TX_GPIO, CONFIG_RADAR_RX_GPIO,
             CONFIG_RADAR_OUT_GPIO,
             (CONFIG_RADAR_BAUD != 0) ? "fisso" : "auto-probe 115200/256000");
    return ESP_OK;
}

esp_err_t radar_get_state(radar_state_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_mux);
    *out = s_state;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t radar_read_module_params(radar_module_params_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    radar_req_t req = {
        .kind = REQ_READ_PARAMS,
        .params_out = out,
    };
    return post_request(&req);
}

esp_err_t radar_set_trigger_mm(uint16_t trigger_mm)
{
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    radar_trigger_set_trigger_mm(&s_trigger, trigger_mm); /* clamps */
    uint16_t applied = s_trigger.cfg.trigger_mm;

    nvs_handle_t h;
    esp_err_t err = nvs_open(RADAR_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u16(h, RADAR_NVS_TRIGGER, applied);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK)
        return err;
    ESP_LOGI(TAG, "trigger distance set to %u mm", applied);
    return ESP_OK;
}

esp_err_t radar_set_gate_limits(uint16_t min_gate, uint16_t max_gate, uint16_t hold_s)
{
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    if (min_gate > RADAR_MAX_GATES || max_gate > RADAR_MAX_GATES ||
        min_gate > max_gate || hold_s > 255u)
        return ESP_ERR_INVALID_ARG;
    radar_req_t req = {
        .kind = REQ_SET_LIMITS,
        .min_gate = min_gate,
        .max_gate = max_gate,
        .hold_s = hold_s,
    };
    return post_request(&req);
}

esp_err_t radar_set_gate_thresholds(uint16_t gate, uint32_t move_thresh,
                                    uint32_t still_thresh)
{
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    if (move_thresh > 65535u || still_thresh > 65535u)
        return ESP_ERR_INVALID_ARG;
    if (gate != RADAR_ALL_GATES && gate >= RADAR_MAX_GATES)
        return ESP_ERR_INVALID_ARG;
    radar_req_t req = {
        .kind = REQ_SET_THRESHOLDS,
        .gate = gate,
        .move = move_thresh,
        .still = still_thresh,
    };
    if (gate == RADAR_ALL_GATES) {
        for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
            req.move_vals[g] = move_thresh;
            req.still_vals[g] = still_thresh;
        }
    }
    return post_request(&req);
}

esp_err_t radar_set_engineering(bool energy_on)
{
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    if (energy_on && (!s_fw_valid || !s_fw.energy_capable))
        return ESP_ERR_NOT_SUPPORTED;
    radar_req_t req = {
        .kind = REQ_SET_ENGINEERING,
        .eng_on = energy_on,
    };
    return post_request(&req);
}

esp_err_t radar_read_fw_version(radar_fw_version_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    radar_req_t req = {
        .kind = REQ_FW_VERSION,
        .fw_out = out,
    };
    return post_request(&req);
}

esp_err_t radar_restart_module(void)
{
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    radar_req_t req = { .kind = REQ_RESTART };
    return post_request(&req);
}

esp_err_t radar_factory_reset(void)
{
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    radar_req_t req = { .kind = REQ_FACTORY_RESET };
    return post_request(&req);
}

esp_err_t radar_calibrate(uint32_t seconds)
{
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    if (seconds == 0)
        seconds = (uint32_t)CONFIG_RADAR_CAL_SECONDS;
    if (seconds > 3600)
        return ESP_ERR_INVALID_ARG;
    radar_req_t req = {
        .kind = REQ_CAL_START,
        .seconds = seconds,
    };
    return post_request(&req);
}

esp_err_t radar_calibrate_get(radar_calibration_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;
    *out = s_cal;
    return s_cal.valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t radar_calibrate_apply(void)
{
    if (s_task == NULL)
        return ESP_ERR_INVALID_STATE;
    radar_req_t req = { .kind = REQ_CAL_APPLY };
    return post_request(&req);
}
