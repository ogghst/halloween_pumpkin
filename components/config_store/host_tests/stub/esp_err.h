/* Minimal esp_err.h stub for host builds of the pure config core:
 * include/config_store.h and config_store_core.c need only esp_err_t and
 * the result codes below (same values as ESP-IDF). The real header comes
 * from ESP-IDF on target builds. */
#ifndef STUB_ESP_ERR_H
#define STUB_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107

/* Stub usato dai log del layer IDF (implementato in stubs.c). */
const char *esp_err_to_name(esp_err_t code);

#endif /* STUB_ESP_ERR_H */
