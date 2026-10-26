/* Minimal esp_err.h stub for host builds of the wifi_prov pure core:
 * prov_mqtt_codec.h needs the esp_err_t type and the result codes used by
 * the codec. The real header comes from ESP-IDF on target builds; values
 * mirror ESP-IDF v5.4 esp_err.h. */
#ifndef STUB_ESP_ERR_H
#define STUB_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106

#endif /* STUB_ESP_ERR_H */
