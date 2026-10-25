/* Host unit tests for fx_color.c (Unity, native build).
   Also runs the fx_current.c tests defined in test_fx_current.c:
   a single main() lives here. See CMakeLists.txt in this directory. */
#include <math.h>
#include "unity.h"
#include "fx_color.h"
#include "fx_current.h"

/* Tests defined in test_fx_current.c. */
void test_current_white_full_and_half_scale(void);
void test_current_rgbw_and_null_white(void);
void test_current_black_and_invalid_args(void);

void setUp(void)
{
}

void tearDown(void)
{
}

/* 1. Gamma endpoints: 0 and 255 exact, mid-scale within +-1 of the formula. */
void test_gamma_endpoints(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, k_fx_gamma[0]);
    TEST_ASSERT_EQUAL_UINT8(255, k_fx_gamma[255]);
    int expected = (int)lround(pow(128.0 / 255.0, 2.2) * 255.0);
    TEST_ASSERT_INT_WITHIN(1, expected, k_fx_gamma[128]);
}

/* 2. Gamma is monotonic non-decreasing over the whole table. */
void test_gamma_monotonic(void)
{
    for (int i = 0; i < 255; i++)
        TEST_ASSERT_TRUE(k_fx_gamma[i] <= k_fx_gamma[i + 1]);
}

/* 3. Sine endpoints: quarter periods land on 0/+127/0/-127. */
void test_sin_endpoints(void)
{
    TEST_ASSERT_EQUAL_INT8(0, k_fx_sin[0]);
    TEST_ASSERT_EQUAL_INT8(127, k_fx_sin[64]);
    TEST_ASSERT_EQUAL_INT8(0, k_fx_sin[128]);
    TEST_ASSERT_EQUAL_INT8(-127, k_fx_sin[192]);
}

/* 4. HSV primaries exact; grey (s=0) with +-1 tolerance because the
   integer pipeline truncates (q = v*255/256), as per the fx_hsv8 doc. */
void test_hsv_primaries(void)
{
    fx_rgb_t red = fx_hsv8(0, 255, 255);
    TEST_ASSERT_EQUAL_UINT8(255, red.r);
    TEST_ASSERT_EQUAL_UINT8(0, red.g);
    TEST_ASSERT_EQUAL_UINT8(0, red.b);

    fx_rgb_t green = fx_hsv8(86, 255, 255);
    TEST_ASSERT_EQUAL_UINT8(0, green.r);
    TEST_ASSERT_EQUAL_UINT8(255, green.g);
    TEST_ASSERT_EQUAL_UINT8(0, green.b);

    fx_rgb_t blue = fx_hsv8(172, 255, 255);
    TEST_ASSERT_EQUAL_UINT8(0, blue.r);
    TEST_ASSERT_EQUAL_UINT8(0, blue.g);
    TEST_ASSERT_EQUAL_UINT8(255, blue.b);

    fx_rgb_t grey = fx_hsv8(0, 0, 123);
    TEST_ASSERT_EQUAL_UINT8(123, grey.r);
    TEST_ASSERT_INT_WITHIN(1, 123, grey.g);
    TEST_ASSERT_INT_WITHIN(1, 123, grey.b);
}

/* Float HSV reference (h in degrees, s/v in 0..1). */
static void hsv_ref(float h_deg, float s, float v, int *r, int *g, int *b)
{
    float c = v * s;
    float hp = fmodf(h_deg, 360.0f);
    if (hp < 0.0f) hp += 360.0f;
    float x = c * (1.0f - fabsf(fmodf(hp / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rr, gg, bb;
    if (hp < 60.0f)       { rr = c; gg = x; bb = 0.0f; }
    else if (hp < 120.0f) { rr = x; gg = c; bb = 0.0f; }
    else if (hp < 180.0f) { rr = 0.0f; gg = c; bb = x; }
    else if (hp < 240.0f) { rr = 0.0f; gg = x; bb = c; }
    else if (hp < 300.0f) { rr = x; gg = 0.0f; bb = c; }
    else                  { rr = c; gg = 0.0f; bb = x; }
    *r = (int)lroundf((rr + m) * 255.0f);
    *g = (int)lroundf((gg + m) * 255.0f);
    *b = (int)lroundf((bb + m) * 255.0f);
}

/* 5. Full hue sweep against the float reference. The mandated integer
   algorithm splits the hue into six 43-count regions (60.7 deg sectors),
   whose drift vs true 60 deg sectors accounts for up to ~15 LSB at full
   saturation on top of the +-2 p/q/t rounding: tolerance is 16 total. */
void test_hsv_sweep_vs_float(void)
{
    static const struct { uint8_t s, v; } combos[] = { { 255, 255 }, { 128, 200 } };
    for (size_t c = 0; c < sizeof(combos) / sizeof(combos[0]); c++) {
        for (int h = 0; h < 256; h++) {
            fx_rgb_t got = fx_hsv8((uint8_t)h, combos[c].s, combos[c].v);
            int r, g, b;
            hsv_ref((float)h * 360.0f / 255.0f,
                    combos[c].s / 255.0f, combos[c].v / 255.0f, &r, &g, &b);
            TEST_ASSERT_INT_WITHIN(16, r, got.r);
            TEST_ASSERT_INT_WITHIN(16, g, got.g);
            TEST_ASSERT_INT_WITHIN(16, b, got.b);
        }
    }
}

/* 6. Output LUT composition: capped at 60%, identical to gamma at 100%,
   all zeros at brightness 0. */
void test_output_lut(void)
{
    uint8_t lut[256];

    fx_output_lut_build(lut, 255, 60);
    TEST_ASSERT_EQUAL_UINT8(0, lut[0]);
    TEST_ASSERT_EQUAL_UINT8(153, lut[255]);
    for (int i = 0; i < 255; i++)
        TEST_ASSERT_TRUE(lut[i] <= lut[i + 1]);

    fx_output_lut_build(lut, 255, 100);
    for (int i = 0; i < 256; i++)
        TEST_ASSERT_EQUAL_UINT8(k_fx_gamma[i], lut[i]);

    fx_output_lut_build(lut, 0, 60);
    for (int i = 0; i < 256; i++)
        TEST_ASSERT_EQUAL_UINT8(0, lut[i]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gamma_endpoints);
    RUN_TEST(test_gamma_monotonic);
    RUN_TEST(test_sin_endpoints);
    RUN_TEST(test_hsv_primaries);
    RUN_TEST(test_hsv_sweep_vs_float);
    RUN_TEST(test_output_lut);
    RUN_TEST(test_current_white_full_and_half_scale);
    RUN_TEST(test_current_rgbw_and_null_white);
    RUN_TEST(test_current_black_and_invalid_args);
    return UNITY_END();
}
