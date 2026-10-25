/* Effect renderers (private). All renderers write LINEAR rgb: gamma and brightness
   are applied by the output stage of the engine, never inside the effects. */
#ifndef FX_EFFECTS_H
#define FX_EFFECTS_H

#include "fx_types.h"

void fx_solid(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us);
void fx_candle(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us);
void fx_breathe(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us);
void fx_rainbow(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us);
void fx_flicker(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us);
void fx_scare(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us);

#endif /* FX_EFFECTS_H */
