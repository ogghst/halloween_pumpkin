/* Effect renderers and effect registry (pumpkin firmware, Phase 1).
 *
 * All renderers write LINEAR rgb into px[0..n): gamma, brightness and the
 * absolute max percent are applied by the engine's output stage, never here.
 * t_us is the time elapsed since the current effect started.
 *
 * candle and flicker share one deterministic xorshift noise generator so the
 * same firmware build produces a reproducible sequence (seedable for tests).
 */
#include <string.h>

#include "fx_effects.h"
#include "fx_color.h"

/* ------------------------------------------------------------------ */
/* Deterministic noise (shared by candle/flicker)                      */
/* ------------------------------------------------------------------ */

static uint16_t s_rng = 0xACE1;

void fx_noise_seed(uint32_t seed)
{
    s_rng = (uint16_t)(seed ? seed : 0xACE1);
}

static uint16_t fx_noise16(void)
{
    uint16_t x = s_rng;
    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);
    s_rng = x;
    return x;
}

/* ------------------------------------------------------------------ */
/* Renderers                                                           */
/* ------------------------------------------------------------------ */

void fx_solid(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us)
{
    (void)t_us;
    for (size_t i = 0; i < n; i++)
        px[i] = p->color;
}

/* Candle uses a fixed warm base color by design: p->color is ignored, only
 * p->speed_q8 scales the flicker step rate. Envelope state is module-static. */
static const fx_rgb_t k_warm = { 255, 140, 60 };
static int s_candle_lvl = 60;          /* envelope level, percent 0..100 */
static uint64_t s_candle_last = 0;     /* last step time [us] */

void fx_candle(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us)
{
    const uint64_t interval = 8000ULL * 256u / p->speed_q8;

    if (t_us - s_candle_last >= interval) {
        int target = 45 + (int)(fx_noise16() % 31u); /* 45..75 */
        s_candle_lvl += (target - s_candle_lvl) / 4;
        s_candle_last = t_us;
    }
    fx_rgb_t out = {
        .r = (uint8_t)(k_warm.r * s_candle_lvl / 100),
        .g = (uint8_t)(k_warm.g * s_candle_lvl / 100),
        .b = (uint8_t)(k_warm.b * s_candle_lvl / 100),
    };
    for (size_t i = 0; i < n; i++)
        px[i] = out;
}

void fx_breathe(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us)
{
    if (n == 0)
        return;
    uint64_t period_us = 4000000ULL * 256u / p->speed_q8;
    uint8_t ph = (uint8_t)((t_us * 256u / period_us) & 0xFFu);
    int m = 140 + (115 * (int)k_fx_sin[ph]) / 128; /* ~25..255, no clamp needed */

    fx_rgb_t out = {
        .r = (uint8_t)((int)p->color.r * m / 255),
        .g = (uint8_t)((int)p->color.g * m / 255),
        .b = (uint8_t)((int)p->color.b * m / 255),
    };
    for (size_t i = 0; i < n; i++)
        px[i] = out;
}

/* Rainbow ignores p->color by design: full-saturation hue wheel, only
 * p->speed_q8 scales the rotation. Whole wheel in 3 s at 1.0x. */
void fx_rainbow(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us)
{
    if (n == 0)
        return;
    uint32_t phase = (uint32_t)(((t_us / 1000u) * p->speed_q8) / 3000u);
    for (size_t i = 0; i < n; i++) {
        uint32_t hue = (phase + (uint32_t)i * 256u / (uint32_t)n) & 0xFFu;
        px[i] = fx_hsv8((uint8_t)hue, 255, 255);
    }
}

/* Defective light bulb: irregular brightness steps with random blackouts. */
static int s_flicker_lvl = 100;                    /* 15..100 percent */
static uint64_t s_flicker_blackout_until = 0;      /* [us] */
static uint64_t s_flicker_last = 0;                /* last step time [us] */

void fx_flicker(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us)
{
    const uint64_t interval = 24000ULL * 256u / p->speed_q8;

    if (t_us - s_flicker_last >= interval) {
        s_flicker_lvl += (int)(fx_noise16() % 17u) - 8;
        if (s_flicker_lvl < 15)
            s_flicker_lvl = 15;
        if (s_flicker_lvl > 100)
            s_flicker_lvl = 100;
        if (fx_noise16() % 53u == 0u)
            s_flicker_blackout_until = t_us + 40000u + (uint64_t)(fx_noise16() % 50000u);
        s_flicker_last = t_us;
    }
    int lvl = s_flicker_lvl;
    if (t_us < s_flicker_blackout_until)
        lvl = 0;

    fx_rgb_t out = {
        .r = (uint8_t)((int)p->color.r * lvl / 100),
        .g = (uint8_t)((int)p->color.g * lvl / 100),
        .b = (uint8_t)((int)p->color.b * lvl / 100),
    };
    for (size_t i = 0; i < n; i++)
        px[i] = out;
}

/* Scare: one-shot ~3 s timeline, fixed colors (p unused). */
void fx_scare(fx_rgb_t *px, size_t n, const fx_params_t *p, uint64_t t_us)
{
    (void)p;
    fx_rgb_t c = { 0, 0, 0 };
    if (t_us < 250000u) {
        /* full white flash */
        c.r = 255;
        c.g = 255;
        c.b = 255;
    } else if (t_us < 500000u) {
        /* red pulses, 60 ms on / 60 ms off */
        if ((((t_us - 250000u) / 60000u) % 2u) == 0u)
            c.r = 255;
    } else if (t_us < 1200000u) {
        /* 15 Hz strobe alternating white/red */
        uint64_t ph = ((t_us - 500000u) / 33333u) % 2u;
        if (ph == 0u) {
            c.r = 255;
            c.g = 255;
            c.b = 255;
        } else {
            c.r = 255;
        }
    } else if (t_us < 3000000u) {
        /* linear red fade k = 1 -> 0 over the remaining 1.8 s */
        uint64_t k = (3000000u - t_us) / 1800000u;
        c.r = (uint8_t)(255u * k);
    }
    /* t >= 3 s: black (the engine restores the previous effect anyway) */
    for (size_t i = 0; i < n; i++)
        px[i] = c;
}

/* ------------------------------------------------------------------ */
/* Registry (stable order: names are used by MQTT/HA in later phases)  */
/* ------------------------------------------------------------------ */

static const fx_effect_t k_effects[] = {
    { "solid",   fx_solid,   0,       false },
    { "candle",  fx_candle,  0,       false },
    { "breathe", fx_breathe, 0,       false },
    { "rainbow", fx_rainbow, 0,       false },
    { "flicker", fx_flicker, 0,       false },
    { "scare",   fx_scare,   3000000, true  },
};

const fx_effect_t *fx_effect_registry(size_t *out_count)
{
    if (out_count)
        *out_count = sizeof(k_effects) / sizeof(k_effects[0]);
    return k_effects;
}

const fx_effect_t *fx_effect_find(const char *name)
{
    size_t count = 0;
    const fx_effect_t *reg = fx_effect_registry(&count);
    if (name == NULL)
        return NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(reg[i].name, name) == 0)
            return &reg[i];
    }
    return NULL;
}
