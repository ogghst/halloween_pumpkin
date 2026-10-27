/* Host unit tests for fx_eyes.c: motore zona occhi (Unity, native build).
   main() lives in test_fx_color.c; these functions are declared there.
   Il clock e' iniettato (now_us del render): tutte le timeline sono
   deterministico-ripetibili con lo stesso seed del PRNG del blink. */
#include <string.h>

#include "unity.h"

#include "fx_eyes.h"
#include "fx_effects.h" /* render corpo per i test di mappatura zona */
#include "fx_current.h"

#define EYES_T(pxi, ch) ((pxi)[0].ch)
#define SENTINEL 0xA5

/* Nessun pixel della zona [0, n) e' stato toccato dal render occhi. */
static bool zone_untouched(const fx_rgb_t *px, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (px[i].r != SENTINEL || px[i].g != SENTINEL || px[i].b != SENTINEL)
            return false;
    }
    return true;
}

/* 13. Mappatura zona: il render corpo con n=64 non tocca i pixel 64-65 e il
   render occhi scrive solo i propri 2 pixel in coda. */
void test_eyes_zone_mapping(void)
{
    fx_rgb_t frame[66];
    fx_params_t p = { { 255, 0, 0 }, 256 };

    memset(frame, SENTINEL, sizeof(frame));
    fx_solid(frame, 64, &p, 0);
    TEST_ASSERT_TRUE_MESSAGE(zone_untouched(&frame[64], 2),
                             "il render corpo non deve toccare i pixel 64-65");

    memset(frame, SENTINEL, sizeof(frame));
    fx_eyes_init(42, false);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("solid"));
    fx_eyes_set_color(10, 20, 30);
    fx_eyes_render(&frame[64], 0);      /* ancora il lerp al t=0 */
    fx_eyes_render(&frame[64], 2000000); /* lerp concluso (1500 ms) */
    TEST_ASSERT_TRUE_MESSAGE(zone_untouched(frame, 64),
                             "il render occhi non deve toccare i pixel 0-63");
    TEST_ASSERT_EQUAL_UINT8(10, frame[64].r);
    TEST_ASSERT_EQUAL_UINT8(20, frame[64].g);
    TEST_ASSERT_EQUAL_UINT8(30, frame[64].b);
    TEST_ASSERT_EQUAL_UINT8(10, frame[65].r);
    TEST_ASSERT_EQUAL_UINT8(20, frame[65].g);
    TEST_ASSERT_EQUAL_UINT8(30, frame[65].b);
}

/* 14. off = nero su entrambi gli occhi; solid = colore base (a lerp
   concluso) identico sui due occhi quando desync e' spento. */
void test_eyes_solid_and_off(void)
{
    fx_rgb_t px[2];

    fx_eyes_init(1, false);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("off"));
    fx_eyes_set_color(255, 128, 0);
    fx_eyes_render(px, 0);
    fx_eyes_render(px, 2000000);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].r);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].g);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].b);
    TEST_ASSERT_EQUAL_UINT8(0, px[1].r);

    fx_eyes_set_effect_idx(fx_eyes_find_effect("solid"));
    fx_eyes_render(px, 2200000);
    TEST_ASSERT_EQUAL_UINT8(255, px[0].r);
    TEST_ASSERT_EQUAL_UINT8(128, px[0].g);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].b);
    TEST_ASSERT_EQUAL_UINT8(255, px[1].r);
    TEST_ASSERT_EQUAL_UINT8(128, px[1].g);
}

/* 15. breathe: periodo 4000 ms, (1-cos)/2 (zero a t=0, pieno a T/2) e
   sfasamento desync di 300 ms dell'occhio 1. */
void test_eyes_breathe_period_and_desync(void)
{
    fx_rgb_t a[2], b[2];

    fx_eyes_init(5, true);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("breathe"));
    fx_eyes_set_color(255, 255, 255);
    fx_eyes_render(a, 0); /* ancora il lerp (1500 ms: i campioni sotto sono
                             tutti oltre, a colore pieno) */

    fx_eyes_render(a, 2000000); /* T/2: pieno */
    TEST_ASSERT_EQUAL_UINT8(255, a[0].r);
    fx_eyes_render(a, 4000000); /* T: zero */
    TEST_ASSERT_EQUAL_UINT8(0, a[0].r);
    fx_eyes_render(a, 5000000); /* quarto di periodo */
    TEST_ASSERT_EQUAL_UINT8(128, a[0].r);
    fx_eyes_render(a, 6000000); /* 2T + T/2: di nuovo pieno (periodo 4 s) */
    TEST_ASSERT_EQUAL_UINT8(255, a[0].r);

    /* desync: l'occhio 1 ritarda di 300 ms -> vale il valore dell'occhio 0
       campionato 300 ms prima (entrambi a lerp concluso). */
    fx_eyes_render(a, 3700000);
    fx_eyes_render(b, 3400000);
    TEST_ASSERT_EQUAL_UINT8(b[0].r, a[1].r);
    TEST_ASSERT_EQUAL_UINT8(b[0].g, a[1].g);

    /* senza desync i due occhi sono identici */
    fx_eyes_init(5, false);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("breathe"));
    fx_eyes_set_color(255, 255, 255);
    fx_eyes_render(a, 0);
    fx_eyes_render(a, 1700000);
    TEST_ASSERT_EQUAL_UINT8(a[0].r, a[1].r);
}

/* 16. color_cycle: giro hue completo in 8000 ms a saturazione/valore piene;
   occhio 1 in ritardo di 300 ms con desync. */
void test_eyes_color_cycle_desync(void)
{
    fx_rgb_t a[2], b[2], c[2];

    fx_eyes_init(6, true);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("color_cycle"));
    fx_eyes_set_color(99, 99, 99); /* ignorato dal ciclo hue */
    fx_eyes_render(a, 0);

    fx_eyes_render(c, 0); /* hue 0: rosso pieno */
    TEST_ASSERT_EQUAL_UINT8(255, c[0].r);
    TEST_ASSERT_EQUAL_UINT8(0, c[0].g);
    TEST_ASSERT_EQUAL_UINT8(0, c[0].b);

    fx_eyes_render(a, 2000000); /* hue 90 gradi: verde (deriva HSV +-16) */
    TEST_ASSERT_INT_WITHIN(16, 127, a[0].r);
    TEST_ASSERT_EQUAL_UINT8(255, a[0].g);
    TEST_ASSERT_EQUAL_UINT8(0, a[0].b);
    fx_eyes_render(b, 10000000); /* un giro e un quarto (10 s): come a 2 s */
    TEST_ASSERT_EQUAL_UINT8(b[0].r, a[0].r);
    TEST_ASSERT_EQUAL_UINT8(b[0].g, a[0].g);
    TEST_ASSERT_EQUAL_UINT8(b[0].b, a[0].b);

    /* desync: occhio 1 = occhio 0 campionato 300 ms prima */
    fx_eyes_render(a, 5000000);
    fx_eyes_render(b, 4700000);
    TEST_ASSERT_EQUAL_UINT8(b[0].r, a[1].r);
    TEST_ASSERT_EQUAL_UINT8(b[0].g, a[1].g);
    TEST_ASSERT_EQUAL_UINT8(b[0].b, a[1].b);
    TEST_ASSERT_TRUE(a[0].r != a[1].r || a[0].g != a[1].g);
}

/* 16-bis. glow: respiro continuo con pavimento (mai nero) su palette di 6
   colori in crossfade; desync occhio 1 come breathe/color_cycle. */
void test_eyes_glow_palette_and_floor(void)
{
    fx_rgb_t a[2], b[2];

    fx_eyes_init(7, true);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("glow"));
    fx_eyes_set_color(10, 10, 10); /* ignorato dal glow (palette propria) */
    fx_eyes_render(a, 0);

    /* centro del segmento 0 (1,5 s): hue 0 = rosso, respiro vicino al picco
       ma non a scala piena (envelope con pavimento) */
    fx_eyes_render(a, 1500000);
    TEST_ASSERT_EQUAL_UINT8(0, a[0].b);
    TEST_ASSERT_TRUE(a[0].r > a[0].g);
    TEST_ASSERT_TRUE(a[0].r > 127);
    TEST_ASSERT_TRUE(a[0].r < 255);

    /* centro del segmento 1 (4,5 s): hue 1 = giallo -> r ~= g, b = 0
       (asserzione sui rapporti: l'inviluppo scala tutti i canali uguale) */
    fx_eyes_render(a, 4500000);
    TEST_ASSERT_EQUAL_UINT8(0, a[0].b);
    TEST_ASSERT_TRUE(a[0].r > 40);
    TEST_ASSERT_TRUE(a[0].g > 40);
    TEST_ASSERT_INT_WITHIN(10, a[0].r, a[0].g);

    /* centro del segmento 2 (7,5 s): hue 2 = verde -> r = 0, g dominante */
    fx_eyes_render(a, 7500000);
    TEST_ASSERT_EQUAL_UINT8(0, a[0].r);
    TEST_ASSERT_TRUE(a[0].g > 40);

    /* pavimento: mai nero completo su un intero giro di palette (18 s) */
    bool black = false;
    for (uint64_t t = 0; t <= 18000000; t += 100000) {
        fx_eyes_render(a, t);
        if (a[0].r == 0 && a[0].g == 0 && a[0].b == 0)
            black = true;
    }
    TEST_ASSERT_FALSE(black);

    /* desync: occhio 1 = occhio 0 campionato 300 ms prima */
    fx_eyes_render(a, 5000000);
    fx_eyes_render(b, 4700000);
    TEST_ASSERT_EQUAL_UINT8(b[0].r, a[1].r);
    TEST_ASSERT_EQUAL_UINT8(b[0].g, a[1].g);
    TEST_ASSERT_EQUAL_UINT8(b[0].b, a[1].b);
}

/* 17. blink: finestra di spegnimento di 120 ms, primo intervallo casuale
   2000-8000 ms deterministico col seed, finestra dell'occhio 1 a +130 ms;
   come effetto base "blink" si applica sempre, con overlay solo se attivo. */
void test_eyes_blink_window_and_schedule(void)
{
    fx_rgb_t px[2];
    uint64_t start = 0, end = 0, start2 = 0;
    bool seen = false;

    fx_eyes_init(1234, true);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("solid"));
    fx_eyes_set_color(200, 100, 50);
    fx_eyes_set_blink_overlay(true);
    fx_eyes_render(px, 0); /* ancora lerp e schedule */

    /* la scansione parte oltre la transizione colore (1500 ms): il primo
       blink e' comunque tra 2 e 8 s */
    for (uint64_t t = 1600000; t <= 11000000; t += 1000) {
        fx_eyes_render(px, t);
        if (!seen && px[0].r == 0) {
            start = t;
            seen = true;
        } else if (seen && px[0].r != 0) {
            end = t;
            break;
        }
    }
    TEST_ASSERT_TRUE(seen);
    TEST_ASSERT_TRUE(start >= 2000000 && start <= 8000000);
    TEST_ASSERT_EQUAL_UINT64(120000, end - start); /* finestra di 120 ms */

    /* Il schedule del blink e' forward-only: per esaminare la finestra
       trovata si riparte dallo stesso seed (sequenza identica). */
    fx_eyes_init(1234, true);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("solid"));
    fx_eyes_set_color(200, 100, 50);
    fx_eyes_set_blink_overlay(true);
    fx_eyes_render(px, 0);      /* ancoraggio lerp */
    fx_eyes_render(px, start);  /* dentro la finestra: occhio 0 spento */

    /* desync blink: occhio 1 spento 130 ms dopo (finestra [+130, +250) ms) */
    fx_eyes_render(px, start + 100000);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].r);
    TEST_ASSERT_NOT_EQUAL(0, px[1].r);
    fx_eyes_render(px, start + 140000);
    TEST_ASSERT_NOT_EQUAL(0, px[0].r);
    TEST_ASSERT_EQUAL_UINT8(0, px[1].r);

    /* overlay disattivo: nessuno spegnimento; effetto base "blink": spegne */
    fx_eyes_set_blink_overlay(false);
    fx_eyes_render(px, start + 50000);
    TEST_ASSERT_NOT_EQUAL(0, px[0].r);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("blink"));
    fx_eyes_render(px, start + 50000);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].r);
    fx_eyes_render(px, start - 100000); /* prima della finestra: acceso */
    TEST_ASSERT_NOT_EQUAL(0, px[0].r);

    /* intervallo casuale fino al blink successivo: 2000-8000 ms dalla fine
       (riparto dal seed: lo stato e' quello dell'inizio della finestra) */
    fx_eyes_init(1234, true);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("solid"));
    fx_eyes_set_color(200, 100, 50);
    fx_eyes_set_blink_overlay(true);
    fx_eyes_render(px, 0);
    uint64_t next = 0;
    for (uint64_t t = end; t <= end + 8200000; t += 1000) {
        fx_eyes_render(px, t);
        if (px[0].r == 0) {
            next = t;
            break;
        }
    }
    TEST_ASSERT_TRUE(next >= end + 1999000 && next <= end + 8001000);

    /* stesso seed -> stessa sequenza */
    fx_eyes_init(1234, true);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("solid"));
    fx_eyes_set_color(200, 100, 50);
    fx_eyes_set_blink_overlay(true);
    fx_eyes_render(px, 0);
    seen = false;
    for (uint64_t t = 1600000; t <= 11000000; t += 1000) {
        fx_eyes_render(px, t);
        if (!seen && px[0].r == 0) {
            start2 = t;
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT64(start, start2);
}

/* 18. glare: nero a t=0, pieno a 40 ms (entro i 50), mantenimento, strobo a
   15 Hz nella finestra di 400 ms, dissolvenza lineare a 0, durata totale =
   duration_ms. Colore bianco -> i canali coincidono col livello. */
void test_eyes_glare_timeline(void)
{
    fx_rgb_t px[2];
    led_fx_eyes_look_t look;

    fx_eyes_init(7, false);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("off"));
    fx_eyes_set_color(255, 255, 255); /* colore oneshot (base target) */
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_play_oneshot(fx_eyes_find_effect("glare"), 1500,
                                                   false, NULL));

    fx_eyes_render(px, 0); /* partenza del glare a t=0 */
    TEST_ASSERT_EQUAL_UINT8(0, px[0].r); /* nero a t=0 */
    fx_eyes_render(px, 20000); /* meta' salita: 20/40 * 255 */
    TEST_ASSERT_EQUAL_UINT8(127, px[0].r);
    fx_eyes_render(px, 40000); /* pieno a 40 ms (< 50 ms) */
    TEST_ASSERT_EQUAL_UINT8(255, px[0].r);
    fx_eyes_render(px, 250000); /* mantenimento */
    TEST_ASSERT_EQUAL_UINT8(255, px[0].r);

    /* strobo 15 Hz nella finestra [300, 700) ms: accesi e spenti, circa 6
       periodi -> 12 commutazioni (quantizzazione del campionamento +-2) */
    int trans = 0;
    uint8_t prev = 255;
    bool saw_on = false, saw_off = false;
    for (uint64_t t = 300000; t < 700000; t += 1000) {
        fx_eyes_render(px, t);
        if (px[0].r == 255)
            saw_on = true;
        if (px[0].r == 0)
            saw_off = true;
        if (px[0].r != prev) {
            trans++;
            prev = px[0].r;
        }
    }
    TEST_ASSERT_TRUE(saw_on);
    TEST_ASSERT_TRUE(saw_off);
    TEST_ASSERT_INT_WITHIN(2, 12, trans);

    fx_eyes_render(px, 1100000); /* meta' dissolvenza: (1500-1100)/800 */
    TEST_ASSERT_EQUAL_UINT8(127, px[0].r);
    fx_eyes_render(px, 1499000); /* quasi a zero */
    TEST_ASSERT_EQUAL_UINT8(0, px[0].r);
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_TRUE(look.oneshot_active); /* ancora in corso a 1499 ms */

    fx_eyes_render(px, 1500000); /* durata esatta: concluso */
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_FALSE(look.oneshot_active);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].r); /* base "off" */
}

/* 19. flash: duration_ms=0 -> default 150 ms, colore base a piena scala
   (sopra il breathe), poi ritorno al look base. */
void test_eyes_flash_default(void)
{
    fx_rgb_t px[2];
    led_fx_eyes_look_t look;

    fx_eyes_init(8, false);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("breathe"));
    fx_eyes_set_color(0, 255, 0);
    fx_eyes_render(px, 0);
    TEST_ASSERT_EQUAL(ESP_OK,
                      fx_eyes_play_oneshot(fx_eyes_find_effect("flash"), 0, false, NULL));
    fx_eyes_render(px, 0); /* il flash parte da qui (start = 0) */

    fx_eyes_render(px, 50000); /* in corso: piena scala, non l'env breathe */
    TEST_ASSERT_EQUAL_UINT8(0, px[0].r);
    TEST_ASSERT_EQUAL_UINT8(255, px[0].g);
    TEST_ASSERT_EQUAL_UINT8(255, px[1].g);
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_TRUE(look.oneshot_active);

    fx_eyes_render(px, 149000);
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_TRUE(look.oneshot_active);

    fx_eyes_render(px, 150000); /* 150 ms esatti: concluso */
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_FALSE(look.oneshot_active);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].g); /* breathe a 150 ms e' ancora scuro */
}

/* 20. Lerp colore: dopo set_color il look riporta subito il target e il
   colore renderizzato progredisce linearmente entro TRANSITION_MS (1500 su
   host). */
void test_eyes_color_lerp_transition(void)
{
    fx_rgb_t px[2];
    led_fx_eyes_look_t look;

    fx_eyes_init(9, false);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("solid"));
    fx_eyes_render(px, 0);

    fx_eyes_set_color(0, 255, 0);
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_EQUAL_UINT8(0, look.r); /* il look riporta il TARGET */
    TEST_ASSERT_EQUAL_UINT8(255, look.g);

    fx_eyes_render(px, 0); /* ancoraggio: al frame di partenza ancora scuro */
    TEST_ASSERT_EQUAL_UINT8(0, px[0].g);
    fx_eyes_render(px, 750000); /* meta' transizione */
    TEST_ASSERT_EQUAL_UINT8(0, px[0].r);
    TEST_ASSERT_INT_WITHIN(1, 127, px[0].g);

    uint8_t prev = 0;
    for (uint64_t t = 100000; t <= 1400000; t += 100000) { /* monotona */
        fx_eyes_render(px, t);
        TEST_ASSERT_TRUE(px[0].g >= prev);
        prev = px[0].g;
    }
    fx_eyes_render(px, 1500000); /* conclusa */
    TEST_ASSERT_EQUAL_UINT8(255, px[0].g);
    fx_eyes_render(px, 2000000);
    TEST_ASSERT_EQUAL_UINT8(255, px[0].g);
}

/* 21. apply_fsm_plan: IDLE (look idle + blink ON), TRIGGERED (glare con
   colore scare a piena scala + brightness scare), COOLDOWN (look idle con
   transizione progressiva via lerp), OFF. */
void test_eyes_apply_fsm_plans(void)
{
    led_fx_eyes_plan_t plan = { 0 };
    led_fx_eyes_look_t look;
    fx_rgb_t px[2];

    fx_eyes_init(11, false);
    fx_eyes_set_follow(true);
    strcpy(plan.idle_effect, "breathe");
    plan.idle_r = 10;
    plan.idle_g = 20;
    plan.idle_b = 30;
    plan.idle_brightness = 200;
    plan.scare_r = 250;
    plan.scare_g = 0;
    plan.scare_b = 0;
    plan.scare_brightness = 255;
    plan.duration_ms = 1500;

    plan.kind = LED_FX_EYES_PLAN_IDLE;
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_apply_fsm_plan(&plan));
    fx_eyes_render(px, 0);
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_EQUAL_STRING("breathe", look.effect);
    TEST_ASSERT_EQUAL_UINT8(10, look.r);
    TEST_ASSERT_EQUAL_UINT8(20, look.g);
    TEST_ASSERT_EQUAL_UINT8(30, look.b);
    TEST_ASSERT_EQUAL_UINT8(200, look.brightness);
    TEST_ASSERT_TRUE(look.blink_overlay);
    TEST_ASSERT_EQUAL_UINT8(200, fx_eyes_brightness());

    plan.kind = LED_FX_EYES_PLAN_TRIGGERED;
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_apply_fsm_plan(&plan));
    fx_eyes_render(px, 1000000); /* il glare parte da qui */
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_TRUE(look.oneshot_active);
    TEST_ASSERT_EQUAL_UINT8(255, look.brightness);
    fx_eyes_render(px, 1040000); /* pieno a +40 ms, colore scare */
    TEST_ASSERT_EQUAL_UINT8(250, px[0].r);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].g);
    TEST_ASSERT_EQUAL_UINT8(250, px[1].r);

    fx_eyes_render(px, 2500000); /* glare concluso (1 s + 1500 ms) */
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_FALSE(look.oneshot_active);

    /* COOLDOWN: look idle, lerp progressivo dal colore scare */
    strcpy(plan.idle_effect, "solid"); /* senza env: matematica pulita */
    plan.kind = LED_FX_EYES_PLAN_COOLDOWN;
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_apply_fsm_plan(&plan));
    fx_eyes_render(px, 2500000); /* ancoraggio del lerp */
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_EQUAL_STRING("solid", look.effect);
    TEST_ASSERT_TRUE(look.blink_overlay);
    fx_eyes_render(px, 3250000); /* meta' transizione */
    TEST_ASSERT_INT_WITHIN(1, 130, px[0].r);
    TEST_ASSERT_INT_WITHIN(1, 10, px[0].g);
    TEST_ASSERT_INT_WITHIN(1, 15, px[0].b);
    fx_eyes_render(px, 4001000); /* conclusa: look idle raggiunto */
    TEST_ASSERT_EQUAL_UINT8(10, px[0].r);
    TEST_ASSERT_EQUAL_UINT8(20, px[0].g);
    TEST_ASSERT_EQUAL_UINT8(30, px[0].b);

    plan.kind = LED_FX_EYES_PLAN_OFF;
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_apply_fsm_plan(&plan));
    fx_eyes_render(px, 5000000);
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&look));
    TEST_ASSERT_EQUAL_STRING("off", look.effect);
    TEST_ASSERT_EQUAL_UINT8(0, px[0].r);
    TEST_ASSERT_EQUAL_UINT8(0, px[1].g);
}

/* 22. Gate FSM e validazioni del piano: con follow disattivo il piano non
   tocca nulla; piano con effetto ignoto/durata 0 -> errore e nessun cambio. */
void test_eyes_plan_gate_and_validation(void)
{
    led_fx_eyes_plan_t plan = { 0 };
    led_fx_eyes_look_t before, after;
    fx_rgb_t px[2];

    fx_eyes_init(12, false);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("solid"));
    fx_eyes_set_color(9, 9, 9);
    fx_eyes_set_brightness(77);
    fx_eyes_set_blink_overlay(true);
    fx_eyes_render(px, 0);       /* ancoraggio del lerp */
    fx_eyes_render(px, 2000000); /* lerp concluso: solid (9,9,9) */
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&before));

    strcpy(plan.idle_effect, "solid");
    plan.idle_r = 1;
    plan.idle_g = 2;
    plan.idle_b = 3;
    plan.idle_brightness = 5;
    plan.kind = LED_FX_EYES_PLAN_IDLE;

    fx_eyes_set_follow(false);
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_apply_fsm_plan(&plan)); /* ignorato */
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&after));
    TEST_ASSERT_EQUAL_UINT8(before.r, after.r);
    TEST_ASSERT_EQUAL_UINT8(before.g, after.g);
    TEST_ASSERT_EQUAL_UINT8(before.brightness, after.brightness);
    TEST_ASSERT_TRUE(after.blink_overlay);
    TEST_ASSERT_EQUAL_STRING(before.effect, after.effect);

    plan.kind = LED_FX_EYES_PLAN_TRIGGERED; /* gate chiuso: niente glare */
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_apply_fsm_plan(&plan));
    fx_eyes_render(px, 2100000);
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&after));
    TEST_ASSERT_FALSE(after.oneshot_active);
    TEST_ASSERT_EQUAL_UINT8(9, px[0].r);

    /* gate riaperto: piani invalidi rifiutati senza cambi di stato */
    fx_eyes_set_follow(true);
    strcpy(plan.idle_effect, "inesistente");
    plan.kind = LED_FX_EYES_PLAN_IDLE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fx_eyes_apply_fsm_plan(&plan));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fx_eyes_apply_fsm_plan(NULL));
    plan.kind = (led_fx_eyes_plan_kind_t)99;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fx_eyes_apply_fsm_plan(&plan));
    strcpy(plan.idle_effect, "solid");
    plan.kind = LED_FX_EYES_PLAN_TRIGGERED;
    plan.duration_ms = 0; /* il glare richiede > 0 */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, fx_eyes_apply_fsm_plan(&plan));
    fx_eyes_render(px, 2200000);
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_get_look(&after));
    TEST_ASSERT_FALSE(after.oneshot_active);
    TEST_ASSERT_EQUAL_UINT8(9, px[0].r); /* look intatto */
}

/* 23. Registro occhi: 8 effetti, nomi stabili, flag oneshot corretti;
   validazioni oneshot (glare > 0 e cap 1 h, solo flash/glare). */
void test_eyes_registry(void)
{
    const led_fx_eyes_effect_info_t *infos = NULL;
    const char *k_names[] = { "off",  "solid", "breathe", "color_cycle",
                              "blink", "glow", "flash", "glare" };
    const bool k_oneshot[] = { false, false, false, false,
                               false, false, true,  true };

    size_t n = fx_eyes_get_effects(&infos);
    TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)n);
    TEST_ASSERT_NOT_NULL(infos);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING(k_names[i], infos[i].name);
        TEST_ASSERT_EQUAL(k_oneshot[i], infos[i].oneshot);
    }

    for (size_t i = 0; i < n; i++)
        TEST_ASSERT_EQUAL_INT((int)i, fx_eyes_find_effect(k_names[i]));
    TEST_ASSERT_EQUAL_INT(-1, fx_eyes_find_effect("nope"));
    TEST_ASSERT_EQUAL_INT(-1, fx_eyes_find_effect(NULL));

    fx_eyes_init(3, false);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      fx_eyes_play_oneshot(fx_eyes_find_effect("solid"), 100, false, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      fx_eyes_play_oneshot(fx_eyes_find_effect("glare"), 0, false, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      fx_eyes_play_oneshot(fx_eyes_find_effect("glare"), 3600001, false, NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      fx_eyes_play_oneshot(fx_eyes_find_effect("glare"), 3600000, false, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, fx_eyes_play_oneshot(fx_eyes_find_effect("flash"), 0, false, NULL));
}

/* 24. Stima corrente: gli occhi a bianco pieno aggiungono 120 mA a scala
   100 (2 px x 3 canali x 20 mA) sul totale dei 66 px della catena. */
void test_eyes_current_estimate(void)
{
    fx_rgb_t frame[66] = { { 0, 0, 0 } };

    TEST_ASSERT_EQUAL_UINT32(0, fx_estimate_current_ma(frame, NULL, 66, 100));

    fx_eyes_init(1, false);
    fx_eyes_set_effect_idx(fx_eyes_find_effect("solid"));
    fx_eyes_set_color(255, 255, 255);
    fx_eyes_render(&frame[64], 0);
    fx_eyes_render(&frame[64], 2000000); /* lerp concluso: bianco pieno */
    TEST_ASSERT_EQUAL_UINT32(120, fx_estimate_current_ma(frame, NULL, 66, 100));

    for (int i = 0; i < 64; i++)
        frame[i] = (fx_rgb_t){ 255, 255, 255 };
    TEST_ASSERT_EQUAL_UINT32(3960, fx_estimate_current_ma(frame, NULL, 66, 100));
}
