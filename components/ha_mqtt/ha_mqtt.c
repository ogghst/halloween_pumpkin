/* Strato trasporto IDF di ha_mqtt (Fase 5).
 *
 * esp_mqtt gestisce connessione, LWT availability ("offline" retained) e
 * riconnessione automatica (10 s). Il callback MQTT esegue solo operazioni
 * veloci (flag, publish, subscribe, armamento timer, copia payload in coda):
 * le callback utente on_connected/on_command girano nel task worker "ha_mqtt"
 * che consuma la coda eventi.
 *
 * Alla CONNECTED si pubblica "online" retained, ci si iscrive a
 * <base>/set/# e homeassistant/status e si arma un announce one-shot con
 * ritardo casuale 1000..5000 ms (jitter anti-temporale alla birth message
 * "online" di HA su homeassistant/status).
 *
 * Nomi topic calcolati in init dal MAC STA (byte 3..5, es. "a1b2c3"), stesso
 * formato del suffisso hostname di net_mgr.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "config_store.h"
#include "ha_commands.h" /* HA_CMD_PAYLOAD_MAX */
#include "ha_mqtt.h"

static const char *TAG = "ha_mqtt";

#define CA_NVS_NAMESPACE "pumpkin_ca"
#define CA_NVS_KEY "ca"
#define CA_PEM_MAX 2048u
#define HA_QUEUE_DEPTH 8
#define HA_WORKER_STACK 6144
#define HA_WORKER_PRIO 5
#define HA_ANNOUNCE_MIN_MS 1000
#define HA_ANNOUNCE_SPAN_MS 4001 /* esp_random() % 4001 + 1000 -> 1000..5000 */
#define HA_STOP_FLUSH_MS 100
#define HA_WORKER_WDT_SLICE_MS 1000

/* Listener extra di (ri)connessione registrati con
 * ha_mqtt_on_connected_add; dispatch nel worker dopo on_connected. */
static struct {
    void (*cb)(void *ctx);
    void *ctx;
} s_connected_listeners[HA_MQTT_CONNECTED_LISTENERS_MAX];

/* Evento consumato dal worker: EVT_CMD consegra un comando, EVT_ANNOUNCE
 * chiede la ripubblicazione discovery+stato. */
typedef enum {
    EVT_CMD = 0,
    EVT_ANNOUNCE,
} ha_evt_type_t;

typedef struct {
    ha_evt_type_t type;
    char key[HA_CMD_KEY_MAX];
    char *payload; /* malloc: liberato dal worker dopo on_command */
    uint16_t len;
} ha_evt_t;

static ha_mqtt_config_t s_cfg;
static bool s_init;
static char s_device_id[HA_ID_LEN];
static char s_base_topic[HA_BASE_TOPIC_LEN];
static char s_avail_topic[HA_AVAIL_TOPIC_LEN];
static char s_discovery_topic[HA_DISCOVERY_TOPIC_LEN];
static char s_client_id[HA_ID_LEN + 9]; /* "pumpkin-a1b2c3" */
static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static esp_timer_handle_t s_announce_timer;
static uint32_t s_publish_dropped;

/* PEM della CA: deve restare valido per tutta la vita del client (esp-mqtt
 * non copia verification.certificate), quindi buffer statico. */
static char s_ca_pem[CA_PEM_MAX];
static size_t s_ca_off;

/* --- worker --- */

static void ha_worker(void *arg)
{
    (void)arg;
    ha_evt_t ev;
#if CONFIG_HA_MQTT_WORKER_WDT
    esp_err_t werr = esp_task_wdt_add(NULL);
    if (werr != ESP_OK && werr != ESP_ERR_INVALID_ARG)
        ESP_LOGW(TAG, "iscrizione task watchdog fallita: %s",
                 esp_err_to_name(werr));
#endif
    for (;;) {
#if CONFIG_HA_MQTT_WORKER_WDT
        /* Attesa limitata a passi da 1 s con feed del watchdog: con la coda
         * vuota il task resta comunque vivo agli occhi del WDT. Il timeout
         * non perde nulla: nessun evento e' in volo mentre scade. */
        (void)esp_task_wdt_reset();
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(HA_WORKER_WDT_SLICE_MS)) != pdTRUE)
            continue;
#else
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE)
            break; /* irraggiungibile con attesa infinita */
#endif
        if (ev.type == EVT_CMD) {
            if (s_cfg.on_command != NULL)
                s_cfg.on_command(s_cfg.ctx, ev.key, ev.payload, ev.len);
            free(ev.payload);
        } else if (ev.type == EVT_ANNOUNCE) {
            if (s_cfg.on_connected != NULL)
                s_cfg.on_connected(s_cfg.ctx);
            for (int i = 0; i < HA_MQTT_CONNECTED_LISTENERS_MAX; i++)
                if (s_connected_listeners[i].cb != NULL)
                    s_connected_listeners[i].cb(s_connected_listeners[i].ctx);
        }
    }
    s_worker = NULL;
    vTaskDelete(NULL);
}

static void announce_timer_cb(void *arg)
{
    (void)arg;
    ha_evt_t ev = { .type = EVT_ANNOUNCE };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE)
        ESP_LOGW(TAG, "coda piena: announce scartato");
}

/* Ritardo casuale 1000..5000 ms; chiamato dal task mqtt (esp_timer usa un
 * task proprio, la callback qui non fa lavoro pesante). */
static void announce_arm(void)
{
    uint32_t delay_ms = esp_random() % HA_ANNOUNCE_SPAN_MS + HA_ANNOUNCE_MIN_MS;
    esp_timer_stop(s_announce_timer); /* no-op se non attivo */
    if (esp_timer_start_once(s_announce_timer, (uint64_t)delay_ms * 1000ULL) != ESP_OK)
        ESP_LOGE(TAG, "armamento announce fallito");
}

/* --- eventi esp_mqtt (task mqtt: solo operazioni veloci) --- */

/* true se topic[0..len) == ref (non NUL-terminato) */
static bool topic_equals(const char *topic, size_t len, const char *ref)
{
    return len == strlen(ref) && strncmp(topic, ref, len) == 0;
}

static void handle_data(esp_mqtt_event_handle_t e)
{
    if (topic_equals(e->topic, (size_t)e->topic_len, "homeassistant/status")) {
        /* birth message di HA: ripubblica discovery+stato */
        if (e->data_len == 6 && strncmp(e->data, "online", 6) == 0)
            announce_arm();
        return;
    }

    size_t base_len = strlen(s_base_topic);
    if ((size_t)e->topic_len <= base_len + 5 ||
        strncmp(e->topic, s_base_topic, base_len) != 0 ||
        strncmp(e->topic + base_len, "/set/", 5) != 0)
        return; /* topic non nostro */

    const char *key = e->topic + base_len + 5;
    size_t key_len = (size_t)e->topic_len - base_len - 5;
    if (key_len == 0 || key_len >= HA_CMD_KEY_MAX ||
        memchr(key, '/', key_len) != NULL) {
        ESP_LOGW(TAG, "comando con chiave malformata (%u char), scartato",
                 (unsigned)key_len);
        return;
    }
    if (e->total_data_len > e->data_len) {
        ESP_LOGW(TAG, "payload fragmentato (%d/%d) su '%.*s', scartato",
                 e->data_len, e->total_data_len, (int)key_len, key);
        return;
    }
    if (e->data_len > HA_CMD_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "payload di %d byte su '%.*s' oltre il limite, scartato",
                 e->data_len, (int)key_len, key);
        return;
    }

    ha_evt_t ev = { .type = EVT_CMD, .len = (uint16_t)e->data_len };
    memcpy(ev.key, key, key_len);
    ev.key[key_len] = '\0';
    /* il payload di MQTT_EVENT_DATA non e' NUL-terminato: copia con lunghezza */
    ev.payload = malloc((size_t)e->data_len);
    if (ev.payload == NULL) {
        ESP_LOGE(TAG, "malloc payload fallita, comando '%s' scartato", ev.key);
        return;
    }
    memcpy(ev.payload, e->data, (size_t)e->data_len);
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "coda piena: comando '%s' scartato", ev.key);
        free(ev.payload);
    }
}

static void mqtt_event_cb(void *handler_args, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t e = event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        {
            char sub[HA_BASE_TOPIC_LEN + 7]; /* base + "/set/#" + NUL */
            snprintf(sub, sizeof(sub), "%s/set/#", s_base_topic);
            esp_mqtt_client_subscribe(s_client, sub, 0);
            esp_mqtt_client_subscribe(s_client, "homeassistant/status", 0);
            esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 0, 1);
        }
        announce_arm();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_data(e);
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle != NULL)
            ESP_LOGE(TAG, "errore MQTT: tipo %d, esp_err %s",
                     e->error_handle->error_type,
                     esp_err_to_name(e->error_handle->esp_tls_last_esp_err));
        else
            ESP_LOGE(TAG, "errore MQTT senza dettagli");
        break;
    default:
        break;
    }
}

/* --- caricamento CA per mqtts:// --- */

/* Restituisce il PEM da usare (buffer statico) o NULL; in caso di tls senza
 * CA disponibile logga e imposta *err. */
static const char *ca_load(const config_mqtt_t *m, esp_err_t *err)
{
    *err = ESP_OK;
    if (!m->tls)
        return NULL; /* mqtt://: con certificate impostato esp-mqtt rifiuta l'init */
#if CONFIG_PUMPKIN_MQTT_CA_NVS
    nvs_handle_t h;
    esp_err_t e = nvs_open(CA_NVS_NAMESPACE, NVS_READONLY, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mqtts:// richiede la CA ma NVS %s/%s e' vuoto: "
                      "usare i comandi console ca_*",
                 CA_NVS_NAMESPACE, CA_NVS_KEY);
        *err = ESP_ERR_NOT_SUPPORTED;
        return NULL;
    }
    size_t ca_len = sizeof(s_ca_pem);
    e = nvs_get_str(h, CA_NVS_KEY, s_ca_pem, &ca_len);
    nvs_close(h);
    if (e != ESP_OK || ca_len < 2) {
        ESP_LOGE(TAG, "mqtts:// richiede la CA ma NVS %s/%s e' vuoto: "
                      "usare i comandi console ca_*",
                 CA_NVS_NAMESPACE, CA_NVS_KEY);
        *err = ESP_ERR_NOT_SUPPORTED;
        return NULL;
    }
    ESP_LOGI(TAG, "CA caricata da NVS (%u byte)", (unsigned)(ca_len - 1));
    return s_ca_pem;
#elif CONFIG_PUMPKIN_MQTT_CA_PARTITION
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        CONFIG_PUMPKIN_MQTT_CA_PART);
    if (part == NULL) {
        ESP_LOGE(TAG, "partizione CA '%s' non trovata",
                 CONFIG_PUMPKIN_MQTT_CA_PART);
        *err = ESP_ERR_NOT_FOUND;
        return NULL;
    }
    size_t want = part->size;
    if (want > CA_PEM_MAX - 1)
        want = CA_PEM_MAX - 1;
    esp_err_t e = esp_partition_read(part, 0, s_ca_pem, want);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "lettura partizione CA '%s' fallita: %s",
                 CONFIG_PUMPKIN_MQTT_CA_PART, esp_err_to_name(e));
        *err = e;
        return NULL;
    }
    size_t ca_len = strnlen(s_ca_pem, want);
    s_ca_pem[ca_len] = '\0';
    if (ca_len == 0) {
        ESP_LOGE(TAG, "partizione CA '%s' vuota", CONFIG_PUMPKIN_MQTT_CA_PART);
        *err = ESP_ERR_NOT_FOUND;
        return NULL;
    }
    ESP_LOGI(TAG, "CA caricata da partizione '%s' (%u byte)",
             CONFIG_PUMPKIN_MQTT_CA_PART, (unsigned)ca_len);
    return s_ca_pem;
#else
    if (m->tls) {
        ESP_LOGE(TAG, "mqtts:// richiede una CA: selezionare in menuconfig "
                      "PUMPKIN_MQTT_CA_NVS o PUMPKIN_MQTT_CA_PARTITION");
        *err = ESP_ERR_NOT_SUPPORTED;
        return NULL;
    }
    return NULL;
#endif
}

/* --- API --- */

esp_err_t ha_mqtt_init(const ha_mqtt_config_t *cfg)
{
    if (cfg == NULL || cfg->on_connected == NULL || cfg->on_command == NULL)
        return ESP_ERR_INVALID_ARG;

    if (s_init) {
        /* idempotente: aggiorna solo le callback */
        s_cfg.on_connected = cfg->on_connected;
        s_cfg.on_command = cfg->on_command;
        s_cfg.ctx = cfg->ctx;
        return ESP_OK;
    }

    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK)
        return err;
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    snprintf(s_base_topic, sizeof(s_base_topic), "pumpkin/%s", s_device_id);
    /* concat esplicito: snprintf su buffer fissi fa scattare
     * -Wformat-truncation anche quando i contenuti sono dimensionati */
    size_t base_len = strlen(s_base_topic);
    if (base_len + sizeof("/availability") > sizeof(s_avail_topic))
        return ESP_ERR_INVALID_SIZE;
    memcpy(s_avail_topic, s_base_topic, base_len);
    memcpy(s_avail_topic + base_len, "/availability", sizeof("/availability"));
    snprintf(s_discovery_topic, sizeof(s_discovery_topic),
             "homeassistant/device/%s/config", s_device_id);
    snprintf(s_client_id, sizeof(s_client_id), "pumpkin-%s", s_device_id);

    s_cfg = *cfg;
    s_init = true;
    return ESP_OK;
}

esp_err_t ha_mqtt_start(void)
{
    if (!s_init || s_client != NULL)
        return ESP_ERR_INVALID_STATE;

    config_mqtt_t m;
    esp_err_t err = config_store_get_mqtt(&m);
    if (err != ESP_OK)
        return err;
    if (m.host[0] == '\0' || m.port == 0) {
        ESP_LOGW(TAG, "parametri MQTT non provisionati (host vuoto o porta 0): "
                      "completare il provisioning");
        return ESP_ERR_INVALID_STATE;
    }

    /* URI: esp-mqtt fa strdup dentro esp_mqtt_client_init, basta lo stack */
    char uri[CONFIG_MQTT_HOST_LEN + 16];
    snprintf(uri, sizeof(uri), "%s://%s:%u", m.tls ? "mqtts" : "mqtt", m.host,
             (unsigned)m.port);

    const char *ca = ca_load(&m, &err);
    if (ca == NULL && err != ESP_OK)
        return err;

    esp_mqtt_client_config_t c = { 0 };
    c.broker.address.uri = uri;
    c.credentials.client_id = s_client_id;
    if (m.user[0] != '\0')
        c.credentials.username = m.user;
    if (m.pass[0] != '\0')
        c.credentials.authentication.password = m.pass;
    if (ca != NULL) {
        c.broker.verification.certificate = ca;
        c.broker.verification.certificate_len = 0; /* PEM terminato da NUL */
    }
    c.session.last_will.topic = s_avail_topic;
    c.session.last_will.msg = "offline";
    c.session.last_will.msg_len = 7;
    c.session.last_will.qos = 0;
    c.session.last_will.retain = 1;
    c.session.keepalive = CONFIG_PUMPKIN_MQTT_KEEPALIVE_S;
    c.network.disable_auto_reconnect = false;
    c.network.reconnect_timeout_ms = 10000;
    c.buffer.size = 1024;

    s_queue = xQueueCreate(HA_QUEUE_DEPTH, sizeof(ha_evt_t));
    if (s_queue == NULL)
        return ESP_ERR_NO_MEM;
    if (xTaskCreate(ha_worker, "ha_mqtt", HA_WORKER_STACK, NULL,
                    HA_WORKER_PRIO, &s_worker) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t targs = {
        .callback = announce_timer_cb,
        .name = "ha_announce",
    };
    err = esp_timer_create(&targs, &s_announce_timer);
    if (err != ESP_OK)
        goto fail;

    s_client = esp_mqtt_client_init(&c);
    if (s_client == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                         mqtt_event_cb, NULL);
    if (err != ESP_OK)
        goto fail;
    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK)
        goto fail;
    ESP_LOGI(TAG, "client MQTT avviato: %s (client_id %s)", uri, s_client_id);
    return ESP_OK;

fail:
    if (s_client != NULL) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_announce_timer != NULL) {
        esp_timer_delete(s_announce_timer);
        s_announce_timer = NULL;
    }
    vTaskDelete(s_worker);
    s_worker = NULL;
    vQueueDelete(s_queue);
    s_queue = NULL;
    return err;
}

esp_err_t ha_mqtt_stop(void)
{
    if (!s_init || s_client == NULL)
        return ESP_ERR_INVALID_STATE;

    if (s_connected) {
        /* best-effort: meglio dell'attesa LWT, che avviene solo su caduta */
        ha_mqtt_publish_availability(false);
        /* breve attesa per il flush verso il broker prima dello stop */
        vTaskDelay(pdMS_TO_TICKS(HA_STOP_FLUSH_MS));
    }

    esp_err_t err = esp_mqtt_client_stop(s_client);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "stop client: %s", esp_err_to_name(err));
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;

    if (s_announce_timer != NULL) {
        esp_timer_stop(s_announce_timer);
        esp_timer_delete(s_announce_timer);
        s_announce_timer = NULL;
    }
    if (s_worker != NULL) {
        /* bloccato in xQueueReceive: nessuna risorsa trattenuta */
#if CONFIG_HA_MQTT_WORKER_WDT
        esp_task_wdt_delete(s_worker); /* NOT_FOUND se mai iscritto: ignorato */
#endif
        vTaskDelete(s_worker);
        s_worker = NULL;
    }
    if (s_queue != NULL) {
        ha_evt_t ev;
        while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
            if (ev.type == EVT_CMD)
                free(ev.payload); /* niente dispersioni a monte del delete */
        }
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    return ESP_OK;
}

int ha_mqtt_on_connected_add(void (*cb)(void *ctx), void *ctx)
{
    if (cb == NULL)
        return -1;
    for (int i = 0; i < HA_MQTT_CONNECTED_LISTENERS_MAX; i++) {
        if (s_connected_listeners[i].cb == NULL) {
            s_connected_listeners[i].cb = cb;
            s_connected_listeners[i].ctx = ctx;
            return i;
        }
    }
    return -1;
}

bool ha_mqtt_is_connected(void)
{
    return s_connected;
}

const char *ha_mqtt_device_id(void)
{
    return s_device_id;
}

const char *ha_mqtt_base_topic(void)
{
    return s_base_topic;
}

const char *ha_mqtt_availability_topic(void)
{
    return s_avail_topic;
}

const char *ha_mqtt_discovery_topic(void)
{
    return s_discovery_topic;
}

const char *ha_mqtt_sw_version(void)
{
    return esp_app_get_description()->version;
}

int ha_mqtt_publish(const char *rel_topic, const char *payload, size_t len,
                    bool retain)
{
    if (rel_topic == NULL || payload == NULL || len > HA_CMD_PAYLOAD_MAX)
        return -1;
    if (s_client == NULL || !s_connected)
        return -1;

    char topic[HA_BASE_TOPIC_LEN + 40];
    int n = snprintf(topic, sizeof(topic), "%s/%s", s_base_topic, rel_topic);
    if (n < 0 || (size_t)n >= sizeof(topic)) {
        s_publish_dropped++;
        ESP_LOGW(TAG, "topic relativo troppo lungo, scartato (%u scarti totali)",
                 (unsigned)s_publish_dropped);
        return -1;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, (int)len, 0,
                                         retain ? 1 : 0);
    if (msg_id < 0) {
        s_publish_dropped++;
        ESP_LOGW(TAG, "publish su %s scartato (%u scarti totali)", topic,
                 (unsigned)s_publish_dropped);
        return -1;
    }
    return 0;
}

int ha_mqtt_publish_discovery(const char *payload)
{
    if (payload == NULL || s_client == NULL || !s_connected)
        return -1;
    return esp_mqtt_client_publish(s_client, s_discovery_topic, payload, 0, 0,
                                   1) < 0
               ? -1
               : 0;
}

int ha_mqtt_publish_availability(bool online)
{
    if (s_client == NULL || !s_connected)
        return -1;
    const char *msg = online ? "online" : "offline";
    return esp_mqtt_client_publish(s_client, s_avail_topic, msg, 0, 0, 1) < 0
               ? -1
               : 0;
}

int ha_mqtt_publish_remove(void)
{
    if (s_client == NULL || !s_connected)
        return -1;
    return esp_mqtt_client_publish(s_client, s_discovery_topic, "", 0, 0, 1) < 0
               ? -1
               : 0;
}

/* --- CA da console --- */

esp_err_t ha_mqtt_ca_clear(void)
{
    s_ca_off = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(CA_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_erase_key(h, CA_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "CA rimossa da NVS");
    return err;
}

esp_err_t ha_mqtt_ca_append_line(const char *line)
{
    if (line == NULL)
        return ESP_ERR_INVALID_ARG;

    size_t n = strlen(line);
    bool end_line = strstr(line, "-----END CERTIFICATE-----") != NULL;
    /* spazio per la riga + '\n' + NUL finale */
    if (s_ca_off + n + 2 > CA_PEM_MAX) {
        ESP_LOGE(TAG, "PEM oltre %u byte: buffer di accumulo azzerato",
                 (unsigned)CA_PEM_MAX);
        s_ca_off = 0;
        return ESP_ERR_NO_MEM;
    }
    memcpy(&s_ca_pem[s_ca_off], line, n);
    s_ca_off += n;
    s_ca_pem[s_ca_off++] = '\n';
    if (!end_line)
        return ESP_OK;

    s_ca_pem[s_ca_off] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(CA_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        s_ca_off = 0;
        return err;
    }
    err = nvs_set_str(h, CA_NVS_KEY, s_ca_pem);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "CA salvata in NVS (%u byte)", (unsigned)s_ca_off);
    else
        ESP_LOGE(TAG, "salvataggio CA in NVS fallito: %s",
                 esp_err_to_name(err));
    s_ca_off = 0;
    return err;
}

esp_err_t ha_mqtt_ca_status(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
        return ESP_ERR_INVALID_ARG;

    size_t len = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(CA_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        /* nvs_get_str con buffer NULL restituisce la dimensione (con NUL) */
        err = nvs_get_str(h, CA_NVS_KEY, NULL, &len);
        nvs_close(h);
    }
    size_t bytes = err == ESP_OK && len > 0 ? len - 1 : 0;
    snprintf(out, out_len, "ca_tls=%s ca_bytes=%u",
             bytes > 0 ? "nvs" : "none", (unsigned)bytes);
    return ESP_OK;
}

esp_err_t ha_mqtt_ca_get(char *out, size_t out_len, size_t *written)
{
    if (written != NULL)
        *written = 0;
    if (out == NULL || out_len < 2)
        return ESP_ERR_INVALID_ARG;

#if CONFIG_PUMPKIN_MQTT_CA_NVS
    size_t len = out_len;
    nvs_handle_t h;
    esp_err_t err = nvs_open(CA_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
        return ESP_ERR_NOT_SUPPORTED;
    err = nvs_get_str(h, CA_NVS_KEY, out, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_INVALID_LENGTH)
        return ESP_ERR_NO_MEM;
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK)
        return err;
    size_t pem_len = strnlen(out, len);
    if (pem_len == 0)
        return ESP_ERR_NOT_FOUND;
    if (written != NULL)
        *written = pem_len;
    return ESP_OK;
#elif CONFIG_PUMPKIN_MQTT_CA_PARTITION
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        CONFIG_PUMPKIN_MQTT_CA_PART);
    if (part == NULL)
        return ESP_ERR_NOT_FOUND;
    size_t want = part->size;
    if (want > out_len - 1)
        want = out_len - 1;
    esp_err_t err = esp_partition_read(part, 0, out, want);
    if (err != ESP_OK)
        return err;
    size_t pem_len = strnlen(out, want);
    out[pem_len] = '\0';
    if (pem_len == 0)
        return ESP_ERR_NOT_FOUND;
    if (written != NULL)
        *written = pem_len;
    return ESP_OK;
#else
    (void)out;
    (void)out_len;
    return ESP_ERR_NOT_FOUND; /* PUMPKIN_MQTT_CA_NONE: nessuna CA */
#endif
}
