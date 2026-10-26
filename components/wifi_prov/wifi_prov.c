/* Target integration of the wifi_prov component (ESP-IDF, Fase 4).
 *
 * Bridge between the Espressif provisioning manager and the pumpkin
 * conventions (see include/wifi_prov.h): events are re-posted on the
 * default loop as PROV_EVENT_*, service name/PoP come from the MAC at
 * runtime, the "mqtt" endpoint merges TLV payloads into config_store,
 * a one-shot esp_timer bounds the session, and BT memory is released as
 * soon as it is no longer needed. The Wi-Fi lifecycle stays with
 * net_mgr: begin() only stops the link and wipes the stored credentials.
 * No Wi-Fi or MQTT password is ever logged.
 */
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if CONFIG_BT_ENABLED
#include "esp_bt.h" /* esp_bt_controller_mem_release (SoftAP early free) */
#endif
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "config_store.h"
#include "net_mgr.h"
#include "prov_mqtt_codec.h"
#include "wifi_prov.h"

#if CONFIG_PUMPKIN_PROV_SCHEME_BLE
#include "wifi_provisioning/scheme_ble.h"
#define PROV_TRANSPORT_NAME "ble"
#elif CONFIG_PUMPKIN_PROV_SCHEME_SOFTAP
#include "wifi_provisioning/scheme_softap.h"
#define PROV_TRANSPORT_NAME "softap"
#else
#error "PUMPKIN_PROV_SCHEME choice is not set"
#endif

#include "wifi_provisioning/manager.h"

ESP_EVENT_DEFINE_BASE(PROV_EVENT);

static const char *TAG = "prov";

#define PROV_EP_MQTT "mqtt"

/* "<prefix>-XXXXXX": Kconfig prefix + '-' + 3 hex bytes + NUL. The Kconfig
 * string literal bounds the prefix at compile time. */
#define PROV_SERVICE_NAME_LEN (sizeof(CONFIG_PUMPKIN_PROV_SERVICE_PREFIX) + 7u)

/* Internal cross-file hook: task starter implemented in prov_button.c,
 * called once by wifi_prov_app_init() (kept out of the public header). */
esp_err_t prov_button_start(void);

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timeout_timer;
static bool s_inited;      /* app_init completed once            */
static bool s_active;      /* begin() OK, END not yet handled    */
static bool s_bt_released; /* BT memory gone for this session    */
static char s_service_name[PROV_SERVICE_NAME_LEN];
static char s_pop[13];

static void prov_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void prov_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* --- BT memory ownership --- */

/* SoftAP scheme only: BT is never needed, free the controller memory
 * (BSS/data and DMA-capable portions) before any BT init could happen.
 * With the controller disabled in the build there is nothing to free.
 * On the BLE scheme this is a no-op: the manager owns BT until END. */
static void release_bt_early(void)
{
#if CONFIG_PUMPKIN_PROV_SCHEME_SOFTAP
#if CONFIG_BT_ENABLED
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_bt_released = true;
        ESP_LOGI(TAG, "BT memory released early, free heap %lu",
                 (unsigned long)esp_get_free_heap_size());
    } else {
        ESP_LOGW(TAG, "esp_bt_controller_mem_release: %s",
                 esp_err_to_name(err));
    }
#else
    s_bt_released = true; /* no BT stack in this build */
    ESP_LOGI(TAG, "no BT in build, nothing to release");
#endif
#endif
}

/* Called from the manager END path. On BLE the FREE_BTDM scheme handler
 * has already released controller+BTDM inside wifi_prov_mgr_deinit()
 * (verified in wifi_provisioning/src/scheme_ble.c): here we only record
 * it and log the heap. On SoftAP it was released in app_init. */
static void release_bt(void)
{
    prov_lock();
    bool was = s_bt_released;
    s_bt_released = true;
    prov_unlock();
    if (!was) {
        ESP_LOGI(TAG, "BT memory released, free heap %lu",
                 (unsigned long)esp_get_free_heap_size());
    }
}

/* --- one-shot session timeout (esp_timer task context) --- */

static void prov_timeout_cb(void *arg)
{
    (void)arg;
    prov_lock();
    bool active = s_active;
    prov_unlock();
    if (!active) {
        return;
    }
    ESP_LOGW(TAG, "provisioning timeout (%d s), stopping",
             CONFIG_PUMPKIN_PROV_TIMEOUT_S);
    esp_event_post(PROV_EVENT, PROV_EVENT_TIMEOUT, NULL, 0, 0);
    /* The manager emits WIFI_PROV_END; the handler below does the rest. */
    wifi_prov_mgr_stop_provisioning();
}

static void timeout_disarm(void)
{
    esp_timer_stop(s_timeout_timer); /* ESP_ERR_INVALID_STATE if idle */
}

/* --- manager event bridge (default loop task) --- */

static void prov_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "manager: provisioning service started");
        esp_event_post(PROV_EVENT, PROV_EVENT_STARTED, NULL, 0, 0);
        break;

    case WIFI_PROV_CRED_RECV: {
        const wifi_sta_config_t *sta = (const wifi_sta_config_t *)event_data;
        ESP_LOGI(TAG, "credentials received: ssid='%s' pass_len=%u",
                 (const char *)sta->ssid,
                 (unsigned)strnlen((const char *)sta->password,
                                   sizeof(sta->password)));
        break;
    }

    case WIFI_PROV_CRED_FAIL: {
        const wifi_prov_sta_fail_reason_t *reason =
            (const wifi_prov_sta_fail_reason_t *)event_data;
        ESP_LOGE(TAG, "credentials failed: %s",
                 (*reason == WIFI_PROV_STA_AUTH_ERROR)
                     ? "authentication error"
                     : "AP not found");
        timeout_disarm();
        esp_event_post(PROV_EVENT, PROV_EVENT_FAILED, NULL, 0, 0);
        break;
    }

    case WIFI_PROV_CRED_SUCCESS: {
        /* The manager posts this without payload: read back the SSID
         * from the station config that just connected. */
        wifi_config_t cfg = { 0 };
        prov_result_t res = { 0 };
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            strncpy(res.ssid, (const char *)cfg.sta.ssid,
                    sizeof(res.ssid) - 1u);
        }
        ESP_LOGI(TAG, "provisioning successful, ssid='%s'", res.ssid);
        timeout_disarm();
        esp_event_post(PROV_EVENT, PROV_EVENT_SUCCESS, &res,
                       sizeof(res), 0);
        break;
    }

    case WIFI_PROV_END:
        timeout_disarm();
        /* Emits WIFI_PROV_DEINIT synchronously: on BLE the scheme
         * handler frees the BTDM memory inside this call. */
        wifi_prov_mgr_deinit();
        release_bt();
        prov_lock();
        s_active = false;
        prov_unlock();
        break;

    default:
        break;
    }
}

/* --- "mqtt" custom endpoint (protocomm request context) --- */

static esp_err_t mqtt_ep_handler(uint32_t session_id, const uint8_t *inbuf,
                                 ssize_t inlen, uint8_t **outbuf,
                                 ssize_t *outlen, void *priv_data)
{
    (void)session_id;
    (void)priv_data;
    *outbuf = NULL;
    *outlen = 0;
    if (!inbuf || inlen < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The protocomm layer frees the response buffer after sending it:
     * worst-case TLV ack is PROV_MQTT_TLV_MAX_LEN bytes. */
    uint8_t *ack = malloc(PROV_MQTT_TLV_MAX_LEN);
    if (!ack) {
        ESP_LOGE(TAG, "no memory for mqtt ack");
        return ESP_ERR_NO_MEM;
    }
    size_t ack_len = 0;
    esp_err_t err = wifi_prov_handle_mqtt_tlv(inbuf, (size_t)inlen,
                                              ack, &ack_len);
    if (err != ESP_OK) {
        free(ack);
        ESP_LOGW(TAG, "mqtt endpoint rejected payload: %s",
                 esp_err_to_name(err));
        return err;
    }
    *outbuf = ack;
    *outlen = (ssize_t)ack_len;
    return ESP_OK;
}

/* --- public API --- */

esp_err_t wifi_prov_app_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    prov_lock();
    if (s_inited) {
        prov_unlock();
        return ESP_OK;
    }

    esp_timer_create_args_t timer_args = {
        .callback = prov_timeout_cb,
        .name = "prov_timeout",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_timeout_timer);
    if (err != ESP_OK) {
        prov_unlock();
        ESP_LOGE(TAG, "esp_timer_create: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                     prov_event_handler, NULL);
    if (err != ESP_OK) {
        esp_timer_delete(s_timeout_timer);
        prov_unlock();
        ESP_LOGE(TAG, "handler register: %s", esp_err_to_name(err));
        return err;
    }

    release_bt_early();
    s_inited = true;
    prov_unlock();

    /* Never fails the init if the button task cannot start: provisioning
     * stays reachable through the app logic. */
    prov_button_start();

    ESP_LOGI(TAG, "wifi_prov ready: scheme=%s free_heap=%lu",
             PROV_TRANSPORT_NAME,
             (unsigned long)esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t wifi_prov_credentials_present(bool *present)
{
    if (!present) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Reads the station config in NVS: needs esp_wifi_init (app duty),
     * not the provisioning manager. */
    return wifi_prov_mgr_is_provisioned(present);
}

esp_err_t wifi_prov_begin(void)
{
    prov_lock();
    if (!s_inited || s_active) {
        prov_unlock();
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_PUMPKIN_PROV_SCHEME_BLE
    if (s_bt_released) {
        prov_unlock();
        /* The BLE controller memory is gone for good: a reboot reruns the
         * boot decision with the credentials currently in NVS. */
        ESP_LOGE(TAG, "BT already released: reboot to redo provisioning");
        esp_restart();
    }
#endif

    /* Stop the link and erase the stored Wi-Fi credentials; the MQTT
     * parameters live in the config_store namespace and survive. */
    esp_err_t err = net_mgr_stop();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "net_mgr_stop: %s", esp_err_to_name(err));
    }
    err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_wifi_restore: %s", esp_err_to_name(err));
    }

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    (void)snprintf(s_service_name, sizeof(s_service_name), "%s-%02X%02X%02X",
                   CONFIG_PUMPKIN_PROV_SERVICE_PREFIX, mac[3], mac[4], mac[5]);
    prov_pop_from_mac(mac, s_pop);

    wifi_prov_mgr_config_t mgr_cfg = {
#if CONFIG_PUMPKIN_PROV_SCHEME_BLE
        .scheme = wifi_prov_scheme_ble,
        /* Manager releases controller+BTDM memory on deinit. */
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
#else
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
#endif
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        /* 0 = legacy behavior: no automatic retry loop, the manager
         * reports WIFI_PROV_CRED_FAIL on the first failed association. */
        .wifi_prov_conn_cfg = { .wifi_conn_attempts = 0 },
    };
    err = wifi_prov_mgr_init(mgr_cfg);
    if (err != ESP_OK) {
        prov_unlock();
        ESP_LOGE(TAG, "wifi_prov_mgr_init: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_prov_mgr_endpoint_create(PROV_EP_MQTT);
    if (err != ESP_OK) {
        wifi_prov_mgr_deinit();
        prov_unlock();
        ESP_LOGE(TAG, "endpoint create: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,
                                           (const void *)s_pop,
                                           s_service_name, NULL);
    if (err != ESP_OK) {
        wifi_prov_mgr_deinit();
        prov_unlock();
        ESP_LOGE(TAG, "start_provisioning: %s", esp_err_to_name(err));
        return err;
    }

    /* Registering is only allowed once the service is running. */
    err = wifi_prov_mgr_endpoint_register(PROV_EP_MQTT, mqtt_ep_handler,
                                          NULL);
    if (err != ESP_OK) {
        wifi_prov_mgr_stop_provisioning();
        prov_unlock();
        ESP_LOGE(TAG, "endpoint register: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_once(s_timeout_timer,
                               (uint64_t)CONFIG_PUMPKIN_PROV_TIMEOUT_S *
                                   1000000ULL);
    if (err != ESP_OK) {
        wifi_prov_mgr_stop_provisioning();
        prov_unlock();
        ESP_LOGE(TAG, "timeout arm: %s", esp_err_to_name(err));
        return err;
    }

    s_active = true;
    prov_unlock();

    ESP_LOGI(TAG, "provisioning: transport=%s service=%s pop=%s heap=%lu",
             PROV_TRANSPORT_NAME, s_service_name, s_pop,
             (unsigned long)esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t wifi_prov_abort(void)
{
    prov_lock();
    if (!s_active) {
        prov_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    prov_unlock();

    timeout_disarm();
    /* END (and the cleanup in its handler) arrives asynchronously. */
    wifi_prov_mgr_stop_provisioning();
    return ESP_OK;
}

bool wifi_prov_is_active(void)
{
    prov_lock();
    bool active = s_active;
    prov_unlock();
    return active;
}

bool wifi_prov_bt_released(void)
{
    prov_lock();
    bool released = s_bt_released;
    prov_unlock();
    return released;
}

esp_err_t wifi_prov_handle_mqtt_tlv(const uint8_t *in, size_t in_len,
                                    uint8_t *out, size_t *out_len)
{
    if (!in || in_len < 1u || (out == NULL) != (out_len == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    prov_mqtt_msg_t msg;
    esp_err_t err = prov_mqtt_decode(in, in_len, &msg);
    if (err != ESP_OK) {
        return err;
    }

    config_mqtt_t next;
    err = config_store_get_mqtt(&next); /* config_store_init required */
    if (err != ESP_OK) {
        return err;
    }

    if (msg.has_host) {
        memcpy(next.host, msg.host, sizeof(next.host));
    }
    if (msg.has_port) {
        next.port = msg.port;
    }
    if (msg.has_user) {
        memcpy(next.user, msg.user, sizeof(next.user));
    }
    if (msg.has_pass) {
        memcpy(next.pass, msg.pass, sizeof(next.pass));
    }
    if (msg.has_tls) {
        next.tls = msg.tls;
    }

    /* Validates and posts CONFIG_EVENT_CHANGED {CFG_PARAM_MQTT}. */
    err = config_store_set_mqtt(&next);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "mqtt config: host='%s' port=%u user='%s' pass_len=%u tls=%d",
             next.host, next.port, next.user,
             (unsigned)strnlen(next.pass, sizeof(next.pass)),
             next.tls ? 1 : 0);

    if (out) {
        /* Ack = the complete current (stored) values. */
        config_mqtt_t now;
        err = config_store_get_mqtt(&now);
        if (err != ESP_OK) {
            return err;
        }
        prov_mqtt_msg_t ack = {
            .has_host = true,
            .has_port = true,
            .has_user = true,
            .has_pass = true,
            .has_tls = true,
            .port = now.port,
            .tls = now.tls,
        };
        memcpy(ack.host, now.host, sizeof(ack.host));
        memcpy(ack.user, now.user, sizeof(ack.user));
        memcpy(ack.pass, now.pass, sizeof(ack.pass));
        return prov_mqtt_encode(&ack, out, *out_len, out_len);
    }
    return ESP_OK;
}
