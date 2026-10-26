/* Host unit tests for fx_overlay.c: pattern colors/timing at key ticks,
 * NET_UP expiry, max() blend (Unity, native build). */
#include "unity.h"
#include "fx_overlay.h"

/* 1. PROV: slow blue pulse, period 2000 ms, triangle envelope peaking at 48
   at mid-period, dark at the boundaries, blue channel only. */
void test_overlay_prov_pulse(void)
{
    uint8_t rgb[3] = { 7, 7, 7 };

    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(0, LED_FX_OVERLAY_PROV, rgb));
    TEST_ASSERT_EQUAL_UINT8(7, rgb[2]); /* dark tick: buffer untouched */
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(500, LED_FX_OVERLAY_PROV, rgb));
    TEST_ASSERT_EQUAL_UINT8(0, rgb[0]);
    TEST_ASSERT_EQUAL_UINT8(0, rgb[1]);
    TEST_ASSERT_EQUAL_UINT8(24, rgb[2]); /* rising: 48*500/1000 */
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(1000, LED_FX_OVERLAY_PROV, rgb));
    TEST_ASSERT_EQUAL_UINT8(48, rgb[2]); /* peak at mid-period */
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(1500, LED_FX_OVERLAY_PROV, rgb));
    TEST_ASSERT_EQUAL_UINT8(24, rgb[2]); /* falling edge symmetric */
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(1999, LED_FX_OVERLAY_PROV, rgb));
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(3000, LED_FX_OVERLAY_PROV, rgb));
    TEST_ASSERT_EQUAL_UINT8(48, rgb[2]); /* period wrap: 3000 == 1000 */
}

/* 2. Every PROV/CONNECTING/NET_DOWN sample stays within the 48 amplitude cap. */
void test_overlay_amplitude_cap(void)
{
    uint8_t rgb[3];
    for (uint32_t t = 0; t < 3000; t += 7) {
        TEST_ASSERT_TRUE(led_overlay_color_at(t, LED_FX_OVERLAY_PROV, rgb) == 0 ||
                         rgb[2] <= LED_OVERLAY_PEAK);
        TEST_ASSERT_TRUE(led_overlay_color_at(t, LED_FX_OVERLAY_CONNECTING, rgb) == 0 ||
                         (rgb[0] == 0 && rgb[1] == 0 && rgb[2] <= LED_OVERLAY_PEAK));
        TEST_ASSERT_TRUE(led_overlay_color_at(t, LED_FX_OVERLAY_NET_DOWN, rgb) == 0 ||
                         (rgb[0] <= LED_OVERLAY_PEAK && rgb[1] == 0 && rgb[2] == 0));
        TEST_ASSERT_TRUE(led_overlay_color_at(t, LED_FX_OVERLAY_NET_UP, rgb) == 0 ||
                         (rgb[0] == 0 && rgb[1] <= LED_OVERLAY_PEAK && rgb[2] == 0));
    }
}

/* 3. CONNECTING: two 250 ms blue blinks ([0,250) [500,750)) then dark until
   the 2000 ms period restarts. */
void test_overlay_connecting_double_blink(void)
{
    uint8_t rgb[3];

    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(0, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_UINT8(48, rgb[2]);
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(249, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(250, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(499, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(500, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_UINT8(48, rgb[2]);
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(749, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(750, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(1500, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(1999, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(2000, LED_FX_OVERLAY_CONNECTING, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(2250, LED_FX_OVERLAY_CONNECTING, rgb));
}

/* 4. NET_UP: three 150 ms green flashes on the 500 ms grid inside 1.5 s
   ([0,150) [500,650) [1000,1150)), green channel only. */
void test_overlay_net_up_flashes(void)
{
    uint8_t rgb[3];

    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(0, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_UINT8(48, rgb[1]);
    TEST_ASSERT_EQUAL_UINT8(0, rgb[0]);
    TEST_ASSERT_EQUAL_UINT8(0, rgb[2]);
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(149, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(150, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(499, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(500, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(649, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(650, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(1000, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(1149, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(1150, LED_FX_OVERLAY_NET_UP, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(1499, LED_FX_OVERLAY_NET_UP, rgb));
}

/* 5. NET_DOWN: slow red pulse, period 3000 ms, peak 48 at mid-period. */
void test_overlay_net_down_pulse(void)
{
    uint8_t rgb[3];

    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(0, LED_FX_OVERLAY_NET_DOWN, rgb));
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(750, LED_FX_OVERLAY_NET_DOWN, rgb));
    TEST_ASSERT_EQUAL_UINT8(24, rgb[0]); /* rising: 48*750/1500 */
    TEST_ASSERT_EQUAL_UINT8(0, rgb[1]);
    TEST_ASSERT_EQUAL_UINT8(0, rgb[2]);
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(1500, LED_FX_OVERLAY_NET_DOWN, rgb));
    TEST_ASSERT_EQUAL_UINT8(48, rgb[0]);
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(2250, LED_FX_OVERLAY_NET_DOWN, rgb));
    TEST_ASSERT_EQUAL_UINT8(24, rgb[0]);
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(3000, LED_FX_OVERLAY_NET_DOWN, rgb));
    TEST_ASSERT_EQUAL_INT(1, led_overlay_color_at(4500, LED_FX_OVERLAY_NET_DOWN, rgb));
    TEST_ASSERT_EQUAL_UINT8(48, rgb[0]);
}

/* 6. Only NET_UP expires (at its 1.5 s window end); the periodic patterns
   never do, whatever the tick. */
void test_overlay_expired_only_net_up(void)
{
    TEST_ASSERT_FALSE(led_overlay_expired(0, LED_FX_OVERLAY_NET_UP));
    TEST_ASSERT_FALSE(led_overlay_expired(1499, LED_FX_OVERLAY_NET_UP));
    TEST_ASSERT_TRUE(led_overlay_expired(1500, LED_FX_OVERLAY_NET_UP));
    TEST_ASSERT_TRUE(led_overlay_expired(5000, LED_FX_OVERLAY_NET_UP));

    static const led_fx_overlay_t periodic[] = {
        LED_FX_OVERLAY_NONE, LED_FX_OVERLAY_PROV, LED_FX_OVERLAY_CONNECTING,
        LED_FX_OVERLAY_NET_DOWN,
    };
    static const uint32_t ticks[] = { 0, 1500, 2000, 3000, 100000 };
    for (size_t i = 0; i < sizeof(periodic) / sizeof(periodic[0]); i++)
        for (size_t j = 0; j < sizeof(ticks) / sizeof(ticks[0]); j++)
            TEST_ASSERT_FALSE(led_overlay_expired(ticks[j], periodic[i]));
}

/* 7. Blend is max() per channel: a dark base is enriched, a bright base is
   untouched, no channel can exceed max(base, overlay). */
void test_overlay_blend_max(void)
{
    uint8_t base[3];
    const uint8_t ov[3] = { 0, 48, 12 };

    base[0] = 10; base[1] = 20; base[2] = 30;
    led_overlay_blend(base, ov);
    TEST_ASSERT_EQUAL_UINT8(10, base[0]);
    TEST_ASSERT_EQUAL_UINT8(48, base[1]);
    TEST_ASSERT_EQUAL_UINT8(30, base[2]);

    base[0] = 200; base[1] = 255; base[2] = 100;
    led_overlay_blend(base, ov);
    TEST_ASSERT_EQUAL_UINT8(200, base[0]);
    TEST_ASSERT_EQUAL_UINT8(255, base[1]);
    TEST_ASSERT_EQUAL_UINT8(100, base[2]);

    base[0] = 0; base[1] = 0; base[2] = 0;
    led_overlay_blend(base, ov);
    TEST_ASSERT_EQUAL_UINT8(0, base[0]);
    TEST_ASSERT_EQUAL_UINT8(48, base[1]);
    TEST_ASSERT_EQUAL_UINT8(12, base[2]);

    base[0] = 48; base[1] = 48; base[2] = 48;
    led_overlay_blend(base, ov); /* equal values stay equal */
    TEST_ASSERT_EQUAL_UINT8(48, base[0]);
    TEST_ASSERT_EQUAL_UINT8(48, base[1]);
    TEST_ASSERT_EQUAL_UINT8(48, base[2]);
}

/* 8. NONE (and any unknown value) emits nothing and leaves the buffer
   untouched; a NULL buffer is safe. */
void test_overlay_none_and_unknown(void)
{
    uint8_t rgb[3] = { 9, 9, 9 };

    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(0, LED_FX_OVERLAY_NONE, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(1000, LED_FX_OVERLAY_NONE, rgb));
    TEST_ASSERT_EQUAL_UINT8(9, rgb[0]);
    TEST_ASSERT_EQUAL_UINT8(9, rgb[1]);
    TEST_ASSERT_EQUAL_UINT8(9, rgb[2]);
    TEST_ASSERT_EQUAL_INT(0,
        led_overlay_color_at(1000, (led_fx_overlay_t)99, rgb));
    TEST_ASSERT_EQUAL_INT(0, led_overlay_color_at(0, LED_FX_OVERLAY_PROV, NULL));
}
