/* Target integration of the net_mgr component (ESP-IDF, Fase 4).
 *
 * Wi-Fi STA owner: esp_netif + esp_wifi come up with the credentials the
 * provisioning flow already stored in NVS (WIFI_STORAGE_FLASH). This file
 * never writes SSID/password: it only round-trips the stored wifi_config_t
 * to apply the Kconfig listen_interval before the first start.
 *
 * Reconnects use the pure exponential backoff (net_backoff.c): every
 * WIFI_EVENT_STA_DISCONNECTED posts NET_EVENT_DOWN and arms a one-shot
 * esp_timer; IP_EVENT_STA_GOT_IP resets the attempt counter, posts
 * NET_EVENT_UP, starts mDNS (first time) and the diagnostic timer (30 s:
 * rssi/channel log + mDNS TXT refresh). Handlers and timer callbacks only
 * mutate a mutex-guarded snapshot, post events and arm timers: no sleep,
 * no busy loop, esp_wifi/esp_netif/mdns calls stay outside the lock.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "mdns.h"

#include "net_backoff.h"
#include "net_mgr.h"

ESP_EVENT_DEFINE_BASE(NET_EVENT);

static const char *TAG = "net";

/* "<prefix>-xxxxxx": 8 + 6 + 1 for the default prefix; a longer Kconfig
 * prefix is truncated by snprintf. */
#define NET_HOSTNAME_LEN 16u

/* mDNS TXT record: firmware fase (bumped per fase), refreshed by the diag
 * timer. */
#define NET_MDNS_FW "4"
#define NET_MDNS_SERVICE "_pumpkin"
#define NET_MDNS_PROTO "_tcp"
#define NET_MDNS_PORT 80

#if CONFIG_PUMPKIN_NET_PS_MIN_MODEM
#define NET_PS_MIN_MODEM_DEFAULT true
#else
#define NET_PS_MIN_MODEM_DEFAULT false
#endif

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_reconn_timer;
static esp_timer_handle_t s_diag_timer;
static esp_event_handler_instance_t s_wifi_inst;
static esp_event_handler_instance_t s_ip_inst;
static esp_netif_t *s_sta_netif;
static char s_hostname[NET_HOSTNAME_LEN];

static bool s_wifi_inited;    /* esp_wifi_init + mode/storage done once */
static bool s_handlers_added; /* default-loop registrations done once */
static bool s_started;        /* start() completed; stop() clears it */
static bool s_connecting;     /* esp_wifi_connect in flight */
static bool s_mdns_up;        /* mdns_init done, service registered */
static bool s_diag_running;   /* periodic diagnostic timer armed */
static uint32_t s_attempt;    /* reconnect counter, s_lock-guarded */

/* Diag snapshot: writers are the event task and the diag timer, always
 * under s_lock; readers copy under s_lock. */
static net_diag_t s_diag = {
    .connected = false,
    .rssi = 0,
    .channel = 0,
    .reconnects = 0,
    .ps_min_modem = NET_PS_MIN_MODEM_DEFAULT,
    .listen_interval = (uint8_t)CONFIG_PUMPKIN_NET_LISTEN_INTERVAL,
};

/* --- small helpers --- */

/* Hostname "<prefix>-xxxxxx" from the last 3 MAC STA bytes, lowercase. */
static void build_hostname(void)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_hostname, sizeof(s_hostname), "%s-%02x%02x%02x",
             CONFIG_PUMPKIN_NET_HOSTNAME_PREFIX, mac[3], mac[4], mac[5]);
}

/* One esp_wifi_connect in flight at a time: WIFI_EVENT_STA_START, the
 * reconnect timer and net_mgr_start() all funnel here. */
static void try_connect(void)
{
    bool go = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_started && !s_connecting && !s_diag.connected) {
        s_connecting = true;
        go = true;
    }
    xSemaphoreGive(s_lock);

    if (!go) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        return;
    }
    ESP_LOGW(TAG, "esp_wifi_connect failed (%s)", esp_err_to_name(err));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_connecting = false;
    xSemaphoreGive(s_lock);
}

/* --- mDNS (espressif/mdns managed component) --- */

/* TXT strings live on the stack of the caller and are copied by the mdns
 * API before it returns. */
typedef struct {
    char rssi[8];
    char ch[4];
    char recon[12];
    mdns_txt_item_t txt[4];
} mdns_txt_buf_t;

static void mdns_txt_fill(mdns_txt_buf_t *b, int8_t rssi, uint8_t channel,
                          uint32_t reconnects)
{
    snprintf(b->rssi, sizeof(b->rssi), "%d", (int)rssi);
    snprintf(b->ch, sizeof(b->ch), "%u", (unsigned)channel);
    snprintf(b->recon, sizeof(b->recon), "%lu", (unsigned long)reconnects);
    mdns_txt_item_t items[4] = {
        { "fw", NET_MDNS_FW },
        { "rssi", b->rssi },
        { "ch", b->ch },
        { "recon", b->recon },
    };
    memcpy(b->txt, items, sizeof(items));
}

static void mdns_publish_diag(int8_t rssi, uint8_t channel,
                              uint32_t reconnects)
{
    mdns_txt_buf_t buf;
    mdns_txt_fill(&buf, rssi, channel, reconnects);
    esp_err_t err = mdns_service_txt_set(NET_MDNS_SERVICE, NET_MDNS_PROTO,
                                         buf.txt, 4);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns TXT refresh failed (%s)", esp_err_to_name(err));
    }
}

/* First NET_EVENT_UP only: init, hostname/instance, then the service with
 * the diag known at connect time as initial TXT. The mdns_* calls are
 * short queued RPCs to the mdns service task and run outside s_lock. */
static esp_err_t mdns_ensure_started(void)
{
    if (s_mdns_up) {
        return ESP_OK;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed (%s)", esp_err_to_name(err));
        return err;
    }
    err = mdns_hostname_set(s_hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set failed (%s)", esp_err_to_name(err));
        return err;
    }
    err = mdns_instance_name_set(s_hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_instance_name_set failed (%s)",
                 esp_err_to_name(err));
        return err;
    }

    net_diag_t diag;
    net_mgr_get_diag(&diag);
    mdns_txt_buf_t buf;
    mdns_txt_fill(&buf, diag.rssi, diag.channel, diag.reconnects);
    err = mdns_service_add(NULL, NET_MDNS_SERVICE, NET_MDNS_PROTO,
                           NET_MDNS_PORT, buf.txt, 4);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_add failed (%s)", esp_err_to_name(err));
        return err;
    }

    s_mdns_up = true;
    ESP_LOGI(TAG, "mDNS: %s " NET_MDNS_SERVICE "." NET_MDNS_PROTO ":%u",
             s_hostname, (unsigned)NET_MDNS_PORT);
    return ESP_OK;
}

/* --- timers (esp_timer task context: quick, non-blocking) --- */

static void reconn_timer_cb(void *arg)
{
    (void)arg;
    try_connect(); /* no-op when stopped, already connected or connecting */
}

static void diag_timer_cb(void *arg)
{
    (void)arg;
    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t reconnects = s_diag.reconnects;
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        /* Not associated: the reconnect timer owns recovery. */
        ESP_LOGI(TAG, "diag: not associated (%s), reconnects=%lu",
                 esp_err_to_name(err), (unsigned long)reconnects);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_diag.rssi = ap.rssi;
    s_diag.channel = ap.primary;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "rssi=%d dBm ch=%u reconnects=%lu",
             (int)ap.rssi, (unsigned)ap.primary, (unsigned long)reconnects);
    mdns_publish_diag(ap.rssi, ap.primary, reconnects);
}

static esp_err_t timers_create(void)
{
    if (s_reconn_timer == NULL) {
        const esp_timer_create_args_t reconn_args = {
            .callback = reconn_timer_cb,
            .name = "net_reconn",
            .skip_unhandled_events = true,
        };
        esp_err_t err = esp_timer_create(&reconn_args, &s_reconn_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "reconnect timer create failed (%s)",
                     esp_err_to_name(err));
            return err;
        }
    }
    if (s_diag_timer == NULL) {
        const esp_timer_create_args_t diag_args = {
            .callback = diag_timer_cb,
            .name = "net_diag",
            .skip_unhandled_events = true,
        };
        esp_err_t err = esp_timer_create(&diag_args, &s_diag_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "diag timer create failed (%s)",
                     esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

/* --- esp_event handlers (default loop task: post/schedule only) --- */

static void on_sta_disconnected(const wifi_event_sta_disconnected_t *ev)
{
    const uint32_t rand_pct = esp_random() % 100u;
    uint32_t next_ms = 0;
    bool schedule = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_connecting = false;
    schedule = s_started;
    if (schedule) {
        s_diag.connected = false;
        s_diag.reconnects++;
        next_ms = net_backoff_compute(s_attempt++, rand_pct);
    }
    xSemaphoreGive(s_lock);

    if (!schedule) {
        /* stop() owns the link now (e.g. provisioning reset): no event,
         * no reconnect. */
        ESP_LOGI(TAG, "disconnected (reason=%d) while stopped", ev->reason);
        return;
    }

    net_down_t down = {
        .reason = ev->reason,
        .next_ms = next_ms,
    };
    esp_err_t err = esp_event_post(NET_EVENT, NET_EVENT_DOWN, &down,
                                   sizeof(down), 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NET_EVENT_DOWN post failed (%s)",
                 esp_err_to_name(err));
    }

    /* One-shot, never a busy loop: the backoff is ms, the timer is us. */
    (void)esp_timer_stop(s_reconn_timer);
    err = esp_timer_start_once(s_reconn_timer,
                               (uint64_t)next_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconnect timer start failed (%s)",
                 esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "disconnected (reason=%d), retry in %lu ms (attempt %lu)",
             ev->reason, (unsigned long)next_ms, (unsigned long)s_attempt);
}

static void on_got_ip(const ip_event_got_ip_t *ev)
{
    net_up_t up;
    esp_ip4addr_ntoa(&ev->ip_info.ip, up.ip, sizeof(up.ip));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_attempt = 0;
    s_connecting = false;
    const bool post = s_started;
    if (post) {
        s_diag.connected = true;
    }
    xSemaphoreGive(s_lock);

    if (!post) {
        return;
    }

    esp_err_t err = esp_event_post(NET_EVENT, NET_EVENT_UP, &up, sizeof(up),
                                   0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NET_EVENT_UP post failed (%s)", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "got IP %s", up.ip);

    if (mdns_ensure_started() == ESP_OK) {
        net_diag_t diag;
        net_mgr_get_diag(&diag);
        mdns_publish_diag(diag.rssi, diag.channel, diag.reconnects);
    }

    bool arm = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_diag_running) {
        s_diag_running = true;
        arm = true;
    }
    xSemaphoreGive(s_lock);
    if (arm) {
        err = esp_timer_start_periodic(s_diag_timer,
                                       (uint64_t)CONFIG_PUMPKIN_NET_DIAG_PERIOD_S *
                                           1000000ULL /* s -> us */);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "diag timer start failed (%s)",
                     esp_err_to_name(err));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        try_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *ev = event_data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_diag.channel = ev->channel;
        xSemaphoreGive(s_lock);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED:
        on_sta_disconnected(event_data);
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        on_got_ip(event_data);
    }
}

/* --- public API --- */

esp_err_t net_mgr_start(void)
{
    /* Init-once like the other components: called from app startup; the
     * started flag makes every subsequent call a no-op. */
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool already = s_started;
    xSemaphoreGive(s_lock);
    if (already) {
        return ESP_OK;
    }

    esp_err_t err = timers_create();
    if (err != ESP_OK) {
        return err;
    }

    /* NETIF + default loop: tolerated when already up (other components
     * may have initialized them first). */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed (%s)", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed (%s)",
                 esp_err_to_name(err));
        return err;
    }

    /* Adopt the default STA netif when another owner (e.g. the bench app
     * that drove provisioning) already created it: esp_netif_new rejects
     * a duplicated "WIFI_STA_DEF" key and the create helper asserts. */
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
            return ESP_FAIL;
        }
    }

    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed (%s)", esp_err_to_name(err));
            return err;
        }
        s_wifi_inited = true;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed (%s)",
                     esp_err_to_name(err));
            return err;
        }
        /* FLASH storage: credentials live in NVS as written by the
         * provisioning flow; nothing is written here. */
        err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_storage failed (%s)",
                     esp_err_to_name(err));
            return err;
        }
    }

    build_hostname();
    err = esp_netif_set_hostname(s_sta_netif, s_hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_set_hostname failed (%s)",
                 esp_err_to_name(err));
    }

    /* Round-trip the stored wifi_config_t untouched except for the Kconfig
     * listen_interval: SSID/password stay whatever provisioning stored. */
    wifi_config_t wifi_cfg;
    err = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
    if (err == ESP_OK) {
        wifi_cfg.sta.listen_interval =
            (uint16_t)CONFIG_PUMPKIN_NET_LISTEN_INTERVAL;
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "listen_interval apply failed (%s)",
                 esp_err_to_name(err));
    }

    if (!s_handlers_added) {
        err = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
            &s_wifi_inst);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "WIFI_EVENT register failed (%s)",
                     esp_err_to_name(err));
            return err;
        }
        err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL,
            &s_ip_inst);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "IP_EVENT register failed (%s)",
                     esp_err_to_name(err));
            return err;
        }
        s_handlers_added = true;
    }

    /* Before esp_wifi_start: WIFI_EVENT_STA_START may fire inside the call
     * and try_connect() refuses to run while !s_started. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_started = true;
    xSemaphoreGive(s_lock);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed (%s)", esp_err_to_name(err));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_started = false;
        xSemaphoreGive(s_lock);
        return err;
    }

#if CONFIG_PUMPKIN_NET_PS_MIN_MODEM
    err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    const bool ps_ok = (err == ESP_OK);
    if (!ps_ok) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(MIN_MODEM) failed (%s)",
                 esp_err_to_name(err));
    }
#else
    const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    const bool ps_ok = (ps_err == ESP_OK);
    if (!ps_ok) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) failed (%s)",
                 esp_err_to_name(ps_err));
    }
#endif
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_diag.ps_min_modem = ps_ok;
    xSemaphoreGive(s_lock);

    /* STA_START already triggered this; the guard keeps exactly one
     * esp_wifi_connect in flight. */
    try_connect();

    ESP_LOGI(TAG, "started (host %s, listen_interval %u, ps %s)",
             s_hostname, (unsigned)CONFIG_PUMPKIN_NET_LISTEN_INTERVAL,
             ps_ok ? "min modem" : "none");
    return ESP_OK;
}

esp_err_t net_mgr_stop(void)
{
    bool was_started = false;
    bool was_diag_running = false;

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        was_started = s_started;
        was_diag_running = s_diag_running;
        s_started = false;
        s_connecting = false;
        s_diag_running = false;
        s_diag.connected = false;
        s_attempt = 0;
        xSemaphoreGive(s_lock);
    }
    if (!was_started) {
        return ESP_OK;
    }

    (void)esp_timer_stop(s_reconn_timer);
    if (was_diag_running) {
        (void)esp_timer_stop(s_diag_timer);
    }
    if (s_mdns_up) {
        mdns_free(); /* also drops the _pumpkin service and the hostname */
        s_mdns_up = false;
    }
    /* Best effort: the provisioning flow owns the station from here; the
     * netif and the esp_wifi init stay alive for the next start(). */
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

void net_mgr_get_diag(net_diag_t *out)
{
    if (out == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_diag;
    xSemaphoreGive(s_lock);
}

esp_err_t net_mgr_set_power_save(bool min_modem)
{
    bool started = false;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        started = s_started;
        xSemaphoreGive(s_lock);
    }
    if (!started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_wifi_set_ps(min_modem ? WIFI_PS_MIN_MODEM
                                              : WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_ps(%s) failed (%s)",
                 min_modem ? "min modem" : "none", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_diag.ps_min_modem = min_modem;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "power save %s", min_modem ? "min modem" : "none");
    return ESP_OK;
}
