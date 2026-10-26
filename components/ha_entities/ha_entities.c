/* Glue MQTT <-> dominio per la zucca: tiene il registro entita' HA, traduce
 * gli eventi radar/FSM/config/rete in pubblicazioni su pumpkin/<id>/state e
 * applica i comandi in ingresso (pumpkin/<id>/set/<key>) a FSM e config_store.
 * Nessun lavoro pesante qui: le callback di ha_mqtt arrivano gia' nel suo
 * task worker, gli eventi nel loop di default, il periodico nel task timer. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ha_commands.h"
#include "ha_entities.h"
#include "ha_mqtt.h"
#include "ha_payloads.h"
#include "ha_registry.h"
#include "ha_state.h"
#include "led_fx.h"
#include "net_mgr.h"
#include "pumpkin_fsm.h"
#include "radar_ld2420.h"

static const char *TAG = "ha_ent";

/* Device info (puntatori a storage statico: discovery letta a ogni announce). */
static ha_device_info_t s_dev;
static char s_dev_name[24]; /* "Zucca pumpkin-" + 6 hex + NUL = 21 */

/* Effect list di led_fx (storage statico del componente), condivisa da
 * light (effect_list) e trigger_anim (options). */
static const char *const *s_options;
static size_t s_options_len;

/* Cache del look manuale: aggiornata dai comandi light, letta dalla
 * derive della light e dagli eventi FSM. Inizializzata da config_store. */
static char s_anim_effect[HA_ANIM_NAME_MAX];
static uint8_t s_bright_pct;
static uint8_t s_color_r, s_color_g, s_color_b;

/* Rate-limit distance/periodico (mutato da loop eventi e task timer). */
static ha_report_state_t s_report;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_timer_handle_t s_periodic_timer;
static bool s_started;
static uint32_t s_pub_dropped; /* pubblicazioni scartate dal trasporto */

/* --- helper di pubblicazione ------------------------------------------- */

/* Flag retained dell'entita' (lettura dal registro: fonte unica). */
static bool entity_retained(const char *key)
{
    size_t n = ha_registry_count();
    for (size_t i = 0; i < n; i++) {
        const ha_entity_desc_t *e = ha_registry_at(i);
        if (e != NULL && strcmp(e->key, key) == 0) {
            return e->retained;
        }
    }
    return false;
}

/* Pubblica su "state/<key>" con retain secondo il registro. */
static void publish_state(const char *key, const char *payload, size_t len)
{
    char topic[sizeof("state/") + HA_CMD_KEY_MAX];
    snprintf(topic, sizeof(topic), "state/%s", key);
    if (ha_mqtt_publish(topic, payload, len, entity_retained(key)) != 0) {
        s_pub_dropped++;
        ESP_LOGW(TAG, "publish %s scartato", topic);
    }
}

static void publish_i32(const char *key, int32_t v)
{
    char *p = ha_payload_i32(v);
    if (p == NULL) {
        s_pub_dropped++;
        return;
    }
    publish_state(key, p, strlen(p));
    free(p);
}

static void publish_u32(const char *key, uint32_t v)
{
    char *p = ha_payload_u32(v);
    if (p == NULL) {
        s_pub_dropped++;
        return;
    }
    publish_state(key, p, strlen(p));
    free(p);
}

static void publish_text(const char *key, const char *v)
{
    char *p = ha_payload_text(v);
    if (p == NULL) {
        s_pub_dropped++;
        return;
    }
    publish_state(key, p, strlen(p));
    free(p);
}

/* "auto" e' ON negli stati automatici (IDLE/TRIGGERED/COOLDOWN). */
static bool auto_on_for(fsm_state_t st)
{
    return st == FSM_STATE_IDLE || st == FSM_STATE_TRIGGERED ||
           st == FSM_STATE_COOLDOWN;
}

static void publish_auto(void)
{
    const char *pl = ha_payload_onoff(auto_on_for(pumpkin_fsm_get_state()));
    publish_state("auto", pl, strlen(pl));
}

/* Look attuale: in MANUAL usa la cache (modificata dai comandi), altrimenti
 * il look idle configurato; il colore resta sempre la cache. */
static void publish_light(void)
{
    fsm_state_t st = pumpkin_fsm_get_state();
    bool on = (st != FSM_STATE_OFF);

    uint8_t r, g, b;
    portENTER_CRITICAL(&s_lock);
    r = s_color_r;
    g = s_color_g;
    b = s_color_b;
    portEXIT_CRITICAL(&s_lock);

    uint8_t pct;
    char anim[HA_ANIM_NAME_MAX];
    if (st == FSM_STATE_MANUAL) {
        portENTER_CRITICAL(&s_lock);
        pct = s_bright_pct;
        memcpy(anim, s_anim_effect, sizeof(anim));
        portEXIT_CRITICAL(&s_lock);
        anim[sizeof(anim) - 1] = '\0';
    } else {
        pumpkin_config_t c;
        if (config_store_get(&c) != ESP_OK) {
            ESP_LOGW(TAG, "publish_light: config_store_get fallito");
            return;
        }
        pct = c.idle_brightness_pct;
        memcpy(anim, c.idle_anim, sizeof(anim)); /* stessa lunghezza (16) */
        anim[sizeof(anim) - 1] = '\0';
    }

    int bright255 = ((int)pct * 255 + 50) / 100; /* round 0-100 -> 0-255 */
    char *pl = ha_payload_light(on, true, bright255, true, r, g, b, true, anim);
    if (pl == NULL) {
        s_pub_dropped++;
        return;
    }
    publish_state("light", pl, strlen(pl));
    free(pl);
}

static void publish_event_trigger(int32_t distance_mm, const char *anim)
{
    char *pl = ha_payload_event_trigger(1, esp_timer_get_time() / 1000,
                                        distance_mm, anim);
    if (pl == NULL) {
        s_pub_dropped++;
        return;
    }
    if (ha_mqtt_publish("event/trigger", pl, strlen(pl), false) != 0) {
        s_pub_dropped++;
        ESP_LOGW(TAG, "publish event/trigger scartato");
    }
    free(pl);
}

/* --- callback ha_mqtt (task worker) ------------------------------------ */

static void announce_cb(void *ctx)
{
    (void)ctx;
    esp_err_t err = ha_entities_announce();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "announce fallito (%s)", esp_err_to_name(err));
    }
}

static void cache_light_update(const ha_cmd_t *cmd)
{
    portENTER_CRITICAL(&s_lock);
    if (cmd->has_color) {
        s_color_r = cmd->r;
        s_color_g = cmd->g;
        s_color_b = cmd->b;
    }
    if (cmd->has_brightness) {
        s_bright_pct = (uint8_t)((cmd->brightness * 100 + 127) / 255);
    }
    portEXIT_CRITICAL(&s_lock);
}

static void cache_anim_update(const char *anim)
{
    portENTER_CRITICAL(&s_lock);
    memcpy(s_anim_effect, anim, strnlen(anim, HA_ANIM_NAME_MAX - 1));
    s_anim_effect[HA_ANIM_NAME_MAX - 1] = '\0';
    portEXIT_CRITICAL(&s_lock);
}

static void command_cb(void *ctx, const char *key, const char *payload,
                       size_t len)
{
    (void)ctx;
    ha_cmd_t cmd;
    char err[64];
    ha_cmd_result_t r = ha_cmd_parse(key, payload, len, s_options,
                                     s_options_len, &cmd, err, sizeof(err));
    if (r == HA_CMD_ERROR) {
        ESP_LOGE(TAG, "comando '%s': argomenti invalidi", key);
        return;
    }
    if (r == HA_CMD_IGNORED) {
        ESP_LOGW(TAG, "comando '%s' scartato: %s", key, err);
        return;
    }

    switch (cmd.target) {
    case HA_TARGET_LIGHT: {
        if (!cmd.light_on) {
            fsm_cmd_set_mode(FSM_MODE_OFF);
        } else {
            if (pumpkin_fsm_get_state() == FSM_STATE_OFF) {
                fsm_cmd_set_mode(FSM_MODE_MANUAL);
            }
            if (cmd.has_brightness || cmd.has_color) {
                /* I canali mancanti arrivano dalla cache del colore. */
                uint8_t cr, cg, cb, pct;
                portENTER_CRITICAL(&s_lock);
                cr = s_color_r;
                cg = s_color_g;
                cb = s_color_b;
                pct = s_bright_pct;
                portEXIT_CRITICAL(&s_lock);
                uint8_t rr = cmd.has_color ? cmd.r : cr;
                uint8_t gg = cmd.has_color ? cmd.g : cg;
                uint8_t bb = cmd.has_color ? cmd.b : cb;
                if (cmd.has_brightness) {
                    pct = (uint8_t)((cmd.brightness * 100 + 127) / 255);
                }
                fsm_cmd_set_light(rr, gg, bb, pct);
                cache_light_update(&cmd);
            }
            if (cmd.has_effect) {
                fsm_cmd_set_effect(cmd.effect);
                cache_anim_update(cmd.effect);
            }
        }
        publish_light(); /* echo (l'evento FSM ripubblica lo stato vero) */
        break;
    }
    case HA_TARGET_AUTO:
        fsm_cmd_set_mode(cmd.onoff ? FSM_MODE_AUTO : FSM_MODE_MANUAL);
        publish_auto(); /* echo */
        break;
    case HA_TARGET_TRIGGER_MM:
        config_store_set_u16(CFG_PARAM_TRIGGER_MM, (uint16_t)cmd.number);
        publish_i32("trigger_mm", cmd.number); /* echo immediato */
        break;
    case HA_TARGET_COOLDOWN_S:
        config_store_set_u16(CFG_PARAM_COOLDOWN_S, (uint16_t)cmd.number);
        publish_i32("cooldown_s", cmd.number); /* echo immediato */
        break;
    case HA_TARGET_TRIGGER_ANIM:
        config_store_set_anim(CFG_PARAM_TRIGGER_ANIM, cmd.anim);
        publish_text("trigger_anim", cmd.anim); /* echo immediato */
        break;
    case HA_TARGET_SCARE:
        fsm_cmd_scare(); /* nessun echo: la FSM notifica col suo evento */
        break;
    default:
        break;
    }
}

/* --- handler eventi (loop di default) ----------------------------------- */

static void radar_event_handler(void *arg, esp_event_base_t base, int32_t id,
                                void *event_data)
{
    (void)arg;
    (void)base;
    const radar_event_data_t *evt = event_data;
    switch (id) {
    case RADAR_EVENT_PRESENCE_ON:
    case RADAR_EVENT_PRESENCE_OFF: {
        const char *pl = ha_payload_onoff(id == RADAR_EVENT_PRESENCE_ON);
        publish_state("presence", pl, strlen(pl));
        break;
    }
    case RADAR_EVENT_DISTANCE_UPDATED: {
        int32_t d = (evt != NULL) ? (int32_t)evt->distance_mm : 0;
        if (d <= 0) {
            break;
        }
        portENTER_CRITICAL(&s_lock);
        bool go = ha_rate_distance(&s_report, d, esp_timer_get_time() / 1000);
        portEXIT_CRITICAL(&s_lock);
        if (go) {
            publish_u32("distance", (uint32_t)d);
        }
        break;
    }
    default:
        break;
    }
}

static void fsm_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *event_data)
{
    (void)arg;
    (void)base;
    if (id != FSM_EVENT_STATE_CHANGED || event_data == NULL) {
        return;
    }
    const fsm_state_change_t *sc = event_data;
    publish_light();
    publish_auto();
    if (sc->state == FSM_STATE_TRIGGERED) {
        int32_t dist = 0;
        radar_state_t rs;
        if (radar_get_state(&rs) == ESP_OK) {
            dist = rs.distance_mm; /* 0 se n/d */
        }
        pumpkin_config_t c;
        const char *anim = "";
        if (config_store_get(&c) == ESP_OK) {
            anim = c.trigger_anim;
        }
        publish_event_trigger(dist, anim);
    }
}

static void cfg_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *event_data)
{
    (void)arg;
    (void)base;
    if (id != CONFIG_EVENT_CHANGED || event_data == NULL) {
        return;
    }
    const config_change_t *ch = event_data;
    pumpkin_config_t c;
    switch (ch->id) {
    case CFG_PARAM_TRIGGER_MM:
        /* Fonte unica config_store: sincronizzo il modulo radar. */
        if (config_store_get(&c) == ESP_OK) {
            esp_err_t err = radar_set_trigger_mm(c.trigger_mm);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "radar_set_trigger_mm fallito (%s)",
                         esp_err_to_name(err));
            }
            publish_i32("trigger_mm", (int32_t)c.trigger_mm); /* echo */
        }
        break;
    case CFG_PARAM_COOLDOWN_S:
        if (config_store_get(&c) == ESP_OK) {
            publish_i32("cooldown_s", (int32_t)c.cooldown_s); /* echo */
        }
        break;
    case CFG_PARAM_TRIGGER_ANIM:
        if (config_store_get(&c) == ESP_OK) {
            publish_text("trigger_anim", c.trigger_anim); /* echo */
        }
        break;
    case CFG_PARAM_MQTT: {
        net_diag_t d;
        net_mgr_get_diag(&d);
        if (d.connected) {
            ESP_LOGI(TAG, "mqtt cambiato: riavvio del client a caldo");
            ha_mqtt_stop();
            esp_err_t err = ha_mqtt_start();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ha_mqtt_start fallito (%s)",
                         esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "mqtt cambiato: usato alla prossima connessione");
        }
        break;
    }
    default:
        break;
    }
}

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *event_data)
{
    (void)arg;
    (void)base;
    if (id == NET_EVENT_UP) {
        const net_up_t *up = event_data;
        ESP_LOGI(TAG, "net up (%s): avvio MQTT",
                 (up != NULL) ? up->ip : "?");
        esp_err_t err = ha_mqtt_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ha_mqtt_start fallito (%s)", esp_err_to_name(err));
        }
    } else if (id == NET_EVENT_DOWN) {
        const net_down_t *down = event_data;
        ESP_LOGW(TAG, "net down (reason=%d, retry %lu ms): stop MQTT",
                 (down != NULL) ? down->reason : 0,
                 (down != NULL) ? (unsigned long)down->next_ms : 0ul);
        ha_mqtt_stop();
    }
}

/* Periodico 60 s: rssi (la batteria non ha ancora sorgente: le sue entita'
 * restano unknown). */
static void periodic_cb(void *arg)
{
    (void)arg;
    net_diag_t d;
    net_mgr_get_diag(&d);
    if (!d.connected) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    bool due = ha_rate_periodic(&s_report, esp_timer_get_time() / 1000);
    portEXIT_CRITICAL(&s_lock);
    if (due) {
        publish_i32("rssi", (int32_t)d.rssi);
    }
}

/* --- API pubblica ------------------------------------------------------- */

esp_err_t ha_entities_init(void)
{
    if (s_started) {
        ESP_LOGW(TAG, "ha_entities_init gia' fatto");
        return ESP_OK;
    }

    /* 1) Effect list (storage statico di led_fx) per light e trigger_anim. */
    s_options = led_fx_get_effect_list(&s_options_len);
    if (s_options == NULL || s_options_len == 0) {
        ESP_LOGE(TAG, "led_fx_get_effect_list vuota");
        return ESP_ERR_INVALID_STATE;
    }

    /* 2) Registro: 11 entita' a key stabili. */
    if (ha_registry_reset() != 0) {
        return ESP_FAIL;
    }
    const ha_entity_desc_t ents[] = {
        { .key = "light", .name = "Luce", .type = HA_ET_LIGHT,
          .category = HA_CAT_NONE, .unit = NULL, .device_class = NULL,
          .retained = true, .has_command = true,
          .options = s_options, .options_len = s_options_len },
        { .key = "presence", .name = "Presenza", .type = HA_ET_BINARY_SENSOR,
          .category = HA_CAT_NONE, .unit = NULL, .device_class = NULL,
          .retained = false, .has_command = false },
        { .key = "distance", .name = "Distanza", .type = HA_ET_SENSOR,
          .category = HA_CAT_NONE, .unit = "mm", .device_class = "distance",
          .retained = false, .has_command = false },
        { .key = "battery_v", .name = "Batteria (V)", .type = HA_ET_SENSOR,
          .category = HA_CAT_DIAGNOSTIC, .unit = "V",
          .device_class = "voltage", .retained = false, .has_command = false },
        { .key = "battery_pct", .name = "Batteria (%)", .type = HA_ET_SENSOR,
          .category = HA_CAT_DIAGNOSTIC, .unit = "%",
          .device_class = "battery", .retained = false, .has_command = false },
        { .key = "rssi", .name = "RSSI WiFi", .type = HA_ET_SENSOR,
          .category = HA_CAT_DIAGNOSTIC, .unit = "dBm",
          .device_class = "signal_strength", .retained = false,
          .has_command = false },
        { .key = "auto", .name = "Modalità automatica", .type = HA_ET_SWITCH,
          .category = HA_CAT_NONE, .unit = NULL, .device_class = NULL,
          .retained = true, .has_command = true },
        { .key = "trigger_mm", .name = "Distanza di trigger",
          .type = HA_ET_NUMBER, .category = HA_CAT_CONFIG, .unit = "mm",
          .device_class = NULL, .retained = true, .has_command = true,
          .num_min = HA_CMD_TRIGGER_MM_MIN, .num_max = HA_CMD_TRIGGER_MM_MAX,
          .num_step = 50 },
        { .key = "cooldown_s", .name = "Cooldown", .type = HA_ET_NUMBER,
          .category = HA_CAT_CONFIG, .unit = "s", .device_class = NULL,
          .retained = true, .has_command = true,
          .num_min = HA_CMD_COOLDOWN_S_MIN, .num_max = HA_CMD_COOLDOWN_S_MAX,
          .num_step = 1 },
        { .key = "trigger_anim", .name = "Animazione di trigger",
          .type = HA_ET_SELECT, .category = HA_CAT_CONFIG, .unit = NULL,
          .device_class = NULL, .retained = true, .has_command = true,
          .options = s_options, .options_len = s_options_len },
        { .key = "scare", .name = "Spaventa", .type = HA_ET_BUTTON,
          .category = HA_CAT_NONE, .unit = NULL, .device_class = NULL,
          .retained = false, .has_command = true },
    };
    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        if (ha_registry_add(&ents[i]) != 0) {
            ESP_LOGE(TAG, "ha_registry_add('%s') fallito", ents[i].key);
            return ESP_FAIL;
        }
    }

    /* Cache look manuale dai valori correnti del config_store. */
    pumpkin_config_t cfg;
    esp_err_t err = config_store_get(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_store_get fallito (%s)", esp_err_to_name(err));
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_color_r = cfg.color_r;
    s_color_g = cfg.color_g;
    s_color_b = cfg.color_b;
    s_bright_pct = cfg.brightness_pct;
    memcpy(s_anim_effect, "solid", sizeof("solid")); /* default manuale */
    portEXIT_CRITICAL(&s_lock);

    /* 3)+4) Trasporto: init prima della device info (init prepara
     * id/topic/sw in storage statico). L'avvio vero e' su NET_EVENT_UP. */
    const ha_mqtt_config_t mqtt_cfg = {
        .on_connected = announce_cb,
        .on_command = command_cb,
        .ctx = NULL,
    };
    err = ha_mqtt_init(&mqtt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ha_mqtt_init fallito (%s)", esp_err_to_name(err));
        return err;
    }

    snprintf(s_dev_name, sizeof(s_dev_name), "Zucca pumpkin-%s",
             ha_mqtt_device_id());
    const char *sw = ha_mqtt_sw_version();
    s_dev = (ha_device_info_t){
        .device_id = ha_mqtt_device_id(),
        .name = s_dev_name,
        .manufacturer = "halloween_pumpkin",
        .model = "ESP32 D1 mini + WS2812B 8x8 + LD2420",
        .sw_version = sw,
        .origin_name = "halloween_pumpkin firmware",
        .origin_sw = sw,
    };

    /* 5) Handler eventi del dominio. */
    err = esp_event_handler_register(PRESENCE_EVENT, ESP_EVENT_ANY_ID,
                                     radar_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(FSM_EVENT, FSM_EVENT_STATE_CHANGED,
                                         fsm_event_handler, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(CONFIG_EVENT, CONFIG_EVENT_CHANGED,
                                         cfg_event_handler, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID,
                                         net_event_handler, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "registrazione handler fallita (%s)",
                 esp_err_to_name(err));
        return err;
    }

    /* 6) Periodico 60 s per rssi (us -> ticks manuale). */
    const esp_timer_create_args_t timer_args = {
        .callback = periodic_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ha_per",
    };
    err = esp_timer_create(&timer_args, &s_periodic_timer);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_timer_start_periodic(s_periodic_timer,
                                   (uint64_t)HA_PERIODIC_MS * 1000ULL);
    if (err != ESP_OK) {
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "pronto: %u entita', id=%s, base=%s",
             (unsigned)ha_registry_count(), ha_mqtt_device_id(),
             ha_mqtt_base_topic());
    return ESP_OK;
}

esp_err_t ha_entities_announce(void)
{
    if (!ha_mqtt_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    char *disc = ha_payload_discovery(&s_dev, ha_mqtt_base_topic(),
                                      ha_mqtt_availability_topic());
    if (disc == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int rc = ha_mqtt_publish_discovery(disc);
    free(disc);
    if (rc != 0) {
        ESP_LOGW(TAG, "publish discovery scartato");
        return ESP_FAIL;
    }

    /* Snapshot completo (battery_v/battery_pct: nessuna sorgente, skip). */
    publish_light();

    radar_state_t rs;
    if (radar_get_state(&rs) == ESP_OK) {
        const char *pl = ha_payload_onoff(rs.presence);
        publish_state("presence", pl, strlen(pl));
        if (rs.distance_mm > 0) {
            publish_u32("distance", (uint32_t)rs.distance_mm);
        }
    }

    net_diag_t d;
    net_mgr_get_diag(&d);
    publish_i32("rssi", (int32_t)d.rssi);

    publish_auto();

    esp_err_t err = ESP_OK;
    pumpkin_config_t c;
    if (config_store_get(&c) == ESP_OK) {
        publish_i32("trigger_mm", (int32_t)c.trigger_mm);
        publish_i32("cooldown_s", (int32_t)c.cooldown_s);
        publish_text("trigger_anim", c.trigger_anim);
    } else {
        err = ESP_FAIL;
    }

    ESP_LOGI(TAG, "annuncio pubblicato (discovery + snapshot)");
    return err;
}

esp_err_t ha_entities_remove(void)
{
    int rc_disc = ha_mqtt_publish_remove();
    int rc_avail = ha_mqtt_publish_availability(false);
    ESP_LOGW(TAG, "rimozione da HA: discovery=%d availability=%d",
             rc_disc, rc_avail);
    return (rc_disc == 0 && rc_avail == 0) ? ESP_OK : ESP_FAIL;
}

void ha_entities_status(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    /* Contatori di scarto interni a ha_mqtt: non esposti, li omettiamo. */
    snprintf(out, out_len,
             "connesso=%u id=%s base=%s discovery=%s sw=%s entita=%u "
             "pub_scartate=%lu",
             (unsigned)ha_mqtt_is_connected(), ha_mqtt_device_id(),
             ha_mqtt_base_topic(), ha_mqtt_discovery_topic(),
             ha_mqtt_sw_version(), (unsigned)ha_registry_count(),
             (unsigned long)s_pub_dropped);
}
