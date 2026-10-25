/* LED effects test app: interactive console over the led_fx public API (Phase 1).
 *
 * - Starts the animation engine (rail OFF until powered), then a background
 *   task that powers the rail on and cycles every registry effect (10 s slot).
 * - REPL on the default UART exposes manual controls: fx, oneshot, color,
 *   bright, speed, power, stats, current, scale.
 *
 * Command handlers parse argv with strtol/strtof and keep all state in static
 * storage: no per-command heap allocation.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_fx.h"

static const char *TAG = "led";

#define CYCLE_SLOT_MS 10000 /* per-effect slot duration */
#define FX_CYCLE_STACK_SIZE 3072
#define FX_CYCLE_PRIORITY 4

/* Mirrors the last scale value set from this console (API has no getter). */
static uint8_t s_console_scale_pct = 100;

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

/* "<x.x>" float multiplier -> Q8 (round(x * 256)), clamped to 32..2048. */
static bool parse_speed_q8(const char *s, uint16_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s || *end != '\0' || v < 0.0f || isnan(v) || isinf(v)) {
        return false;
    }
    long q8 = lroundf(v * 256.0f);
    if (q8 < 32) {
        q8 = 32;
    } else if (q8 > 2048) {
        q8 = 2048;
    }
    *out = (uint16_t)q8;
    return true;
}

/* --- registry helpers --- */

static bool fx_name_valid(const char *name)
{
    size_t count = 0;
    const char *const *names = led_fx_get_effect_list(&count);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void fx_print_names(void)
{
    size_t count = 0;
    const char *const *names = led_fx_get_effect_list(&count);
    printf("available effects:");
    for (size_t i = 0; i < count; i++) {
        printf(" %s", names[i]);
    }
    printf("\n");
}

static void log_stats(void)
{
    led_fx_stats_t st;
    led_fx_get_stats(&st);
    ESP_LOGI(TAG, "stats: frames=%lu dropped=%lu avg_us=%lu max_us=%lu est_mA=%lu",
             (unsigned long)st.frames, (unsigned long)st.dropped,
             (unsigned long)st.render_us_avg, (unsigned long)st.render_us_max,
             (unsigned long)led_fx_estimate_current_ma());
}

/* --- console command handlers --- */

static int cmd_fx(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: fx <name>\n");
        fx_print_names();
        return 1;
    }
    if (!fx_name_valid(argv[1])) {
        printf("unknown effect '%s'\n", argv[1]);
        fx_print_names();
        return 1;
    }
    esp_err_t err = led_fx_set_effect(argv[1]);
    if (err != ESP_OK) {
        printf("fx: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("effect set: %s\n", argv[1]);
    return 0;
}

static int cmd_oneshot(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: oneshot <name>\n");
        fx_print_names();
        return 1;
    }
    if (!fx_name_valid(argv[1])) {
        printf("unknown effect '%s'\n", argv[1]);
        fx_print_names();
        return 1;
    }
    esp_err_t err = led_fx_play_oneshot(argv[1]);
    if (err != ESP_OK) {
        printf("oneshot: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("oneshot started: %s\n", argv[1]);
    return 0;
}

static int cmd_color(int argc, char **argv)
{
    uint8_t r, g, b;
    if (argc != 4 || !parse_u8(argv[1], &r) || !parse_u8(argv[2], &g) ||
        !parse_u8(argv[3], &b)) {
        printf("usage: color <r> <g> <b>  (0-255 each)\n");
        return 1;
    }
    esp_err_t err = led_fx_set_color(r, g, b);
    if (err != ESP_OK) {
        printf("color: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("color set: r=%u g=%u b=%u\n", r, g, b);
    return 0;
}

static int cmd_bright(int argc, char **argv)
{
    uint8_t v;
    if (argc != 2 || !parse_u8(argv[1], &v)) {
        printf("usage: bright <0-255>\n");
        return 1;
    }
    esp_err_t err = led_fx_set_brightness(v);
    if (err != ESP_OK) {
        printf("bright: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("brightness set: %u\n", v);
    return 0;
}

static int cmd_speed(int argc, char **argv)
{
    uint16_t q8;
    if (argc != 2 || !parse_speed_q8(argv[1], &q8)) {
        printf("usage: speed <x.x>  (multiplier, e.g. 0.5 / 1 / 2; clamped 32..2048 q8)\n");
        return 1;
    }
    esp_err_t err = led_fx_set_speed(q8);
    if (err != ESP_OK) {
        printf("speed: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("speed set: %u q8 (%.3fx)\n", q8, (double)q8 / 256.0);
    return 0;
}

static int cmd_power(int argc, char **argv)
{
    uint8_t v;
    if (argc != 2 || !parse_u8(argv[1], &v) || (v != 0 && v != 1)) {
        printf("usage: power <0|1>\n");
        return 1;
    }
    esp_err_t err = led_fx_power(v == 1);
    if (err != ESP_OK) {
        printf("power: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("power %s (rail %s)\n", v == 1 ? "on" : "off",
           led_fx_is_powered() ? "on" : "off");
    return 0;
}

static int cmd_stats(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    led_fx_stats_t st;
    led_fx_get_stats(&st);
    printf("stats: frames=%lu dropped=%lu avg_us=%lu max_us=%lu\n",
           (unsigned long)st.frames, (unsigned long)st.dropped,
           (unsigned long)st.render_us_avg, (unsigned long)st.render_us_max);
    return 0;
}

static int cmd_current(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("current: est=%lu mA scale=%u%%\n",
           (unsigned long)led_fx_estimate_current_ma(), s_console_scale_pct);
    return 0;
}

static int cmd_scale(int argc, char **argv)
{
    uint8_t v;
    if (argc != 2 || !parse_u8(argv[1], &v) || v < 1) {
        printf("usage: scale <1-100>  (percent applied to the current estimate)\n");
        return 1;
    }
    esp_err_t err = led_fx_set_current_scale_pct(v);
    if (err != ESP_OK) {
        printf("scale: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    s_console_scale_pct = v;
    printf("scale set: %u%%\n", v);
    return 0;
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
    register_cmd("fx", "Select an effect by name", "<name>", cmd_fx);
    register_cmd("oneshot", "Play a one-shot effect once, then restore the previous one", "<name>", cmd_oneshot);
    register_cmd("color", "Set base effect color", "<r> <g> <b>", cmd_color);
    register_cmd("bright", "Set global brightness", "<0-255>", cmd_bright);
    register_cmd("speed", "Set effect speed multiplier", "<x.x>", cmd_speed);
    register_cmd("power", "Switch the LED rail", "<0|1>", cmd_power);
    register_cmd("stats", "Show rendering statistics", NULL, cmd_stats);
    register_cmd("current", "Estimate LED rail current", NULL, cmd_current);
    register_cmd("scale", "Set current-estimate scale percent", "<1-100>", cmd_scale);
}

/* --- background demo cycle --- */

/* Power on, then cycle every registry effect: 10 s slot each, stats logged at
 * the end of every slot. "scare" runs as a one-shot. */
static void fx_cycle_task(void *arg)
{
    (void)arg;
    esp_err_t err = led_fx_power(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cycle: power on failed (%s)", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        size_t count = 0;
        const char *const *names = led_fx_get_effect_list(&count);
        for (size_t i = 0; i < count; i++) {
            if (strcmp(names[i], "scare") == 0) {
                err = led_fx_play_oneshot(names[i]);
            } else {
                err = led_fx_set_effect(names[i]);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "cycle: %s failed (%s)", names[i], esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "cycle: %s", names[i]);
            vTaskDelay(pdMS_TO_TICKS(CYCLE_SLOT_MS));
            log_stats();
        }
    }
}

void app_main(void)
{
    esp_err_t err = led_fx_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_fx_init failed (%s)", esp_err_to_name(err));
        abort();
    }

    register_console_commands();

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "led>";
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    BaseType_t ok = xTaskCreate(fx_cycle_task, "fx_cycle", FX_CYCLE_STACK_SIZE,
                                NULL, FX_CYCLE_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create fx_cycle task");
        abort();
    }
}
