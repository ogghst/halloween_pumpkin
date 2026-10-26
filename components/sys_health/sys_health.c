/* Salute di sistema Fase 7 (glue IDF): conferma rollback dell'immagine OTA,
 * statistiche di reset su NVS, telemetria stabilita'/task su MQTT,
 * iscrizione del proprio task al watchdog e comando heap tracing.
 * Il nucleo puro host-testabile e' in sys_health_core.c. */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#endif
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "ha_mqtt.h"
#include "net_mgr.h"
#include "pumpkin_fsm.h"
#include "radar_ld2420.h"

#include "sys_health.h"
#include "sys_health_core.h"

static const char *TAG = "sys_health";

#define SYS_NVS_NAMESPACE "pumpkin_sys"
#define SYS_NVS_KEY_RESETS "resets"
#define SYS_NVS_KEY_REASON "reason"
#define SYS_TASK_STACK 4096
#define SYS_TASK_PRIO 3
#define SYS_WDT_SLICE_MS 1000
#define SYS_HEAPTRACE_RECORDS 200
#define SYS_STATUS_TASKS_MAX 24
#define SYS_JSON_CAP 512 /* = HA_CMD_PAYLOAD_MAX; stability resta sotto 400 */

static sys_health_flags_t s_flags = {
    .wifi_up = false,
    .mqtt_connected = false,
    .sensor_ok = false,
    .fsm_ok = true, /* FSM non avviata = IDLE */
};
static uint32_t s_wifi_reconn;
static uint32_t s_mqtt_reconn;
static uint32_t s_reset_count;
static int s_reset_reason;
static bool s_init_done;
static TaskHandle_t s_task;

#if CONFIG_HEAP_TRACING_STANDALONE
static heap_trace_record_t s_trace_buf[SYS_HEAPTRACE_RECORDS];
static bool s_trace_inited;
#endif

/* --- conferma rollback --- */

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
/* Riverifica se l'immagine in pending verify ha meritato la conferma
 * (Wi-Fi + MQTT + sensore + FSM in IDLE/OFF, una sola volta). Immagini in
 * ogni altro stato: mai toccate. Chiamato a ogni cambio flag e a ogni
 * periodo del task: la conferma ripetuta e' impedita dal cambio di stato
 * dell'immagine (PENDING_VERIFY -> VALID). */
static void rollback_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK)
        return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY)
        return;
    sys_health_flags_t f = s_flags;
    if (!sys_health_core_should_confirm(&f, true))
        return;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK)
        ESP_LOGI(TAG, "immagine %s confermata valida: rollback annullato "
                      "(wifi + mqtt + sensore + fsm ok)",
                 esp_app_get_description()->version);
    else
        ESP_LOGE(TAG, "conferma immagine fallita: %s", esp_err_to_name(err));
}
#else
static inline void rollback_check(void)
{
}
#endif

/* --- statistiche di reset --- */

/* Al boot: incrementa il contatore di accensioni e registra il motivo
 * dell'ultimo reset (NVS namespace "pumpkin_sys"). Senza NVS il contatore
 * resta in RAM (parte da 1). */
static void reset_stats_load(void)
{
    s_reset_reason = (int)esp_reset_reason();
    nvs_handle_t h;
    esp_err_t err = nvs_open(SYS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS %s non disponibile (%s): contatore reset solo in RAM",
                 SYS_NVS_NAMESPACE, esp_err_to_name(err));
        s_reset_count = 1;
        return;
    }
    uint32_t resets = 0;
    (void)nvs_get_u32(h, SYS_NVS_KEY_RESETS, &resets); /* default 0 */
    s_reset_count = resets + 1;
    nvs_set_u32(h, SYS_NVS_KEY_RESETS, s_reset_count);
    nvs_set_u32(h, SYS_NVS_KEY_REASON, (uint32_t)s_reset_reason);
    if (nvs_commit(h) != ESP_OK)
        ESP_LOGW(TAG, "commit statistiche reset fallito");
    nvs_close(h);
}

/* --- eventi (task esp_event, operazioni veloci) --- */

static void on_net_event(void *arg, esp_event_base_t base, int32_t id,
                         void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == NET_EVENT_UP) {
        s_flags.wifi_up = true;
        s_wifi_reconn++;
    } else if (id == NET_EVENT_DOWN) {
        s_flags.wifi_up = false;
    }
    rollback_check();
}

static void on_fsm_event(void *arg, esp_event_base_t base, int32_t id,
                         void *data)
{
    (void)arg;
    (void)base;
    if (id == FSM_EVENT_STATE_CHANGED && data != NULL) {
        const fsm_state_change_t *ch = data;
        s_flags.fsm_ok = ch->state == FSM_STATE_IDLE ||
                         ch->state == FSM_STATE_OFF;
    }
    rollback_check();
}

static void on_radar_event(void *arg, esp_event_base_t base, int32_t id,
                           void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    /* RADAR_OK dopo radar_init o a fine recovery; FAULT a sensore muto:
     * entrambe le vie (evento e notify_sensor_ok) sono idempotenti. */
    if (id == RADAR_EVENT_RADAR_OK)
        s_flags.sensor_ok = true;
    else if (id == RADAR_EVENT_RADAR_FAULT)
        s_flags.sensor_ok = false;
    rollback_check();
}

/* --- listener MQTT (task worker di ha_mqtt) --- */

/* A ogni (ri)connessione: flag + contatore + publish retained del riepilogo
 * reset. Gira nel worker: solo operazioni veloci, come le altre callback. */
static void on_mqtt_connected(void *ctx)
{
    (void)ctx;
    s_flags.mqtt_connected = true;
    s_mqtt_reconn++;

    char payload[256];
    int n = snprintf(payload, sizeof(payload),
                     "{\"count\":%u,\"reason\":%d,\"reason_str\":\"%s\","
                     "\"v\":\"%s\"}",
                     (unsigned)s_reset_count, s_reset_reason,
                     sys_health_core_reset_reason_str(s_reset_reason),
                     esp_app_get_description()->version);
    if (n > 0 && (size_t)n < sizeof(payload))
        ha_mqtt_publish("status/reset", payload, (size_t)n, true);
    rollback_check();
}

/* --- campioni e publish --- */

static void sample_fill(sys_health_sample_t *s)
{
    s_flags.mqtt_connected = ha_mqtt_is_connected();
    *s = (sys_health_sample_t){
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
        .free_heap = esp_get_free_heap_size(),
        .min_heap = esp_get_minimum_free_heap_size(),
        .max_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        .wifi_reconn = s_wifi_reconn,
        .mqtt_reconn = s_mqtt_reconn,
        .resets = s_reset_count,
        .version = esp_app_get_description()->version,
    };
}

/* Istantanea dei task: nomi + high water mark in byte. I task oltre il cap
 * restano fuori (il JSON dei task resta sotto 512 B). */
static size_t tasks_fill(sys_health_task_hwm_t *out, size_t max)
{
    TaskStatus_t st[SYS_STATUS_TASKS_MAX];
    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n > SYS_STATUS_TASKS_MAX)
        n = SYS_STATUS_TASKS_MAX;
    n = uxTaskGetSystemState(st, n, NULL);
    UBaseType_t count = n < max ? n : (UBaseType_t)max;
    for (UBaseType_t i = 0; i < count; i++) {
        out[i].name = st[i].pcTaskName;
        out[i].hwm = (uint32_t)st[i].usStackHighWaterMark *
                     (uint32_t)sizeof(StackType_t);
    }
    return count;
}

static void telemetry_publish(void)
{
    if (!ha_mqtt_is_connected())
        return;

    sys_health_sample_t s;
    sample_fill(&s);
    char json[SYS_JSON_CAP];
    size_t len = sys_health_core_stability_json(json, sizeof(json), &s);
    if (len > 0)
        ha_mqtt_publish("status/stability", json, len, false);

    sys_health_task_hwm_t tasks[SYS_STATUS_TASKS_MAX];
    size_t n = tasks_fill(tasks, SYS_STATUS_TASKS_MAX);
    len = sys_health_core_tasks_json(json, sizeof(json), tasks, n);
    if (len > 0)
        ha_mqtt_publish("status/tasks", json, len, false);
}

/* --- task telemetria --- */

/* Attesa a passi da 1 s con feed del watchdog (il periodo, di default 60 s,
 * e' oltre il timeout di 5 s del task watchdog); a ogni periodo compiuto
 * pubblica stability + tasks e riverifica il rollback. */
static void telemetry_task(void *arg)
{
    (void)arg;
#if CONFIG_SYS_HEALTH_TASK_WDT
    esp_err_t werr = esp_task_wdt_add(NULL);
    if (werr != ESP_OK && werr != ESP_ERR_INVALID_ARG)
        ESP_LOGW(TAG, "iscrizione task watchdog fallita: %s",
                 esp_err_to_name(werr));
#endif
    const uint32_t period_s = (uint32_t)CONFIG_SYS_HEALTH_PERIOD_S;
    uint32_t elapsed = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SYS_WDT_SLICE_MS));
#if CONFIG_SYS_HEALTH_TASK_WDT
        esp_task_wdt_reset();
#endif
        if (++elapsed < period_s)
            continue;
        elapsed = 0;
        telemetry_publish();
        rollback_check();
    }
}

/* --- API --- */

esp_err_t sys_health_init(void)
{
    if (s_init_done)
        return ESP_ERR_INVALID_STATE;

    reset_stats_load();

#if CONFIG_HEAP_TRACING_STANDALONE
    if (!s_trace_inited) {
        esp_err_t err = heap_trace_init_standalone(s_trace_buf,
                                                   SYS_HEAPTRACE_RECORDS);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "heap trace non disponibile: %s",
                     esp_err_to_name(err));
        else
            s_trace_inited = true;
    }
#endif

    /* Lettura iniziale: la FSM puo' aver gia' cambiato stato prima della
     * init (i suoi eventi successivi arrivano via handler). */
    fsm_state_t st = pumpkin_fsm_get_state();
    s_flags.fsm_ok = st == FSM_STATE_IDLE || st == FSM_STATE_OFF;

    esp_err_t err = esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID,
                                               on_net_event, NULL);
    if (err == ESP_OK)
        err = esp_event_handler_register(FSM_EVENT, ESP_EVENT_ANY_ID,
                                         on_fsm_event, NULL);
    if (err == ESP_OK)
        err = esp_event_handler_register(PRESENCE_EVENT, ESP_EVENT_ANY_ID,
                                         on_radar_event, NULL);
    if (err != ESP_OK)
        return err;

    if (ha_mqtt_on_connected_add(on_mqtt_connected, NULL) < 0)
        ESP_LOGW(TAG, "nessuno slot listener MQTT libero: niente status/reset");

    if (xTaskCreate(telemetry_task, "sys_health", SYS_TASK_STACK, NULL,
                    SYS_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_init_done = true;
    ESP_LOGI(TAG, "salute di sistema attiva (periodo %u s, resets=%u, "
                  "ultimo reset: %s)",
             (unsigned)CONFIG_SYS_HEALTH_PERIOD_S, (unsigned)s_reset_count,
             sys_health_core_reset_reason_str(s_reset_reason));
    return ESP_OK;
}

void sys_health_notify_sensor_ok(void)
{
    bool was = s_flags.sensor_ok;
    s_flags.sensor_ok = true;
    if (!was) /* idempotente: solo la transizione puo' sbloccare la conferma */
        rollback_check();
}

sys_health_flags_t sys_health_get_flags(void)
{
    return s_flags;
}

uint32_t sys_health_reset_count(void)
{
    return s_reset_count;
}

uint32_t sys_health_wifi_reconnections(void)
{
    return s_wifi_reconn;
}

uint32_t sys_health_mqtt_reconnections(void)
{
    return s_mqtt_reconn;
}

/* --- console --- */

static const char *ota_state_str(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    default:                         return "UNDEFINED";
    }
}

static int cmd_sysinfo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run != NULL)
        (void)esp_ota_get_state_partition(run, &st); /* resta UNDEFINED se fallisce */

    sys_health_sample_t s;
    sample_fill(&s);
    printf("version=%s project=%s idf=%s\n", app->version, app->project_name,
           app->idf_ver);
    printf("ota_state=%s partition=%s\n", ota_state_str(st),
           run != NULL ? run->label : "n/d");
    printf("resets=%u reason=%d (%s)\n", (unsigned)s_reset_count,
           s_reset_reason, sys_health_core_reset_reason_str(s_reset_reason));
    printf("uptime_s=%u wifi_up=%d mqtt=%d sensor_ok=%d fsm_ok=%d\n",
           (unsigned)s.uptime_s, s_flags.wifi_up, s_flags.mqtt_connected,
           s_flags.sensor_ok, s_flags.fsm_ok);
    printf("wifi_reconn=%u mqtt_reconn=%u\n", (unsigned)s.wifi_reconn,
           (unsigned)s.mqtt_reconn);
    printf("heap free=%u min=%u maxblk=%u\n", (unsigned)s.free_heap,
           (unsigned)s.min_heap, (unsigned)s.max_block);
    return 0;
}

static int cmd_stability(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sys_health_sample_t s;
    sample_fill(&s);
    char json[SYS_JSON_CAP];
    size_t len = sys_health_core_stability_json(json, sizeof(json), &s);
    if (len == 0) {
        printf("errore: sample non serializzabile\n");
        return 1;
    }
    printf("%s\n", json);
    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    sys_health_task_hwm_t tasks[SYS_STATUS_TASKS_MAX];
    size_t n = tasks_fill(tasks, SYS_STATUS_TASKS_MAX);
    char json[SYS_JSON_CAP];
    size_t len = sys_health_core_tasks_json(json, sizeof(json), tasks, n);
    if (len == 0) {
        printf("errore: nessun task campionato\n");
        return 1;
    }
    printf("%u task: %s\n", (unsigned)n, json);
    return 0;
}

#if CONFIG_HEAP_TRACING_STANDALONE
static int cmd_heaptrace(int argc, char **argv)
{
    if (argc < 2) {
        printf("uso: heaptrace start|stop|dump [n]\n");
        return 1;
    }
    if (strcmp(argv[1], "start") == 0) {
        esp_err_t err = heap_trace_start(HEAP_TRACE_ALL);
        printf("heaptrace start: %s\n", esp_err_to_name(err));
    } else if (strcmp(argv[1], "stop") == 0) {
        esp_err_t err = heap_trace_stop();
        printf("heaptrace stop: %s\n", esp_err_to_name(err));
    } else if (strcmp(argv[1], "dump") == 0) {
        size_t max = heap_trace_get_count();
        if (argc >= 3) {
            unsigned long want = strtoul(argv[2], NULL, 0);
            if ((size_t)want < max)
                max = (size_t)want;
        }
        size_t shown = 0;
        for (size_t i = 0; i < heap_trace_get_count() && shown < max; i++) {
            heap_trace_record_t r;
            if (heap_trace_get(i, &r) != ESP_OK)
                break;
            printf("%p alloc %u byte\n", r.address, (unsigned)r.size);
            shown++;
        }
        printf("heaptrace: %u/%u record\n", (unsigned)shown,
               (unsigned)heap_trace_get_count());
    } else {
        printf("uso: heaptrace start|stop|dump [n]\n");
        return 1;
    }
    return 0;
}
#endif

esp_err_t sys_health_register_console(void)
{
    const esp_console_cmd_t sysinfo = {
        .command = "sysinfo",
        .help = "Versione, stato immagine OTA, reset, uptime, flag e heap",
        .hint = NULL,
        .func = &cmd_sysinfo,
    };
    esp_err_t err = esp_console_cmd_register(&sysinfo);
    if (err != ESP_OK)
        return err;

    const esp_console_cmd_t stability = {
        .command = "stability",
        .help = "JSON di telemetria stabilita' (come status/stability)",
        .hint = NULL,
        .func = &cmd_stability,
    };
    err = esp_console_cmd_register(&stability);
    if (err != ESP_OK)
        return err;

    const esp_console_cmd_t tasks = {
        .command = "tasks",
        .help = "Task attivi con high water mark dello stack (JSON)",
        .hint = NULL,
        .func = &cmd_tasks,
    };
    err = esp_console_cmd_register(&tasks);
    if (err != ESP_OK)
        return err;

#if CONFIG_HEAP_TRACING_STANDALONE
    const esp_console_cmd_t heaptrace = {
        .command = "heaptrace",
        .help = "Heap tracing: start | stop | dump [n]",
        .hint = "start | stop | dump [n]",
        .func = &cmd_heaptrace,
    };
    err = esp_console_cmd_register(&heaptrace);
    if (err != ESP_OK)
        return err;
#endif

    return ESP_OK;
}
