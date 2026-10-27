/* Stub esp_timer.h per gli host test: solo i tipi e le firme usati da
 * config_store.c. L'implementazione finta sta in stubs.c (il timer non
 * scatta mai). L'header reale arriva da ESP-IDF nei build su target. */
#ifndef STUB_ESP_TIMER_H
#define STUB_ESP_TIMER_H

#include <stdint.h>

#include "esp_err.h"

typedef void *esp_timer_handle_t;

/* Solo i campi usati dagli initializer designati di config_store.c; il
 * resto e' azzerato. */
typedef struct {
    void (*callback)(void *arg);
    const char *name;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *out_timer);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);

#endif /* STUB_ESP_TIMER_H */
