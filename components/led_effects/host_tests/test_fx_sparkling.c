/* Test host dell'effetto corpo "sparkling" (Fase 8-bis): stelle che si
 * accendono e spengono su sfondo nero, posizioni casuali deterministiche
 * col seed, limite concorrenza, reset quando il clock torna indietro. */
#include "unity.h"
#include "fx_effects.h"
#include "fx_types.h"

#include <stddef.h>

void test_sparkling_stars_and_background(void);
void test_sparkling_effect_restart(void);

/* 1. Stelle: compaiono su sfondo nero, nei limiti della zona, al massimo
   FX_STAR_MAX (6) pixel accesi contemporaneamente; col colore bianco le
   stelle sono bianche (canali uguali fra loro sul singolo pixel). */
void test_sparkling_stars_and_background(void)
{
    fx_rgb_t px[16];
    fx_params_t p = { .color = { 255, 255, 255 }, .speed_q8 = 256 };
    int lit_frames = 0;
    int max_lit = 0;

    fx_noise_seed(42);
    fx_sparkling(px, 16, &p, 0);

    for (uint64_t t = 0; t <= 5000000; t += 20000) {
        fx_sparkling(px, 16, &p, t);
        int lit = 0;
        for (size_t i = 0; i < 16; i++) {
            if (px[i].r || px[i].g || px[i].b) {
                lit++;
                /* stella bianca: i tre canali sono uguali (tinta = bianco) */
                TEST_ASSERT_EQUAL_UINT8(px[i].r, px[i].g);
                TEST_ASSERT_EQUAL_UINT8(px[i].g, px[i].b);
            }
        }
        if (lit > 0)
            lit_frames++;
        if (lit > max_lit)
            max_lit = lit;
        TEST_ASSERT_TRUE(max_lit <= 6);
    }
    TEST_ASSERT_TRUE(lit_frames > 50); /* il cielo non resta nero */
    TEST_ASSERT_TRUE(max_lit >= 1);
}

/* 2. Riavvio dell'effetto (clock dell'effetto tornato indietro): le stelle
   attive vengono azzerate e il frame e' nero all'istante del reset. */
void test_sparkling_effect_restart(void)
{
    fx_rgb_t px[8];
    fx_params_t p = { .color = { 255, 255, 255 }, .speed_q8 = 256 };
    bool lit_before = false;

    fx_noise_seed(7);
    for (uint64_t t = 0; t <= 2000000; t += 50000) {
        fx_sparkling(px, 8, &p, t);
        for (size_t i = 0; i < 8; i++)
            if (px[i].r || px[i].g || px[i].b)
                lit_before = true;
    }
    TEST_ASSERT_TRUE(lit_before);

    fx_sparkling(px, 8, &p, 1000); /* clock indietro: reset */
    for (size_t i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_UINT8(0, px[i].r);
}
