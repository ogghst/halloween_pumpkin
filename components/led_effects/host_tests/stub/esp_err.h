/* Minimal esp_err.h stub for host builds of the led_effects pure sources:
 * include/led_fx.h needs the esp_err_t type and the esp_event base
 * declarations (stub/esp_event.h) to parse; the real ones come from
 * ESP-IDF on target builds. */
#ifndef STUB_ESP_ERR_H
#define STUB_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

/* Costanti di esito identiche a esp_err.h di ESP-IDF (le fonti pure del
 * componente restituiscono gli stessi codici su host e su target). */
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107

#endif /* STUB_ESP_ERR_H */
