/* Layer IDF dell'OTA: gate di politica nel chiamante, poi task dedicato
 * con esp_https_ota ADVANCED (begin/perform/finish). HTTPS obbligatorio:
 * il certificato della CA arriva da ha_mqtt_ca_get e la verifica non e'
 * mai disabilitabile. Gli eventi di stato escono su update/status
 * (non retained, JSON < 512 B come il cap di ha_mqtt_publish). */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ha_commands.h" /* HA_CMD_PAYLOAD_MAX (cap eventi su ha_mqtt_publish) */
#include "ha_mqtt.h"
#include "led_fx.h"
#include "ota_policy.h"
#include "pumpkin_ota.h"

static const char *TAG = "pumpkin_ota";

#define OTA_STATUS_TOPIC "update/status"
#define OTA_CA_BUF_LEN 4096 /* CA_PEM_MAX di ha_mqtt e' 2048: margine 2x */
#define OTA_TASK_STACK 8192
#define OTA_TASK_PRIO 5
#define OTA_SUCCESS_FLUSH_MS 3000
#define OTA_REASON_MAX 24

/* Stato condiviso: scritto da start (chiamante console/worker) e dal task
 * OTA; lettura solo di interi piccoli/flag. */
static bool s_init;
static volatile bool s_running;
static int (*s_battery_provider)(void);
static uint32_t s_rejects;
static uint32_t s_starts;
static char s_last[48]; /* ultimo esito: motivo/errore, senza lock */

/* --- eventi ------------------------------------------------------------ */

/* Pubblica l'evento su update/status; -1 di ha_mqtt_publish (non connesso,
 * payload oltre cap) e' un'informazione persa, non un errore fatale. */
static void publish_event(const pumpkin_ota_event_t *ev)
{
    char json[HA_CMD_PAYLOAD_MAX];
    size_t n = pumpkin_ota_policy_event_json(json, sizeof(json), ev);
    if (n == 0) {
        ESP_LOGE(TAG, "evento %s: JSON non rappresentabile", ev->event_str);
        return;
    }
    if (ha_mqtt_publish(OTA_STATUS_TOPIC, json, n, false) != 0) {
        ESP_LOGW(TAG, "evento %s scartato (MQTT non connesso?)", ev->event_str);
    }
}

static void publish_measured(const char *event_str, const char *detail,
                             const char *installed, int progress,
                             bool battery_unknown)
{
    pumpkin_ota_event_t ev = {
        .event_str = event_str,
        .detail = detail,
        .installed = installed,
        .free_heap = esp_get_free_heap_size(),
        .max_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        .progress = progress,
        .battery_unknown = battery_unknown,
    };
    publish_event(&ev);
}

/* Esito del task: fail() copia il motivo in s_last per lo status. */
static void ota_fail(const char *detail, const char *installed)
{
    snprintf(s_last, sizeof(s_last), "%.40s", detail != NULL ? detail : "?");
    publish_measured("failed", detail, installed, 0, false);
}

/* --- gate nel chiamante ------------------------------------------------ */

static int battery_now(void)
{
    if (s_battery_provider == NULL) {
        return -1;
    }
    int pct = s_battery_provider();
    /* Fuori dal contratto 0..100 / -1: batteria sconosciuta. */
    if (pct < 0 || pct > 100) {
        return -1;
    }
    return pct;
}

/* --- task OTA ----------------------------------------------------------- */

static void ota_task(void *arg)
{
    char *url = arg;
    char *ca = malloc(OTA_CA_BUF_LEN);
    size_t ca_len = 0;
    bool wdt_added = false;
    esp_err_t err = ha_mqtt_ca_get(ca, ca ? OTA_CA_BUF_LEN : 0, &ca_len);
    if (err != ESP_OK || ca_len == 0) {
        ESP_LOGE(TAG, "CA assente o illeggibile (%s): HTTPS OTA rifiutato",
                 esp_err_to_name(err));
        ota_fail("no_ca", NULL);
        goto exit;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .cert_pem = ca,
        .buffer_size = 4096,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t h = NULL;
    err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin: %s", esp_err_to_name(err));
        ota_fail(esp_err_to_name(err), NULL);
        goto exit;
    }

    /* Versione dell'immagine scaricata prima di toccare la flash. */
    esp_app_desc_t desc;
    err = esp_https_ota_get_img_desc(h, &desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "descrittore immagine non leggibile: %s",
                 esp_err_to_name(err));
        esp_https_ota_abort(h);
        ota_fail(esp_err_to_name(err), NULL);
        goto exit;
    }
    if (pumpkin_ota_policy_version_changed(
            esp_app_get_description()->version, desc.version) != 0) {
        ESP_LOGW(TAG, "stessa versione dell'immagine in esecuzione ('%s')",
                 desc.version);
        esp_https_ota_abort(h);
        snprintf(s_last, sizeof(s_last), "same_version");
        publish_measured("rejected", "same_version", desc.version, 0, false);
        goto exit;
    }

#if CONFIG_PUMPKIN_OTA_TASK_WDT
    if (esp_task_wdt_add(NULL) == ESP_OK) {
        wdt_added = true;
    }
#endif

    int last_step = -1;
    for (;;) {
        err = esp_https_ota_perform(h);
#if CONFIG_PUMPKIN_OTA_TASK_WDT
        esp_task_wdt_reset();
#endif
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int size = esp_https_ota_get_image_size(h);
        int read = esp_https_ota_get_image_len_read(h);
        int pct = (size > 0) ? (read * 100) / size : 0;
        if (pct / 10 > last_step) {
            last_step = pct / 10;
            publish_measured("progress", NULL, NULL, last_step * 10, false);
        }
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        /* Col rollback abilitato marca l'immagine PENDING_VERIFY: la
         * validazione post-reboot e' di sys_health, non qui. */
        err = esp_https_ota_finish(h);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA completato: versione '%s', riavvio tra %d ms",
                     desc.version, OTA_SUCCESS_FLUSH_MS);
            snprintf(s_last, sizeof(s_last), "success '%.24s'", desc.version);
            publish_measured("success", NULL, desc.version, 100, false);
            /* Attesa per il flush MQTT (QoS 0: il socket va svuotato). */
            vTaskDelay(pdMS_TO_TICKS(OTA_SUCCESS_FLUSH_MS));
            esp_restart();
        }
        ESP_LOGE(TAG, "esp_https_ota_finish: %s", esp_err_to_name(err));
        ota_fail(esp_err_to_name(err), NULL);
    } else {
        /* Download interrotto o incompleto: abort (finish non e' mai stato
         * chiamato su questo percorso). */
        esp_https_ota_abort(h);
        ota_fail(err == ESP_OK ? "incomplete" : esp_err_to_name(err), NULL);
    }

exit:
#if CONFIG_PUMPKIN_OTA_TASK_WDT
    if (wdt_added) {
        esp_task_wdt_delete(NULL);
    }
#endif
    free(ca);
    free(url);
    s_running = false;
    vTaskDelete(NULL);
}

/* --- API --------------------------------------------------------------- */

esp_err_t pumpkin_ota_init(void)
{
    s_init = true;
    return ESP_OK;
}

void pumpkin_ota_set_battery_provider(int (*fn)(void))
{
    s_battery_provider = fn;
}

esp_err_t pumpkin_ota_start(const char *url, const char *version,
                            const char *release_summary)
{
    if (!s_init) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        ESP_LOGW(TAG, "update gia' in corso");
        s_rejects++;
        snprintf(s_last, sizeof(s_last), "busy");
        publish_measured("rejected", "busy", NULL, 0, false);
        return ESP_ERR_INVALID_STATE;
    }

    pumpkin_ota_policy_input_t in = {
        .url = url,
        .battery_pct = battery_now(),
        .led_current_ma = led_fx_estimate_current_ma(),
        .free_heap = esp_get_free_heap_size(),
        .max_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        .min_free_heap = CONFIG_PUMPKIN_OTA_MIN_FREE_HEAP,
        .max_led_ma = CONFIG_PUMPKIN_OTA_MAX_LED_CURRENT_MA,
        .url_max = PUMPKIN_OTA_URL_MAX,
        .warn_battery_unknown = false,
    };
    char reason[OTA_REASON_MAX];
    if (pumpkin_ota_policy_check(&in, reason, sizeof(reason)) != 0) {
        ESP_LOGW(TAG, "update rifiutato (%s): batt=%d led=%lu heap=%lu "
                      "block=%lu",
                 reason, in.battery_pct,
                 (unsigned long)in.led_current_ma,
                 (unsigned long)in.free_heap, (unsigned long)in.max_block);
        s_rejects++;
        snprintf(s_last, sizeof(s_last), "rejected %s", reason);
        publish_measured("rejected", reason, NULL, 0,
                         in.warn_battery_unknown);
        return ESP_ERR_INVALID_STATE;
    }

    /* Il task possiede la sua copia dell'URL. */
    size_t url_len = strlen(url);
    char *url_copy = malloc(url_len + 1);
    if (url_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(url_copy, url, url_len + 1);

    ESP_LOGI(TAG, "update accettato: url=%s version=%s summary=%s batt=%d "
                  "led=%lu mA heap=%lu",
             url, version != NULL ? version : "-",
             release_summary != NULL ? release_summary : "-",
             in.battery_pct, (unsigned long)in.led_current_ma,
             (unsigned long)in.free_heap);

    s_running = true;
    s_starts++;
    snprintf(s_last, sizeof(s_last), "started");
    publish_measured("started", NULL, NULL, 0, in.warn_battery_unknown);

    if (xTaskCreate(ota_task, "pumpkin_ota", OTA_TASK_STACK, url_copy,
                    OTA_TASK_PRIO, NULL) != pdPASS) {
        free(url_copy);
        s_running = false;
        ESP_LOGE(TAG, "task OTA non creato");
        ota_fail("no_task", NULL);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool pumpkin_ota_is_running(void)
{
    return s_running;
}

void pumpkin_ota_status(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    snprintf(out, out_len,
             "attivo=%u starts=%lu rejects=%lu ultimo=%s heap=%lu",
             (unsigned)s_running, (unsigned long)s_starts,
             (unsigned long)s_rejects, s_last,
             (unsigned long)esp_get_free_heap_size());
}
