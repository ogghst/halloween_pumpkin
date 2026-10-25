/* 8x8 matrix geometry: strip index of a matrix cell (x, y).
   The matrix is one serpentine WS2812B chain, row-major, origin top-left
   (pixel 0 = x0,y0): even rows run left->right, odd rows right->left.
   Pure C, no IDF dependencies (also compiled by the host test suite). */
#include "led_fx.h"

int led_fx_xy_index(int x, int y)
{
    if (x < 0 || x >= LED_FX_MATRIX_W || y < 0 || y >= LED_FX_MATRIX_H)
        return -1;
    return y * LED_FX_MATRIX_W + ((y & 1) == 0 ? x : LED_FX_MATRIX_W - 1 - x);
}
