/* Radar LD2420 test app: interactive console over the radar_ld2420 public
 * API (Phase 2, ESP32 classic).
 *
 * - Starts the radar (UART task, baud probe, boot config sync), registers
 *   on the PRESENCE_EVENT default loop and logs presence/distance/fault
 *   events.
 * - REPL on the default UART exposes manual controls: status, version,
 *   params, trigger, sensitivity, limits, engineering, restart,
 *   factoryreset, calibrate, csv.
 * - A 20 Hz task prints one CSV line per state change:
 *   ms,mode,pres,dist_mm,latched,fault plus the 16 per-gate energies
 *   (comma separated) whenever the module streams energy frames
 *   (gates_valid); enabled by default, "radar csv off" silences it.
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
#include "radar_ld2420.h"

static const char *TAG = "app";

#define RADAR_CSV_PERIOD_MS 50 /* 20 Hz poll of the state snapshot */
#define RADAR_CSV_STACK_SIZE 3072
#define RADAR_CSV_PRIORITY 3

#define RADAR_CSV_HEADER "ms,mode,pres,dist_mm,latched,fault"

/* CSV output gate shared with the csv task (plain volatile flag, like the
 * console scale mirror in the led app). */
static volatile bool s_csv_enabled = true;

/* Last row printed: rows repeat only when something changes. */
static bool s_csv_prev_valid;
static radar_state_t s_csv_prev;

/* --- argv parsing helpers (no allocation, strict validation) --- */

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

static const char *mode_name(radar_module_mode_t mode)
{
    return (mode == RADAR_MODULE_MODE_ENERGY) ? "ENG" : "RUN";
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
    long long silence_ms = (esp_timer_get_time() - st.last_frame_us) / 1000;
    printf("latched=%u raw_pres=%u fault=%u degraded=%u mode=%s calibrating=%u\n",
           (unsigned)st.presence, (unsigned)st.raw_presence,
           (unsigned)st.fault, (unsigned)st.degraded, mode_name(st.mode),
           (unsigned)st.calibrating);
    printf("dist=%u mm gates_valid=%u fw='%s' baud=%lu silence_ms=%lld\n",
           st.distance_mm, (unsigned)st.gates_valid, st.fw_text,
           (unsigned long)st.baud, silence_ms);
    printf("frames ok=%lu bad=%lu dropped=%lu recoveries=%lu\n",
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
    printf("fw: %s (numeric %ld, energy frames %s)\n", fw.text,
           (long)fw.numeric, fw.energy_capable ? "yes (fw >= 1.5.4)" : "no");
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
    printf("min_gate=%u max_gate=%u timeout=%us\n", p.min_gate, p.max_gate,
           p.timeout_s);
    printf("gate    move   still\n");
    for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
        printf("%4u %6lu %6lu\n", g, (unsigned long)p.move_thresh[g],
               (unsigned long)p.still_thresh[g]);
    }
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
    uint16_t move, still;
    if (argc != 5 || !parse_u16(argv[3], &move) || !parse_u16(argv[4], &still)) {
        printf("usage: radar sensitivity <gate|all> <move> <still>  (levels 0-65535)\n");
        return 1;
    }
    uint16_t gate;
    if (strcmp(argv[2], "all") == 0) {
        gate = RADAR_ALL_GATES;
    } else if (!parse_u16(argv[2], &gate) || gate >= RADAR_MAX_GATES) {
        printf("usage: radar sensitivity <gate|all> <move> <still>  (gate 0-%u)\n",
               (unsigned)(RADAR_MAX_GATES - 1u));
        return 1;
    }
    esp_err_t err = radar_set_gate_thresholds(gate, move, still);
    if (err != ESP_OK) {
        printf("radar sensitivity: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("thresholds set: gate=%s move=%u still=%u (persisted)\n", argv[2],
           move, still);
    return 0;
}

static int cmd_limits(int argc, char **argv)
{
    uint16_t min_gate, max_gate, hold_s;
    if (argc != 5 || !parse_u16(argv[2], &min_gate) ||
        !parse_u16(argv[3], &max_gate) || !parse_u16(argv[4], &hold_s) ||
        min_gate > RADAR_MAX_GATES || max_gate > RADAR_MAX_GATES ||
        min_gate > max_gate || hold_s > 255) {
        printf("usage: radar limits <min> <max> <s>  (gates 0-%u, min<=max, "
               "hold 0-255 s)\n", (unsigned)RADAR_MAX_GATES);
        return 1;
    }
    esp_err_t err = radar_set_gate_limits(min_gate, max_gate, hold_s);
    if (err != ESP_OK) {
        printf("radar limits: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("limits set: min=%u max=%u timeout=%us (persisted)\n", min_gate,
           max_gate, hold_s);
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
        if (err == ESP_ERR_NOT_SUPPORTED) {
            printf("radar engineering: not supported (energy frames need "
                   "module fw >= 1.5.4)\n");
        } else {
            printf("radar engineering: failed (%s)\n", esp_err_to_name(err));
        }
        return 1;
    }
    printf("serial mode %s\n", on ? "ENERGY" : "RUNNING");
    return 0;
}

static int cmd_restart(void)
{
    esp_err_t err = radar_restart_module();
    if (err != ESP_OK) {
        printf("radar restart: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("module restart sent (the component re-runs mode selection and "
           "config sync)\n");
    return 0;
}

static int cmd_factoryreset(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[2], "confirm") != 0) {
        printf("WARNING: factoryreset rewrites the factory table (ESPHome "
               "thresholds, timeout 120 s, min gate 1, max gate 12) and "
               "erases ALL NVS keys of namespace 'radar'; append 'confirm' "
               "to run it\n");
        return 1;
    }
    esp_err_t err = radar_factory_reset();
    if (err != ESP_OK) {
        printf("radar factoryreset: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("factory table written and NVS 'radar' erased (the boot sync "
           "re-applies only the Kconfig gate limits)\n");
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
        printf("proposal: %lu samples\n", (unsigned long)cal.samples);
        printf("gate   peak   move   still\n");
        for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
            printf("%4u %6u %6lu %6lu\n", g, cal.peak[g],
                   (unsigned long)cal.move[g], (unsigned long)cal.still[g]);
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
        printf("usage: radar calibrate [seconds] | radar calibrate show | "
               "radar calibrate apply\n");
        return 1;
    }
    esp_err_t err = radar_calibrate(seconds);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_SUPPORTED) {
            printf("radar calibrate: not supported (needs energy mode: "
                   "fw >= 1.5.4 and 'radar engineering on')\n");
        } else {
            printf("radar calibrate: failed (%s)\n", esp_err_to_name(err));
        }
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
    printf("  status | version | params\n");
    printf("  trigger <mm>\n");
    printf("  sensitivity <gate|all> <move> <still>\n");
    printf("  limits <min> <max> <s>\n");
    printf("  engineering <on|off>\n");
    printf("  restart\n");
    printf("  factoryreset confirm\n");
    printf("  calibrate [seconds] | calibrate show | calibrate apply\n");
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
    register_cmd("radar", "Radar LD2420 controls (status/version/params/trigger/...)",
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
        ESP_LOGI(TAG, "presence ON (distance=%u mm, degraded=%u)",
                 ed->distance_mm, (unsigned)ed->degraded);
        break;
    case RADAR_EVENT_PRESENCE_OFF:
        ESP_LOGI(TAG, "presence OFF (degraded=%u)", (unsigned)ed->degraded);
        break;
    case RADAR_EVENT_DISTANCE_UPDATED:
        ESP_LOGI(TAG, "distance updated: %u mm", ed->distance_mm);
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
    if (a->mode != b->mode || a->raw_presence != b->raw_presence ||
        a->presence != b->presence || a->distance_mm != b->distance_mm ||
        a->fault != b->fault || a->gates_valid != b->gates_valid) {
        return true;
    }
    if (a->gates_valid) {
        return memcmp(a->gate_energy, b->gate_energy,
                      sizeof(a->gate_energy)) != 0;
    }
    return false;
}

/* Polls the state snapshot at 20 Hz and prints one line per change, direct
 * printf, nothing allocated. Columns: ms,mode,pres,dist_mm,latched,fault
 * plus 16 gate energies appended whenever the module streams them
 * (gates_valid). */
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
        printf("%lld,%s,%u,%u,%u,%u",
               (long long)(esp_timer_get_time() / 1000),
               mode_name(st.mode), (unsigned)st.raw_presence, st.distance_mm,
               (unsigned)st.presence, (unsigned)st.fault);
        if (st.gates_valid) {
            for (unsigned g = 0; g < RADAR_MAX_GATES; g++) {
                printf(",%u", st.gate_energy[g]);
            }
        }
        printf("\n");
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
