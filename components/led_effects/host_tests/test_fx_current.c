/* Host unit tests for fx_current.c (Unity, native build).
   main() lives in test_fx_color.c; these functions are declared there. */
#include <string.h>
#include "unity.h"
#include "fx_current.h"

/* 7. 64 full-white pixels (8x8 matrix): 3 channels * 20 mA * 64 / 255 = 3840 mA,
   halved by a 50% scale factor. */
void test_current_white_full_and_half_scale(void)
{
    fx_rgb_t px[64];
    for (int i = 0; i < 64; i++)
        px[i] = (fx_rgb_t){ 255, 255, 255 };

    TEST_ASSERT_EQUAL_UINT32(3840, fx_estimate_current_ma(px, NULL, 64, 100));
    TEST_ASSERT_EQUAL_UINT32(1920, fx_estimate_current_ma(px, NULL, 64, 50));
}

/* 8. RGBW adds a fourth full channel: 4 * 20 mA * 64 / 255 = 5120 mA.
   A NULL white pointer must equal an all-zero white array. */
void test_current_rgbw_and_null_white(void)
{
    fx_rgb_t px[64];
    uint8_t w[64];
    uint8_t zero[64];
    memset(zero, 0, sizeof(zero));
    for (int i = 0; i < 64; i++) {
        px[i] = (fx_rgb_t){ 255, 255, 255 };
        w[i] = 255;
    }

    TEST_ASSERT_EQUAL_UINT32(5120, fx_estimate_current_ma(px, w, 64, 100));
    TEST_ASSERT_EQUAL_UINT32(fx_estimate_current_ma(px, zero, 64, 100),
                             fx_estimate_current_ma(px, NULL, 64, 100));
}

/* 9. Black frame and invalid arguments all estimate 0 mA. */
void test_current_black_and_invalid_args(void)
{
    fx_rgb_t black[4] = { { 0, 0, 0 } };
    uint8_t w[4] = { 0 };

    TEST_ASSERT_EQUAL_UINT32(0, fx_estimate_current_ma(black, w, 4, 100));
    TEST_ASSERT_EQUAL_UINT32(0, fx_estimate_current_ma(NULL, w, 4, 100));
    TEST_ASSERT_EQUAL_UINT32(0, fx_estimate_current_ma(black, w, 0, 100));
}
