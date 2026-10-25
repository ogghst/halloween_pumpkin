/* Host unit tests for fx_matrix.c: serpentine 8x8 mapping (Unity, native build).
   main() lives in test_fx_color.c; these functions are declared there. */
#include "unity.h"
#include "led_fx.h"

/* 10. Corners and serpentine direction: pixel 0 top-left, even rows
   left->right, odd rows right->left. Bottom-left cell (0,7) is the last
   pixel of the chain (63); bottom-right (7,7) is index 56. */
void test_matrix_corners_and_serpentine(void)
{
    TEST_ASSERT_EQUAL_INT(0, led_fx_xy_index(0, 0));
    TEST_ASSERT_EQUAL_INT(7, led_fx_xy_index(7, 0));
    TEST_ASSERT_EQUAL_INT(8, led_fx_xy_index(7, 1));  /* row 1 starts at x=7 */
    TEST_ASSERT_EQUAL_INT(15, led_fx_xy_index(0, 1));
    TEST_ASSERT_EQUAL_INT(16, led_fx_xy_index(0, 2));
    TEST_ASSERT_EQUAL_INT(56, led_fx_xy_index(7, 7));
    TEST_ASSERT_EQUAL_INT(63, led_fx_xy_index(0, 7));
}

/* 11. Out-of-range coordinates return -1 (x/y == 8 and negatives). */
void test_matrix_out_of_range(void)
{
    TEST_ASSERT_EQUAL_INT(-1, led_fx_xy_index(8, 0));
    TEST_ASSERT_EQUAL_INT(-1, led_fx_xy_index(0, 8));
    TEST_ASSERT_EQUAL_INT(-1, led_fx_xy_index(8, 8));
    TEST_ASSERT_EQUAL_INT(-1, led_fx_xy_index(-1, 0));
    TEST_ASSERT_EQUAL_INT(-1, led_fx_xy_index(0, -1));
    TEST_ASSERT_EQUAL_INT(-1, led_fx_xy_index(-1, -1));
}

/* 12. The mapping is a bijection over the whole matrix: every cell maps
   into [0, 64), no index is produced twice, every index is produced. */
void test_matrix_bijection(void)
{
    uint8_t seen[LED_FX_MATRIX_W * LED_FX_MATRIX_H] = { 0 };

    for (int y = 0; y < LED_FX_MATRIX_H; y++) {
        for (int x = 0; x < LED_FX_MATRIX_W; x++) {
            int idx = led_fx_xy_index(x, y);
            TEST_ASSERT_TRUE(idx >= 0);
            TEST_ASSERT_LESS_THAN_INT(LED_FX_MATRIX_W * LED_FX_MATRIX_H, idx);
            TEST_ASSERT_FALSE(seen[idx]);
            seen[idx] = 1;
        }
    }
    for (size_t i = 0; i < sizeof(seen); i++)
        TEST_ASSERT_TRUE(seen[i]);
}
