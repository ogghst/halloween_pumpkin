/* Shared internal types of the led_effects component (private). */
#ifndef FX_TYPES_H
#define FX_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} fx_rgb_t;

/* Per-effect parameters shared by every renderer. */
typedef struct {
    fx_rgb_t color;    /* base color, linear 0-255 */
    uint16_t speed_q8; /* 256 == 1.0x */
} fx_params_t;

typedef void (*fx_render_fn)(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us);

typedef struct {
    const char *name;     /* stable effect name (used by MQTT/HA later) */
    fx_render_fn render;  /* fills px[0..n) with LINEAR rgb for time t_us */
    uint64_t duration_us; /* 0 = endless */
    bool oneshot;         /* true = previous effect is restored after duration_us */
} fx_effect_t;

/* Effect registry (defined in fx_effects.c). */
const fx_effect_t *fx_effect_registry(size_t *out_count);
const fx_effect_t *fx_effect_find(const char *name);

/* Seed for the deterministic noise generator (candle/flicker). */
void fx_noise_seed(uint32_t seed);

#endif /* FX_TYPES_H */
