/* Implementazione degli stub IDF per gli host test di config_store:
 * NVS assente (il negozio resta in RAM, come sul target senza
 * partizione valida), mutex no-op (test single-thread), timer che non
 * scatta mai, event loop finto che registra i post in due contatori
 * leggibili dai test (stub_event_post_count / stub_event_last_id).
 * Le controparti reali arrivano da ESP-IDF nei build su target. */
#include <stddef.h>
#include <stdint.h>

#include "config_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

int stub_event_post_count;
int32_t stub_event_last_id;

esp_err_t esp_event_post(esp_event_base_t event_base, int32_t event_id,
                         const void *event_data, size_t event_data_size,
                         uint32_t ticks_to_wait)
{
    (void)event_base;
    (void)event_id;    /* CONFIG_EVENT_CHANGED, costante: non utile ai test */
    (void)event_data_size;
    (void)ticks_to_wait;
    stub_event_post_count++;
    /* il payload e' config_change_t: registra il parametro cambiato */
    if (event_data != NULL)
        stub_event_last_id = (int32_t)((const config_change_t *)event_data)->id;
    return ESP_OK;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return (SemaphoreHandle_t)1;
}

int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t ticks)
{
    (void)lock;
    (void)ticks;
    return 1;
}

int xSemaphoreGive(SemaphoreHandle_t lock)
{
    (void)lock;
    return 1;
}

void vSemaphoreDelete(SemaphoreHandle_t lock)
{
    (void)lock;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *out_timer)
{
    (void)args;
    *out_timer = (esp_timer_handle_t)1;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    (void)timer;
    (void)timeout_us;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    (void)timer;
    return ESP_ERR_INVALID_STATE; /* come un timer idle reale */
}

/* NVS non disponibile: nvs_open fallisce e load_raw scarta il caricamento. */
esp_err_t nvs_flash_init(void)
{
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_flash_erase(void)
{
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    (void)namespace_name;
    (void)open_mode;
    (void)out_handle;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    (void)handle;
    (void)key;
    (void)out_value;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length)
{
    (void)handle;
    (void)key;
    (void)out_value;
    (void)length;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    (void)handle;
    (void)key;
    (void)value;
    (void)length;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_ERR_NOT_FOUND;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "ERR";
}
