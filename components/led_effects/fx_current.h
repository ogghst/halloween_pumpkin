/* Pure current estimation for the LED rail (Phase 6 hook). No IDF dependencies. */
#ifndef FX_CURRENT_H
#define FX_CURRENT_H

#include <stddef.h>
#include <stdint.h>
#include "fx_types.h"

/* Current drawn by one fully-lit channel of one pixel, in mA. */
#define FX_MA_PER_CHANNEL 20

/* Estimate instantaneous rail current [mA] for a frame of LINEAR rgb pixels.
   w[] is the per-pixel white channel (may be NULL when the strip is RGB).
   scale_pct is a global factor in percent (100 = no scaling). */
uint32_t fx_estimate_current_ma(const fx_rgb_t *px, const uint8_t *w, size_t n, uint8_t scale_pct);

#endif /* FX_CURRENT_H */
