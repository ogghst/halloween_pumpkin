/* Status overlay patterns (pumpkin firmware, Phase 4).
 * Dim colors (<= LED_OVERLAY_PEAK per channel) shown on top of the current
 * effect by the anim task: fused with max() per channel AFTER gamma/brightness
 * so they stay visible even at low brightness. Pure C, no IDF dependencies
 * (also compiled by the host test suite). */
#ifndef FX_OVERLAY_H
#define FX_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "led_fx.h"

/* Amplitude cap of every overlay pattern [0-255 output scale]. */
#define LED_OVERLAY_PEAK 48u

/* Pattern timing, all in milliseconds of the anim-task clock. */
#define LED_OVERLAY_PROV_PERIOD_MS    2000u /* slow blue pulse */
#define LED_OVERLAY_CONNECT_PERIOD_MS 2000u /* two 250 ms blue blinks */
#define LED_OVERLAY_CONNECT_BLINK_MS  250u
#define LED_OVERLAY_UP_WINDOW_MS      1500u /* three 150 ms green flashes ... */
#define LED_OVERLAY_UP_FLASH_MS       150u  /* ... then the overlay expires */
#define LED_OVERLAY_DOWN_PERIOD_MS    3000u /* slow red pulse */

/* Overlay color at tick_ms (ms elapsed since the overlay was activated).
 * Returns 1 and fills out_rgb[3] when the pattern emits light at that tick,
 * 0 with out_rgb untouched when it is dark there (also for NONE and any
 * value outside the enum). */
int led_overlay_color_at(uint32_t tick_ms, led_fx_overlay_t ov, uint8_t out_rgb[3]);

/* True only for LED_FX_OVERLAY_NET_UP once tick_ms reached the end of its
 * 1.5 s window (the anim task then clears the overlay back to NONE). Every
 * other pattern is periodic and never expires. */
bool led_overlay_expired(uint32_t tick_ms, led_fx_overlay_t ov);

/* Per-channel max() fusion, in place: base[i] = max(base[i], ov[i]).
 * A dim overlay enriches a dark base and never dims a bright one. */
void led_overlay_blend(uint8_t base[3], const uint8_t ov[3]);

#endif /* FX_OVERLAY_H */
