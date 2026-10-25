/* Pure color math: gamma LUT, sine LUT, HSV->RGB, output LUT. No IDF dependencies. */
#ifndef FX_COLOR_H
#define FX_COLOR_H

#include <stdint.h>
#include "fx_types.h"

/* Gamma 2.2 LUT: lut[i] = round((i/255)^2.2 * 255). */
extern const uint8_t k_fx_gamma[256];

/* Sine LUT: k_fx_sin[i] = round(127 * sin(2*pi*i/256)), i = phase 0-255. */
extern const int8_t k_fx_sin[256];

/* Integer HSV->RGB (h, s, v all 0-255). Exact at primary hues, +-1 vs float reference. */
fx_rgb_t fx_hsv8(uint8_t h, uint8_t s, uint8_t v);

/* Compose the output LUT: gamma, then brightness (0-255), then absolute max percent.
   lut[i] = round(k_fx_gamma[i] * brightness * max_pct / (255*100)).
   Called only from the anim task whenever brightness changes. */
void fx_output_lut_build(uint8_t *lut, uint8_t brightness, uint8_t max_pct);

#endif /* FX_COLOR_H */
