/* Rate-limit di report per distance e ciclo periodico (puro). */
#include "ha_state.h"
#include <stddef.h>

bool ha_rate_distance(ha_report_state_t *st, int32_t new_mm, int64_t now_ms)
{
    if (st == NULL)
        return false;

    if (!st->distance_seen) {
        /* Primo campione: solo registrazione, niente publish. */
        st->distance_seen = true;
        st->last_distance_mm = new_mm;
        st->last_distance_pub_ms = now_ms;
        return false;
    }

    int32_t delta = new_mm - st->last_distance_mm;
    if (delta < 0)
        delta = -delta;

    /* Confronto sempre contro l'ultimo valore PUBBLICATO: il valore di
     * confronto resta identico finche' non si publica. */
    if (delta > HA_DISTANCE_DELTA_MM &&
        now_ms - st->last_distance_pub_ms >= HA_DISTANCE_MIN_INTERVAL_MS) {
        st->last_distance_mm = new_mm;
        st->last_distance_pub_ms = now_ms;
        return true;
    }
    return false;
}

bool ha_rate_periodic(ha_report_state_t *st, int64_t now_ms)
{
    if (st == NULL)
        return false;
    /* last_periodic_ms == 0 marca la prima chiamata (struct azzerata dal
     * caller); se la prima chiamata avviene a now_ms == 0 si memorizza -1,
     * altrimenti il marcatore resterebbe ambiguo alla chiamata successiva. */
    if (st->last_periodic_ms == 0) {
        st->last_periodic_ms = (now_ms != 0) ? now_ms : -1;
        return true;
    }
    if (now_ms - st->last_periodic_ms >= HA_PERIODIC_MS) {
        st->last_periodic_ms = now_ms;
        return true;
    }
    return false;
}
