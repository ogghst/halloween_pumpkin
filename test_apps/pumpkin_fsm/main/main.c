/* Pumpkin FSM test app: bench console over the full Phase 3 chain
 * (config_store + pumpkin_fsm + led_effects + radar_ld2420, ESP32 classic).
 *
 * - Boots config_store (Kconfig defaults), the LED effects engine, the
 *   radar, then the FSM. CFG_PARAM_TRIGGER_MM is forwarded to the radar
 *   module at boot and on every change: config_store is the single
 *   source of truth.
 * - Logs CONFIG_EVENT and FSM_EVENT state changes with millisecond
 *   timestamps so transition timing can be checked on the bench.
 * - REPL commands: fsm, fsm_mode, fsm_scare, fsm_light, fsm_anim,
 *   cfg_get, cfg_set, cfg_flush.
 *
 * Log tags: components log under their own tags ("fsm", "radar", ...);
 * this app logs under "app".
 *
 * Command handlers parse argv with strtol and keep all state in static
 * storage: no per-command heap allocation.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_fx.h"
#include "nvs_flash.h"
#include "pumpkin_fsm.h"
#include "radar_ld2420.h"

static const char *TAG = "app";

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

static bool parse_pct(const char *s, uint8_t *out)
{
    return parse_u8(s, out) && *out <= 100u;
}

static bool parse_bool01(const char *s, bool *out)
{
    if (strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    if (strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    return false;
}

/* --- shared print helpers --- */

static const char *state_name(fsm_state_t s)
{
    switch (s) {
    case FSM_STATE_IDLE:      return "IDLE";
    case FSM_STATE_TRIGGERED: return "TRIGGERED";
    case FSM_STATE_COOLDOWN:  return "COOLDOWN";
    case FSM_STATE_MANUAL:    return "MANUAL";
    case FSM_STATE_OFF:       return "OFF";
    default:                  return "?";
    }
}

static const char *param_name(config_param_id_t id)
{
    switch (id) {
    case CFG_PARAM_TRIGGER_MM:         return "trigger_mm";
    case CFG_PARAM_COOLDOWN_S:         return "cooldown_s";
    case CFG_PARAM_TRIGGER_DURATION_S: return "duration_s";
    case CFG_PARAM_TRIGGER_ANIM:       return "trigger_anim";
    case CFG_PARAM_IDLE_ANIM:          return "idle_anim";
    case CFG_PARAM_IDLE_BRIGHTNESS_PCT: return "idle_bright";
    case CFG_PARAM_MAX_BRIGHTNESS_PCT: return "max_bright";
    case CFG_PARAM_AUTO_MODE:          return "auto";
    case CFG_PARAM_COLOR:              return "color";
    case CFG_PARAM_BRIGHTNESS_PCT:     return "bright";
    case CFG_PARAM_POWER_ON:           return "power_on";
    default:                           return "?";
    }
}

/* --- fsm_* console commands --- */

static int cmd_fsm_state(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fsm_state_t s = pumpkin_fsm_get_state();
    printf("state=%s (%d)\n", state_name(s), (int)s);
    return 0;
}

static int cmd_fsm_mode(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: fsm_mode <auto|manual|off>\n");
        return 1;
    }
    fsm_mode_t m;
    if (strcmp(argv[1], "auto") == 0) {
        m = FSM_MODE_AUTO;
    } else if (strcmp(argv[1], "manual") == 0) {
        m = FSM_MODE_MANUAL;
    } else if (strcmp(argv[1], "off") == 0) {
        m = FSM_MODE_OFF;
    } else {
        printf("usage: fsm_mode <auto|manual|off>\n");
        return 1;
    }
    esp_err_t err = fsm_cmd_set_mode(m);
    printf("fsm_mode %s: %s\n", argv[1],
           (err == ESP_OK) ? "queued" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

static int cmd_fsm_scare(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = fsm_cmd_scare();
    printf("fsm_scare: %s\n",
           (err == ESP_OK) ? "queued" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

static int cmd_fsm_light(int argc, char **argv)
{
    uint8_t r, g, b, pct;
    if (argc != 5 || !parse_u8(argv[1], &r) || !parse_u8(argv[2], &g) ||
        !parse_u8(argv[3], &b) || !parse_pct(argv[4], &pct)) {
        printf("usage: fsm_light <r> <g> <b> <pct> (0-255, pct 0-100)\n");
        return 1;
    }
    esp_err_t err = fsm_cmd_set_light(r, g, b, pct);
    printf("fsm_light r=%u g=%u b=%u pct=%u: %s\n", r, g, b, pct,
           (err == ESP_OK) ? "queued" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

static int cmd_fsm_anim(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: fsm_anim <nome> (registry led_effects)\n");
        return 1;
    }
    esp_err_t err = fsm_cmd_set_effect(argv[1]);
    printf("fsm_anim %s: %s\n", argv[1],
           (err == ESP_OK) ? "queued" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* --- cfg_* console commands --- */

static int cmd_cfg_get(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pumpkin_config_t c;
    esp_err_t err = config_store_get(&c);
    if (err != ESP_OK) {
        printf("cfg_get: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("version=%u\n", c.version);
    printf("trigger_mm=%u cooldown_s=%u duration_s=%u\n",
           c.trigger_mm, c.cooldown_s, c.trigger_duration_s);
    printf("trigger_anim='%s' idle_anim='%s'\n", c.trigger_anim, c.idle_anim);
    printf("idle_bright=%u%% max_bright=%u%% bright=%u%%\n",
           c.idle_brightness_pct, c.max_brightness_pct, c.brightness_pct);
    printf("auto=%u power_on=%u color=#%02x%02x%02x\n",
           (unsigned)c.auto_mode, (unsigned)c.power_on,
           c.color_r, c.color_g, c.color_b);
    return 0;
}

static int cfg_set_u16(config_param_id_t id, const char *name,
                       const char *value)
{
    uint16_t v;
    if (!parse_u16(value, &v)) {
        printf("usage: cfg_set %s <0-65535>\n", name);
        return 1;
    }
    esp_err_t err = config_store_set_u16(id, v);
    printf("cfg_set %s=%u: %s\n", name, v,
           (err == ESP_OK) ? "ok" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

static int cfg_set_pct(config_param_id_t id, const char *name,
                       const char *value)
{
    uint8_t v;
    if (!parse_pct(value, &v)) {
        printf("usage: cfg_set %s <0-100>\n", name);
        return 1;
    }
    esp_err_t err = config_store_set_pct(id, v);
    printf("cfg_set %s=%u: %s\n", name, v,
           (err == ESP_OK) ? "ok" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

static int cfg_set_anim(config_param_id_t id, const char *name,
                        const char *value)
{
    esp_err_t err = config_store_set_anim(id, value);
    printf("cfg_set %s='%s': %s\n", name, value,
           (err == ESP_OK) ? "ok" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

static void cfg_set_print_usage(void)
{
    printf("usage: cfg_set <sub> <value> [...]\n");
    printf("  trigger_mm <mm> | cooldown_s <s> | duration_s <s>\n");
    printf("  idle_bright <pct> | max_bright <pct> | bright <pct>\n");
    printf("  auto <0|1>\n");
    printf("  color <r> <g> <b>\n");
    printf("  trigger_anim <nome> | idle_anim <nome>\n");
    printf("  power_on <0|1>\n");
}

static int cmd_cfg_set(int argc, char **argv)
{
    if (argc < 3) {
        cfg_set_print_usage();
        return 1;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "trigger_mm") == 0) {
        return cfg_set_u16(CFG_PARAM_TRIGGER_MM, "trigger_mm", argv[2]);
    }
    if (strcmp(sub, "cooldown_s") == 0) {
        return cfg_set_u16(CFG_PARAM_COOLDOWN_S, "cooldown_s", argv[2]);
    }
    if (strcmp(sub, "duration_s") == 0) {
        return cfg_set_u16(CFG_PARAM_TRIGGER_DURATION_S, "duration_s", argv[2]);
    }
    if (strcmp(sub, "idle_bright") == 0) {
        return cfg_set_pct(CFG_PARAM_IDLE_BRIGHTNESS_PCT, "idle_bright", argv[2]);
    }
    if (strcmp(sub, "max_bright") == 0) {
        return cfg_set_pct(CFG_PARAM_MAX_BRIGHTNESS_PCT, "max_bright", argv[2]);
    }
    if (strcmp(sub, "bright") == 0) {
        return cfg_set_pct(CFG_PARAM_BRIGHTNESS_PCT, "bright", argv[2]);
    }
    if (strcmp(sub, "auto") == 0) {
        bool v;
        if (argc != 3 || !parse_bool01(argv[2], &v)) {
            printf("usage: cfg_set auto <0|1>\n");
            return 1;
        }
        esp_err_t err = config_store_set_bool(CFG_PARAM_AUTO_MODE, v);
        printf("cfg_set auto=%u: %s\n", (unsigned)v,
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (strcmp(sub, "power_on") == 0) {
        bool v;
        if (argc != 3 || !parse_bool01(argv[2], &v)) {
            printf("usage: cfg_set power_on <0|1>\n");
            return 1;
        }
        esp_err_t err = config_store_set_bool(CFG_PARAM_POWER_ON, v);
        printf("cfg_set power_on=%u: %s\n", (unsigned)v,
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (strcmp(sub, "color") == 0) {
        uint8_t r, g, b;
        if (argc != 5 || !parse_u8(argv[2], &r) || !parse_u8(argv[3], &g) ||
            !parse_u8(argv[4], &b)) {
            printf("usage: cfg_set color <r> <g> <b> (0-255)\n");
            return 1;
        }
        esp_err_t err = config_store_set_color(r, g, b);
        printf("cfg_set color=#%02x%02x%02x: %s\n", r, g, b,
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (strcmp(sub, "trigger_anim") == 0) {
        return cfg_set_anim(CFG_PARAM_TRIGGER_ANIM, "trigger_anim", argv[2]);
    }
    if (strcmp(sub, "idle_anim") == 0) {
        return cfg_set_anim(CFG_PARAM_IDLE_ANIM, "idle_anim", argv[2]);
    }
    printf("unknown subcommand '%s'\n", sub);
    cfg_set_print_usage();
    return 1;
}

static int cmd_cfg_flush(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = config_store_flush();
    printf("cfg_flush: %s\n",
           (err == ESP_OK) ? "ok" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* --- console registration --- */

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
    register_cmd("fsm", "Current FSM state", NULL, cmd_fsm_state);
    register_cmd("fsm_mode", "Target FSM mode", "<auto|manual|off>",
                 cmd_fsm_mode);
    register_cmd("fsm_scare", "Play the trigger look once", NULL,
                 cmd_fsm_scare);
    register_cmd("fsm_light", "Set the light (manual or current-state look)",
                 "<r> <g> <b> <pct>", cmd_fsm_light);
    register_cmd("fsm_anim", "Set the effect by registry name", "<nome>",
                 cmd_fsm_anim);
    register_cmd("cfg_get", "Dump the persisted configuration", NULL,
                 cmd_cfg_get);
    register_cmd("cfg_set", "Change a configuration parameter",
                 "<sub> <value> [...]", cmd_cfg_set);
    register_cmd("cfg_flush", "Commit pending configuration to NVS now", NULL,
                 cmd_cfg_flush);
}

/* --- event logging --- */

static void fsm_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    if (id != FSM_EVENT_STATE_CHANGED || event_data == NULL) {
        return;
    }
    const fsm_state_change_t *sc = event_data;
    ESP_LOGI(TAG, "%lld ms: fsm %s -> %s",
             (long long)(esp_timer_get_time() / 1000),
             state_name(sc->prev_state), state_name(sc->state));
}

/* Config source of truth: forward the trigger distance to the radar
 * module whenever the store changes it, and log every change. */
static void cfg_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    if (id != CONFIG_EVENT_CHANGED || event_data == NULL) {
        return;
    }
    const config_change_t *ch = event_data;
    if (ch->id == CFG_PARAM_TRIGGER_MM) {
        pumpkin_config_t c;
        if (config_store_get(&c) == ESP_OK) {
            esp_err_t err = radar_set_trigger_mm(c.trigger_mm);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "radar_set_trigger_mm failed (%s)",
                         esp_err_to_name(err));
            }
        }
    }
    ESP_LOGI(TAG, "%lld ms: cfg %s changed",
             (long long)(esp_timer_get_time() / 1000), param_name(ch->id));
}

/* --- boot --- */

/* Kconfig-backed defaults, used by config_store_init when nothing (or an
 * unparsable blob) is stored; the store sanitizes a copy of them. */
static pumpkin_config_t build_defaults(void)
{
    pumpkin_config_t d = {
        .version = PUMPKIN_CONFIG_VERSION,
        .trigger_mm = CONFIG_PUMPKIN_TRIGGER_MM,
        .cooldown_s = CONFIG_PUMPKIN_COOLDOWN_S,
        .trigger_duration_s = CONFIG_PUMPKIN_TRIGGER_DURATION_S,
        .idle_brightness_pct = CONFIG_PUMPKIN_IDLE_BRIGHTNESS_PCT,
        .max_brightness_pct = CONFIG_PUMPKIN_MAX_BRIGHTNESS_PCT,
        .auto_mode = CONFIG_PUMPKIN_AUTO_MODE,
        .color_r = CONFIG_PUMPKIN_COLOR_R,
        .color_g = CONFIG_PUMPKIN_COLOR_G,
        .color_b = CONFIG_PUMPKIN_COLOR_B,
        .brightness_pct = CONFIG_PUMPKIN_BRIGHTNESS_PCT,
        .power_on = true, /* boot policy overridden by the stored value */
    };
    strncpy(d.trigger_anim, CONFIG_PUMPKIN_TRIGGER_ANIM,
            sizeof(d.trigger_anim) - 1);
    d.trigger_anim[sizeof(d.trigger_anim) - 1] = '\0';
    strncpy(d.idle_anim, CONFIG_PUMPKIN_IDLE_ANIM, sizeof(d.idle_anim) - 1);
    d.idle_anim[sizeof(d.idle_anim) - 1] = '\0';
    return d;
}

void app_main(void)
{
    /* NVS must be up before config_store loads the persisted blob (like
     * the radar app: erase + retry on layout/new-version errors). */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    pumpkin_config_t defaults = build_defaults();
    esp_err_t err = config_store_init(&defaults);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_store_init failed (%s)", esp_err_to_name(err));
        abort();
    }

    err = led_fx_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_fx_init failed (%s)", esp_err_to_name(err));
        abort();
    }

    err = radar_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radar_init failed (%s)", esp_err_to_name(err));
        abort();
    }

    err = pumpkin_fsm_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pumpkin_fsm_init failed (%s)", esp_err_to_name(err));
        abort();
    }

    ESP_ERROR_CHECK(esp_event_handler_register(CONFIG_EVENT,
                                               CONFIG_EVENT_CHANGED,
                                               cfg_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(FSM_EVENT,
                                               FSM_EVENT_STATE_CHANGED,
                                               fsm_event_handler, NULL));

    /* Initial sync of the trigger distance (store -> radar module). */
    pumpkin_config_t cfg;
    ESP_ERROR_CHECK(config_store_get(&cfg));
    err = radar_set_trigger_mm(cfg.trigger_mm);
    ESP_LOGI(TAG, "radar trigger synced to %u mm (%s)", cfg.trigger_mm,
             esp_err_to_name(err));

    register_console_commands();

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "fsm>";
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
