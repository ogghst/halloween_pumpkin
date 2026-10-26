/* Provision button: long-press detection by polling (Fase 4).
 *
 * GPIO PUMPKIN_PROV_BTN_GPIO, input with internal pull-up, active low
 * (button wired between the pin and GND). A 50 ms poll task measures the
 * continuous press; holding it for at least PUMPKIN_PROV_BTN_HOLD_MS
 * triggers wifi_prov_begin() once per press. No interrupts: polling is
 * enough for a human-scale press and keeps the GPIO usage trivial.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "wifi_prov.h"

static const char *TAG = "prov";

#define PROV_BTN_POLL_MS 50

/* Internal entry point, called by wifi_prov_app_init(). */
esp_err_t prov_button_start(void);

#if CONFIG_PUMPKIN_PROV_BTN_GPIO >= 0
static void prov_button_task(void *arg);

esp_err_t prov_button_start(void)
{
    if (xTaskCreate(prov_button_task, "prov_btn", 2048, NULL,
                    tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#else
esp_err_t prov_button_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

#if CONFIG_PUMPKIN_PROV_BTN_GPIO >= 0
static void prov_button_task(void *arg)
{
    (void)arg;
    const gpio_num_t pin = (gpio_num_t)CONFIG_PUMPKIN_PROV_BTN_GPIO;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_PUMPKIN_PROV_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "button gpio %d config failed", CONFIG_PUMPKIN_PROV_BTN_GPIO);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "button on gpio %d, hold %d ms",
             CONFIG_PUMPKIN_PROV_BTN_GPIO, CONFIG_PUMPKIN_PROV_BTN_HOLD_MS);

    bool pressed = false;
    bool fired = false;
    int64_t press_start_us = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PROV_BTN_POLL_MS));
        bool down = gpio_get_level(pin) == 0;
        int64_t now_us = esp_timer_get_time();

        if (down && !pressed) {
            pressed = true;
            fired = false;
            press_start_us = now_us;
        } else if (down && pressed && !fired &&
                   now_us - press_start_us >=
                       (int64_t)CONFIG_PUMPKIN_PROV_BTN_HOLD_MS * 1000LL) {
            fired = true;
            if (wifi_prov_is_active()) {
                ESP_LOGI(TAG, "hold detected: provisioning already active");
            } else {
                esp_err_t err = wifi_prov_begin();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "prov begin from button: %s",
                             esp_err_to_name(err));
                }
            }
        } else if (!down) {
            pressed = false; /* one begin per press */
        }
    }
}
#endif
