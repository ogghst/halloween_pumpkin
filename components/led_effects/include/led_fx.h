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

/* LED matrix geometry: 8x8 panel wired as a single serpentine chain.
 * Pixel 0 is the top-left cell; even rows run left->right, odd rows
 * right->left. W*H (64) is the default CONFIG_LED_FX_NUM_PIXELS.
 * HARDWARE ASSUMPTION: verify the actual wiring on the bench. */
#define LED_FX_MATRIX_W 8
#define LED_FX_MATRIX_H 8

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

/** Matrix cell (x, y) -> strip index (serpentine, origin top-left).
 *  Returns -1 when x or y is outside [0, LED_FX_MATRIX_W/H). */
int led_fx_xy_index(int x, int y);

/** Pixel-test hold: the anim task renders a black frame with only cell
 *  (x, y) lit (r/g/b linear 0-255) instead of the current effect, until
 *  led_fx_pixel_test_off. ESP_ERR_INVALID_ARG for out-of-matrix coords. */
esp_err_t led_fx_pixel_test(int x, int y, uint8_t r, uint8_t g, uint8_t b);

/** Release the pixel-test hold; the previous effect resumes with its clock
 *  untouched. ESP_OK also when no hold is active. */
esp_err_t led_fx_pixel_test_off(void);

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
