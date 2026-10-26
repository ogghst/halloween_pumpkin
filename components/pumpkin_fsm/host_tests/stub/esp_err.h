/* Minimal esp_err.h stub for host builds of the pumpkin_fsm pure core:
 * pumpkin_fsm.h needs the esp_err_t type and the result codes used by
 * fsm_core.h/fsm_core.c. The real header comes from ESP-IDF on target
 * builds; values mirror ESP-IDF v5.4 esp_err.h. */
#ifndef STUB_ESP_ERR_H
#define STUB_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

#endif /* STUB_ESP_ERR_H */
