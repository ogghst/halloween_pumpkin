/* App di produzione della zucca (Fase 7): wiring completo Fase 3-5
 * (config_store + led_effects + pumpkin_fsm + radar + wifi_prov + net_mgr +
 * ha_entities/ha_mqtt) piu' pumpkin_ota (HTTPS OTA con gate) e sys_health
 * (salute post-OTA, conferma rollback).
 *
 * - Boot: NVS + event loop, config_store, LED, radar, FSM, eyes_ctrl
 *   (ponte eventi FSM/config -> zona occhi, Fase 8), ha_entities
 *   (registro entita' + handler dominio + trasporto MQTT avviato su
 *   NET_EVENT_UP), pumpkin_ota, sys_health. Poi rete: netif STA creato UNA
 *   volta qui, net_mgr_start adotta; senza credenziali -> provisioning BLE.
 * - MQTT: comando set/update {"url":..,"version":..,"release_summary":..}
 *   -> pumpkin_ota_start (gate batteria/LED/heap, versione immagine,
 *   eventi su update/status; installazione = reboot, immagine in
 *   PENDING_VERIFY fino alla conferma di sys_health dopo il riavvio).
 * - REPL: net, prov, cfg_get, cfg_flush, fsm_state, fsm_mode, eyes, ha,
 *   heap, ota, ota_status. Nessuna credenziale viene stampata.
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
#include "eyes_ctrl.h"
#include "ha_entities.h"
#include "ha_mqtt.h"
#include "led_fx.h"
#include "net_mgr.h"
#include "nvs_flash.h"
#include "prov_mqtt_codec.h"
#include "pumpkin_fsm.h"
#include "pumpkin_ota.h"
#include "radar_ld2420.h"
#include "sys_health.h"
#include "wifi_prov.h"

static const char *TAG = "app";

/* Free heap a fine boot: riferimento per i comandi heap/ota_status. */
static uint32_t s_heap_baseline;

/* Provisioning: schema scelto a compile time dal Kconfig di wifi_prov
 * (i bool CONFIG_* sono definiti solo se abilitati). */
#if CONFIG_PUMPKIN_PROV_SCHEME_BLE
#define PROV_SCHEMA_STR "ble"
#elif CONFIG_PUMPKIN_PROV_SCHEME_SOFTAP
#define PROV_SCHEMA_STR "softap"
#else
#define PROV_SCHEMA_STR "?"
#endif

/* --- helper di parsing argv (nessuna allocazione) --- */

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

/* --- helper di stampa --- */

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

/* Identita' provisioning, derivata in-app come fa il componente (nessun
 * getter nella API di wifi_prov): "<prefix>-XXXXXX" dai MAC [3..5], PoP
 * "pump-XXXXXX". Stampati di proposito: MAC pubblico, PoP solo comodita'
 * di pairing (vedi ADR-008). */
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

/* --- comando net --- */

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

/* --- comando prov --- */

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
     * config_store_set_mqtt, con ack TLV dei valori correnti completi. */
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

/* --- comandi cfg --- */

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

/* --- comandi fsm --- */

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

/* --- comando eyes (Fase 8: zona occhi, 2 pixel in coda alla catena) --- */

static int cmd_eyes(int argc, char **argv)
{
    if (argc == 1) {
        led_fx_eyes_look_t look;
        esp_err_t err = led_fx_eyes_get_look(&look);
        if (err != ESP_OK) {
            printf("eyes: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("eyes: effect=%s rgb=%u/%u/%u bright=%u blink=%u oneshot=%u\n",
               look.effect, look.r, look.g, look.b, look.brightness,
               look.blink_overlay ? 1 : 0, look.oneshot_active ? 1 : 0);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "effect") == 0) {
        esp_err_t err = led_fx_eyes_set_effect(argv[2]);
        printf("eyes effect %s: %s\n", argv[2],
               (err == ESP_OK) ? "queued" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 5 && strcmp(argv[1], "color") == 0) {
        uint8_t r, g, b;
        if (!parse_u8(argv[2], &r) || !parse_u8(argv[3], &g) ||
            !parse_u8(argv[4], &b)) {
            printf("usage: eyes color <r 0-255> <g 0-255> <b 0-255>\n");
            return 1;
        }
        esp_err_t err = led_fx_eyes_set_color(r, g, b);
        printf("eyes color %u/%u/%u: %s\n", r, g, b,
               (err == ESP_OK) ? "queued" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "bright") == 0) {
        uint8_t v;
        if (!parse_u8(argv[2], &v)) {
            printf("usage: eyes bright <0-255>\n");
            return 1;
        }
        esp_err_t err = led_fx_eyes_set_brightness(v);
        printf("eyes bright %u: %s\n", v,
               (err == ESP_OK) ? "queued" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc >= 2 && argc <= 3 && strcmp(argv[1], "flash") == 0) {
        uint8_t ms = 0;
        if (argc == 3 && !parse_u8(argv[2], &ms)) {
            printf("usage: eyes flash [ms 0-255]\n");
            return 1;
        }
        esp_err_t err = led_fx_eyes_play_oneshot("flash", ms);
        printf("eyes flash %u ms: %s\n", (unsigned)ms,
               (err == ESP_OK) ? "queued" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "glare") == 0) {
        uint16_t s;
        if (!parse_u16(argv[2], &s) || s == 0) {
            printf("usage: eyes glare <durata_s 1-3600>\n");
            return 1;
        }
        esp_err_t err = led_fx_eyes_play_oneshot("glare", (uint32_t)s * 1000u);
        printf("eyes glare %u s: %s\n", (unsigned)s,
               (err == ESP_OK) ? "queued" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "idle") == 0) {
        /* Effetto di riposo persistito (config_store -> evento -> eyes_ctrl
         * ri-applica il piano IDLE: glow/impostazione sopravvive al reboot). */
        esp_err_t err = config_store_set_anim(CFG_PARAM_EYES_IDLE_EFFECT,
                                              argv[2]);
        printf("eyes idle %s: %s\n", argv[2],
               (err == ESP_OK) ? "ok (persistito)" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 3 && (strcmp(argv[1], "follow") == 0 ||
                      strcmp(argv[1], "blink") == 0)) {
        bool v;
        if (!parse_bool01(argv[2], &v)) {
            printf("usage: eyes %s <0|1>\n", argv[1]);
            return 1;
        }
        esp_err_t err = (strcmp(argv[1], "follow") == 0)
                            ? led_fx_eyes_follow_fsm(v)
                            : led_fx_eyes_set_blink_overlay(v);
        printf("eyes %s %u: %s\n", argv[1], v ? 1 : 0,
               (err == ESP_OK) ? "queued" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "effects") == 0) {
        const led_fx_eyes_effect_info_t *fx = NULL;
        size_t n = led_fx_eyes_get_effects(&fx);
        printf("effetti occhi (%u):", (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            printf(" %s%s", fx[i].name, fx[i].oneshot ? "*" : "");
        }
        printf("  (* = one-shot)\n");
        return 0;
    }
    printf("usage: eyes | effects | effect <nome> | color <r> <g> <b> | "
           "bright <0-255> | flash [ms] | glare <s> | idle <nome> | "
           "follow <0|1> | blink <0|1>\n");
    return 1;
}

/* --- comando ha (Home Assistant / MQTT) --- */

/* CA TLS per ha_mqtt e OTA: accumulo del PEM riga per riga da console,
 * commit automatico alla riga "-----END CERTIFICATE-----". */
static int ha_ca_cmd(int argc, char **argv)
{
    /* Dispatcher annidato da cmd_ha SENZA shift: qui argv[0]="ha",
     * argv[1]="ca", argv[2]=sottocomando. */
    if (argc == 3 && strcmp(argv[2], "clear") == 0) {
        esp_err_t err = ha_mqtt_ca_clear();
        printf("ha ca clear: %s\n",
               (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[2], "begin") == 0) {
        printf("Incolla il PEM una riga per volta; l'argomento va quotato "
               "perche' resti unico (le righe BEGIN/END contengono spazi):\n"
               "  ha ca \"+-----BEGIN CERTIFICATE-----\"\n"
               "  ha ca +MIIBszCCAVmgAwIBAgIUXxxx...\n"
               "  ha ca \"+-----END CERTIFICATE-----\"\n"
               "Commit su NVS automatico alla riga END.\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "status") == 0) {
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

/* --- comandi ota (Fase 7) --- */

static int cmd_ota(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("usage: ota <url https> [versione]\n");
        return 1;
    }
    if (pumpkin_ota_is_running()) {
        printf("ota: aggiornamento gia' in corso\n");
        return 1;
    }
    /* Il gate (batteria sconosciuta -> consentita, LED, heap) e il
     * rifiuto per URL/versione sono dentro pumpkin_ota_start, che pubblica
     * l'evento rejected con il motivo; qui basta l'esito. */
    esp_err_t err = pumpkin_ota_start(argv[1], argc == 3 ? argv[2] : NULL,
                                      NULL);
    if (err == ESP_OK) {
        printf("ota: avviato (download e flash in background, eventi su "
               "update/status)\n");
        return 0;
    }
    printf("ota: rifiutato (%s); dettaglio con 'ota_status'\n",
           esp_err_to_name(err));
    return 1;
}

static int cmd_ota_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char st[160];
    pumpkin_ota_status(st, sizeof(st));
    printf("%s\n", st);
    return 0;
}

/* --- comando heap --- */

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

/* --- comando stats (Fase 8: verifica 60 fps a banco) --- */

static int cmd_stats(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    led_fx_stats_t st;
    led_fx_get_stats(&st);
    printf("led: frames=%lu dropped=%lu render_avg=%lu us render_max=%lu us\n",
           (unsigned long)st.frames, (unsigned long)st.dropped,
           (unsigned long)st.render_us_avg,
           (unsigned long)st.render_us_max);
    printf("heap free=%lu min=%lu\n",
           (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size());
    return 0;
}

/* --- registrazione console --- */

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
    register_cmd("eyes", "Occhi (Fase 8): look, effetti, one-shot, follow",
                 "| effects | effect <nome> | color <r> <g> <b> | "
                 "bright <0-255> | flash [ms] | glare <s> | idle <nome> | "
                 "follow <0|1> | blink <0|1>",
                 cmd_eyes);
    register_cmd("ha", "Home Assistant: stato, announce, remove, CA, pub debug",
                 "status | announce | remove | "
                 "ca clear|begin|+<riga>|status | pub <key> <payload>",
                 cmd_ha);
    register_cmd("heap", "Heap libero e largest block", NULL, cmd_heap);
    register_cmd("stats", "Statistiche render LED e heap minimi", NULL,
                 cmd_stats);
    register_cmd("ota", "Avvia l'aggiornamento firmware da URL HTTPS",
                 "<url https> [versione]", cmd_ota);
    register_cmd("ota_status", "Stato/contatori dell'OTA", NULL,
                 cmd_ota_status);
    ESP_ERROR_CHECK(sys_health_register_console());
}

/* --- handler eventi (task del loop di default) --- */

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

/* Fonte unica config_store: inoltra la distanza di trigger al modulo radar
 * a ogni cambio (come fa ha_entities). Log di ogni cambiamento. */
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

/* Esito provisioning -> overlay + avvio rete. net_mgr_start() senza
 * credenziali resta offline e riprova con backoff. */
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

/* Default dal Kconfig, usati da config_store_init senza blob (o blob non
 * interpretabile); lo store sanifica una copia. */
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
        .power_on = true, /* sovrascritto dal valore persistito */
        /* Fase 8: zona occhi (default = default della migrate case 2). */
        .eyes_enabled = CONFIG_PUMPKIN_EYES_ENABLED,
        .eyes_idle_r = CONFIG_PUMPKIN_EYES_IDLE_COLOR_R,
        .eyes_idle_g = CONFIG_PUMPKIN_EYES_IDLE_COLOR_G,
        .eyes_idle_b = CONFIG_PUMPKIN_EYES_IDLE_COLOR_B,
        .eyes_scare_r = CONFIG_PUMPKIN_EYES_SCARE_COLOR_R,
        .eyes_scare_g = CONFIG_PUMPKIN_EYES_SCARE_COLOR_G,
        .eyes_scare_b = CONFIG_PUMPKIN_EYES_SCARE_COLOR_B,
        .eyes_idle_brightness_pct = CONFIG_PUMPKIN_EYES_IDLE_BRIGHTNESS_PCT,
        .eyes_scare_brightness_pct = CONFIG_PUMPKIN_EYES_SCARE_BRIGHTNESS_PCT,
        .eyes_follow_fsm = CONFIG_PUMPKIN_EYES_FOLLOW_FSM,
    };
    strncpy(d.trigger_anim, CONFIG_PUMPKIN_TRIGGER_ANIM,
            sizeof(d.trigger_anim) - 1);
    d.trigger_anim[sizeof(d.trigger_anim) - 1] = '\0';
    strncpy(d.idle_anim, CONFIG_PUMPKIN_IDLE_ANIM, sizeof(d.idle_anim) - 1);
    d.idle_anim[sizeof(d.idle_anim) - 1] = '\0';
    strncpy(d.eyes_idle_effect, CONFIG_PUMPKIN_EYES_IDLE_EFFECT,
            sizeof(d.eyes_idle_effect) - 1);
    d.eyes_idle_effect[sizeof(d.eyes_idle_effect) - 1] = '\0';
    return d;
}

static void fatal(const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "%s failed (%s)", what, esp_err_to_name(err));
    abort();
}

void app_main(void)
{
    /* NVS prima di config_store (blob persistito) e del provisioning. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Catena offline (identica alle app di banco). */
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

    /* Fase 8: occhi — ponte eventi FSM/config -> zona occhi (si registra
     * dopo led_fx + config_store + FSM; prima di ha_entities cosi' il look
     * iniziale e' gia' applicato quando arrivano le entita'). */
    err = eyes_ctrl_init();
    if (err != ESP_OK) {
        fatal("eyes_ctrl_init", err);
    }

    /* Registro entita' HA + handler dominio + trasporto MQTT pronto
     * (avvio su NET_EVENT_UP). Registra i suoi handler prima dei nostri. */
    err = ha_entities_init();
    if (err != ESP_OK) {
        fatal("ha_entities_init", err);
    }

    /* Fase 7: OTA (gate + task dedicato) e salute. Nessun provider di
     * batteria registrato: pumpkin_ota tratta la batteria come sconosciuta
     * (-1), consentendo l'update con "battery":"unknown" nell'evento. */
    err = pumpkin_ota_init();
    if (err != ESP_OK) {
        fatal("pumpkin_ota_init", err);
    }

    err = sys_health_init();
    if (err != ESP_OK) {
        fatal("sys_health_init", err);
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

    /* Sync iniziale della distanza di trigger (store -> modulo radar). */
    pumpkin_config_t cfg;
    ESP_ERROR_CHECK(config_store_get(&cfg));
    err = radar_set_trigger_mm(cfg.trigger_mm);
    ESP_LOGI(TAG, "radar trigger synced to %u mm (%s)", cfg.trigger_mm,
             esp_err_to_name(err));

    /* Rete di proprieta' dell'app (contratto wifi_prov): netif STA creato
     * una volta sola qui, esp_wifi_init qui; net_mgr_start li adotta. */
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

    /* Baseline heap per i comandi heap/ota_status (dopo tutte le init). */
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
    repl_config.prompt = "pumpkin>";
    esp_console_dev_uart_config_t hw_config =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config,
                                              &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
