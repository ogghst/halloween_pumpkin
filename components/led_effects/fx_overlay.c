/* Status overlay pattern implementations (pumpkin firmware, Phase 4).
 * Pure C, no IDF dependencies (also compiled by the host test suite).
 * All timings are milliseconds relative to the overlay activation, so the
 * pattern phase always starts at 0 and uint32 wrap-around stays consistent. */
#include "fx_overlay.h"

/* Triangle envelope 0..PEAK..0 over one period: fades up for the first half
 * and back down for the second (a slow breathing pulse, dark only at the
 * period boundaries). */
static uint8_t overlay_pulse(uint32_t tick_ms, uint32_t period_ms)
{
    uint32_t phase = tick_ms % period_ms;
    uint32_t half = period_ms / 2u;
    uint32_t v = (phase < half) ? (LED_OVERLAY_PEAK * phase) / half
                                : (LED_OVERLAY_PEAK * (period_ms - phase)) / half;
    return (uint8_t)v;
}

int led_overlay_color_at(uint32_t tick_ms, led_fx_overlay_t ov, uint8_t out_rgb[3])
{
    if (out_rgb == NULL)
        return 0;

    /* Single active channel amplitude; a dark tick returns 0 with the buffer
     * untouched (no overlay contribution at all). */
    uint8_t c = 0;
    int on = 0;

    switch (ov) {
    case LED_FX_OVERLAY_PROV:
        c = overlay_pulse(tick_ms, LED_OVERLAY_PROV_PERIOD_MS);
        on = c != 0;
        break;

    case LED_FX_OVERLAY_CONNECTING:
        /* Two blinks (250 ms on / 250 ms off) at the start of every 2 s
         * period, dark for the remaining 1000 ms. */
        {
            uint32_t phase = tick_ms % LED_OVERLAY_CONNECT_PERIOD_MS;
            on = phase < 4u * LED_OVERLAY_CONNECT_BLINK_MS &&
                 phase % (2u * LED_OVERLAY_CONNECT_BLINK_MS) <
                     LED_OVERLAY_CONNECT_BLINK_MS;
            c = on ? (uint8_t)LED_OVERLAY_PEAK : 0;
        }
        break;

    case LED_FX_OVERLAY_NET_UP:
        /* Three 150 ms green flashes on a 500 ms grid inside the 1.5 s
         * window: [0,150) [500,650) [1000,1150), then expired. */
        on = tick_ms < LED_OVERLAY_UP_WINDOW_MS &&
             tick_ms % (LED_OVERLAY_UP_WINDOW_MS / 3u) < LED_OVERLAY_UP_FLASH_MS;
        c = on ? (uint8_t)LED_OVERLAY_PEAK : 0;
        break;

    case LED_FX_OVERLAY_NET_DOWN:
        c = overlay_pulse(tick_ms, LED_OVERLAY_DOWN_PERIOD_MS);
        on = c != 0;
        break;

    case LED_FX_OVERLAY_NONE:
    default:
        break;
    }

    if (!on)
        return 0;

    switch (ov) {
    case LED_FX_OVERLAY_NET_UP:
        out_rgb[0] = 0;
        out_rgb[1] = c;
        out_rgb[2] = 0;
        break;
    case LED_FX_OVERLAY_NET_DOWN:
        out_rgb[0] = c;
        out_rgb[1] = 0;
        out_rgb[2] = 0;
        break;
    default: /* PROV and CONNECTING: blue channel */
        out_rgb[0] = 0;
        out_rgb[1] = 0;
        out_rgb[2] = c;
        break;
    }
    return 1;
}

bool led_overlay_expired(uint32_t tick_ms, led_fx_overlay_t ov)
{
    return ov == LED_FX_OVERLAY_NET_UP && tick_ms >= LED_OVERLAY_UP_WINDOW_MS;
}

void led_overlay_blend(uint8_t base[3], const uint8_t ov[3])
{
    for (int i = 0; i < 3; i++)
        if (ov[i] > base[i])
            base[i] = ov[i];
}
