/* Stub nvs.h per gli host test: l'implementazione finta in stubs.c
 * rende la NVS "assente", cosi' config_store.c degrada alla modalita'
 * RAM-only (percorso gia' previsto sul target). L'header reale arriva
 * da ESP-IDF nei build su target. */
#ifndef STUB_NVS_H
#define STUB_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

/* Codici NVS che config_store.c confronta esplicitamente: stessi valori
 * di ESP-IDF (components/nvs_flash/include/nvs.h). Gli stub qui sotto
 * restituiscono invece il generico ESP_ERR_NOT_FOUND. */
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NO_FREE_PAGES (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

#endif /* STUB_NVS_H */
