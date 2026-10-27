/* IDF layer of config_store: the RAM copy lives behind a FreeRTOS mutex,
 * typed setters post CONFIG_EVENT_CHANGED on the default event loop right
 * away, and a one-shot esp_timer debounces the NVS commit (namespace
 * "pumpkin", keys "ver" + "cfg") to limit flash wear. config_store_flush
 * commits immediately.
 *
 * FreeRTOS mutex over portMUX: critical sections call into NVS and
 * esp_event (both may block), a spinlock would busy-wait the other core
 * through a flash write. All callers are tasks, never ISRs.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config_store.h"

#define TAG "config_store"

ESP_EVENT_DEFINE_BASE(CONFIG_EVENT);

#define KEY_VERSION "ver"
#define KEY_BLOB "cfg"

/* Compile-time upper bound of the serialized blob; the runtime size comes
 * from config_serialized_size() and is checked against this. The v3 blob
 * is 235 bytes; stored v2 (209) or legacy v1 (46) blobs are still read
 * for migration. */
#define BLOB_MAX 256u

/* Debounce delay in microseconds for esp_timer_start_once. */
#define DEBOUNCE_US ((uint64_t)CONFIG_STORE_DEBOUNCE_MS * 1000ULL)

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_debounce;
static pumpkin_config_t s_cfg;
static bool s_dirty;
static bool s_init;

/* Serialize cfg and persist both keys, then commit. Called with s_lock
 * held (flush, debounce callback) or before the object is shared (init
 * seeding). */
static esp_err_t commit_nvs(const pumpkin_config_t *cfg)
{
    uint8_t blob[BLOB_MAX];
    size_t sz = config_serialized_size();
    if (sz > sizeof(blob))
        return ESP_ERR_INVALID_SIZE;
    config_serialize(cfg, blob, sz);

    nvs_handle_t h;
    esp_err_t err = nvs_open(CONFIG_STORE_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u8(h, KEY_VERSION, cfg->version);
    if (err == ESP_OK)
        err = nvs_set_blob(h, KEY_BLOB, blob, sz);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_store_flush(void)
{
    if (!s_init)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_dirty) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    esp_err_t err = commit_nvs(&s_cfg);
    if (err == ESP_OK) {
        s_dirty = false;
        esp_timer_stop(s_debounce); /* disarms; an idle timer errors, fine */
    } else {
        /* stay dirty: the debounced retry or the next flush tries again */
        ESP_LOGW(TAG, "NVS commit failed (%s)", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

static void debounce_cb(void *arg)
{
    (void)arg; /* l'esp_timer reale passa il campo .arg, qui non usato */
    esp_err_t err = config_store_flush();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "debounced commit failed (%s)", esp_err_to_name(err));
}

/* Called with s_lock held, right after the RAM copy changed. Posts the
 * change event immediately (never debounced), then rearms the one-shot
 * commit timer. */
static void notify_and_schedule(config_param_id_t id)
{
    config_change_t ev = { .id = id };
    esp_err_t err = esp_event_post(CONFIG_EVENT, CONFIG_EVENT_CHANGED, &ev,
                                   sizeof(ev), 0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "event %d post failed (%s)", (int)id, esp_err_to_name(err));

    s_dirty = true;
    esp_timer_stop(s_debounce); /* ESP_ERR_INVALID_STATE when idle: fine */
    err = esp_timer_start_once(s_debounce, DEBOUNCE_US);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "debounce rearm failed (%s)", esp_err_to_name(err));
}

/* Read and deserialize the stored blob. Returns false when absent or
 * corrupt (wrong blob size, bad version); *out is filled only on success.
 * Migration/sanitizing is left to the caller so it can diff against the
 * raw copy and rewrite only when something actually changed. */
static bool load_raw(pumpkin_config_t *out)
{
    uint8_t blob[BLOB_MAX];
    size_t sz = sizeof(blob);
    uint8_t ver = 0;

    nvs_handle_t h;
    if (nvs_open(CONFIG_STORE_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;
    bool ok = nvs_get_u8(h, KEY_VERSION, &ver) == ESP_OK &&
              nvs_get_blob(h, KEY_BLOB, blob, &sz) == ESP_OK;
    nvs_close(h);
    if (!ok)
        return false; /* missing or unreadable blob */

    pumpkin_config_t tmp = { 0 };
    tmp.version = ver;
    /* config_deserialize validates the size (v2, or legacy v1 for
     * migration); anything else is corrupt. */
    if (config_deserialize(&tmp, blob, sz) != ESP_OK)
        return false;
    *out = tmp;
    return true;
}

esp_err_t config_store_init(const pumpkin_config_t *defaults)
{
    if (defaults == NULL)
        return ESP_ERR_INVALID_ARG;
    if (s_init)
        return ESP_ERR_INVALID_STATE;

    pumpkin_config_t work = *defaults;
    if (config_sanitize(&work) != ESP_OK)
        ESP_LOGW(TAG, "Kconfig defaults out of range, sanitized");

    /* NVS may already be up (another component initialized it); the call
     * is idempotent. If the partition is unusable the store still works
     * in RAM, without persistence. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init: %s, erasing", esp_err_to_name(nvs_err));
        if (nvs_flash_erase() == ESP_OK)
            nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK)
        ESP_LOGW(TAG, "NVS init failed (%s): no persistence",
                 esp_err_to_name(nvs_err));

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
        return ESP_ERR_NO_MEM;

    const esp_timer_create_args_t timer_args = {
        .callback = debounce_cb,
        .name = "cfg_commit",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_debounce);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    /* Load path: valid stored blob -> migrate, sanitize, substitute empty
     * anim names from the (sanitized) defaults; anything unusable (absent,
     * corrupt, newer version) falls back to the defaults. */
    pumpkin_config_t final_cfg = work;
    bool needs_write = true;
    pumpkin_config_t raw;
    if (load_raw(&raw) && config_migrate(&raw) == ESP_OK) {
        (void)config_sanitize(&raw);
        if (raw.trigger_anim[0] == '\0')
            memcpy(raw.trigger_anim, work.trigger_anim, CONFIG_ANIM_NAME_LEN);
        if (raw.idle_anim[0] == '\0')
            memcpy(raw.idle_anim, work.idle_anim, CONFIG_ANIM_NAME_LEN);
        final_cfg = raw;

        uint8_t a[BLOB_MAX];
        uint8_t b[BLOB_MAX];
        config_serialize(&final_cfg, a, sizeof(a));
        config_serialize(&work, b, sizeof(b));
        /* Rewrite only when migrate/sanitize/default substitution really
         * touched something vs what was stored (comparison is against the
         * raw loaded copy, never the defaults, or every boot would write). */
        needs_write = memcmp(a, b, config_serialized_size()) != 0;
    }

    s_cfg = final_cfg;
    s_dirty = false;
    if (needs_write && nvs_err == ESP_OK) {
        err = commit_nvs(&s_cfg); /* seed/migration write, not debounced */
        if (err != ESP_OK)
            ESP_LOGW(TAG, "seed write failed (%s)", esp_err_to_name(err));
    }

    s_init = true;
    ESP_LOGI(TAG, "ready (v%u, trigger %u mm, anims '%s'/'%s', power_on %d)",
             s_cfg.version, s_cfg.trigger_mm, s_cfg.trigger_anim,
             s_cfg.idle_anim, s_cfg.power_on);
    return ESP_OK;
}

esp_err_t config_store_get(pumpkin_config_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;
    if (!s_init)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_store_set_u16(config_param_id_t id, uint16_t value)
{
    uint16_t *field;
    uint16_t lo;
    uint16_t hi;
    switch (id) {
    case CFG_PARAM_TRIGGER_MM:
        field = &s_cfg.trigger_mm;
        lo = 100;
        hi = 10000;
        break;
    case CFG_PARAM_COOLDOWN_S:
        field = &s_cfg.cooldown_s;
        lo = 0;
        hi = 3600;
        break;
    case CFG_PARAM_TRIGGER_DURATION_S:
        field = &s_cfg.trigger_duration_s;
        lo = 1;
        hi = 3600;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (value < lo || value > hi)
        return ESP_ERR_INVALID_ARG;
    if (!s_init)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (*field != value) {
        *field = value;
        notify_and_schedule(id);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_store_set_pct(config_param_id_t id, uint8_t pct)
{
    uint8_t *field;
    uint8_t lo;
    uint8_t hi;
    switch (id) {
    case CFG_PARAM_IDLE_BRIGHTNESS_PCT:
        field = &s_cfg.idle_brightness_pct;
        lo = 0;
        hi = 100;
        break;
    case CFG_PARAM_MAX_BRIGHTNESS_PCT:
        field = &s_cfg.max_brightness_pct;
        lo = 5;
        hi = 100;
        break;
    case CFG_PARAM_BRIGHTNESS_PCT:
        field = &s_cfg.brightness_pct;
        lo = 0;
        hi = 100;
        break;
    case CFG_PARAM_EYES_IDLE_BRIGHTNESS_PCT:
        field = &s_cfg.eyes_idle_brightness_pct;
        lo = 0;
        hi = 100;
        break;
    case CFG_PARAM_EYES_SCARE_BRIGHTNESS_PCT:
        field = &s_cfg.eyes_scare_brightness_pct;
        lo = 0;
        hi = 100;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (pct < lo || pct > hi)
        return ESP_ERR_INVALID_ARG;
    if (!s_init)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (*field != pct) {
        *field = pct;
        notify_and_schedule(id);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_store_set_bool(config_param_id_t id, bool value)
{
    bool *field;
    switch (id) {
    case CFG_PARAM_AUTO_MODE:
        field = &s_cfg.auto_mode;
        break;
    case CFG_PARAM_POWER_ON:
        field = &s_cfg.power_on;
        break;
    case CFG_PARAM_EYES_ENABLED:
        field = &s_cfg.eyes_enabled;
        break;
    case CFG_PARAM_EYES_FOLLOW_FSM:
        field = &s_cfg.eyes_follow_fsm;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_init)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (*field != value) {
        *field = value;
        notify_and_schedule(id);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_store_set_color_param(config_param_id_t id, uint8_t r,
                                       uint8_t g, uint8_t b)
{
    uint8_t *r_field;
    uint8_t *g_field;
    uint8_t *b_field;
    switch (id) {
    case CFG_PARAM_COLOR:
        r_field = &s_cfg.color_r;
        g_field = &s_cfg.color_g;
        b_field = &s_cfg.color_b;
        break;
    case CFG_PARAM_EYES_IDLE_COLOR:
        r_field = &s_cfg.eyes_idle_r;
        g_field = &s_cfg.eyes_idle_g;
        b_field = &s_cfg.eyes_idle_b;
        break;
    case CFG_PARAM_EYES_SCARE_COLOR:
        r_field = &s_cfg.eyes_scare_r;
        g_field = &s_cfg.eyes_scare_g;
        b_field = &s_cfg.eyes_scare_b;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_init)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (*r_field != r || *g_field != g || *b_field != b) {
        *r_field = r;
        *g_field = g;
        *b_field = b;
        notify_and_schedule(id);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_store_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    return config_store_set_color_param(CFG_PARAM_COLOR, r, g, b);
}

esp_err_t config_store_set_anim(config_param_id_t id, const char *name)
{
    char *field;
    switch (id) {
    case CFG_PARAM_TRIGGER_ANIM:
        field = s_cfg.trigger_anim;
        break;
    case CFG_PARAM_IDLE_ANIM:
        field = s_cfg.idle_anim;
        break;
    case CFG_PARAM_EYES_IDLE_EFFECT:
        field = s_cfg.eyes_idle_effect;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (name == NULL)
        return ESP_ERR_INVALID_ARG;
    size_t len = strlen(name);
    if (len >= CONFIG_ANIM_NAME_LEN) /* 15 chars + NUL is the max */
        return ESP_ERR_INVALID_ARG;
    if (!s_init)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(field, name) != 0) {
        memcpy(field, name, len + 1);
        notify_and_schedule(id);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_store_get_mqtt(config_mqtt_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;
    if (!s_init)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg.mqtt;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_store_set_mqtt(const config_mqtt_t *in)
{
    if (in == NULL)
        return ESP_ERR_INVALID_ARG;
    /* Same strictness as the other setters: reject bad input instead of
     * clamping (sanitize owns the load path). Strings must carry their
     * NUL inside the buffer, the port range is 1..65535. */
    if (strnlen(in->host, CONFIG_MQTT_HOST_LEN) >= CONFIG_MQTT_HOST_LEN ||
        strnlen(in->user, CONFIG_MQTT_USER_LEN) >= CONFIG_MQTT_USER_LEN ||
        strnlen(in->pass, CONFIG_MQTT_PASS_LEN) >= CONFIG_MQTT_PASS_LEN ||
        in->port == 0)
        return ESP_ERR_INVALID_ARG;
    if (!s_init)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (memcmp(&s_cfg.mqtt, in, sizeof(config_mqtt_t)) != 0) {
        s_cfg.mqtt = *in;
        notify_and_schedule(CFG_PARAM_MQTT);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
