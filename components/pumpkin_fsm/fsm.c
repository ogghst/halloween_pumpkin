/* Target integration of the pumpkin_fsm component (ESP-IDF, Fase 3).
 *
 * One owner task drains a single event queue (radar presence, config
 * changes, user commands, timer polls) and forwards the pure-core
 * outputs to the hardware adapters: led_fx for the looks, config_store
 * for command-driven persistence. All decisions live in fsm_core.c;
 * this file only transports inputs in and outputs out.
 *
 * Wiring:
 * - esp_event (default loop): the PRESENCE_EVENT and CONFIG_EVENT
 *   handlers run in the esp_event task and only enqueue; every FSM
 *   computation happens in the fsm task.
 * - esp_timer one-shot posts FSM_IN_POLL when the core deadline
 *   expires; after every processed input the task re-arms (or cancels)
 *   it, so at most one deadline is armed at a time.
 * - State changes are published to a single aligned word (atomic on
 *   this target) for the lock-free pumpkin_fsm_get_state, and posted on
 *   FSM_EVENT for the rest of the system.
 */
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "config_store.h"
#include "led_fx.h"
#include "pumpkin_fsm.h"
#include "radar_ld2420.h"

#include "fsm_core.h"

ESP_EVENT_DEFINE_BASE(FSM_EVENT);

static const char *TAG = "fsm";

static QueueHandle_t s_queue;
static esp_timer_handle_t s_timer;
static fsm_core_t s_core;               /* owned by the fsm task after boot */
static volatile fsm_state_t s_published; /* owner-written, lock-free read */

/* --- small helpers --- */

static int64_t clock_now_us(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

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

static const char *input_name(fsm_input_type_t t)
{
    switch (t) {
    case FSM_IN_PRESENCE_ON:    return "presence_on";
    case FSM_IN_PRESENCE_OFF:   return "presence_off";
    case FSM_IN_POLL:           return "poll";
    case FSM_IN_CMD_MODE:       return "cmd_mode";
    case FSM_IN_CMD_LIGHT:      return "cmd_light";
    case FSM_IN_CMD_EFFECT:     return "cmd_effect";
    case FSM_IN_CMD_SCARE:      return "cmd_scare";
    case FSM_IN_CONFIG_CHANGED: return "config_changed";
    default:                    return "?";
    }
}

static bool input_is_command(fsm_input_type_t t)
{
    return t == FSM_IN_CMD_MODE || t == FSM_IN_CMD_LIGHT ||
           t == FSM_IN_CMD_EFFECT || t == FSM_IN_CMD_SCARE;
}

/* pumpkin_config_t -> fsm_config_t. Numeric fields are already clamped
 * by the store; anim names are NUL-safe copies. */
static void translate_config(const pumpkin_config_t *pc, fsm_config_t *fc)
{
    memset(fc, 0, sizeof(*fc));
    fc->trigger_duration_s = pc->trigger_duration_s;
    fc->cooldown_s = pc->cooldown_s;
    fc->auto_mode = pc->auto_mode;
    strncpy(fc->idle_anim, pc->idle_anim, sizeof(fc->idle_anim) - 1);
    fc->idle_anim[sizeof(fc->idle_anim) - 1] = '\0';
    strncpy(fc->trigger_anim, pc->trigger_anim, sizeof(fc->trigger_anim) - 1);
    fc->trigger_anim[sizeof(fc->trigger_anim) - 1] = '\0';
    fc->idle_brightness_pct = pc->idle_brightness_pct;
    fc->brightness_pct = pc->brightness_pct;
    fc->max_brightness_pct = pc->max_brightness_pct;
    fc->color_r = pc->color_r;
    fc->color_g = pc->color_g;
    fc->color_b = pc->color_b;
    fc->power_on = pc->power_on;
}

/* --- queue plumbing (any task, incl. the esp_event and timer tasks) --- */

static void post_input(const fsm_input_t *in)
{
    if (xQueueSend(s_queue, in, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full: %s dropped", input_name(in->type));
    }
}

static void timer_cb(void *arg)
{
    (void)arg;
    fsm_input_t in = { .type = FSM_IN_POLL };
    post_input(&in);
}

/* esp_event handlers: runs in the esp_event task, enqueue only. */

static void presence_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data; /* radar_event_data_t: the FSM needs the edge only */
    fsm_input_t in = { 0 };
    switch (id) {
    case RADAR_EVENT_PRESENCE_ON:
        in.type = FSM_IN_PRESENCE_ON;
        break;
    case RADAR_EVENT_PRESENCE_OFF:
        in.type = FSM_IN_PRESENCE_OFF;
        break;
    default:
        return; /* distance/fault events are not FSM inputs */
    }
    post_input(&in);
}

static void config_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data; /* the full config is re-read from the store */
    if (id != CONFIG_EVENT_CHANGED) {
        return;
    }
    pumpkin_config_t pc;
    esp_err_t err = config_store_get(&pc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_store_get failed (%s)", esp_err_to_name(err));
        return;
    }
    fsm_input_t in = { .type = FSM_IN_CONFIG_CHANGED };
    translate_config(&pc, &in.u.cfg);
    post_input(&in);
}

/* --- output forwarding (fsm task only) --- */

/* Looks go to led_fx. led_fx not being initialized yet is not fatal:
 * warn and go on, the next look replaces the missed one. */
static void forward_look(const fsm_output_t *out)
{
    esp_err_t err;
    if (out->do_power) {
        err = led_fx_power(out->power_on);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "led_fx_power(%d) failed (%s)",
                     out->power_on ? 1 : 0, esp_err_to_name(err));
        }
    }
    if (out->do_effect) {
        err = led_fx_set_effect(out->effect);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "led_fx_set_effect('%s') failed (%s)",
                     out->effect, esp_err_to_name(err));
        }
    }
    if (out->do_color) {
        err = led_fx_set_color(out->r, out->g, out->b);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "led_fx_set_color failed (%s)", esp_err_to_name(err));
        }
    }
    if (out->do_brightness) {
        err = led_fx_set_brightness(out->brightness_0_255);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "led_fx_set_brightness failed (%s)",
                     esp_err_to_name(err));
        }
    }
}

/* Core core-ids -> config_store param ids, then the writes. The setters
 * post CONFIG_EVENT_CHANGED back, but same-value writes are no-ops, so
 * the loop-back is idempotent. */
static config_param_id_t anim_param_id(fsm_persist_param_t p)
{
    return (p == FSM_PERSIST_TRIGGER_ANIM) ? CFG_PARAM_TRIGGER_ANIM
                                           : CFG_PARAM_IDLE_ANIM;
}

static config_param_id_t bright_param_id(fsm_persist_param_t p)
{
    return (p == FSM_PERSIST_BRIGHTNESS) ? CFG_PARAM_BRIGHTNESS_PCT
                                         : CFG_PARAM_IDLE_BRIGHTNESS_PCT;
}

static void forward_persist(const fsm_output_t *out)
{
    esp_err_t err;
    if (out->persist.anim) {
        err = config_store_set_anim(anim_param_id(out->persist.anim_param),
                                    out->persist.anim_name);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "persist anim '%s' failed (%s)",
                     out->persist.anim_name, esp_err_to_name(err));
        }
    }
    if (out->persist.bright) {
        err = config_store_set_pct(bright_param_id(out->persist.bright_param),
                                   out->persist.bright_pct);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "persist brightness %u%% failed (%s)",
                     out->persist.bright_pct, esp_err_to_name(err));
        }
    }
    if (out->persist.color) {
        err = config_store_set_color(out->persist.r, out->persist.g,
                                     out->persist.b);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "persist color failed (%s)", esp_err_to_name(err));
        }
    }
}

/* Rearm the single one-shot: timer_ms == 0 cancels (the core emits the
 * cancel explicitly so a stale deadline never survives a state change).
 * The stop is best-effort: ESP_ERR_NOT_FOUND just means it was unarmed. */
static void apply_timer(const fsm_output_t *out)
{
    if (!out->timer_valid) {
        return;
    }
    (void)esp_timer_stop(s_timer);
    if (out->timer_ms == 0) {
        return;
    }
    esp_err_t err = esp_timer_start_once(s_timer,
                                         (uint64_t)out->timer_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_once(%lu ms) failed (%s)",
                 (unsigned long)out->timer_ms, esp_err_to_name(err));
    }
}

static void publish_state(const fsm_output_t *out)
{
    if (!out->state_changed) {
        return;
    }
    s_published = out->new_state;
    fsm_state_change_t ev = {
        .state = out->new_state,
        .prev_state = out->prev_state,
    };
    esp_err_t err = esp_event_post(FSM_EVENT, FSM_EVENT_STATE_CHANGED,
                                   &ev, sizeof(ev), 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FSM_EVENT post failed (%s)", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "%lld ms: state %s -> %s",
             (long long)(esp_timer_get_time() / 1000),
             state_name(out->prev_state), state_name(out->new_state));
}

static void apply_output(const fsm_output_t *out)
{
    forward_look(out);
    forward_persist(out);
    apply_timer(out);
    publish_state(out);
}

/* --- owner task --- */

static void fsm_task(void *arg)
{
    (void)arg;
    for (;;) {
        fsm_input_t in;
        if (xQueueReceive(s_queue, &in, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        fsm_output_t out;
        fsm_core_process(&s_core, &in, &out); /* FSM_IN_POLL -> fsm_core_poll */
        if (input_is_command(in.type) && in.result != ESP_OK) {
            ESP_LOGW(TAG, "%s rejected (%s)", input_name(in.type),
                     esp_err_to_name(in.result));
        }
        apply_output(&out);
        /* Boot-policy bookkeeping: power_on follows the OFF boundary so a
         * reboot comes back OFF when the user left it OFF. Same-value
         * writes are no-ops inside the store (no event loop-back). */
        if (in.type == FSM_IN_CMD_MODE) {
            (void)config_store_set_bool(CFG_PARAM_POWER_ON,
                                        s_core.state != FSM_STATE_OFF);
        }
    }
}

/* --- public API --- */

fsm_state_t pumpkin_fsm_get_state(void)
{
    return s_published;
}

static esp_err_t post_cmd(fsm_input_t *in)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_queue, in, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t fsm_cmd_set_mode(fsm_mode_t mode)
{
    if (mode != FSM_MODE_AUTO && mode != FSM_MODE_MANUAL &&
        mode != FSM_MODE_OFF) {
        return ESP_ERR_INVALID_ARG;
    }
    fsm_input_t in = { .type = FSM_IN_CMD_MODE, .u.mode = mode };
    return post_cmd(&in);
}

esp_err_t fsm_cmd_set_light(uint8_t r, uint8_t g, uint8_t b,
                            uint8_t brightness_pct)
{
    fsm_input_t in = { .type = FSM_IN_CMD_LIGHT };
    in.u.light.r = r;
    in.u.light.g = g;
    in.u.light.b = b;
    in.u.light.brightness_pct = brightness_pct;
    return post_cmd(&in);
}

esp_err_t fsm_cmd_set_effect(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    fsm_input_t in = { .type = FSM_IN_CMD_EFFECT };
    strncpy(in.u.anim, name, sizeof(in.u.anim) - 1);
    in.u.anim[sizeof(in.u.anim) - 1] = '\0';
    return post_cmd(&in);
}

esp_err_t fsm_cmd_scare(void)
{
    fsm_input_t in = { .type = FSM_IN_CMD_SCARE };
    return post_cmd(&in);
}

esp_err_t pumpkin_fsm_init(void)
{
    if (s_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    pumpkin_config_t pc;
    esp_err_t err = config_store_get(&pc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_store_get failed (%s): config_store_init first",
                 esp_err_to_name(err));
        return err;
    }

    fsm_config_t cfg;
    translate_config(&pc, &cfg);
    const fsm_clock_t clock = {
        .ctx = NULL,
        .now_us = clock_now_us,
    };
    fsm_core_init(&s_core, &cfg, &clock);
    s_published = s_core.state;

    s_queue = xQueueCreate(CONFIG_PUMPKIN_FSM_QUEUE_LEN, sizeof(fsm_input_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_timer = NULL; /* esp_timer_create may not write it on failure */
    const esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .name = "fsm_timer",
    };
    err = esp_timer_create(&timer_args, &s_timer);
    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_event_handler_register(PRESENCE_EVENT, ESP_EVENT_ANY_ID,
                                     presence_handler, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_event_handler_register(CONFIG_EVENT, CONFIG_EVENT_CHANGED,
                                     config_handler, NULL);
    if (err != ESP_OK) {
        esp_event_handler_unregister(PRESENCE_EVENT, ESP_EVENT_ANY_ID,
                                     presence_handler);
        goto fail;
    }

    /* Boot presentation before the task exists: no input can ever be
     * processed against a half-booted core. The boot output carries no
     * timer and no state change (state was set by fsm_core_init). */
    fsm_output_t out;
    fsm_core_boot(&s_core, &out);
    apply_output(&out);

    if (xTaskCreate(fsm_task, "fsm", CONFIG_PUMPKIN_FSM_TASK_STACK_SIZE,
                    NULL, CONFIG_PUMPKIN_FSM_TASK_PRIO, NULL) != pdPASS) {
        esp_event_handler_unregister(PRESENCE_EVENT, ESP_EVENT_ANY_ID,
                                     presence_handler);
        esp_event_handler_unregister(CONFIG_EVENT, CONFIG_EVENT_CHANGED,
                                     config_handler);
        goto fail;
    }

    ESP_LOGI(TAG, "started (state=%s)", state_name(s_core.state));
    return ESP_OK;

fail:
    if (s_timer != NULL) {
        esp_timer_delete(s_timer);
        s_timer = NULL;
    }
    vQueueDelete(s_queue);
    s_queue = NULL;
    return (err != ESP_OK) ? err : ESP_ERR_NO_MEM;
}
