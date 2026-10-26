/* Pure TLV codec for the "mqtt" provisioning endpoint (host-tested).
 *
 * Wire format (little-endian, no padding):
 *   byte 0     : version, PROV_MQTT_TLV_VERSION (anything else is
 *                ESP_ERR_INVALID_ARG)
 *   then record*: type u8, len u16 LE, value[len]
 *
 * Record types: 1 host, 2 port (u16 LE, len must be 2), 3 user, 4 pass,
 * 5 tls (u8, len must be 1, value 0 or 1). Unknown types are skipped and
 * parsing continues. Record lengths running past the buffer, a truncated
 * trailing record, port/tls with the wrong length or a tls value > 1 are
 * all ESP_ERR_INVALID_ARG. Decoded strings are always NUL-terminated
 * (values longer than the destination are truncated to fit). Fields
 * absent from the payload keep has_* false and the destination zeroed.
 *
 * This file has no IDF dependency: it compiles on the host against the
 * stubs in host_tests/stub/ and on target as part of the component.
 */
#ifndef PROV_MQTT_CODEC_H
#define PROV_MQTT_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config_store.h" /* CONFIG_MQTT_HOST_LEN / _USER_LEN / _PASS_LEN */

#ifdef __cplusplus
extern "C" {
#endif

#define PROV_MQTT_TLV_VERSION 1

/* Record types on the wire. */
typedef enum {
    PROV_MQTT_TLV_HOST = 1,
    PROV_MQTT_TLV_PORT = 2, /* u16 LE          */
    PROV_MQTT_TLV_USER = 3,
    PROV_MQTT_TLV_PASS = 4,
    PROV_MQTT_TLV_TLS = 5,  /* u8, 0 or 1      */
} prov_mqtt_tlv_type_t;

/* Worst-case encoded size: version byte + one record per field with the
 * value length including the NUL (3 + 64, 3 + 2, 3 + 32, 3 + 64, 3 + 1).
 * Sizing hint for the ack buffer of wifi_prov_handle_mqtt_tlv(). */
#define PROV_MQTT_TLV_MAX_LEN \
    (1u + (3u + CONFIG_MQTT_HOST_LEN) + (3u + 2u) + \
     (3u + CONFIG_MQTT_USER_LEN) + (3u + CONFIG_MQTT_PASS_LEN) + (3u + 1u))

/* One decoded/decoded-to-be-sent MQTT payload. Strings share the
 * config_store limits and are NUL-terminated after decode. */
typedef struct {
    bool has_host;
    bool has_port;
    bool has_user;
    bool has_pass;
    bool has_tls;
    char host[CONFIG_MQTT_HOST_LEN];
    uint16_t port;
    char user[CONFIG_MQTT_USER_LEN];
    char pass[CONFIG_MQTT_PASS_LEN];
    bool tls;
} prov_mqtt_msg_t;

/* Parse buf/len into out (zeroed first; last record of a type wins).
 * ESP_ERR_INVALID_ARG for NULL args, empty buffer, wrong version or any
 * malformed record. */
esp_err_t prov_mqtt_decode(const uint8_t *buf, size_t len, prov_mqtt_msg_t *out);

/* Serialize the fields with has_* set, in type order; strings are written
 * NUL-included. buf/out_len: *out_len receives the encoded size. Returns
 * ESP_ERR_INVALID_ARG for NULL args or a non-terminated string, and
 * ESP_ERR_NO_MEM when buf is smaller than the encoded size. */
esp_err_t prov_mqtt_encode(const prov_mqtt_msg_t *in, uint8_t *buf,
                           size_t buf_len, size_t *out_len);

/* Proof of possession from the device: "pump-" plus the last 3 MAC STA
 * bytes in uppercase hex, e.g. mac {.., 0x01, 0x02, 0x03} -> "pump-010203"
 * (12 chars + NUL, always fills out[13]). NULL args leave out untouched. */
void prov_pop_from_mac(const uint8_t mac[6], char out[13]);

#ifdef __cplusplus
}
#endif
#endif /* PROV_MQTT_CODEC_H */
