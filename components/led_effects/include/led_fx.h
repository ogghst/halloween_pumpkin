/* Public API of the led_effects component (pumpkin firmware, Phase 1). */
#ifndef LED_FX_H
#define LED_FX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-point speed format: 256 == 1.0x. */
#define LED_FX_SPEED_ONE 256u

/* Rendering statistics, updated by the anim task. */
typedef struct {
    uint32_t frames;        /* rendered frames since init */
    uint32_t dropped;       /* frames whose render+flush exceeded the frame period */
    uint32_t render_us_avg; /* EMA (weight 1/8) of per-frame render+flush time [us] */
    uint32_t render_us_max; /* worst-case per-frame render+flush time [us] */
} led_fx_stats_t;

/**
 * Start the anim task, create the command queue and the LED strip driver.
 * The LED rail stays OFF after init: no light until led_fx_power(true).
 */
esp_err_t led_fx_init(void);

/** Switch the LED power rail (see led_fx_is_powered for the resulting state). */
esp_err_t led_fx_power(bool on);
bool led_fx_is_powered(void);

/** Select effect by name (see led_fx_get_effect_list). Cancels a running oneshot. */
esp_err_t led_fx_set_effect(const char *name);

/** Set the base effect color (linear 0-255 per channel, before gamma/brightness). */
esp_err_t led_fx_set_color(uint8_t r, uint8_t g, uint8_t b);

/** Global brightness 0-255, applied only at the output stage. */
esp_err_t led_fx_set_brightness(uint8_t brightness);

/** Effect speed multiplier in Q8 (LED_FX_SPEED_ONE == normal speed). */
esp_err_t led_fx_set_speed(uint16_t speed_q8);

/** Play a one-shot effect (e.g. "scare") once, then restore the previous effect. */
esp_err_t led_fx_play_oneshot(const char *name);

/** Array of effect names in registry order (static storage); *out_count receives its length. */
const char *const *led_fx_get_effect_list(size_t *out_count);

/** Copy of current rendering statistics. */
void led_fx_get_stats(led_fx_stats_t *out);

/* --- Phase 6 hook: power estimation (no active limiting yet) --- */

/** Estimate instantaneous LED rail current [mA] from the last rendered frame. */
uint32_t led_fx_estimate_current_ma(void);

/** Global scale factor [percent, 1-100] applied by led_fx_estimate_current_ma. Default 100. */
esp_err_t led_fx_set_current_scale_pct(uint8_t pct);

#ifdef __cplusplus
}
#endif
#endif /* LED_FX_H */
