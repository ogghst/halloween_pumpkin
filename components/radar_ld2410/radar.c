/* Target integration of the radar_ld2410 component (ESP-IDF, Fase 2).
 *
 * Owns the UART and the parser, runs the trigger, posts presence events on
 * the default event loop, keeps the module configuration in sync at boot
 * (writes ONLY what differs), detects sensor faults, degrades to the OUT
 * pin and recovers with cyclic attempts and exponential backoff.
 *
 * Design choices (Fase 2):
 *  - everything runs in one "radar" task (priority 5, stack
 *    CONFIG_RADAR_TASK_STACK): the RX loop, all config sessions (console
 *    requests are routed through a one-slot queue so the UART and the
 *    parser never have two consumers) and the calibration sampling;
 *  - UART poll 20 ms; per-command ACK budget 250 ms; console request
 *    round-trip budget 8 s (a full session with 8 gate writes stays under
 *    4 s);
 *  - recovery: cyclic attempts (1) UART reinit, (2) MODULE_RESET session,
 *    (3) RADAR_EN toggle when wired, waiting 1 s * 2^n between attempts
 *    (cap 30 s, reset on the first valid frame); the system keeps running
 *    with no sensor: no crash, no block;
 *  - degraded mode: while the fault is active the OUT pin level drives
 *    PRESENCE_ON/OFF events flagged degraded=true; the trigger hold keeps
 *    expiring on wall time via synthetic no-target frames;
 *  - NVS namespace "radar": "trigger_mm" (u16), "sens" (18-byte blob:
 *    motion[0..8] then static[0..8], written by calibrate apply / runtime
 *    sensitivity changes), "max_mov"/"max_st" (u8) and "unmanned_s" (u16)
 *    desired limits (else the Kconfig defaults); the boot sync only rewrites
 *    module parameters that differ, and sensitivity is compared at all only
 *    when the calibration blob exists.
 *
 * Concurrency: the radar task is the only writer of the module-facing
 * state; console readers copy under a spinlock. No dynamic allocation in
 * the RX path (single stack buffer, caller-owned parser).
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
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "radar_ld2410.h"

ESP_EVENT_DEFINE_BASE(PRESENCE_EVENT);

static const char *TAG = "radar";

/* --- timing choices (see file header) --- */
#define RADAR_RX_POLL_MS      20u
#define RADAR_CMD_TIMEOUT_MS  250u
#define RADAR_REQ_WAIT_MS     8000u
#define RADAR_REBOOT_DELAY_MS 600u  /* 0xA2/0xA3: module reboots after the ACK */
#define RADAR_EN_PULSE_MS     100u
#define RADAR_BACKOFF_INIT_MS 1000u
#define RADAR_BACKOFF_CAP_MS  30000u
#define RADAR_TASK_PRIORITY   5

/* --- persistent state (namespace "radar") --- */
#define RADAR_NVS_NAMESPACE "radar"
#define RADAR_NVS_TRIGGER   "trigger_mm"
#define RADAR_NVS_SENS      "sens"
#define RADAR_NVS_MAX_MOV   "max_mov"
#define RADAR_NVS_MAX_ST    "max_st"
#define RADAR_NVS_UNMANNED  "unmanned_s"
#define RADAR_SENS_BLOB_LEN (2u * (RADAR_MAX_GATES + 1u)) /* mov[0..8]+st[0..8] */

typedef struct {
    uint8_t max_mov;
    uint8_t max_st;
    uint16_t unmanned_s;
} radar_limits_t;

typedef enum {
    REQ_READ_PARAMS,
    REQ_FW_VERSION,
    REQ_RESOLUTION,
    REQ_SET_SENSITIVITY,
    REQ_SET_LIMITS,
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
    uint8_t motion;
    uint8_t static_sens;
    uint16_t max_mov;
    uint16_t max_st;
    uint16_t unmanned_s;
    bool eng_on;
    uint32_t seconds;
    /* outputs */
    radar_module_params_t *params_out;
    radar_fw_version_t *fw_out;
    uint16_t *res_out;
} radar_req_t;

/* --- static state --- */
static uart_port_t s_uart = (uart_port_t)CONFIG_RADAR_UART_NUM;
static radar_parser_t s_parser;
static radar_trigger_t s_trigger;

static radar_limits_t s_limits;              /* desired limits (NVS else Kconfig) */
static radar_module_params_t s_params;       /* last module read */
static radar_fw_version_t s_fw;
static bool s_fw_valid;
static uint8_t s_module_max_gates = RADAR_MAX_GATES;
static bool s_synced;                        /* boot config sync done */
static uint8_t s_stage;                      /* cyclic recovery stage 0..2 */
static uint32_t s_backoff_ms = RADAR_BACKOFF_INIT_MS;
static int64_t s_next_action_us;

static volatile bool s_fault;
static bool s_degraded_presence;             /* OUT-pin latch while faulted */
#if CONFIG_RADAR_OUT_ENABLE
static int s_out_level;
#endif

static bool s_engineering;                   /* wanted engineering state */
static bool s_calibrating;
static int64_t s_cal_end_us;
static radar_calibration_t s_cal;
static uint8_t s_cal_max_mov[RADAR_MAX_GATES + 1];
static uint8_t s_cal_max_st[RADAR_MAX_GATES + 1];

static uint16_t s_res_index;
static uint16_t s_gate_mm = 750;             /* factory default until queried */
static uint16_t s_last_distance_mm;
static uint8_t s_last_moving_energy;
static radar_target_state_t s_last_target_state;
static uint16_t s_last_moving_distance_mm;
static uint16_t s_last_static_distance_mm;
static uint8_t s_last_static_energy;
static int64_t s_last_frame_us;
static uint32_t s_recoveries;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t s_req_queue;
static SemaphoreHandle_t s_req_done;
static SemaphoreHandle_t s_api_lock;         /* serializes console requests */
static esp_err_t s_req_result;
static TaskHandle_t s_task;

/* --- small helpers --- */

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* Detection distance quantized to the current gate width. */
static uint16_t quant_mm(uint16_t mm)
{
    uint16_t g = s_gate_mm;
    if (g == 0)
        return mm;
    return (uint16_t)(mm - (uint16_t)(mm % g));
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

static void post_presence(radar_event_t id, bool presence, bool present_static,
                          bool degraded, uint16_t distance_mm, uint8_t moving_energy)
{
    radar_event_data_t ed = {
        .presence = presence,
        .present_static = present_static,
        .degraded = degraded,
        .distance_mm = distance_mm,
        .moving_energy = moving_energy,
    };
    post_event(id, &ed);
}

/* --- NVS persistence --- */

static esp_err_t nvs_blob_read(const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RADAR_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return err;
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

static void nvs_erase_module_config(void)
{
    nvs_handle_t h;
    if (nvs_open(RADAR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    (void)nvs_erase_key(h, RADAR_NVS_SENS);
    (void)nvs_erase_key(h, RADAR_NVS_MAX_MOV);
    (void)nvs_erase_key(h, RADAR_NVS_MAX_ST);
    (void)nvs_erase_key(h, RADAR_NVS_UNMANNED);
    (void)nvs_commit(h);
    nvs_close(h);
}

/* Desired limits: NVS values when present, else the Kconfig defaults. */
static void limits_load(void)
{
    s_limits.max_mov = (uint8_t)CONFIG_RADAR_MAX_MOVING_GATE;
    s_limits.max_st = (uint8_t)CONFIG_RADAR_MAX_STATIC_GATE;
    s_limits.unmanned_s = (uint16_t)CONFIG_RADAR_UNMANNED_S;

    nvs_handle_t h;
    if (nvs_open(RADAR_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return;
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, RADAR_NVS_MAX_MOV, &u8) == ESP_OK &&
        u8 >= 2 && u8 <= RADAR_MAX_GATES)
        s_limits.max_mov = u8;
    if (nvs_get_u8(h, RADAR_NVS_MAX_ST, &u8) == ESP_OK &&
        u8 >= 2 && u8 <= RADAR_MAX_GATES)
        s_limits.max_st = u8;
    if (nvs_get_u16(h, RADAR_NVS_UNMANNED, &u16) == ESP_OK)
        s_limits.unmanned_s = u16;
    nvs_close(h);
}

/* Update the sensitivity mirror blob after a runtime change (so the next
 * boot compares against what the module actually holds). */
static void sens_blob_update(uint16_t gate, uint8_t motion, uint8_t static_sens)
{
    uint8_t blob[RADAR_SENS_BLOB_LEN];
    size_t len = sizeof(blob);
    if (nvs_blob_read(RADAR_NVS_SENS, blob, &len) != ESP_OK || len != sizeof(blob)) {
        /* Seed from the last module read so untouched gates keep their
         * current values; unknown gates stay 0. */
        memset(blob, 0, sizeof(blob));
        for (unsigned g = 0; g <= RADAR_MAX_GATES; g++) {
            blob[g] = s_params.motion_sensitivity[g];
            blob[RADAR_MAX_GATES + 1u + g] = s_params.static_sensitivity[g];
        }
    }
    if (gate == RADAR_ALL_GATES) {
        for (unsigned g = 0; g <= RADAR_MAX_GATES; g++) {
            blob[g] = motion;
            blob[RADAR_MAX_GATES + 1u + g] = static_sens;
        }
    } else {
        blob[gate] = motion;
        blob[RADAR_MAX_GATES + 1u + gate] = static_sens;
    }
    if (nvs_blob_write(RADAR_NVS_SENS, blob, sizeof(blob)) != ESP_OK)
        ESP_LOGW(TAG, "sensitivity blob persist failed");
}

/* --- UART --- */

static esp_err_t uart_setup(void)
{
    esp_err_t err = uart_driver_install(s_uart, CONFIG_RADAR_RX_BUFFER, 0, 0, NULL, 0);
    if (err == ESP_ERR_INVALID_STATE) /* already installed (reinit path) */
        err = ESP_OK;
    if (err != ESP_OK)
        return err;

    const uart_config_t cfg = {
        .baud_rate = CONFIG_RADAR_BAUD,
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
    int n = uart_read_bytes(s_uart, buf, len, pdMS_TO_TICKS(1));
    if (n > 0) {
        /* Command traffic proves the link is alive: without this a long
         * config session would trip the fault watchdog. */
        portENTER_CRITICAL(&s_mux);
        s_last_frame_us = now_us();
        portEXIT_CRITICAL(&s_mux);
    }
    return (n < 0) ? -1 : n;
}

static const radar_command_io_t s_cmd_io = {
    .write = io_write,
    .read = io_read,
    .ctx = NULL,
    .timeout_ms = RADAR_CMD_TIMEOUT_MS,
};

/* --- config session steps (run only in the radar task) --- */

/* READ_PARAMS (0x0061) into *out (and always into s_params). */
static int step_read_params(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    radar_frame_t ack;
    int rc = radar_command_transact(io, p, RADAR_CMD_READ_PARAMS, NULL, 0, &ack);
    if (rc != RADAR_CMD_OK)
        return rc;
    radar_module_params_t tmp;
    if (radar_params_parse(ack.ack.value, ack.ack.value_len, &tmp) != 0)
        return RADAR_CMD_ERR_STATUS;
    s_params = tmp;
    if (s_params.max_gates > 0 && s_params.max_gates <= RADAR_MAX_GATES)
        s_module_max_gates = s_params.max_gates;
    if (ctx != NULL)
        *(radar_module_params_t *)ctx = tmp;
    return RADAR_CMD_OK;
}

static int step_fw_version(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    radar_frame_t ack;
    int rc = radar_command_transact(io, p, RADAR_CMD_FW_VERSION, NULL, 0, &ack);
    if (rc != RADAR_CMD_OK)
        return rc;
    if (radar_fw_version_parse(ack.ack.value, ack.ack.value_len, &s_fw) != 0)
        return RADAR_CMD_ERR_STATUS;
    s_fw_valid = true;
    if (ctx != NULL)
        *(radar_fw_version_t *)ctx = s_fw;
    return RADAR_CMD_OK;
}

static int step_resolution(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    radar_frame_t ack;
    int rc = radar_command_transact(io, p, RADAR_CMD_QUERY_RESOLUTION, NULL, 0, &ack);
    if (rc != RADAR_CMD_OK)
        return rc;
    uint16_t idx;
    if (radar_resolution_parse(ack.ack.value, ack.ack.value_len, &idx) != 0)
        return RADAR_CMD_ERR_STATUS;
    s_res_index = idx;
    s_gate_mm = radar_gate_mm(idx);
    radar_trigger_set_gate_mm(&s_trigger, s_gate_mm);
    if (ctx != NULL)
        *(uint16_t *)ctx = idx;
    return RADAR_CMD_OK;
}

static int step_eng(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    const bool on = *(const bool *)ctx;
    return radar_command_transact(io, p, on ? RADAR_CMD_ENG_ON : RADAR_CMD_ENG_OFF,
                                  NULL, 0, NULL);
}

static int step_set_sensitivity(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    const radar_req_t *req = ctx;
    uint8_t val[RADAR_CMD_VALUE_MAX];
    int n = radar_params_build_sensitivity(req->gate, req->motion, req->static_sens,
                                           val, sizeof(val));
    if (n < 0)
        return RADAR_CMD_ERR_ARG;
    return radar_command_transact(io, p, RADAR_CMD_SET_SENSITIVITY, val, (size_t)n, NULL);
}

static int step_set_limits(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    const radar_req_t *req = ctx;
    uint8_t val[RADAR_CMD_VALUE_MAX];
    int n = radar_params_build_limits(req->max_mov, req->max_st, req->unmanned_s,
                                      val, sizeof(val));
    if (n < 0)
        return RADAR_CMD_ERR_ARG;
    return radar_command_transact(io, p, RADAR_CMD_LIMITS, val, (size_t)n, NULL);
}

static int step_module_reset(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    (void)ctx;
    return radar_command_transact(io, p, RADAR_CMD_MODULE_RESET, NULL, 0, NULL);
}

static int step_factory_reset(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    (void)ctx;
    return radar_command_transact(io, p, RADAR_CMD_FACTORY_RESET, NULL, 0, NULL);
}

/* Calibrate apply: SET_SENSITIVITY (0x0064) for every gate up to the module
 * maximum; gates beyond it are excluded (proposal is 100 there and the
 * module would reject the write). */
static int step_cal_apply(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    (void)ctx;
    for (unsigned g = 0; g <= RADAR_MAX_GATES; g++) {
        if (g > s_module_max_gates)
            break;
        uint8_t val[RADAR_CMD_VALUE_MAX];
        int n = radar_params_build_sensitivity((uint16_t)g, s_cal.motion[g],
                                               s_cal.static_sens[g], val, sizeof(val));
        if (n < 0)
            return RADAR_CMD_ERR_ARG;
        int rc = radar_command_transact(io, p, RADAR_CMD_SET_SENSITIVITY,
                                        val, (size_t)n, NULL);
        if (rc != RADAR_CMD_OK)
            return rc;
    }
    return RADAR_CMD_OK;
}

/* Boot sync: read, compare with the desired configuration, write ONLY what
 * differs, verify, then fw version + resolution (log-only, never fatal).
 * Nothing is written when the module already matches. */
static int step_boot_sync(const radar_command_io_t *io, radar_parser_t *p, void *ctx)
{
    (void)ctx;
    radar_frame_t ack;
    radar_module_params_t cur;
    int rc = step_read_params(io, p, &cur);
    if (rc != RADAR_CMD_OK)
        return rc;

    bool wrote = false;
    if (cur.max_moving_gate != s_limits.max_mov ||
        cur.max_static_gate != s_limits.max_st ||
        cur.unmanned_duration_s != s_limits.unmanned_s) {
        uint8_t val[RADAR_CMD_VALUE_MAX];
        int n = radar_params_build_limits(s_limits.max_mov, s_limits.max_st,
                                          s_limits.unmanned_s, val, sizeof(val));
        if (n < 0)
            return RADAR_CMD_ERR_ARG;
        rc = radar_command_transact(io, p, RADAR_CMD_LIMITS, val, (size_t)n, &ack);
        if (rc != RADAR_CMD_OK)
            return rc;
        wrote = true;
        ESP_LOGI(TAG, "limits written: max_mov=%u max_st=%u unmanned=%us",
                 s_limits.max_mov, s_limits.max_st, s_limits.unmanned_s);
    }

    /* Sensitivity is compared only when a calibration blob exists. */
    uint8_t blob[RADAR_SENS_BLOB_LEN];
    bool have_blob = false;
    size_t blen = sizeof(blob);
    if (nvs_blob_read(RADAR_NVS_SENS, blob, &blen) == ESP_OK && blen == sizeof(blob))
        have_blob = true;
    if (have_blob) {
        uint8_t ngates = s_module_max_gates;
        if (ngates > RADAR_MAX_GATES)
            ngates = RADAR_MAX_GATES;
        int changed = 0;
        for (uint8_t g = 0; g <= ngates; g++) {
            if (blob[g] == cur.motion_sensitivity[g] &&
                blob[RADAR_MAX_GATES + 1u + g] == cur.static_sensitivity[g])
                continue;
            uint8_t val[RADAR_CMD_VALUE_MAX];
            int n = radar_params_build_sensitivity(g, blob[g],
                                                   blob[RADAR_MAX_GATES + 1u + g],
                                                   val, sizeof(val));
            if (n < 0)
                return RADAR_CMD_ERR_ARG;
            rc = radar_command_transact(io, p, RADAR_CMD_SET_SENSITIVITY,
                                        val, (size_t)n, &ack);
            if (rc != RADAR_CMD_OK)
                return rc;
            changed++;
        }
        if (changed > 0) {
            wrote = true;
            ESP_LOGI(TAG, "sensitivity written for %d gate(s)", changed);
        }
    }

    if (wrote) {
        radar_module_params_t chk;
        if (step_read_params(io, p, &chk) == RADAR_CMD_OK) {
            cur = chk;
            bool ok = cur.max_moving_gate == s_limits.max_mov &&
                      cur.max_static_gate == s_limits.max_st &&
                      cur.unmanned_duration_s == s_limits.unmanned_s;
            if (have_blob) {
                uint8_t ngates = s_module_max_gates;
                if (ngates > RADAR_MAX_GATES)
                    ngates = RADAR_MAX_GATES;
                for (uint8_t g = 0; g <= ngates; g++)
                    ok = ok && blob[g] == cur.motion_sensitivity[g] &&
                         blob[RADAR_MAX_GATES + 1u + g] == cur.static_sensitivity[g];
            }
            ESP_LOGI(TAG, "sync verify: %s", ok ? "ok" : "MISMATCH");
        } else {
            ESP_LOGW(TAG, "sync verify read failed");
        }
    } else {
        ESP_LOGI(TAG, "params match, no writes");
    }

    rc = step_fw_version(io, p, NULL);
    if (rc == RADAR_CMD_OK)
        ESP_LOGI(TAG, "module fw: %s (type 0x%04x)", s_fw.text, s_fw.fw_type);
    else
        ESP_LOGW(TAG, "fw version read failed (rc=%d)", rc);

    rc = step_resolution(io, p, NULL);
    if (rc == RADAR_CMD_OK)
        ESP_LOGI(TAG, "resolution: index %u (%u mm/gate)", s_res_index, s_gate_mm);
    else
        ESP_LOGW(TAG, "resolution query failed (rc=%d)", rc);

    /* Engineering is not persistent in the module: re-apply the wanted
     * state after a restart (no-op right after power-up). */
    if (s_engineering) {
        if (radar_command_transact(io, p, RADAR_CMD_ENG_ON, NULL, 0, NULL) == RADAR_CMD_OK)
            ESP_LOGI(TAG, "engineering mode re-applied");
    }
    return RADAR_CMD_OK;
}

static bool boot_sync_attempt(void)
{
    int rc = radar_command_session(&s_cmd_io, &s_parser, step_boot_sync, NULL);
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
    if (uart_setup() != ESP_OK)
        ESP_LOGE(TAG, "UART reinit failed");
}

static void stage_module_reset(void)
{
    ESP_LOGW(TAG, "recovery 2/3: module reset");
    int rc = radar_command_session(&s_cmd_io, &s_parser, step_module_reset, NULL);
    if (rc != RADAR_CMD_OK)
        ESP_LOGD(TAG, "module reset failed (rc=%d)", rc);
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
    none.target_state = RADAR_TARGET_NONE;
    none.timestamp_us = t;
    radar_trigger_events_t ev;
    radar_trigger_feed(&s_trigger, &none, t, &ev);
    if (ev.presence_off)
        post_presence(RADAR_EVENT_PRESENCE_OFF, false, false, true,
                      s_last_distance_mm, 0);
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
             lvl ? "high" : "low", lvl ? "ON" : "OFF");
    post_presence(lvl ? RADAR_EVENT_PRESENCE_ON : RADAR_EVENT_PRESENCE_OFF,
                  lvl != 0, false, true, s_last_distance_mm, 0);
}
#endif

static void fault_watch(int64_t t)
{
    if (!s_fault) {
        int64_t silence_us = t - s_last_frame_us;
        if (silence_us > (int64_t)CONFIG_RADAR_FAULT_TIMEOUT_MS * 1000) {
            s_fault = true;
            ESP_LOGW(TAG, "no valid frame for %d ms -> FAULT",
                     CONFIG_RADAR_FAULT_TIMEOUT_MS);
            portENTER_CRITICAL(&s_mux);
            bool trig_presence = s_trigger.st.presence;
            portEXIT_CRITICAL(&s_mux);
            post_presence(RADAR_EVENT_RADAR_FAULT, trig_presence,
                          s_trigger.st.present_static, true,
                          s_last_distance_mm, s_last_moving_energy);
        }
        return;
    }

    /* fault active: degrade to the OUT pin and keep the hold expiring */
#if CONFIG_RADAR_OUT_ENABLE
    degraded_out_poll();
#endif
    degraded_hold_feed(t);

    if (t >= s_next_action_us) {
        recovery_attempt();
        boot_sync_attempt();
        s_next_action_us = now_us() + (int64_t)s_backoff_ms;
        s_backoff_ms *= 2u;
        if (s_backoff_ms > RADAR_BACKOFF_CAP_MS)
            s_backoff_ms = RADAR_BACKOFF_CAP_MS;
    }
}

/* --- calibration --- */

static void cal_sample(const radar_data_frame_t *d)
{
    s_cal.samples++;
    for (unsigned g = 0; g <= RADAR_MAX_GATES; g++) {
        if (d->moving_gate_energy[g] > s_cal_max_mov[g])
            s_cal_max_mov[g] = d->moving_gate_energy[g];
        if (d->static_gate_energy[g] > s_cal_max_st[g])
            s_cal_max_st[g] = d->static_gate_energy[g];
    }
}

static void cal_finish(void)
{
    s_calibrating = false;
    if (!s_engineering) {
        /* restore the pre-calibration engineering state */
        if (radar_command_session(&s_cmd_io, &s_parser, step_eng,
                                  &(bool){ false }) != RADAR_CMD_OK)
            ESP_LOGW(TAG, "engineering off after calibration failed");
    }
    if (s_fault) {
        ESP_LOGW(TAG, "calibration aborted: sensor went mute (%lu samples)",
                 (unsigned long)s_cal.samples);
        s_cal.valid = false;
        return;
    }
    if (s_cal.samples == 0) {
        ESP_LOGW(TAG, "calibration aborted: no engineering frames received");
        s_cal.valid = false;
        return;
    }
    uint8_t ngates = s_module_max_gates;
    if (ngates > RADAR_MAX_GATES)
        ngates = RADAR_MAX_GATES;
    for (unsigned g = 0; g <= RADAR_MAX_GATES; g++) {
        uint8_t m_max = s_cal_max_mov[g];
        uint8_t s_max = s_cal_max_st[g];
        s_cal.max_energy[g] = (m_max > s_max) ? m_max : s_max;
        if (g <= ngates) {
            unsigned m = (unsigned)m_max + CONFIG_RADAR_CAL_MARGIN;
            unsigned s = (unsigned)s_max + CONFIG_RADAR_CAL_MARGIN;
            s_cal.motion[g] = (m > 100u) ? 100u : (uint8_t)m;
            s_cal.static_sens[g] = (s > 100u) ? 100u : (uint8_t)s;
        } else {
            /* gates beyond the module maximum are excluded */
            s_cal.motion[g] = 100;
            s_cal.static_sens[g] = 100;
        }
    }
    s_cal.valid = true;
    ESP_LOGI(TAG, "calibration done (%lu samples, margin %d):",
             (unsigned long)s_cal.samples, CONFIG_RADAR_CAL_MARGIN);
    ESP_LOGI(TAG, "  motion: %u %u %u %u %u %u %u %u %u",
             s_cal.motion[0], s_cal.motion[1], s_cal.motion[2], s_cal.motion[3],
             s_cal.motion[4], s_cal.motion[5], s_cal.motion[6], s_cal.motion[7],
             s_cal.motion[8]);
    ESP_LOGI(TAG, "  static: %u %u %u %u %u %u %u %u %u",
             s_cal.static_sens[0], s_cal.static_sens[1], s_cal.static_sens[2],
             s_cal.static_sens[3], s_cal.static_sens[4], s_cal.static_sens[5],
             s_cal.static_sens[6], s_cal.static_sens[7], s_cal.static_sens[8]);
}

/* --- frame handling --- */

static void handle_frame(const radar_data_frame_t *d, int64_t t)
{
    portENTER_CRITICAL(&s_mux);
    s_last_frame_us = t;
    portEXIT_CRITICAL(&s_mux);

    if (s_fault) {
        s_fault = false;
        s_recoveries++;
        s_backoff_ms = RADAR_BACKOFF_INIT_MS;
        s_next_action_us = 0;
        ESP_LOGI(TAG, "frames flowing again (recoveries=%lu)",
                 (unsigned long)s_recoveries);
        post_presence(RADAR_EVENT_RADAR_OK, s_trigger.st.presence,
                      s_trigger.st.present_static, false,
                      s_last_distance_mm, s_last_moving_energy);
        if (s_degraded_presence) {
            /* hand presence back to the trigger: it re-fires on its own */
            s_degraded_presence = false;
            post_presence(RADAR_EVENT_PRESENCE_OFF, false, false, false,
                          s_last_distance_mm, 0);
        }
    }

    if (s_calibrating && d->engineering)
        cal_sample(d);

    radar_trigger_events_t ev;
    radar_trigger_feed(&s_trigger, d, t, &ev);

    s_last_distance_mm = quant_mm(d->detection_distance_mm);
    s_last_moving_energy = d->moving_energy;
    s_last_target_state = d->target_state;
    s_last_moving_distance_mm = d->moving_distance_mm;
    s_last_static_distance_mm = d->static_distance_mm;
    s_last_static_energy = d->static_energy;

    radar_event_data_t ed = {
        .presence = s_trigger.st.presence,
        .present_static = s_trigger.st.present_static,
        .degraded = false,
        .distance_mm = s_last_distance_mm,
        .moving_energy = d->moving_energy,
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
    case REQ_READ_PARAMS: {
        int rc = radar_command_session(&s_cmd_io, &s_parser, step_read_params,
                                       req->params_out);
        return cmd_err_to_esp(rc);
    }
    case REQ_FW_VERSION: {
        int rc = radar_command_session(&s_cmd_io, &s_parser, step_fw_version,
                                       req->fw_out);
        return cmd_err_to_esp(rc);
    }
    case REQ_RESOLUTION: {
        int rc = radar_command_session(&s_cmd_io, &s_parser, step_resolution,
                                       req->res_out);
        return cmd_err_to_esp(rc);
    }
    case REQ_SET_SENSITIVITY: {
        if (req->gate != RADAR_ALL_GATES && req->gate > s_module_max_gates)
            return ESP_ERR_INVALID_ARG;
        int rc = radar_command_session(&s_cmd_io, &s_parser, step_set_sensitivity,
                                       (void *)req);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        sens_blob_update(req->gate, req->motion, req->static_sens);
        return ESP_OK;
    }
    case REQ_SET_LIMITS: {
        if (req->max_mov < 2 || req->max_mov > RADAR_MAX_GATES ||
            req->max_st < 2 || req->max_st > RADAR_MAX_GATES)
            return ESP_ERR_INVALID_ARG;
        int rc = radar_command_session(&s_cmd_io, &s_parser, step_set_limits,
                                       (void *)req);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        s_limits.max_mov = (uint8_t)req->max_mov;
        s_limits.max_st = (uint8_t)req->max_st;
        s_limits.unmanned_s = req->unmanned_s;
        nvs_handle_t h;
        esp_err_t err = nvs_open(RADAR_NVS_NAMESPACE, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            (void)nvs_set_u8(h, RADAR_NVS_MAX_MOV, s_limits.max_mov);
            (void)nvs_set_u8(h, RADAR_NVS_MAX_ST, s_limits.max_st);
            (void)nvs_set_u16(h, RADAR_NVS_UNMANNED, s_limits.unmanned_s);
            (void)nvs_commit(h);
            nvs_close(h);
        } else {
            ESP_LOGW(TAG, "limits persist failed (%s)", esp_err_to_name(err));
        }
        return ESP_OK;
    }
    case REQ_SET_ENGINEERING: {
        int rc = radar_command_session(&s_cmd_io, &s_parser, step_eng,
                                       (void *)&req->eng_on);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        s_engineering = req->eng_on;
        return ESP_OK;
    }
    case REQ_RESTART: {
        int rc = radar_command_session(&s_cmd_io, &s_parser, step_module_reset, NULL);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        vTaskDelay(pdMS_TO_TICKS(RADAR_REBOOT_DELAY_MS));
        s_synced = false; /* loop re-syncs and re-applies engineering */
        return ESP_OK;
    }
    case REQ_FACTORY_RESET: {
        int rc = radar_command_session(&s_cmd_io, &s_parser, step_factory_reset, NULL);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        vTaskDelay(pdMS_TO_TICKS(RADAR_REBOOT_DELAY_MS));
        /* the module reverts to factory: forget our mirror of the module
         * configuration so the boot sync compares against Kconfig defaults */
        nvs_erase_module_config();
        s_limits.max_mov = (uint8_t)CONFIG_RADAR_MAX_MOVING_GATE;
        s_limits.max_st = (uint8_t)CONFIG_RADAR_MAX_STATIC_GATE;
        s_limits.unmanned_s = (uint16_t)CONFIG_RADAR_UNMANNED_S;
        s_synced = false;
        return ESP_OK;
    }
    case REQ_CAL_START: {
        if (s_calibrating)
            return ESP_ERR_INVALID_STATE;
        if (!s_engineering) {
            bool on = true;
            int rc = radar_command_session(&s_cmd_io, &s_parser, step_eng, &on);
            if (rc != RADAR_CMD_OK)
                return cmd_err_to_esp(rc);
        }
        memset(&s_cal, 0, sizeof(s_cal));
        memset(s_cal_max_mov, 0, sizeof(s_cal_max_mov));
        memset(s_cal_max_st, 0, sizeof(s_cal_max_st));
        s_cal_end_us = now_us() + (int64_t)req->seconds * 1000000;
        s_calibrating = true;
        ESP_LOGI(TAG, "calibration started (%lus, keep the room empty)",
                 (unsigned long)req->seconds);
        return ESP_OK;
    }
    case REQ_CAL_APPLY: {
        if (!s_cal.valid || s_calibrating)
            return ESP_ERR_INVALID_STATE;
        int rc = radar_command_session(&s_cmd_io, &s_parser, step_cal_apply, NULL);
        if (rc != RADAR_CMD_OK)
            return cmd_err_to_esp(rc);
        uint8_t blob[RADAR_SENS_BLOB_LEN];
        memcpy(blob, s_cal.motion, RADAR_MAX_GATES + 1u);
        memcpy(blob + RADAR_MAX_GATES + 1u, s_cal.static_sens, RADAR_MAX_GATES + 1u);
        if (nvs_blob_write(RADAR_NVS_SENS, blob, sizeof(blob)) != ESP_OK)
            ESP_LOGW(TAG, "calibration persist failed");
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
        if (xSemaphoreTake(s_req_done, pdMS_TO_TICKS(RADAR_REQ_WAIT_MS)) == pdTRUE)
            res = s_req_result;
    }
    xSemaphoreGive(s_api_lock);
    return res;
}

/* --- task --- */

static void radar_task(void *arg)
{
    (void)arg;
    uint8_t rxbuf[64];

    boot_sync_attempt();
    s_next_action_us = now_us() + (int64_t)s_backoff_ms;
    s_backoff_ms *= 2u;

    for (;;) {
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

        int64_t t = now_us();
        fault_watch(t);
        if (s_calibrating && t >= s_cal_end_us)
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

    limits_load();

    radar_trigger_config_t tc = {
        .trigger_mm = (uint16_t)CONFIG_RADAR_TRIGGER_MM,
        .trigger_energy = (uint8_t)CONFIG_RADAR_TRIGGER_ENERGY,
        .confirm_frames = (uint8_t)CONFIG_RADAR_CONFIRM_FRAMES,
        .hold_ms = (uint32_t)CONFIG_RADAR_HOLD_MS,
        .hysteresis_gates = (uint8_t)CONFIG_RADAR_HYSTERESIS_GATES,
        .gate_mm = s_gate_mm,
#if CONFIG_RADAR_STATIC_TRIGGER
        .static_trigger = true,
#endif
    };
    radar_trigger_init(&s_trigger, &tc);

    nvs_handle_t h;
    if (nvs_open(RADAR_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint16_t mm;
        if (nvs_get_u16(h, RADAR_NVS_TRIGGER, &mm) == ESP_OK)
            radar_trigger_set_trigger_mm(&s_trigger, mm); /* clamps */
        nvs_close(h);
    }

    err = uart_setup();
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

    s_last_frame_us = now_us();

    if (xTaskCreate(radar_task, "radar", CONFIG_RADAR_TASK_STACK, NULL,
                    RADAR_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "radar task started (uart%d %d 8N1, tx=%d rx=%d)", CONFIG_RADAR_UART_NUM,
             CONFIG_RADAR_BAUD, CONFIG_RADAR_TX_GPIO, CONFIG_RADAR_RX_GPIO);
    return ESP_OK;
}

esp_err_t radar_get_state(radar_state_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_mux);
    int64_t last = s_last_frame_us;
    portEXIT_CRITICAL(&s_mux);

    out->presence = s_fault ? s_degraded_presence : s_trigger.st.presence;
    out->present_static = s_fault ? false : s_trigger.st.present_static;
    out->fault = s_fault;
    out->engineering = s_engineering;
    out->distance_mm = s_last_distance_mm;
    out->moving_energy = s_last_moving_energy;
    out->target_state = s_last_target_state;
    out->moving_distance_mm = s_last_moving_distance_mm;
    out->static_distance_mm = s_last_static_distance_mm;
    out->static_energy = s_last_static_energy;
    out->gate_mm = s_gate_mm;
    out->resolution_index = s_res_index;
    out->last_frame_us = last;
    out->frames_ok = s_parser.frames_ok;
    out->frames_bad = s_parser.frames_bad;
    out->bytes_dropped = s_parser.bytes_dropped;
    out->recoveries = s_recoveries;
    out->calibrating = s_calibrating;
    return ESP_OK;
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

esp_err_t radar_set_sensitivity(uint16_t gate, uint8_t motion, uint8_t static_sens)
{
    if (motion > 100 || static_sens > 100)
        return ESP_ERR_INVALID_ARG;
    radar_req_t req = {
        .kind = REQ_SET_SENSITIVITY,
        .gate = gate,
        .motion = motion,
        .static_sens = static_sens,
    };
    return post_request(&req);
}

esp_err_t radar_set_engineering(bool on)
{
    radar_req_t req = {
        .kind = REQ_SET_ENGINEERING,
        .eng_on = on,
    };
    return post_request(&req);
}

esp_err_t radar_read_module_params(radar_module_params_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;
    radar_req_t req = {
        .kind = REQ_READ_PARAMS,
        .params_out = out,
    };
    return post_request(&req);
}

esp_err_t radar_read_fw_version(radar_fw_version_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;
    radar_req_t req = {
        .kind = REQ_FW_VERSION,
        .fw_out = out,
    };
    return post_request(&req);
}

esp_err_t radar_read_resolution(uint16_t *index_out)
{
    if (index_out == NULL)
        return ESP_ERR_INVALID_ARG;
    radar_req_t req = {
        .kind = REQ_RESOLUTION,
        .res_out = index_out,
    };
    return post_request(&req);
}

esp_err_t radar_restart_module(void)
{
    radar_req_t req = { .kind = REQ_RESTART };
    return post_request(&req);
}

esp_err_t radar_factory_reset(void)
{
    radar_req_t req = { .kind = REQ_FACTORY_RESET };
    return post_request(&req);
}

esp_err_t radar_calibrate(uint32_t seconds)
{
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
    radar_req_t req = { .kind = REQ_CAL_APPLY };
    return post_request(&req);
}

/* Runtime limits setter (declared in the public header): routed through the
 * radar task like every other module command, persisted on success. */
esp_err_t radar_set_limits(uint16_t max_moving_gate, uint16_t max_static_gate,
                           uint16_t unmanned_s)
{
    radar_req_t req = {
        .kind = REQ_SET_LIMITS,
        .max_mov = max_moving_gate,
        .max_st = max_static_gate,
        .unmanned_s = unmanned_s,
    };
    return post_request(&req);
}
