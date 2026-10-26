/* Minimal esp_event.h stub for host builds of the wifi_prov pure core:
 * prov_mqtt_codec.h only pulls config_store.h, which declares the
 * CONFIG_EVENT base. The real header comes from ESP-IDF on target builds. */
#ifndef STUB_ESP_EVENT_H
#define STUB_ESP_EVENT_H

#include "esp_err.h"

typedef const char *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t id

#endif /* STUB_ESP_EVENT_H */
