/* Stub esp_err.h per i build host del nucleo puro di sys_health: il
 * contratto pubblico sys_health.h include esp_err.h. L'header reale arriva
 * da ESP-IDF nei build target; i valori rispecchiano esp_err.h di IDF v5.4. */
#ifndef STUB_ESP_ERR_H
#define STUB_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

#endif /* STUB_ESP_ERR_H */
