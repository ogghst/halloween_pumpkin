/* LED effects engine (pumpkin firmware, Phase 1 + Fase 8 eyes).
 *
 * Owns the anim task (fixed rate CONFIG_LED_FX_FPS), the command queue
 * (depth 12), the LED rail ON/OFF sequencing and the output stage
 * (gamma -> brightness -> max percent LUT, separate LUT for the eyes zone).
 * Renderers in fx_effects.c produce LINEAR colors for the body zone
 * [0, CONFIG_LED_FX_EYES_START); fx_eyes.c renders the 2 eyes in tail.
 * This file maps them onto the strip through the led_strip v3 RMT backend.
 *
 * All delays live in the anim task only. No dynamic allocation happens in
 * the render loop: frame buffer, white buffer and LUT are static.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "led_fx.h"
#include "fx_color.h"
#include "fx_current.h"
#include "fx_effects.h"
#include "fx_eyes.h"
#include "fx_overlay.h"
#include "fx_types.h"

static const char *TAG = "led";

ESP_EVENT_DEFINE_BASE(LED_FX_EVENT);

/* Zona occhi: la config deve rappresentare 2 pixel interamente in coda alla
 * catena. L'unica eccezione gestita a runtime (log W in init, zona
 * disattivata) e' la catena che coincide esattamente con la zona richiesta
 * (LED_FX_NUM_PIXELS == LED_FX_EYES_START, app legacy a 64 px): ogni altra
 * combinazione incoerente non compila. */
#if CONFIG_LED_FX_EYES_START + 2 > CONFIG_LED_FX_NUM_PIXELS
#if CONFIG_LED_FX_EYES_START != CONFIG_LED_FX_NUM_PIXELS
#error "LED_FX_EYES_START + 2 eccede LED_FX_NUM_PIXELS: zona occhi non rappresentabile"
#endif
#endif

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
#define FX_QUEUE_DEPTH     12
#define FX_TASK_STACK      4096
#define FX_TASK_PRIO       5
#define FX_MAX_EFFECTS     16

/* Command carried by the queue: multiple producers (API/console), single
 * consumer (anim task). Il campo plan serve solo ai comandi occhi
 * (CMD_EYES_*): la coda cresce, va bene, nessuna allocazione dinamica. */
typedef struct {
    uint8_t type;
    uint8_t idx;
    uint8_t r, g, b, brightness;
    uint16_t speed;
    led_fx_eyes_plan_t plan;
} led_fx_cmd_t;

enum { CMD_SET_EFFECT, CMD_ONESHOT, CMD_COLOR, CMD_BRIGHTNESS, CMD_SPEED, CMD_POWER,
       CMD_PIXEL_TEST, CMD_PIXEL_TEST_OFF, CMD_OVERLAY,
       CMD_EYES_EFFECT, CMD_EYES_COLOR, CMD_EYES_BRIGHTNESS, CMD_EYES_ONESHOT,
       CMD_EYES_FOLLOW, CMD_EYES_BLINK_OVERLAY, CMD_EYES_PLAN };

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

/* Zona occhi: LUT di uscita dedicata (brightness occhi != brightness corpo)
 * e cache della luminosita' con cui e' stata costruita. s_eyes_ok vale
 * false nelle app la cui catena non ha i 2 pixel occhi in coda. */
static bool s_eyes_ok;
static uint8_t s_eyes_lut[256];
static uint8_t s_eyes_lut_src;

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

/* Evento occhi sul loop di default, dopo l'applicazione di qualsiasi
 * comando occhi (payload: copia dello stato corrente). Contesto task:
 * esp_event_post e' consentito. */
static void fx_eyes_notify(void)
{
    led_fx_eyes_look_t look;
    if (fx_eyes_get_look(&look) == ESP_OK)
        esp_event_post(LED_FX_EVENT, LED_FX_EVENT_EYES, &look, sizeof(look), 0);
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
    case CMD_EYES_EFFECT:
        fx_eyes_set_effect_idx(c->idx);
        fx_eyes_notify();
        break;
    case CMD_EYES_COLOR:
        fx_eyes_set_color(c->r, c->g, c->b);
        fx_eyes_notify();
        break;
    case CMD_EYES_BRIGHTNESS:
        fx_eyes_set_brightness(c->brightness);
        fx_eyes_notify();
        break;
    case CMD_EYES_ONESHOT:
        fx_eyes_play_oneshot(c->idx, c->plan.duration_ms, false, NULL);
        fx_eyes_notify();
        break;
    case CMD_EYES_FOLLOW:
        fx_eyes_set_follow(c->idx != 0);
        fx_eyes_notify();
        break;
    case CMD_EYES_BLINK_OVERLAY:
        fx_eyes_set_blink_overlay(c->idx != 0);
        fx_eyes_notify();
        break;
    case CMD_EYES_PLAN:
        fx_eyes_apply_fsm_plan(&c->plan);
        fx_eyes_notify();
        break;
    default:
        break;
    }
}

static void anim_task(void *arg)
{
    (void)arg;
#if CONFIG_LED_FX_TASK_WDT
    esp_err_t werr = esp_task_wdt_add(NULL);
    if (werr != ESP_OK && werr != ESP_ERR_INVALID_ARG)
        ESP_LOGW(TAG, "iscrizione task watchdog fallita: %s",
                 esp_err_to_name(werr));
#endif
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
#if CONFIG_LED_FX_TASK_WDT
        esp_task_wdt_reset(); /* feed a ogni frame, prima di ogni continue */
#endif

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
         * with no timer reset. Il corpo occupa [0, body_n), gli occhi i 2
         * pixel in coda [body_n, body_n+2) quando la zona e' presente. */
        const size_t body_n = (size_t)CONFIG_LED_FX_EYES_START;
        int64_t t0 = esp_timer_get_time();
        if (s_pixel_test) {
            memset(s_frame, 0, sizeof(s_frame));
            if (s_pixel_test_idx < CONFIG_LED_FX_NUM_PIXELS)
                s_frame[s_pixel_test_idx] = s_pixel_test_color;
        } else {
            reg[s_effect].render(s_frame, body_n, &s_params,
                                 (uint64_t)t_eff);
            if (s_eyes_ok)
                fx_eyes_render(&s_frame[body_n], (uint64_t)now);
        }
        /* LUT occhi dedicata: ricostruita solo quando cambia la luminosita'
         * occhi (stessa gamma e stessa percentuale massima del corpo). */
        uint8_t eyes_bright = fx_eyes_brightness();
        if (eyes_bright != s_eyes_lut_src) {
            fx_output_lut_build(s_eyes_lut, eyes_bright,
                                CONFIG_LED_FX_MAX_BRIGHTNESS_PCT);
            s_eyes_lut_src = eyes_bright;
        }
        for (int i = 0; i < CONFIG_LED_FX_NUM_PIXELS; i++) {
            const fx_rgb_t *f = &s_frame[i];
            const uint8_t *lut = ((size_t)i < body_n) ? s_lut : s_eyes_lut;
            uint8_t r = lut[f->r];
            uint8_t g = lut[f->g];
            uint8_t b = lut[f->b];
            /* Overlay di stato solo sul corpo: gli occhi raccontano la FSM,
             * non la connettivita'. */
            if (ov_on && (size_t)i < body_n) {
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

/* Send blocking for a couple of frames: the anim task drains up to
 * FX_QUEUE_DEPTH commands per frame, so a burst (e.g. a look re-emitted per
 * immediate CONFIG_EVENT_CHANGED) liquefies in ~2 frames instead of dropping
 * the tail of the burst (banco 2026-10-27: brightness/effect scartati con
 * ESP_ERR_TIMEOUT). Caller context is task-level only, never ISR/timer. */
static esp_err_t fx_send_cmd(const led_fx_cmd_t *cmd)
{
    if (s_q == NULL)
        return ESP_ERR_INVALID_STATE;
    const TickType_t two_frames =
        (TickType_t)(((uint64_t)2 * FX_FRAME_PERIOD_US * configTICK_RATE_HZ) /
                     1000000ULL);
    return (xQueueSend(s_q, cmd, two_frames) == pdTRUE) ? ESP_OK
                                                        : ESP_ERR_TIMEOUT;
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

    /* 3b. Zona occhi: attiva solo se i 2 pixel stanno in coda alla catena
     * (guardia runtime per l'app legacy a 64 px, vedi #error in testa). Il
     * motore parte comunque: con la zona spenta il render lo salta e le API
     * occhi rispondono ESP_ERR_INVALID_STATE. Desync da Kconfig con guardia
     * del preprocessore, mai ternari runtime sui bool CONFIG_*. */
#if CONFIG_LED_FX_EYES_START + 2 <= CONFIG_LED_FX_NUM_PIXELS
    s_eyes_ok = true;
#else
    s_eyes_ok = false;
    ESP_LOGW(TAG, "zona occhi disattivata: EYES_START=%d + 2 non entra in "
                  "NUM_PIXELS=%d",
             CONFIG_LED_FX_EYES_START, CONFIG_LED_FX_NUM_PIXELS);
#endif
    fx_eyes_init((uint32_t)esp_timer_get_time(),
#if CONFIG_LED_FX_EYES_DESYNC
                 true
#else
                 false
#endif
    );
    s_eyes_lut_src = fx_eyes_brightness();
    fx_output_lut_build(s_eyes_lut, s_eyes_lut_src,
                        CONFIG_LED_FX_MAX_BRIGHTNESS_PCT);

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

    ESP_LOGI(TAG, "init: %d pixel (corpo %d%s) su GPIO%d (en GPIO%d), %d fps",
             CONFIG_LED_FX_NUM_PIXELS, CONFIG_LED_FX_EYES_START,
             s_eyes_ok ? " + occhi 2" : "", CONFIG_LED_FX_DATA_GPIO,
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

/* --- Fase 8: zona occhi -------------------------------------------------
 * Le API di comando passano dalla coda comandi (applicate dal task anim,
 * che poi emette LED_FX_EVENT_EYES); get_look e get_effects leggono
 * direttamente (get_look e' thread-safe via seqlock nel motore). Con la
 * zona occhi disattiva (catena senza i 2 pixel in coda) i comandi
 * rispondono ESP_ERR_INVALID_STATE; la validazione e' fatta qui, prima di
 * accodare, perche' gli errori dell'applicazione non risalgono al chiamante. */

esp_err_t led_fx_eyes_set_effect(const char *name)
{
    if (!s_eyes_ok)
        return ESP_ERR_INVALID_STATE;
    const led_fx_eyes_effect_info_t *infos = NULL;
    size_t count = fx_eyes_get_effects(&infos);
    int idx = fx_eyes_find_effect(name);
    if (idx < 0 || (size_t)idx >= count || infos[idx].oneshot)
        return ESP_ERR_INVALID_ARG; /* i oneshot passano da play_oneshot */
    led_fx_cmd_t cmd = { .type = CMD_EYES_EFFECT, .idx = (uint8_t)idx };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_eyes_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_eyes_ok)
        return ESP_ERR_INVALID_STATE;
    led_fx_cmd_t cmd = { .type = CMD_EYES_COLOR, .r = r, .g = g, .b = b };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_eyes_set_brightness(uint8_t brightness)
{
    if (!s_eyes_ok)
        return ESP_ERR_INVALID_STATE;
    led_fx_cmd_t cmd = { .type = CMD_EYES_BRIGHTNESS, .brightness = brightness };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_eyes_play_oneshot(const char *name, uint32_t duration_ms)
{
    if (!s_eyes_ok)
        return ESP_ERR_INVALID_STATE;
    int idx = fx_eyes_find_effect(name);
    if (fx_eyes_validate_oneshot(idx, duration_ms) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    led_fx_cmd_t cmd = { .type = CMD_EYES_ONESHOT, .idx = (uint8_t)idx };
    cmd.plan.duration_ms = duration_ms;
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_eyes_follow_fsm(bool enable)
{
    if (!s_eyes_ok)
        return ESP_ERR_INVALID_STATE;
    led_fx_cmd_t cmd = { .type = CMD_EYES_FOLLOW, .idx = enable ? 1u : 0u };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_eyes_set_blink_overlay(bool enable)
{
    if (!s_eyes_ok)
        return ESP_ERR_INVALID_STATE;
    led_fx_cmd_t cmd = { .type = CMD_EYES_BLINK_OVERLAY, .idx = enable ? 1u : 0u };
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_eyes_apply_fsm_plan(const led_fx_eyes_plan_t *plan)
{
    if (!s_eyes_ok)
        return ESP_ERR_INVALID_STATE;
    if (plan == NULL)
        return ESP_ERR_INVALID_ARG;
    led_fx_cmd_t cmd = { .type = CMD_EYES_PLAN };
    cmd.plan = *plan;
    return fx_send_cmd(&cmd);
}

esp_err_t led_fx_eyes_get_look(led_fx_eyes_look_t *out)
{
    return fx_eyes_get_look(out);
}

size_t led_fx_eyes_get_effects(const led_fx_eyes_effect_info_t **out)
{
    return fx_eyes_get_effects(out);
}
