/* Stub nvs_flash.h per gli host test: vedi stubs.c (init sempre
 * fallito -> RAM-only). L'header reale arriva da ESP-IDF nei build su
 * target. */
#ifndef STUB_NVS_FLASH_H
#define STUB_NVS_FLASH_H

#include "esp_err.h"

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);

#endif /* STUB_NVS_FLASH_H */
