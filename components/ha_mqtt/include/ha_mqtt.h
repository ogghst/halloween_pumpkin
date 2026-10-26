#pragma once
/* Trasporto MQTT (esp_mqtt) della zucca: LWT availability, subscribe comandi,
 * worker task (nessun lavoro pesante nel callback mqtt), ripubblicazione
 * discovery a ogni connessione e alla birth message di HA.
 * Le callback on_connected/on_command girano nel task worker di ha_mqtt. */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define HA_ID_LEN 8                /* "a1b2c3" + NUL */
#define HA_BASE_TOPIC_LEN 24       /* "pumpkin/a1b2c3" */
#define HA_AVAIL_TOPIC_LEN 32      /* base + "/availability" */
#define HA_DISCOVERY_TOPIC_LEN 48  /* "homeassistant/device/a1b2c3/config" */
#define HA_CMD_KEY_MAX 24

typedef struct {
    void (*on_connected)(void *ctx); /* dopo subscribe: pubblicare discovery+stato */
    void (*on_command)(void *ctx, const char *key, const char *payload, size_t len);
    void *ctx;
} ha_mqtt_config_t;

esp_err_t ha_mqtt_init(const ha_mqtt_config_t *cfg); /* salva config, prepara id/topic */
esp_err_t ha_mqtt_start(void);  /* legge config_store MQTT + Kconfig CA, crea e avvia client */
esp_err_t ha_mqtt_stop(void);   /* "offline" retained best-effort + stop e destroy client */
bool ha_mqtt_is_connected(void);
const char *ha_mqtt_device_id(void);
const char *ha_mqtt_base_topic(void);
const char *ha_mqtt_availability_topic(void);
const char *ha_mqtt_discovery_topic(void);
const char *ha_mqtt_sw_version(void); /* esp_app_get_description()->version */
/* publish su pumpkin/<id>/<rel_topic>; ritorna 0 inviato/in coda, -1 scartato
 * (non connesso o payload oltre HA_CMD_PAYLOAD_MAX); mai bloccante */
int ha_mqtt_publish(const char *rel_topic, const char *payload, size_t len, bool retain);
int ha_mqtt_publish_discovery(const char *payload); /* retained */
int ha_mqtt_publish_availability(bool online);     /* retained */
int ha_mqtt_publish_remove(void);                  /* payload vuoto retained su discovery */
/* CA TLS (Kconfig PUMPKIN_MQTT_CA=nvs): accumulo PEM da console, commit automatico
 * alla riga "-----END CERTIFICATE-----"; namespace NVS "pumpkin_ca", chiave "ca" */
esp_err_t ha_mqtt_ca_clear(void);
esp_err_t ha_mqtt_ca_append_line(const char *line);
esp_err_t ha_mqtt_ca_status(char *out, size_t out_len);
