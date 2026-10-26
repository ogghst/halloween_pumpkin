/* Public API of the wifi_prov component (pumpkin firmware, Fase 4).
 *
 * Wraps the ESP-IDF provisioning manager (wifi_provisioning) behind the
 * pumpkin conventions: service name and proof-of-possession are derived at
 * runtime from the MAC (no secrets in the sources), the Wi-Fi lifecycle
 * stays with net_mgr, and the MQTT broker parameters reach config_store
 * through a custom "mqtt" provisioning endpoint (TLV codec in
 * prov_mqtt_codec.h, host-tested).
 *
 * Expected call order (the caller owns the chip-level init):
 *   1. config_store_init (NVS ready), esp_netif_init, default event loop,
 *      default STA netif, esp_wifi_init.
 *   2. wifi_prov_app_init(): registers the manager event bridge on the
 *      default loop, creates the internal timeout timer, releases the BT
 *      memory right away on the SoftAP scheme and starts the provision
 *      button poll task.
 *   3. wifi_prov_credentials_present(): false -> wifi_prov_begin();
 *      true -> net_mgr_start() (app decision, not done here).
 *
 * All PROV_EVENT_* ids are posted on the default esp_event loop. Only
 * PROV_EVENT_SUCCESS carries a payload (prov_result_t). Handlers must not
 * block: they run in the default loop task. No Wi-Fi or MQTT password is
 * ever logged by this component.
 */
#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(PROV_EVENT);

/* Event ids posted on PROV_EVENT. */
typedef enum {
    PROV_EVENT_STARTED = 0, /* no payload                              */
    PROV_EVENT_SUCCESS,     /* event_data: prov_result_t               */
    PROV_EVENT_TIMEOUT,     /* no payload; manager stop was requested  */
    PROV_EVENT_FAILED,      /* no payload; credentials rejected        */
} prov_event_t;

/* Payload of PROV_EVENT_SUCCESS: the SSID just provisioned, NUL-terminated. */
typedef struct {
    char ssid[33];
} prov_result_t;

/* One-time setup, idempotent (ESP_OK on later calls). Preconditions (not
 * checked here): NVS ready (config_store_init), esp_netif_init + default
 * event loop created, default STA netif created, esp_wifi_init done. Logs
 * the free-heap baseline under tag "prov". On the SoftAP scheme it also
 * releases the BT controller memory immediately and marks it released;
 * on the BLE scheme nothing is freed here (the manager owns BT until END).
 * Starts the internal provision button poll task (Kconfig GPIO). */
esp_err_t wifi_prov_app_init(void);

/* True when Wi-Fi credentials are stored in NVS: thin wrapper of
 * wifi_prov_mgr_is_provisioned (needs esp_wifi_init, not the manager). */
esp_err_t wifi_prov_credentials_present(bool *present);

/* Start the provisioning service (security 1, PoP from MAC, service name
 * "<PUMPKIN_PROV_SERVICE_PREFIX>-XXXXXX"). ESP_ERR_INVALID_STATE when not
 * initialized or already active. On the BLE scheme, if the BT memory was
 * already released by a previous provisioning run, logs and reboots
 * (esp_restart): after the restart the boot decision reruns with the
 * credentials now in NVS. Before starting, stops net_mgr and erases the
 * stored Wi-Fi credentials (esp_wifi_restore); the MQTT parameters live
 * in the config_store namespace and survive. Arms the one-shot timeout
 * timer (PUMPKIN_PROV_TIMEOUT_S). */
esp_err_t wifi_prov_begin(void);

/* Stop an active provisioning service without waiting for the timeout:
 * disarms the timer and asks the manager to stop; the cleanup completes
 * asynchronously in the WIFI_PROV_END handler. ESP_ERR_INVALID_STATE when
 * nothing is active. Posts no event by itself. */
esp_err_t wifi_prov_abort(void);

/* True between a successful wifi_prov_begin() and the manager END event. */
bool wifi_prov_is_active(void);

/* True once the BT memory is gone for the rest of the session: after END
 * on the BLE scheme (the manager frees it in deinit), or immediately on
 * the SoftAP scheme. wifi_prov_begin() refuses to run BLE again when set. */
bool wifi_prov_bt_released(void);

/* Merge an MQTT TLV payload (prov_mqtt_codec.h format) into config_store:
 * decode -> merge over the current config_mqtt_t -> config_store_set_mqtt
 * (which posts CONFIG_EVENT_CHANGED {CFG_PARAM_MQTT} and schedules the
 * debounced NVS commit). When out/out_len are both non-NULL, *out_len must
 * hold the buffer size on input and an ack TLV with the complete current
 * values is written to out (at most PROV_MQTT_TLV_MAX_LEN bytes). Usable
 * from the console for bench tests, not only from the endpoint handler. */
esp_err_t wifi_prov_handle_mqtt_tlv(const uint8_t *in, size_t in_len,
                                    uint8_t *out, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* WIFI_PROV_H */
