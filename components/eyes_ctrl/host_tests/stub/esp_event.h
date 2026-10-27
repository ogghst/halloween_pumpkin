/* Minimal esp_event.h stub for host builds of the pure eyes core:
 * pumpkin_fsm.h only declares the FSM_EVENT base. The real header
 * comes from ESP-IDF on target builds. */
#ifndef STUB_ESP_EVENT_H
#define STUB_ESP_EVENT_H

#include "esp_err.h"

typedef const char *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t id
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t id = (esp_event_base_t)0

#endif /* STUB_ESP_EVENT_H */
