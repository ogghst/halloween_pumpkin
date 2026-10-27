/* Minimal esp_event.h stub for host builds: config_store.h declares the
 * CONFIG_EVENT base and the IDF layer calls esp_event_post; the stub
 * implementation in stubs.c just counts the posts so tests can assert
 * on them. The real header comes from ESP-IDF on target builds. */
#ifndef STUB_ESP_EVENT_H
#define STUB_ESP_EVENT_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef const char *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t id
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t id = (esp_event_base_t)0

/* Contatori del post finto (vedi stubs.c), leggibili dai test. */
extern int stub_event_post_count;
extern int32_t stub_event_last_id;

/* Firma identica a ESP-IDF ma con TickType_t reso come uint32_t: il
 * core puro include questo stub senza il FreeRTOS finto, e nei build su
 * target l'header reale di esp_event sostituisce comunque questo. */
esp_err_t esp_event_post(esp_event_base_t event_base, int32_t event_id,
                         const void *event_data, size_t event_data_size,
                         uint32_t ticks_to_wait);

#endif /* STUB_ESP_EVENT_H */
