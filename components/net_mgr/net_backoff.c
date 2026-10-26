/* Pure core of the net_mgr reconnect backoff (host-tested). No IDF
 * headers: the Kconfig cap arrives via sdkconfig.h on target and via the
 * host_tests stub on the host. */
#include "sdkconfig.h"

#include "net_backoff.h"
#include "net_mgr.h"

#ifndef CONFIG_PUMPKIN_NET_BACKOFF_MAX_MS
#error "CONFIG_PUMPKIN_NET_BACKOFF_MAX_MS is expected from Kconfig (or the host stub)"
#endif

uint32_t net_backoff_compute(uint32_t attempt, uint32_t rand_pct)
{
    const uint32_t cap = (uint32_t)CONFIG_PUMPKIN_NET_BACKOFF_MAX_MS;
    uint32_t base = NET_BACKOFF_BASE_MS;

    /* Double per attempt, stopping at the cap so a huge `attempt` cannot
     * spin and 1000 << attempt can never overflow. */
    while (base < cap && attempt-- > 0) {
        uint32_t doubled = base << 1;
        if (doubled < base /* u32 wrap */ || doubled > cap) {
            base = cap;
            break;
        }
        base = doubled;
    }
    if (base > cap) {
        base = cap;
    }

    /* Deterministic jitter: rand_pct 0..99 -> factor 85..115 (base scale).
     * base * factor fits in u32: base is capped by the Kconfig range
     * (<= 3600000) and factor <= 115, so the product stays < 2^32. The
     * final clamp keeps every result within the cap. */
    uint32_t pct = rand_pct % 100u;
    uint32_t factor = 100u - NET_BACKOFF_JITTER_PCT
                    + (pct * 2u * NET_BACKOFF_JITTER_PCT) / 99u;
    uint32_t out = (base * factor) / 100u;
    return out > cap ? cap : out;
}
