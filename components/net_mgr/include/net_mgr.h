/* Public API of the net_mgr component (pumpkin firmware, Fase 4).
 *
 * Owns the Wi-Fi STA lifecycle: brings up esp_netif + esp_wifi with the
 * credentials already stored in NVS by the provisioning flow, reconnects
 * with an exponential backoff (pure core net_backoff.c, host-tested in
 * host_tests/), posts NET_EVENT_UP/NET_EVENT_DOWN on the default esp_event
 * loop, serves a diagnostic snapshot (net_mgr_get_diag) and advertises the
 * board over mDNS (_pumpkin._tcp) while connected. Event handlers and
 * timers only post/schedule: nothing blocks, no busy loops, and no SSID or
 * password is ever written by this component.
 */
#ifndef NET_MGR_H
#define NET_MGR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(NET_EVENT);

/* Event ids posted on NET_EVENT. */
typedef enum {
    NET_EVENT_UP = 0, /* event_data: net_up_t   */
    NET_EVENT_DOWN,   /* event_data: net_down_t */
} net_event_t;

/* Payload of NET_EVENT_UP: station IPv4 address as a dotted string. */
typedef struct {
    char ip[16];
} net_up_t;

/* Payload of NET_EVENT_DOWN. */
typedef struct {
    int reason;       /* disconnect reason (wifi_err_reason_t) */
    uint32_t next_ms; /* scheduled reconnect delay */
} net_down_t;

/* Diagnostic snapshot, served by copy. `reconnects` counts the handled
 * disconnects since net_mgr_start; rssi/channel are the latest samples of
 * the diagnostic timer (0 until the first one fires). */
typedef struct {
    bool connected;
    int8_t rssi;             /* dBm */
    uint8_t channel;         /* primary channel, 0 when not associated */
    uint32_t reconnects;
    bool ps_min_modem;       /* current Wi-Fi power-save mode */
    uint8_t listen_interval; /* active listen_interval (beacons) */
} net_diag_t;

/* Bring the station up. Idempotent: creates the default STA netif and
 * initializes esp_wifi once, sets hostname "<prefix>-xxxxxx" (Kconfig
 * prefix + last 3 MAC STA bytes in lowercase hex), writes the Kconfig
 * listen_interval into wifi_config_t.sta, starts Wi-Fi and connects with
 * whatever credentials provisioning stored in NVS. */
esp_err_t net_mgr_start(void);

/* Stop/disconnect (used by the provisioning reset flow): cancels the
 * reconnect and diagnostic timers, tears down mDNS and stops the station.
 * The STA netif and the esp_wifi init stay alive so net_mgr_start() can
 * bring the link back without redoing the one-time setup. */
esp_err_t net_mgr_stop(void);

/* Thread-safe copy of the diagnostic snapshot; no-op when out is NULL. */
void net_mgr_get_diag(net_diag_t *out);

/* Runtime power-save toggle for bench latency checks: WIFI_PS_MIN_MODEM
 * when min_modem, WIFI_PS_NONE otherwise. ESP_ERR_INVALID_STATE when the
 * manager is not started. */
esp_err_t net_mgr_set_power_save(bool min_modem);

/* --- Pure core (host-tested; see components/net_mgr/host_tests) --- */

/* Reconnect delay for `attempt` (0-based): base 1000 ms doubled per
 * attempt, deterministic jitter in [-15%, +15%] driven by rand_pct (0..99:
 * 0 -> -15%, 99 -> +15%), final result capped at
 * CONFIG_PUMPKIN_NET_BACKOFF_MAX_MS. Huge attempts terminate immediately
 * at the cap. */
uint32_t net_backoff_compute(uint32_t attempt, uint32_t rand_pct);

#ifdef __cplusplus
}
#endif
#endif /* NET_MGR_H */
