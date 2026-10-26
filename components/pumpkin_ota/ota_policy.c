/* Politica di gate OTA (pura): nessuna dipendenza ESP-IDF, host-testabile.
 * I motivi di rifiuto sono stringhe stabili del contratto MQTT
 * (evento rejected su update/status): non rinominarli senza aggiornare
 * il consumer. */
#include "ota_policy.h"
#include <stdio.h>
#include <string.h>

static const char k_https_prefix[] = "https://";

static int reject(char *reason, size_t reason_len, const char *r)
{
    if (reason != NULL && reason_len > 0) {
        snprintf(reason, reason_len, "%s", r);
    }
    return -1;
}

static bool url_ok(const char *url, size_t url_max)
{
    if (url == NULL || url[0] == '\0') {
        return false;
    }
    if (strncmp(url, k_https_prefix, sizeof(k_https_prefix) - 1) != 0) {
        return false;
    }
    /* url_max e' la dimensione del buffer inclusa il NUL: un URL senza lo
     * spazio per il terminatore e' considerato troppo lungo. */
    if (strlen(url) + 1 > url_max) {
        return false;
    }
    return true;
}

int pumpkin_ota_policy_check(const pumpkin_ota_policy_input_t *in,
                             char *reason, size_t reason_len)
{
    if (in == NULL) {
        return reject(reason, reason_len, "bad_url");
    }
    /* Contratto: warn_battery_unknown e' output pur essendo nel struct di
     * input (firma const). L'oggetto puntato non e' mai const-qualified
     * (campo locale del chiamante). */
    ((pumpkin_ota_policy_input_t *)(void *)in)->warn_battery_unknown =
        (in->battery_pct < 0);
    if (!url_ok(in->url, in->url_max)) {
        return reject(reason, reason_len, "bad_url");
    }
    if (in->battery_pct >= 0 && in->battery_pct < PUMPKIN_OTA_BATTERY_MIN_PCT) {
        return reject(reason, reason_len, "battery_low");
    }
    if (in->led_current_ma > in->max_led_ma) {
        return reject(reason, reason_len, "led_full_light");
    }
    if (in->free_heap < in->min_free_heap || in->max_block < in->min_free_heap) {
        return reject(reason, reason_len, "heap_low");
    }
    return 0;
}

/* Salta una 'v' iniziale (es. "v1.2.0" <-> "1.2.0"). */
static const char *strip_v(const char *s)
{
    if (s != NULL && (s[0] == 'v' || s[0] == 'V')) {
        s++;
    }
    return s;
}

int pumpkin_ota_policy_version_changed(const char *running, const char *incoming)
{
    if (running == NULL || incoming == NULL) {
        return -1;
    }
    const char *r = strip_v(running);
    const char *i = strip_v(incoming);
    return (strcmp(r, i) == 0) ? -1 : 0;
}

size_t pumpkin_ota_policy_event_json(char *out, size_t cap,
                                     const pumpkin_ota_event_t *ev)
{
    if (out == NULL || cap == 0 || ev == NULL || ev->event_str == NULL) {
        return 0;
    }

    size_t off = 0;
    /* Scrive un pezzo e verifica ogni volta che non ci sia stato
     * troncamento: in caso di cap insufficiente esce con 0. */
    #define APPEND(...)                                                    \
        do {                                                               \
            int _n = snprintf(out + off, cap - off, __VA_ARGS__);          \
            if (_n < 0 || (size_t)_n >= cap - off) {                       \
                return 0;                                                  \
            }                                                              \
            off += (size_t)_n;                                             \
        } while (0)

    APPEND("{\"event\":\"%s\"", ev->event_str);
    if (ev->detail != NULL) {
        APPEND(",\"detail\":\"%s\"", ev->detail);
    }
    if (ev->installed != NULL) {
        APPEND(",\"installed\":\"%s\"", ev->installed);
    }
    APPEND(",\"free_heap\":%lu,\"max_block\":%lu,\"progress\":%d",
           (unsigned long)ev->free_heap, (unsigned long)ev->max_block,
           ev->progress);
    if (ev->battery_unknown) {
        APPEND(",\"battery\":\"unknown\"");
    }
    APPEND("}");

    #undef APPEND
    return off;
}
