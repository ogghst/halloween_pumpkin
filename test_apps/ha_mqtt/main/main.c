/* ha_mqtt test app: bench console over the Phase 5 chain (Phase 4 network
 * chain: config_store v2 + led_effects overlay + pumpkin_fsm + radar_ld2420
 * coexisting offline, wifi_prov + net_mgr online; Phase 5: ha_entities +
 * ha_mqtt toward Home Assistant), ESP32 classic D1 mini.
 *
 * - Boot: NVS + default event loop, config_store (Kconfig defaults), LED
 *   effects, radar, FSM (identical wiring to the pumpkin_fsm app), then
 *   ha_entities (entity registry + domain event handlers + MQTT transport,
 *   started on NET_EVENT_UP), then the network bring-up owned by the app as
 *   required by the wifi_prov contract: esp_netif_init, default STA netif
 *   created ONCE here, esp_wifi_init here. net_mgr_start() reuses both.
 * - No stored credentials -> wifi_prov_begin() (BLE NimBLE by default);
 *   PROV_EVENT_SUCCESS/TIMEOUT/FAILED always end with net_mgr_start():
 *   without credentials it stays offline and its backoff retries, the
 *   GPIO32 long-press re-provisions.
 * - Overlays (led_fx_overlay_set): PROV on STARTED, cleared on the PROV
 *   outcome, NET_UP/NET_DOWN on the NET_EVENT pair. The FSM is never
 *   touched by the network state.
 * - REPL: net, prov, cfg_get [mqtt], cfg_flush, fsm_state, fsm_mode,
 *   ha, heap. No SSID/password is ever printed (only lengths for secrets).
 *
 * Log tags: components log under their own tags ("prov", "net", "ha_ent",
 * ...); this app logs under "app".
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "ha_entities.h"
#include "ha_mqtt.h"
#include "led_fx.h"
#include "net_mgr.h"
#include "nvs_flash.h"
#include "prov_mqtt_codec.h"
#include "pumpkin_fsm.h"
#include "radar_ld2420.h"
#include "wifi_prov.h"

static const char *TAG = "app";

/* Free heap at the end of boot: reference for the `prov` command. */
static uint32_t s_heap_baseline;

/* Provisioning transport: chosen at build time by the wifi_prov Kconfig
 * (bool symbols land in sdkconfig.h only when enabled). */
#if CONFIG_PUMPKIN_PROV_SCHEME_BLE
#define PROV_SCHEMA_STR "ble"
#elif CONFIG_PUMPKIN_PROV_SCHEME_SOFTAP
#define PROV_SCHEMA_STR "softap"
#else
#define PROV_SCHEMA_STR "?"
#endif

/* TLV buffers for `prov mqtt`: payload and ack both fit the component
 * worst case (PROV_MQTT_TLV_MAX_LEN, 179 B with the current limits). */

/* --- argv parsing helpers (no allocation, strict validation) --- */

static bool parse_u8(const char *s, uint8_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 255) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static bool parse_u16(const char *s, uint16_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 65535) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

static bool parse_bool01(const char *s, bool *out)
{
    if (strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    if (strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    return false;
}

/* --- shared print helpers --- */

static const char *state_name(fsm_state_t s)
{
    switch (s) {
    case FSM_STATE_IDLE:      return "IDLE";
    case FSM_STATE_TRIGGERED: return "TRIGGERED";
    case FSM_STATE_COOLDOWN:  return "COOLDOWN";
    case FSM_STATE_MANUAL:    return "MANUAL";
    case FSM_STATE_OFF:       return "OFF";
    default:                  return "?";
    }
}

static const char *param_name(config_param_id_t id)
{
    switch (id) {
    case CFG_PARAM_TRIGGER_MM:          return "trigger_mm";
    case CFG_PARAM_COOLDOWN_S:          return "cooldown_s";
    case CFG_PARAM_TRIGGER_DURATION_S:  return "duration_s";
    case CFG_PARAM_TRIGGER_ANIM:        return "trigger_anim";
    case CFG_PARAM_IDLE_ANIM:           return "idle_anim";
    case CFG_PARAM_IDLE_BRIGHTNESS_PCT: return "idle_bright";
    case CFG_PARAM_MAX_BRIGHTNESS_PCT:  return "max_bright";
    case CFG_PARAM_AUTO_MODE:           return "auto";
    case CFG_PARAM_COLOR:               return "color";
    case CFG_PARAM_BRIGHTNESS_PCT:      return "bright";
    case CFG_PARAM_POWER_ON:            return "power_on";
    case CFG_PARAM_MQTT:                return "mqtt";
    default:                            return "?";
    }
}

/* Provisioning identity, derived in-app exactly like the component does
 * (no getter in the wifi_prov API): service name "<prefix>-XXXXXX" from
 * the last 3 STA MAC bytes, PoP "pump-XXXXXX". Both are printed by
 * design: the MAC is public and the PoP is only a pairing convenience
 * (see ADR-008 for the threat-assumption note). */
static void prov_identity(char *name, size_t name_len, char *pop /* [13] */)
{
    uint8_t mac[6] = { 0 };
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_get_mac failed (%s)", esp_err_to_name(err));
    }
    snprintf(name, name_len, "%s-%02X%02X%02X",
             CONFIG_PUMPKIN_PROV_SERVICE_PREFIX, mac[3], mac[4], mac[5]);
    prov_pop_from_mac(mac, pop);
}

static void print_mqtt(const config_mqtt_t *m)
{
    printf("mqtt host='%s' port=%u user='%s' pass=<%u car., mascherata> "
           "tls=%u\n",
           m->host, m->port, m->user, (unsigned)strlen(m->pass),
           (unsigned)m->tls);
}

/* --- net console command --- */

static int cmd_net(int argc, char **argv)
{
    if (argc == 1) {
        net_diag_t d;
        net_mgr_get_diag(&d);
        printf("connected=%u rssi=%d ch=%u reconnects=%lu ps=%s "
               "listen_interval=%u\n",
               (unsigned)d.connected, (int)d.rssi, d.channel,
               (unsigned long)d.reconnects,
               d.ps_min_modem ? "min_modem" : "none",
               d.listen_interval);
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        const char *hostname = NULL;
        if (sta != NULL && esp_netif_get_hostname(sta, &hostname) == ESP_OK &&
            hostname != NULL) {
            printf("hostname=%s\n", hostname);
        } else {
            printf("hostname=- (netif STA non ancora creato)\n");
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "ps") == 0) {
        bool min_modem;
        if (strcmp(argv[2], "min") == 0) {
            min_modem = true;
        } else if (strcmp(argv[2], "max") == 0) {
            min_modem = false;
        } else {
            printf("usage: net ps <min|max>\n");
            return 1;
        }
        esp_err_t err = net_mgr_set_power_save(min_modem);
        printf("net ps %s: %s\n", min_modem ? "min" : "max",
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "listen") == 0) {
        uint8_t li;
        if (!parse_u8(argv[2], &li) || li < 1u || li > 10u) {
            printf("usage: net listen <1..10> (beacon)\n");
            return 1;
        }
        wifi_config_t wifi_cfg;
        esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
        if (err == ESP_OK) {
            wifi_cfg.sta.listen_interval = li;
            err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        }
        if (err != ESP_OK) {
            printf("net listen %u: %s\n", li, esp_err_to_name(err));
            return 1;
        }
        printf("net listen %u: scritto in wifi_config (effetto alla "
               "prossima associazione)\n",
               li);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "reconnect") == 0) {
        esp_err_t err = esp_wifi_disconnect();
        printf("net reconnect: esp_wifi_disconnect %s (il backoff di "
               "net_mgr riconnette)\n",
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    printf("usage: net | net ps <min|max> | net listen <1..10> | "
           "net reconnect\n");
    return 1;
}

/* --- prov console command --- */

static int prov_mqtt_cmd(int argc, char **argv)
{
    prov_mqtt_msg_t msg = { 0 };
    for (int i = 2; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL || eq == argv[i] || eq[1] == '\0') {
            printf("usage: prov mqtt [host=X] [port=N] [user=U] [pass=P] "
                   "[tls=0|1] (merge sui valori correnti)\n");
            return 1;
        }
        *eq = '\0';
        const char *key = argv[i];
        const char *val = eq + 1;
        if (strcmp(key, "host") == 0) {
            snprintf(msg.host, sizeof(msg.host), "%s", val);
            msg.has_host = true;
        } else if (strcmp(key, "port") == 0) {
            if (!parse_u16(val, &msg.port) || msg.port == 0) {
                printf("prov mqtt: port deve essere 1..65535\n");
                return 1;
            }
            msg.has_port = true;
        } else if (strcmp(key, "user") == 0) {
            snprintf(msg.user, sizeof(msg.user), "%s", val);
            msg.has_user = true;
        } else if (strcmp(key, "pass") == 0) {
            snprintf(msg.pass, sizeof(msg.pass), "%s", val);
            msg.has_pass = true;
        } else if (strcmp(key, "tls") == 0) {
            if (!parse_bool01(val, &msg.tls)) {
                printf("prov mqtt: tls deve essere 0 o 1\n");
                return 1;
            }
            msg.has_tls = true;
        } else {
            printf("prov mqtt: chiave '%s' sconosciuta\n", key);
            return 1;
        }
    }

    uint8_t buf[PROV_MQTT_TLV_MAX_LEN];
    size_t buf_len = 0;
    esp_err_t err = prov_mqtt_encode(&msg, buf, sizeof(buf), &buf_len);
    if (err != ESP_OK) {
        printf("prov mqtt: encode failed (%s)\n", esp_err_to_name(err));
        return 1;
    }

    /* Stesso percorso dell'endpoint di provisioning: decode + merge +
     * config_store_set_mqtt, con ack TLV dei valori correnti completi.
     * Contratto: *out_len in ingresso = dimensione del buffer. */
    uint8_t ack[PROV_MQTT_TLV_MAX_LEN];
    size_t ack_len = sizeof(ack);
    err = wifi_prov_handle_mqtt_tlv(buf, buf_len, ack, &ack_len);
    printf("prov mqtt: TLV %u B -> %s\n", (unsigned)buf_len,
           (err == ESP_OK) ? "applicati" : esp_err_to_name(err));
    if (err != ESP_OK) {
        return 1;
    }
    if (ack_len > 0) {
        printf("prov mqtt: ack TLV %u B\n", (unsigned)ack_len);
    }
    config_mqtt_t cur;
    if (config_store_get_mqtt(&cur) == ESP_OK) {
        print_mqtt(&cur);
    }
    return 0;
}

static int cmd_prov(int argc, char **argv)
{
    if (argc == 1) {
        char name[32];
        char pop[13];
        prov_identity(name, sizeof(name), pop);
        bool present = false;
        esp_err_t perr = wifi_prov_credentials_present(&present);
        uint32_t now = esp_get_free_heap_size();
        printf("attivo=%u schema=%s service=%s pop=%s credenziali=%s "
               "(%s) bt_released=%u heap=%lu B (baseline %lu B, %ld B)\n",
               (unsigned)wifi_prov_is_active(), PROV_SCHEMA_STR, name, pop,
               perr == ESP_OK ? (present ? "si" : "no") : "?",
               esp_err_to_name(perr), (unsigned)wifi_prov_bt_released(),
               (unsigned long)now, (unsigned long)s_heap_baseline,
               (long)((int64_t)now - (int64_t)s_heap_baseline));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "start") == 0) {
        esp_err_t err = wifi_prov_begin();
        printf("prov start: %s\n", (err == ESP_OK) ? "avviato"
                                                   : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        esp_err_t err = wifi_prov_abort();
        printf("prov stop: %s\n",
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "mqtt") == 0) {
        return prov_mqtt_cmd(argc, argv);
    }
    printf("usage: prov | prov start | prov stop | prov mqtt "
           "[host=X] [port=N] [user=U] [pass=P] [tls=0|1]\n");
    return 1;
}

/* --- cfg console commands --- */

static int cmd_cfg_get(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "mqtt") == 0) {
        config_mqtt_t m;
        esp_err_t err = config_store_get_mqtt(&m);
        if (err != ESP_OK) {
            printf("cfg_get mqtt: failed (%s)\n", esp_err_to_name(err));
            return 1;
        }
        print_mqtt(&m);
        return 0;
    }
    if (argc != 1) {
        printf("usage: cfg_get [mqtt]\n");
        return 1;
    }
    pumpkin_config_t c;
    esp_err_t err = config_store_get(&c);
    if (err != ESP_OK) {
        printf("cfg_get: failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    printf("version=%u\n", c.version);
    printf("trigger_mm=%u cooldown_s=%u duration_s=%u\n",
           c.trigger_mm, c.cooldown_s, c.trigger_duration_s);
    printf("trigger_anim='%s' idle_anim='%s'\n", c.trigger_anim, c.idle_anim);
    printf("idle_bright=%u%% max_bright=%u%% bright=%u%%\n",
           c.idle_brightness_pct, c.max_brightness_pct, c.brightness_pct);
    printf("auto=%u power_on=%u color=#%02x%02x%02x\n",
           (unsigned)c.auto_mode, (unsigned)c.power_on,
           c.color_r, c.color_g, c.color_b);
    print_mqtt(&c.mqtt);
    return 0;
}

static int cmd_cfg_flush(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = config_store_flush();
    printf("cfg_flush: %s\n",
           (err == ESP_OK) ? "ok" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* --- fsm console commands (portable from the pumpkin_fsm app) --- */

static int cmd_fsm_state(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fsm_state_t s = pumpkin_fsm_get_state();
    printf("state=%s (%d)\n", state_name(s), (int)s);
    return 0;
}

static int cmd_fsm_mode(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: fsm_mode <auto|manual|off>\n");
        return 1;
    }
    fsm_mode_t m;
    if (strcmp(argv[1], "auto") == 0) {
        m = FSM_MODE_AUTO;
    } else if (strcmp(argv[1], "manual") == 0) {
        m = FSM_MODE_MANUAL;
    } else if (strcmp(argv[1], "off") == 0) {
        m = FSM_MODE_OFF;
    } else {
        printf("usage: fsm_mode <auto|manual|off>\n");
        return 1;
    }
    esp_err_t err = fsm_cmd_set_mode(m);
    printf("fsm_mode %s: %s\n", argv[1],
           (err == ESP_OK) ? "queued" : esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* --- ha console command (Fase 5: Home Assistant / MQTT) --- */

/* CA TLS per ha_mqtt: accumulate a PEM line at a time from the console,
 * automatic commit on the "-----END CERTIFICATE-----" line. */
static int ha_ca_cmd(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "clear") == 0) {
        esp_err_t err = ha_mqtt_ca_clear();
        printf("ha ca clear: %s\n",
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "begin") == 0) {
        printf("Incolla il PEM una riga per volta:\n"
               "  ha ca +-----BEGIN CERTIFICATE-----\n"
               "  ha ca +MIIBszCCAVmgAwIBAgIUXxxx...\n"
               "  ha ca +-----END CERTIFICATE-----\n"
               "Commit su NVS automatico alla riga END.\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        char st[128];
        esp_err_t err = ha_mqtt_ca_status(st, sizeof(st));
        if (err != ESP_OK) {
            printf("ha ca status: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("ha ca status: %s\n", st);
        return 0;
    }
    if (argc == 3 && argv[2][0] == '+' && argv[2][1] != '\0') {
        /* La riga PEM arriva come token unico dopo il '+' iniziale. */
        esp_err_t err = ha_mqtt_ca_append_line(argv[2] + 1);
        printf("ha ca +: %s\n",
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    printf("usage: ha ca clear | begin | +<riga PEM> | status\n");
    return 1;
}

static int cmd_ha(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        char st[192];
        ha_entities_status(st, sizeof(st));
        printf("%s\n", st);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "announce") == 0) {
        if (!ha_mqtt_is_connected()) {
            printf("ha announce: MQTT non connesso\n");
            return 1;
        }
        esp_err_t err = ha_entities_announce();
        printf("ha announce: %s\n",
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "remove") == 0) {
        esp_err_t err = ha_entities_remove();
        printf("ha remove: %s\n",
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "ca") == 0) {
        return ha_ca_cmd(argc, argv);
    }
    if (argc == 4 && strcmp(argv[1], "pub") == 0) {
        char topic[sizeof("state/") + HA_CMD_KEY_MAX];
        snprintf(topic, sizeof(topic), "state/%s", argv[2]);
        int rc = ha_mqtt_publish(topic, argv[3], strlen(argv[3]), false);
        printf("ha pub %s: %s\n", topic,
               (rc == 0) ? "in coda" : "scartato");
        return (rc == 0) ? 0 : 1;
    }
    printf("usage: ha status | announce | remove | "
           "ca clear|begin|+<riga PEM>|status | pub <key> <payload>\n");
    return 1;
}

/* --- heap console command --- */

static int cmd_heap(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("free=%lu B largest=%lu B (baseline boot %lu B)\n",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned long)s_heap_baseline);
    return 0;
}

/* --- console registration --- */

static void register_cmd(const char *name, const char *help, const char *hint,
                         esp_console_cmd_func_t func)
{
    esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .hint = hint,
        .func = func,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void register_console_commands(void)
{
    esp_console_register_help_command();
    register_cmd("net", "Diag rete, power save, listen interval, reconnect",
                  "| ps <min|max> | listen <1..10> | reconnect", cmd_net);
    register_cmd("prov", "Stato/start/stop provisioning, parametri MQTT",
                  "| start | stop | mqtt k=v [...]", cmd_prov);
    register_cmd("cfg_get", "Dump configurazione (o solo mqtt)",
                 "[mqtt]", cmd_cfg_get);
    register_cmd("cfg_flush", "Commit immediato della configurazione su NVS",
                 NULL, cmd_cfg_flush);
    register_cmd("fsm_state", "Stato corrente della FSM", NULL,
                 cmd_fsm_state);
    register_cmd("fsm_mode", "Modo della FSM", "<auto|manual|off>",
                 cmd_fsm_mode);
    register_cmd("ha", "Home Assistant: stato, announce, remove, CA, pub debug",
                 "status | announce | remove | "
                 "ca clear|begin|+<riga>|status | pub <key> <payload>",
                 cmd_ha);
    register_cmd("heap", "Heap libero e largest block", NULL, cmd_heap);
}

/* --- event handlers (default loop task) --- */

static void fsm_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    if (id != FSM_EVENT_STATE_CHANGED || event_data == NULL) {
        return;
    }
    const fsm_state_change_t *sc = event_data;
    ESP_LOGI(TAG, "%lld ms: fsm %s -> %s",
             (long long)(esp_timer_get_time() / 1000),
             state_name(sc->prev_state), state_name(sc->state));
}

/* Config source of truth: forward the trigger distance to the radar
 * module whenever the store changes it (like the pumpkin_fsm app), and
 * log every change including CFG_PARAM_MQTT. ha_entities does the same
 * sync + echo: radar_set_trigger_mm writes only what differs. */
static void cfg_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    if (id != CONFIG_EVENT_CHANGED || event_data == NULL) {
        return;
    }
    const config_change_t *ch = event_data;
    if (ch->id == CFG_PARAM_TRIGGER_MM) {
        pumpkin_config_t c;
        if (config_store_get(&c) == ESP_OK) {
            esp_err_t err = radar_set_trigger_mm(c.trigger_mm);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "radar_set_trigger_mm failed (%s)",
                         esp_err_to_name(err));
            }
        }
    }
    ESP_LOGI(TAG, "%lld ms: cfg %s changed",
             (long long)(esp_timer_get_time() / 1000), param_name(ch->id));
}

/* Provisioning outcome -> overlay + network bring-up. net_mgr_start()
 * without stored credentials stays offline and retries with backoff. */
static void prov_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case PROV_EVENT_STARTED:
        ESP_LOGI(TAG, "prov started");
        led_fx_overlay_set(LED_FX_OVERLAY_PROV);
        break;
    case PROV_EVENT_SUCCESS: {
        const prov_result_t *r = event_data;
        ESP_LOGI(TAG, "prov success (ssid='%s')",
                 (r != NULL) ? r->ssid : "?");
        led_fx_overlay_set(LED_FX_OVERLAY_NONE);
        ESP_LOGI(TAG, "net_mgr_start: %s",
                 esp_err_to_name(net_mgr_start()));
        ESP_LOGI(TAG, "config_store_flush: %s",
                 esp_err_to_name(config_store_flush()));
        break;
    }
    case PROV_EVENT_TIMEOUT:
        ESP_LOGW(TAG, "prov timeout: riprovo la rete comunque (offline se "
                      "senza credenziali; tasto lungo per ri-provisionare)");
        led_fx_overlay_set(LED_FX_OVERLAY_NONE);
        ESP_LOGI(TAG, "net_mgr_start: %s",
                 esp_err_to_name(net_mgr_start()));
        break;
    case PROV_EVENT_FAILED:
        ESP_LOGW(TAG, "prov failed: riprovo la rete comunque (tasto lungo "
                      "per ri-provisionare)");
        led_fx_overlay_set(LED_FX_OVERLAY_NONE);
        ESP_LOGI(TAG, "net_mgr_start: %s",
                 esp_err_to_name(net_mgr_start()));
        break;
    default:
        break;
    }
}

static void net_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    if (id == NET_EVENT_UP) {
        const net_up_t *up = event_data;
        ESP_LOGI(TAG, "net up ip=%s", (up != NULL) ? up->ip : "?");
        led_fx_overlay_set(LED_FX_OVERLAY_NET_UP);
    } else if (id == NET_EVENT_DOWN) {
        const net_down_t *down = event_data;
        ESP_LOGW(TAG, "net down reason=%d next in %lu ms",
                 (down != NULL) ? down->reason : 0,
                 (down != NULL) ? (unsigned long)down->next_ms : 0ul);
        led_fx_overlay_set(LED_FX_OVERLAY_NET_DOWN);
    }
}

/* --- boot --- */

/* Kconfig-backed defaults, used by config_store_init when nothing (or an
 * unparsable blob) is stored; the store sanitizes a copy of them. Same
 * values as the pumpkin_fsm app. */
static pumpkin_config_t build_defaults(void)
{
    pumpkin_config_t d = {
        .version = PUMPKIN_CONFIG_VERSION,
        .trigger_mm = CONFIG_PUMPKIN_TRIGGER_MM,
        .cooldown_s = CONFIG_PUMPKIN_COOLDOWN_S,
        .trigger_duration_s = CONFIG_PUMPKIN_TRIGGER_DURATION_S,
        .idle_brightness_pct = CONFIG_PUMPKIN_IDLE_BRIGHTNESS_PCT,
        .max_brightness_pct = CONFIG_PUMPKIN_MAX_BRIGHTNESS_PCT,
        .auto_mode = CONFIG_PUMPKIN_AUTO_MODE,
        .color_r = CONFIG_PUMPKIN_COLOR_R,
        .color_g = CONFIG_PUMPKIN_COLOR_G,
        .color_b = CONFIG_PUMPKIN_COLOR_B,
        .brightness_pct = CONFIG_PUMPKIN_BRIGHTNESS_PCT,
        .power_on = true, /* boot policy overridden by the stored value */
    };
    strncpy(d.trigger_anim, CONFIG_PUMPKIN_TRIGGER_ANIM,
            sizeof(d.trigger_anim) - 1);
    d.trigger_anim[sizeof(d.trigger_anim) - 1] = '\0';
    strncpy(d.idle_anim, CONFIG_PUMPKIN_IDLE_ANIM, sizeof(d.idle_anim) - 1);
    d.idle_anim[sizeof(d.idle_anim) - 1] = '\0';
    return d;
}

static void fatal(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s failed (%s)", what, esp_err_to_name(err));
    abort();
}

void app_main(void)
{
    /* NVS must be up before config_store loads the persisted blob (and
     * before provisioning writes to it); erase + retry on layout
     * changes, like the other bench apps. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Offline chain, identical to the pumpkin_fsm bench app: it proves
     * the radar + FSM + LED stack coexists with Wi-Fi/BLE. */
    pumpkin_config_t defaults = build_defaults();
    esp_err_t err = config_store_init(&defaults);
    if (err != ESP_OK) {
        fatal("config_store_init", err);
    }

    err = led_fx_init();
    if (err != ESP_OK) {
        fatal("led_fx_init", err);
    }

    err = radar_init();
    if (err != ESP_OK) {
        fatal("radar_init", err);
    }

    err = pumpkin_fsm_init();
    if (err != ESP_OK) {
        fatal("pumpkin_fsm_init", err);
    }

    /* Fase 5: registro entita' HA + handler radar/fsm/config/net + client
     * MQTT pronto (l'avvio effettivo avviene su NET_EVENT_UP, quando
     * net_mgr annuncia l'IP). Prima dei handler net/prov: registra i suoi. */
    err = ha_entities_init();
    if (err != ESP_OK) {
        fatal("ha_entities_init", err);
    }

    ESP_ERROR_CHECK(esp_event_handler_register(CONFIG_EVENT,
                                               CONFIG_EVENT_CHANGED,
                                               cfg_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(FSM_EVENT,
                                               FSM_EVENT_STATE_CHANGED,
                                               fsm_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROV_EVENT, ESP_EVENT_ANY_ID,
                                               prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID,
                                               net_event_handler, NULL));

    /* Initial sync of the trigger distance (store -> radar module). */
    pumpkin_config_t cfg;
    ESP_ERROR_CHECK(config_store_get(&cfg));
    err = radar_set_trigger_mm(cfg.trigger_mm);
    ESP_LOGI(TAG, "radar trigger synced to %u mm (%s)", cfg.trigger_mm,
             esp_err_to_name(err));

    /* Network bring-up owned by the app (wifi_prov contract): netif STA
     * created exactly once here, esp_wifi_init here. net_mgr_start()
     * adopts both instead of re-creating them. */
    ESP_ERROR_CHECK(esp_netif_init());
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        abort();
    }
    wifi_init_config_t wifi_cfg_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg_init));

    err = wifi_prov_app_init();
    if (err != ESP_OK) {
        fatal("wifi_prov_app_init", err);
    }

    /* Boot baseline for the heap command (after all init tasks live). */
    s_heap_baseline = esp_get_free_heap_size();

    bool present = false;
    err = wifi_prov_credentials_present(&present);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_credentials_present failed (%s)",
                 esp_err_to_name(err));
    }
    if (present) {
        ESP_LOGI(TAG, "credenziali Wi-Fi presenti (bt_released=%u): "
                      "avvio net_mgr",
                 (unsigned)wifi_prov_bt_released());
        ESP_LOGI(TAG, "net_mgr_start: %s",
                 esp_err_to_name(net_mgr_start()));
    } else {
        ESP_LOGI(TAG, "nessuna credenziale Wi-Fi: avvio provisioning");
        err = wifi_prov_begin();
        ESP_LOGI(TAG, "wifi_prov_begin: %s", esp_err_to_name(err));
    }

    register_console_commands();

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "ha>";
    esp_console_dev_uart_config_t hw_config =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config,
                                              &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
