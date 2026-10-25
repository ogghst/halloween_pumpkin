/* Pumpkin firmware - bootstrap entry point (Phase 0 skeleton). */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "pumpkin.main";

void app_main(void)
{
    ESP_LOGI(TAG, "pumpkin firmware boot: phase 0 skeleton");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
