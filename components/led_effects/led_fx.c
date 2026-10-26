/* LED effects engine (pumpkin firmware, Phase 1).
 *
 * Owns the anim task (fixed rate CONFIG_LED_FX_FPS), the command queue
 * (depth 8), the LED rail ON/OFF sequencing and the output stage
 * (gamma -> brightness -> max percent LUT). Renderers in fx_effects.c
 * produce LINEAR colors; this file maps them onto the strip through the
 * led_strip v3 RMT backend.
 *
 * All delays live in the anim task only. No dynamic allocation happens in
 * the render loop: frame buffer, white buffer and LUT are static.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "led_fx.h"
#include "fx_color.h"
#include "fx_current.h"
#include "fx_effects.h"
#include "fx_overlay.h"
#include "fx_types.h"

static const char *TAG = "led";

/* Color component format from Kconfig (component order + RGB vs RGBW).
 * Preprocessor guards, not runtime ternaries: Kconfig bools that are off are
 * simply absent from the generated sdkconfig.h, so an undefined identifier
 * would break the build. #if treats undefined as 0. */
#if CONFIG_LED_FX_ORDER_RGB
#if CONFIG_LED_FX_RGBW
#define FX_COLOR_FORMAT LED_STRIP_COLOR_COMPONENT_FMT_RGBW
#else
#define FX_COLOR_FORMAT LED_STRIP_COLOR_COMPONENT_FMT_RGB
#endif
#else
#if CONFIG_LED_FX_RGBW
#define FX_COLOR_FORMAT LED_STRIP_COLOR_COMPONENT_FMT_GRBW
#else
#define FX_COLOR_FORMAT LED_STRIP_COLOR_COMPONENT_FMT_GRB
#endif
#endif

#define FX_FRAME_PERIOD_US (1000000U / CONFIG_LED_FX_FPS)
#define FX_QUEUE_DEPTH     8
#define FX_TASK_STACK      4096
#define FX_TASK_PRIO       5
#define FX_MAX_EFFECTS     16

/* Command carried by the queue: multiple producers (API/console), single
 * consumer (anim task). */
typedef struct {
    uint8_t type;
    uint8_t idx;
    uint8_t r, g, b, brightness;
    uint16_t speed;
} led_fx_cmd_t;

enum { CMD_SET_EFFECT, CMD_ONESHOT, CMD_COLOR, CMD_BRIGHTNESS, CMD_SPEED, CMD_POWER,
       CMD_PIXEL_TEST, CMD_PIXEL_TEST_OFF, CMD_OVERLAY };

static led_strip_handle_t s_strip;
static QueueHandle_t s_q;
static TaskHandle_t s_task;
static bool s_powered;          /* written only by the anim task */
static bool s_power_req;        /* requested rail state, from CMD_POWER */
static uint8_t s_effect;        /* index into the effect registry */
static uint8_t s_prev;          /* effect to restore after a oneshot */
static bool s_oneshot_active;
static int64_t s_oneshot_start; /* [us, esp_timer] */
static int64_t s_effect_start;  /* [us, esp_timer] */
static fx_rgb_t s_color = { 255, 170, 80 };
static uint8_t s_brightness = 255;
static uint16_t s_speed = LED_FX_SPEED_ONE;
static uint8_t s_scale_pct = 100;
static bool s_pixel_test;         /* pixel-test hold: display override */
static uint8_t s_pixel_test_idx;  /* strip index of the held pixel */
static fx_rgb_t s_pixel_test_color;
static led_fx_overlay_t s_overlay;      /* status overlay, written by anim task */
static uint32_t s_overlay_start_ms;     /* anim-task ms clock at activation */
static fx_rgb_t s_frame[CONFIG_LED_FX_NUM_PIXELS];
#if CONFIG_LED_FX_RGBW
static uint8_t s_white[CONFIG_LED_FX_NUM_PIXELS];
#endif
static uint8_t s_lut[256];
static led_fx_stats_t s_stats;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static fx_params_t s_params = { { 255, 170, 80 }, LED_FX_SPEED_ONE };

/* Registry name snapshot, filled once in led_fx_init for MQTT/HA use. */
static const char *s_names[FX_MAX_EFFECTS];
static size_t s_name_count;

#if CONFIG_LED_FX_RGBW
static uint8_t fx_min3(uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t m = (a < b) ? a : b;
    return (m < c) ? m : c;
}
#endif

/* Plain output driven low; level is set before enabling the direction so no
 * glitch can reach the rail/data line during configuration. */
static esp_err_t fx_gpio_output_low(int gpio_num)
{
    esp_err_t err = gpio_set_level(gpio_num, 0);
    if (err != ESP_OK)
        return err;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

/* Apply one dequeued command. Called from the anim task only. */
static void fx_apply_cmd(const led_fx_cmd_t *c)
{
    int64_t now = esp_timer_get_time();
    switch (c->type) {
    case CMD_SET_EFFECT:
        s_oneshot_active = false;
        s_effect = c->idx;
        s_effect_start = now;
        break;
    case CMD_ONESHOT:
        s_prev = s_oneshot_active ? s_prev : s_effect;
        s_effect = c->idx;
        s_oneshot_active = true;
        s_oneshot_start = now;
        break;
    case CMD_COLOR:
        s_color.r = c->r;
        s_color.g = c->g;
        s_color.b = c->b;
        s_params.color = s_color;
        break;
    case CMD_BRIGHTNESS:
        s_brightness = c->brightness;
        fx_output_lut_build(s_lut, s_brightness, CONFIG_LED_FX_MAX_BRIGHTNESS_PCT);
        break;
    case CMD_SPEED:
        s_speed = c->speed;
        s_params.speed_q8 = s_speed;
        break;
    case CMD_POWER:
        s_power_req = (c->idx != 0);
        break;
    case CMD_PIXEL_TEST:
        s_pixel_test = true;
        s_pixel_test_idx = c->idx;
        s_pixel_test_color.r = c->r;
        s_pixel_test_color.g = c->g;
        s_pixel_test_color.b = c->b;
        break;
    case CMD_PIXEL_TEST_OFF:
        s_pixel_test = false;
        break;
    case CMD_OVERLAY:
        /* Same-value is a no-op so the pattern phase is not restarted. */
        if ((led_fx_overlay_t)c->idx != s_overlay) {
            portENTER_CRITICAL(&s_lock);
            s_overlay = (led_fx_overlay_t)c->idx;
            s_overlay_start_ms = (uint32_t)(now / 1000);
            portEXIT_CRITICAL(&s_lock);
        }
        break;
    default:
        break;
    }
}

static void anim_task(void *arg)
{
    (void)arg;
    /* Portable us-to-tick conversion: pdUS_TO_TICKS is not provided by every
     * FreeRTOS configuration. */
    const TickType_t period =
        (TickType_t)(((uint64_t)FX_FRAME_PERIOD_US * configTICK_RATE_HZ) / 1000000ULL);
    TickType_t last_wake = xTaskGetTickCount();

    size_t reg_count = 0;
    const fx_effect_t *reg = fx_effect_registry(&reg_count);
    led_fx_cmd_t cmd;

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        /* 1. Drain the command queue. */
        while (xQueueReceive(s_q, &cmd, 0) == pdTRUE)
            fx_apply_cmd(&cmd);

        /* 2. Rail transitions (blocking delays allowed here only). */
        if (s_power_req && !s_powered) {
            gpio_set_level(CONFIG_LED_FX_EN_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(20)); /* rail stabilization */
            s_powered = true;              /* first frame goes out below */
            ESP_LOGI(TAG, "rail on");
        } else if (!s_power_req && s_powered) {
            led_strip_clear(s_strip);
            led_strip_refresh(s_strip); /* black frame out before cutting power */
            gpio_set_level(CONFIG_LED_FX_EN_GPIO, 0);
            s_powered = false;
            ESP_LOGI(TAG, "rail off"); /* RMT idle low keeps DIN low while unpowered */
        }

        /* 3. Rail off: nothing to render. */
        if (!s_powered)
            continue;

        /* 4. Effect clock and oneshot restore. */
        int64_t now = esp_timer_get_time();
        int64_t t_eff = s_oneshot_active ? (now - s_oneshot_start)
                                         : (now - s_effect_start);
        if (s_oneshot_active && t_eff >= (int64_t)reg[s_effect].duration_us) {
            ESP_LOGI(TAG, "oneshot '%s' done, restoring '%s'",
                     reg[s_effect].name, reg[s_prev].name);
            s_effect = s_prev;
            s_oneshot_active = false;
            s_effect_start = now;
            t_eff = 0;
        }

        /* 4b. Status overlay: ms clock relative to its activation and
         * auto-expiry (only NET_UP expires, back to NONE). */
        uint8_t ov_rgb[3];
        led_fx_overlay_t ov;
        uint32_t ov_tick;
        portENTER_CRITICAL(&s_lock);
        ov = s_overlay;
        ov_tick = (uint32_t)(now / 1000) - s_overlay_start_ms;
        portEXIT_CRITICAL(&s_lock);
        if (led_overlay_expired(ov_tick, ov)) {
            portENTER_CRITICAL(&s_lock);
            s_overlay = LED_FX_OVERLAY_NONE;
            portEXIT_CRITICAL(&s_lock);
            ov = LED_FX_OVERLAY_NONE;
            ESP_LOGI(TAG, "overlay net_up done");
        }
        int ov_on = ov != LED_FX_OVERLAY_NONE && !s_pixel_test &&
                    led_overlay_color_at(ov_tick, ov, ov_rgb) != 0;

        /* 5. Render + output stage + flush, measured. Pixel-test hold
         * displays a black frame with one lit pixel instead of the effect;
         * the effect clock keeps running, so on release the effect resumes
         * with no timer reset. */
        int64_t t0 = esp_timer_get_time();
        if (s_pixel_test) {
            memset(s_frame, 0, sizeof(s_frame));
            if (s_pixel_test_idx < CONFIG_LED_FX_NUM_PIXELS)
                s_frame[s_pixel_test_idx] = s_pixel_test_color;
        } else {
            reg[s_effect].render(s_frame, CONFIG_LED_FX_NUM_PIXELS, &s_params,
                                 (uint64_t)t_eff);
        }
        for (int i = 0; i < CONFIG_LED_FX_NUM_PIXELS; i++) {
            const fx_rgb_t *f = &s_frame[i];
            uint8_t r = s_lut[f->r];
            uint8_t g = s_lut[f->g];
            uint8_t b = s_lut[f->b];
            if (ov_on) {
                /* Overlay fused after gamma/brightness (max per channel),
                 * visible even at low brightness; white channel untouched. */
                uint8_t px[3] = { r, g, b };
                led_overlay_blend(px, ov_rgb);
                r = px[0];
                g = px[1];
                b = px[2];
            }
#if CONFIG_LED_FX_RGBW
            uint8_t w_lin = fx_min3(f->r, f->g, f->b);
            led_strip_set_pixel_rgbw(s_strip, (uint32_t)i, r, g, b, s_lut[w_lin]);
            s_white[i] = w_lin;
#else
            led_strip_set_pixel(s_strip, (uint32_t)i, r, g, b);
#endif
        }
        led_strip_refresh(s_strip);
        int32_t dt = (int32_t)(esp_timer_get_time() - t0);

        /* 6. Stats. */
        s_stats.frames++;
        s_stats.render_us_avg = (s_stats.render_us_avg * 7u + (uint32_t)dt) / 8u;
        if ((uint32_t)dt > s_stats.render_us_max)
            s_stats.render_us_max = (uint32_t)dt;
        if (dt > (int32_t)FX_FRAME_PERIOD_US)
            s_stats.dropped++;
    }
}

/* Send with no block: the anim task drains the queue every frame, so a full
 * queue means the caller is too far ahead and must retry. */
static esp_err_t fx_send_cmd(const led_fx_cmd_t *cmd)
{
    if (s_q == NULL)
        return ESP_ERR_INVALID_STATE;
    return (xQueueSend(s_q, cmd, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t led_fx_init(void)
{
    if (s_task != NULL || s_strip != NULL)
        return ESP_ERR_INVALID_STATE;

    /* 1-2. Both lines low before the strip driver takes over the data pin:
            rail off, DIN idle low (no parasitic powering through data). */
    esp_err_t err = fx_gpio_output_low(CONFIG_LED_FX_EN_GPIO);
    if (err != ESP_OK)
        return err;
    err = fx_gpio_output_low(CONFIG_LED_FX_DATA_GPIO);
    if (err != ESP_OK)
        return err;

    /* 3. Time-seeded noise and initial output LUT; effect clocks start now. */
    fx_noise_seed((uint32_t)esp_timer_get_time());
    fx_output_lut_build(s_lut, s_brightness, CONFIG_LED_FX_MAX_BRIGHTNESS_PCT);
    s_effect_start = esp_timer_get_time();
    s_oneshot_start = s_effect_start;

    /* Name snapshot for led_fx_get_effect_list. */
    size_t reg_count = 0;
    const fx_effect_t *reg = fx_effect_registry(&reg_count);
    s_name_count = (reg_count < FX_MAX_EFFECTS) ? reg_count : FX_MAX_EFFECTS;
    for (size_t i = 0; i < s_name_count; i++)
        s_names[i] = reg[i].name;

    /* 4. Strip over RMT. */
    led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_LED_FX_DATA_GPIO,
        .max_leds = CONFIG_LED_FX_NUM_PIXELS,
#if CONFIG_LED_FX_RGBW
        .led_model = LED_MODEL_SK6812,
#else
        .led_model = LED_MODEL_WS2812,
#endif
        .color_component_format = FX_COLOR_FORMAT,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    err = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "strip create failed: %d", err);
        return err;
    }

    /* 5. Queue first (APIs may enqueue as soon as it exists), then the task. */
    s_q = xQueueCreate(FX_QUEUE_DEPTH, sizeof(led_fx_cmd_t));
    if (s_q == NULL) {
        led_strip_del(s_strip);
        s_strip = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(anim_task, "anim", FX_TASK_STACK, NULL, FX_TASK_PRIO,
                    &s_task) != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        led_strip_del(s_strip);
        s_strip = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init: %d pixels on GPIO%d (en GPIO%d), %d fps",
             CONFIG_LED_FX_NUM_PIXELS, CONFIG_LED_FX_DATA_GPIO,
             CONFIG_LED_FX_EN_GPIO, CONFIG_LED_FX_FPS);
    return ESP_OK;
}

esp_err_t led_fx_power(bool on)
{
    led_fx_cmd_t cmd = { .type = CMD_POWER, .idx = on ? 1u : 0u };
    return fx_send_cmd(&cmd);
}

bool led_fx_is_powered(void)
{
    return s_powered;
}

esp_err_t led_fx_set_effect(const char *name)
{
    const fx_effect_t *fx = fx_effect_find(name);
    if (fx == NULL)
        return ESP_ERR_INVALID_ARG;
    size_t reg_count = 0;
    const fx_effect_t *reg = fx_effect_registry(&reg_count);
    led_fx_cmd_t cmd = { .type = CMD_SET_EFFECT, .idx = (uint8_t)(fx - reg) };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    led_fx_cmd_t cmd = { .type = CMD_COLOR, .r = r, .g = g, .b = b };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_set_brightness(uint8_t brightness)
{
    led_fx_cmd_t cmd = { .type = CMD_BRIGHTNESS, .brightness = brightness };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_set_speed(uint16_t speed_q8)
{
    if (speed_q8 == 0)
        return ESP_ERR_INVALID_ARG; /* zero would divide by zero in renderers */
    led_fx_cmd_t cmd = { .type = CMD_SPEED, .speed = speed_q8 };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_play_oneshot(const char *name)
{
    const fx_effect_t *fx = fx_effect_find(name);
    if (fx == NULL)
        return ESP_ERR_INVALID_ARG;
    size_t reg_count = 0;
    const fx_effect_t *reg = fx_effect_registry(&reg_count);
    led_fx_cmd_t cmd = { .type = CMD_ONESHOT, .idx = (uint8_t)(fx - reg) };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_pixel_test(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    int idx = led_fx_xy_index(x, y);
    if (idx < 0)
        return ESP_ERR_INVALID_ARG;
    led_fx_cmd_t cmd = { .type = CMD_PIXEL_TEST, .idx = (uint8_t)idx,
                         .r = r, .g = g, .b = b };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_pixel_test_off(void)
{
    led_fx_cmd_t cmd = { .type = CMD_PIXEL_TEST_OFF };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_overlay_set(led_fx_overlay_t ov)
{
    if ((unsigned)ov > (unsigned)LED_FX_OVERLAY_NET_DOWN)
        return ESP_ERR_INVALID_ARG;
    led_fx_cmd_t cmd = { .type = CMD_OVERLAY, .idx = (uint8_t)ov };
    return fx_send_cmd(&cmd);
}

led_fx_overlay_t led_fx_overlay_get(void)
{
    portENTER_CRITICAL(&s_lock);
    led_fx_overlay_t cur = s_overlay;
    portEXIT_CRITICAL(&s_lock);
    return cur;
}

const char *const *led_fx_get_effect_list(size_t *out_count)
{
    if (out_count != NULL)
        *out_count = s_name_count;
    return (const char *const *)s_names;
}

void led_fx_get_stats(led_fx_stats_t *out)
{
    if (out == NULL)
        return;
    portENTER_CRITICAL(&s_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t led_fx_estimate_current_ma(void)
{
    fx_rgb_t frame[CONFIG_LED_FX_NUM_PIXELS];
#if CONFIG_LED_FX_RGBW
    uint8_t white[CONFIG_LED_FX_NUM_PIXELS];
#endif
    portENTER_CRITICAL(&s_lock);
    memcpy(frame, s_frame, sizeof(frame));
#if CONFIG_LED_FX_RGBW
    memcpy(white, s_white, sizeof(white));
#endif
    uint8_t scale = s_scale_pct;
    portEXIT_CRITICAL(&s_lock);
#if CONFIG_LED_FX_RGBW
    return fx_estimate_current_ma(frame, white, CONFIG_LED_FX_NUM_PIXELS, scale);
#else
    return fx_estimate_current_ma(frame, NULL, CONFIG_LED_FX_NUM_PIXELS, scale);
#endif
}

esp_err_t led_fx_set_current_scale_pct(uint8_t pct)
{
    if (pct < 1 || pct > 100)
        return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    s_scale_pct = pct;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
