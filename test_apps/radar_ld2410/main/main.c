/* Radar LD2410B test app: interactive console over the radar_ld2410 public
 * API (Phase 2, ESP32 classic).
 *
 * - Starts the radar (UART task, boot config sync), registers on the
 *   PRESENCE_EVENT default loop and logs presence/distance/fault events.
 * - REPL on the default UART exposes manual controls: status, version,
 *   params, resolution, trigger, sensitivity, limits, engineering, restart,
 *   factoryreset, calibrate, csv.
 * - A 20 Hz task prints one CSV line per state change (telemetry for bench
 *   logs); enabled by default, "radar csv off" silences it.
 *
 * Log tags: the component logs under "radar"; this app logs under "app" so
 * component and application layers stay distinguishable in one monitor.
 *
 * Command handlers parse argv with strtol and keep all state in static
 * storage: no per-command heap allocation.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "radar_ld2410.h"

static const char *TAG = "app";

#define RADAR_CSV_PERIOD_MS 50 /* 20 Hz poll of the state snapshot */
#define RADAR_CSV_STACK_SIZE 3072
#define RADAR_CSV_PRIORITY 3

#define RADAR_CSV_HEADER \
    "ms,state,mov_mm,mov_en,st_mm,st_en,det_mm,presence,static,eng,deg,fault"

/* CSV output gate shared with the csv task (plain volatile flag, like the
 * console scale mirror in the led app). */
static volatile bool s_csv_enabled = true;

/* Last row printed: rows repeat only when something changes. */
static bool s_csv_prev_valid;
static radar_state_t s_csv_prev;

/* --- argv parsing helpers (no allocation, strict validation) --- */

static bool parse_u8(const char *s, uint8_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 255) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static bool parse_u16(const char *s, uint16_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 65535) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

/* --- shared print helpers --- */

/* Module target state (§2.3.2 Table 14) collapsed to the four CSV names;
 * the noise-calibration states only appear during the module's own
 * background-noise procedure and map to NONE. */
static const char *target_state_name(radar_target_state_t t)
{
    switch (t) {
    case RADAR_TARGET_MOVING:
        return "MOV";
    case RADAR_TARGET_STATIC:
        return "ST";
    case RADAR_TARGET_BOTH:
        return "BOTH";
    default:
        return "NONE";
    }
}

/* --- console command handlers (all dispatched from cmd_radar) --- */

static int cmd_status(void)
{
    radar_state_t st;
    esp_err_t err = radar_get_state(&st);
    if (err != ESP_OK) {
        printf("radar status: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("presence=%u static=%u fault=%u engineering=%u calibrating=%u\n",
           (unsigned)st.presence, (unsigned)st.present_static,
           (unsigned)st.fault, (unsigned)st.engineering,
           (unsigned)st.calibrating);
    printf("target=%s det=%u mm mov=%u mm (en %u) st=%u mm (en %u)\n",
           target_state_name(st.target_state), st.distance_mm,
           st.moving_distance_mm, st.moving_energy,
           st.static_distance_mm, st.static_energy);
    printf("gate=%u mm res_index=%u frames ok=%lu bad=%lu dropped=%lu recoveries=%lu\n",
           st.gate_mm, st.resolution_index,
           (unsigned long)st.frames_ok, (unsigned long)st.frames_bad,
           (unsigned long)st.bytes_dropped, (unsigned long)st.recoveries);
    return 0;
}

static int cmd_version(void)
{
    radar_fw_version_t fw;
    esp_err_t err = radar_read_fw_version(&fw);
    if (err != ESP_OK) {
        printf("radar version: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("fw: %s (type 0x%04x)\n", fw.text, fw.fw_type);
    return 0;
}

static int cmd_params(void)
{
    radar_module_params_t p;
    esp_err_t err = radar_read_module_params(&p);
    if (err != ESP_OK) {
        printf("radar params: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("max_gates=%u max_moving=%u max_static=%u unmanned=%us\n",
           p.max_gates, p.max_moving_gate, p.max_static_gate,
           p.unmanned_duration_s);
    printf("gate motion static\n");
    for (unsigned g = 0; g <= p.max_gates && g <= RADAR_MAX_GATES; g++) {
        printf("%4u %6u %6u\n", g, p.motion_sensitivity[g],
               p.static_sensitivity[g]);
    }
    return 0;
}

static int cmd_resolution(void)
{
    uint16_t index = 0;
    esp_err_t err = radar_read_resolution(&index);
    if (err != ESP_OK) {
        printf("radar resolution: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("resolution: index %u (%u mm/gate)\n", index, radar_gate_mm(index));
    return 0;
}

static int cmd_trigger(int argc, char **argv)
{
    uint16_t mm;
    if (argc != 3 || !parse_u16(argv[2], &mm)) {
        printf("usage: radar trigger <mm>  (detection distance, e.g. 3000)\n");
        return 1;
    }
    esp_err_t err = radar_set_trigger_mm(mm);
    if (err != ESP_OK) {
        printf("radar trigger: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("trigger distance set (clamped to the valid range)\n");
    return 0;
}

static int cmd_sensitivity(int argc, char **argv)
{
    uint8_t motion, static_sens;
    if (argc != 5 || !parse_u8(argv[3], &motion) || !parse_u8(argv[4], &static_sens) ||
        motion > 100 || static_sens > 100) {
        printf("usage: radar sensitivity <gate|all> <motion> <static>  (levels 0-100)\n");
        return 1;
    }
    uint16_t gate;
    if (strcmp(argv[2], "all") == 0) {
        gate = RADAR_ALL_GATES;
    } else if (!parse_u16(argv[2], &gate) || gate > RADAR_MAX_GATES) {
        printf("usage: radar sensitivity <gate|all> <motion> <static>  (gate 0-%u)\n",
               (unsigned)RADAR_MAX_GATES);
        return 1;
    }
    esp_err_t err = radar_set_sensitivity(gate, motion, static_sens);
    if (err != ESP_OK) {
        printf("radar sensitivity: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("sensitivity set: gate=%s motion=%u static=%u\n", argv[2], motion,
           static_sens);
    return 0;
}

static int cmd_limits(int argc, char **argv)
{
    uint16_t mov, st, dur;
    if (argc != 5 || !parse_u16(argv[2], &mov) || !parse_u16(argv[3], &st) ||
        !parse_u16(argv[4], &dur) ||
        mov < 2 || mov > RADAR_MAX_GATES || st < 2 || st > RADAR_MAX_GATES) {
        printf("usage: radar limits <mov> <st> <durata_s>  (gates 2-%u, duration 0-65535 s)\n",
               (unsigned)RADAR_MAX_GATES);
        return 1;
    }
    esp_err_t err = radar_set_limits(mov, st, dur);
    if (err != ESP_OK) {
        printf("radar limits: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("limits set: max_mov=%u max_st=%u unmanned=%us (persisted)\n", mov,
           st, dur);
    return 0;
}

static int cmd_engineering(int argc, char **argv)
{
    bool on;
    if (argc == 3 && strcmp(argv[2], "on") == 0) {
        on = true;
    } else if (argc == 3 && strcmp(argv[2], "off") == 0) {
        on = false;
    } else {
        printf("usage: radar engineering <on|off>\n");
        return 1;
    }
    esp_err_t err = radar_set_engineering(on);
    if (err != ESP_OK) {
        printf("radar engineering: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("engineering mode %s\n", on ? "on" : "off");
    return 0;
}

static int cmd_restart(void)
{
    esp_err_t err = radar_restart_module();
    if (err != ESP_OK) {
        printf("radar restart: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("module restart sent (the component re-syncs and re-applies its state)\n");
    return 0;
}

static int cmd_factoryreset(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[2], "confirm") != 0) {
        printf("WARNING: factoryreset restores the module defaults (baud 256000, "
               "factory sensitivity) and erases the stored radar config; "
               "append 'confirm' to run it\n");
        return 1;
    }
    esp_err_t err = radar_factory_reset();
    if (err != ESP_OK) {
        printf("radar factoryreset: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("factory reset done (stored config erased; boot sync will rewrite "
           "the configured values)\n");
    return 0;
}

static int cmd_calibrate(int argc, char **argv)
{
    if (argc == 2) {
        esp_err_t err = radar_calibrate((uint32_t)CONFIG_RADAR_CAL_SECONDS);
        if (err != ESP_OK) {
            printf("radar calibrate: failed (%s)\n", esp_err_to_name(err));
            return 1;
        }
        printf("calibration started (%us default): keep the room empty, then "
               "'radar calibrate show' / 'apply'\n", CONFIG_RADAR_CAL_SECONDS);
        return 0;
    }
    if (strcmp(argv[2], "show") == 0) {
        radar_calibration_t cal;
        esp_err_t err = radar_calibrate_get(&cal);
        if (err != ESP_OK) {
            printf("radar calibrate show: no proposal available "
                   "(run 'radar calibrate [seconds]' first)\n");
            return 1;
        }
        printf("proposal: %lu samples (margin %d)\n",
               (unsigned long)cal.samples, CONFIG_RADAR_CAL_MARGIN);
        printf("gate motion static max_energy\n");
        for (unsigned g = 0; g <= RADAR_MAX_GATES; g++) {
            printf("%4u %6u %6u %6u\n", g, cal.motion[g], cal.static_sens[g],
                   cal.max_energy[g]);
        }
        return 0;
    }
    if (strcmp(argv[2], "apply") == 0) {
        esp_err_t err = radar_calibrate_apply();
        if (err != ESP_OK) {
            printf("radar calibrate apply: failed (%s)\n", esp_err_to_name(err));
            return 1;
        }
        printf("proposal applied and persisted (boot sync compares from now on)\n");
        return 0;
    }
    uint16_t seconds;
    if (!parse_u16(argv[2], &seconds) || seconds == 0) {
        printf("usage: radar calibrate [secondi] | radar calibrate show | radar calibrate apply\n");
        return 1;
    }
    esp_err_t err = radar_calibrate(seconds);
    if (err != ESP_OK) {
        printf("radar calibrate: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("calibration started (%us): keep the room empty, then "
           "'radar calibrate show' / 'apply'\n", seconds);
    return 0;
}

static int cmd_csv(int argc, char **argv)
{
    bool on;
    if (argc == 3 && strcmp(argv[2], "on") == 0) {
        on = true;
    } else if (argc == 3 && strcmp(argv[2], "off") == 0) {
        on = false;
    } else {
        printf("usage: radar csv <on|off>\n");
        return 1;
    }
    s_csv_enabled = on;
    if (on) {
        /* force the next poll to print a fresh row after the header */
        s_csv_prev_valid = false;
        printf("%s\n", RADAR_CSV_HEADER);
    }
    printf("csv %s\n", on ? "on" : "off");
    return 0;
}

static void radar_print_usage(void)
{
    printf("usage: radar <subcommand>\n");
    printf("  status | version | params | resolution\n");
    printf("  trigger <mm>\n");
    printf("  sensitivity <gate|all> <motion> <static>\n");
    printf("  limits <mov> <st> <durata_s>\n");
    printf("  engineering <on|off>\n");
    printf("  restart\n");
    printf("  factoryreset confirm\n");
    printf("  calibrate [secondi] | calibrate show | calibrate apply\n");
    printf("  csv <on|off>\n");
}

static int cmd_radar(int argc, char **argv)
{
    if (argc < 2) {
        radar_print_usage();
        return 1;
    }
    if (strcmp(argv[1], "status") == 0) {
        return cmd_status();
    }
    if (strcmp(argv[1], "version") == 0) {
        return cmd_version();
    }
    if (strcmp(argv[1], "params") == 0) {
        return cmd_params();
    }
    if (strcmp(argv[1], "resolution") == 0) {
        return cmd_resolution();
    }
    if (strcmp(argv[1], "trigger") == 0) {
        return cmd_trigger(argc, argv);
    }
    if (strcmp(argv[1], "sensitivity") == 0) {
        return cmd_sensitivity(argc, argv);
    }
    if (strcmp(argv[1], "limits") == 0) {
        return cmd_limits(argc, argv);
    }
    if (strcmp(argv[1], "engineering") == 0) {
        return cmd_engineering(argc, argv);
    }
    if (strcmp(argv[1], "restart") == 0) {
        return cmd_restart();
    }
    if (strcmp(argv[1], "factoryreset") == 0) {
        return cmd_factoryreset(argc, argv);
    }
    if (strcmp(argv[1], "calibrate") == 0) {
        return cmd_calibrate(argc, argv);
    }
    if (strcmp(argv[1], "csv") == 0) {
        return cmd_csv(argc, argv);
    }
    printf("unknown subcommand '%s'\n", argv[1]);
    radar_print_usage();
    return 1;
}

static void register_cmd(const char *name, const char *help, const char *hint,
                         esp_console_cmd_func_t func)
{
    esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .hint = hint,
        .func = func,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void register_console_commands(void)
{
    esp_console_register_help_command();
    register_cmd("radar", "Radar LD2410B controls (status/version/params/trigger/...)",
                 "<subcommand> [...]", cmd_radar);
}

/* --- presence events --- */

static void radar_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    const radar_event_data_t *ed = event_data;
    switch (id) {
    case RADAR_EVENT_PRESENCE_ON:
        ESP_LOGI(TAG, "presence ON (distance=%u mm, moving energy=%u)",
                 ed->distance_mm, ed->moving_energy);
        break;
    case RADAR_EVENT_PRESENCE_OFF:
        ESP_LOGI(TAG, "presence OFF");
        break;
    case RADAR_EVENT_DISTANCE_UPDATED:
        ESP_LOGI(TAG, "distance updated: %u mm (moving energy=%u)",
                 ed->distance_mm, ed->moving_energy);
        break;
    case RADAR_EVENT_RADAR_FAULT:
        ESP_LOGW(TAG, "radar fault: no frames, degrading to the OUT pin");
        break;
    case RADAR_EVENT_RADAR_OK:
        ESP_LOGI(TAG, "radar ok: frames flowing again");
        break;
    default:
        break;
    }
}

/* --- CSV telemetry --- */

/* True when any of the printed columns differs (ms excluded by design). */
static bool csv_row_changed(const radar_state_t *a, const radar_state_t *b)
{
    return a->target_state != b->target_state ||
           a->moving_distance_mm != b->moving_distance_mm ||
           a->moving_energy != b->moving_energy ||
           a->static_distance_mm != b->static_distance_mm ||
           a->static_energy != b->static_energy ||
           a->distance_mm != b->distance_mm ||
           a->presence != b->presence ||
           a->present_static != b->present_static ||
           a->engineering != b->engineering ||
           a->fault != b->fault;
}

/* Polls the state snapshot at 20 Hz and prints one line per change, direct
 * printf, nothing allocated. deg: in this component the degraded mode
 * (presence driven by the OUT pin) coincides with the fault condition, so
 * the deg column mirrors fault. */
static void csv_task(void *arg)
{
    (void)arg;
    printf("%s\n", RADAR_CSV_HEADER);
    TickType_t next_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(RADAR_CSV_PERIOD_MS));
        if (!s_csv_enabled) {
            continue;
        }
        radar_state_t st;
        if (radar_get_state(&st) != ESP_OK) {
            continue;
        }
        if (s_csv_prev_valid && !csv_row_changed(&st, &s_csv_prev)) {
            continue;
        }
        printf("%lld,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
               (long long)(esp_timer_get_time() / 1000),
               target_state_name(st.target_state),
               st.moving_distance_mm, st.moving_energy,
               st.static_distance_mm, st.static_energy,
               st.distance_mm,
               (unsigned)st.presence, (unsigned)st.present_static,
               (unsigned)st.engineering, (unsigned)st.fault,
               (unsigned)st.fault);
        s_csv_prev = st;
        s_csv_prev_valid = true;
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* radar_init tolerates an existing default loop (ESP_ERR_INVALID_STATE). */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(PRESENCE_EVENT, ESP_EVENT_ANY_ID,
                                               radar_event_handler, NULL));

    err = radar_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radar_init failed (%s)", esp_err_to_name(err));
        abort();
    }

    register_console_commands();

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "radar>";
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    BaseType_t ok = xTaskCreate(csv_task, "radar_csv", RADAR_CSV_STACK_SIZE,
                                NULL, RADAR_CSV_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create csv task");
        abort();
    }
}
