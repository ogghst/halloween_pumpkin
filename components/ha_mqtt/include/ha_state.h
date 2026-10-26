#pragma once
/* Stato di report e rate-limit (puro). */
#include <stdbool.h>
#include <stdint.h>

#define HA_DISTANCE_DELTA_MM 50
#define HA_DISTANCE_MIN_INTERVAL_MS 1000
#define HA_PERIODIC_MS 60000

typedef struct {
    int32_t last_distance_mm;
    int64_t last_distance_pub_ms;
    int64_t last_periodic_ms;
    bool distance_seen;
} ha_report_state_t;

/* true se pubblicare: |delta| > HA_DISTANCE_DELTA_MM E trascorsi >= 1000 ms */
bool ha_rate_distance(ha_report_state_t *st, int32_t new_mm, int64_t now_ms);
/* true una volta ogni HA_PERIODIC_MS; alla prima chiamata restituisce true */
bool ha_rate_periodic(ha_report_state_t *st, int64_t now_ms);
