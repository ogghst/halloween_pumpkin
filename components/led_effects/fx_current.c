/* Current estimation for the LED rail: per-pixel channel sum times the
   per-channel budget, scaled by a global percent factor.
   Pure C, no IDF dependencies (also compiled by the host test suite). */
#include "fx_current.h"

uint32_t fx_estimate_current_ma(const fx_rgb_t *px, const uint8_t *w, size_t n, uint8_t scale_pct)
{
    if (!px || n == 0) return 0;
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc += ((uint32_t)px[i].r + px[i].g + px[i].b + (w ? w[i] : 0u)) * FX_MA_PER_CHANNEL;
    acc = acc * scale_pct / 100u;
    return (uint32_t)(acc / 255u);
}
