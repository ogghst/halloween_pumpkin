/* Public API of the config_store component (pumpkin firmware, Fase 3).
 *
 * Single source of truth for the user-visible pumpkin parameters. Values
 * live in RAM (mutex-protected), are served by copy, and are written to
 * NVS (namespace "pumpkin") with a debounce timer to limit flash wear.
 * Every change posts CONFIG_EVENT_CHANGED on the default esp_event loop
 * carrying the id of the parameter that changed.
 */
#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(CONFIG_EVENT);

#define CONFIG_STORE_NAMESPACE "pumpkin"

/* Event ids posted on CONFIG_EVENT. */
typedef enum {
    CONFIG_EVENT_CHANGED = 0, /* event_data: config_change_t */
} config_event_t;

/* Ids of the configurable parameters. */
typedef enum {
    CFG_PARAM_TRIGGER_MM = 0,        /* u16, mm */
    CFG_PARAM_COOLDOWN_S,            /* u16, s */
    CFG_PARAM_TRIGGER_DURATION_S,    /* u16, s */
    CFG_PARAM_TRIGGER_ANIM,          /* anim name */
    CFG_PARAM_IDLE_ANIM,             /* anim name */
    CFG_PARAM_IDLE_BRIGHTNESS_PCT,   /* pct 0-100 */
    CFG_PARAM_MAX_BRIGHTNESS_PCT,    /* pct 5-100 */
    CFG_PARAM_AUTO_MODE,             /* bool */
    CFG_PARAM_COLOR,                 /* r/g/b */
    CFG_PARAM_BRIGHTNESS_PCT,        /* pct 0-100, TRIGGERED look */
    CFG_PARAM_POWER_ON,              /* bool, boot policy: false -> OFF */
    CFG_PARAM_MQTT,                  /* config_mqtt_t, v2 */
    /* v3: zona occhi, appesa in coda, mai prima dei campi v1/v2 */
    CFG_PARAM_EYES_ENABLED,          /* bool */
    CFG_PARAM_EYES_IDLE_COLOR,       /* r/g/b, look idle */
    CFG_PARAM_EYES_SCARE_COLOR,      /* r/g/b, look scare */
    CFG_PARAM_EYES_IDLE_EFFECT,      /* anim name, look idle */
    CFG_PARAM_EYES_IDLE_BRIGHTNESS_PCT,  /* pct 0-100 */
    CFG_PARAM_EYES_SCARE_BRIGHTNESS_PCT, /* pct 0-100 */
    CFG_PARAM_EYES_FOLLOW_FSM,       /* bool: occhi seguono idle/scare FSM */
} config_param_id_t;

/* Payload of CONFIG_EVENT_CHANGED. */
typedef struct {
    config_param_id_t id; /* the single parameter that changed */
} config_change_t;

/* Effect names are stored as fixed strings matching the led_effects
 * registry names ("solid", "candle", "breathe", "rainbow", "flicker",
 * "scare"). Name validity is enforced by the consumer when applied. */
#define CONFIG_ANIM_NAME_LEN 16

/* MQTT broker settings (v2). Lengths include the NUL. No credentials are
 * ever hardcoded: empty strings mean "not provisioned yet". */
#define CONFIG_MQTT_HOST_LEN 64
#define CONFIG_MQTT_USER_LEN 32
#define CONFIG_MQTT_PASS_LEN 64

typedef struct {
    char host[CONFIG_MQTT_HOST_LEN];
    uint16_t port; /* 1..65535 */
    char user[CONFIG_MQTT_USER_LEN];
    char pass[CONFIG_MQTT_PASS_LEN];
    bool tls;
} config_mqtt_t;

/* Persisted configuration, versioned. Never reorder or remove fields;
 * append new ones at the end and bump PUMPKIN_CONFIG_VERSION with a
 * migration step in config_store_core.c.
 *
 * Serialized blob (little-endian, no padding; the version travels in the
 * NVS key "ver", not in the blob). v3 appends the eyes block after the
 * byte-identical v2 head:
 *   [0..45]     v1 fields (see config_store_core.c for the v1 layout)
 *   [46..109]   mqtt.host[64]
 *   [110..111]  mqtt.port, u16 LE
 *   [112..143]  mqtt.user[32]
 *   [144..207]  mqtt.pass[64]
 *   [208]       mqtt.tls, u8 0/1
 *   [209]       eyes_enabled, u8 0/1
 *   [210..212]  eyes_idle r/g/b
 *   [213..215]  eyes_scare r/g/b
 *   [216..231]  eyes_idle_effect[16]
 *   [232]       eyes_idle_brightness_pct
 *   [233]       eyes_scare_brightness_pct
 *   [234]       eyes_follow_fsm, u8 0/1
 * Total: 235 bytes. config_deserialize still accepts a 209-byte v2 blob
 * (the eyes tail then reads as zeros until config_migrate fills the
 * v2 -> v3 defaults) or the legacy 46-byte v1 blob (mqtt zeroed too). */
#define PUMPKIN_CONFIG_VERSION 3

typedef struct {
    uint8_t version;
    uint16_t trigger_mm;
    uint16_t cooldown_s;
    uint16_t trigger_duration_s;
    char trigger_anim[CONFIG_ANIM_NAME_LEN];
    char idle_anim[CONFIG_ANIM_NAME_LEN];
    uint8_t idle_brightness_pct;
    uint8_t max_brightness_pct;
    bool auto_mode;
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
    uint8_t brightness_pct;
    bool power_on; /* false: user left the pumpkin OFF; boot policy reads this */
    config_mqtt_t mqtt; /* v2: appended in tail, never before the v1 fields */
    /* v3: zona occhi, appesa in coda, mai prima dei campi v1/v2 */
    bool eyes_enabled;
    uint8_t eyes_idle_r, eyes_idle_g, eyes_idle_b;
    uint8_t eyes_scare_r, eyes_scare_g, eyes_scare_b;
    char eyes_idle_effect[CONFIG_ANIM_NAME_LEN];
    uint8_t eyes_idle_brightness_pct;
    uint8_t eyes_scare_brightness_pct;
    bool eyes_follow_fsm;
} pumpkin_config_t;

/* --- Pure core (host-tested; see components/config_store/host_tests) --- */

/* Sanitize a configuration in place: clamp numeric ranges, terminate
 * strings, replace empty/too-long anim names. Returns ESP_OK when nothing
 * was corrected, ESP_ERR_INVALID_STATE when a fix was applied. */
esp_err_t config_sanitize(pumpkin_config_t *cfg);

/* Migrate an older-version configuration in place up to
 * PUMPKIN_CONFIG_VERSION. ESP_ERR_NOT_SUPPORTED for versions newer than
 * this firmware knows. Unset fields introduced by a step get safe values. */
esp_err_t config_migrate(pumpkin_config_t *cfg);

/* Dimensione in byte del blob serializzato v3 (chiave NVS "cfg"): il
 * blob v2 da 209 byte piu' la coda occhi (26 byte), 235 in totale. */
size_t config_serialized_size(void);

/* Serialize/deserialize the fixed-layout representation (version field
 * excluded; the NVS key "ver" stores it separately). Little-endian, no
 * padding: stable across struct layout changes. config_deserialize
 * accepts the v3 length (235), the v2 length (209: the eyes tail is
 * zeroed and config_migrate supplies the defaults) or the legacy v1
 * length (46: mqtt zeroed as well). */
void config_serialize(const pumpkin_config_t *cfg, void *buf, size_t len);
esp_err_t config_deserialize(pumpkin_config_t *cfg, const void *buf, size_t len);

/* --- IDF layer --- */

/* Load (or seed) the configuration and start the debounce writer.
 * `defaults` provides the Kconfig-backed values used when nothing (or an
 * unparsable blob) is stored; it is sanitized internally, not mutated. */
esp_err_t config_store_init(const pumpkin_config_t *defaults);

/* Thread-safe copy of the current configuration. */
esp_err_t config_store_get(pumpkin_config_t *out);

/* Typed setters: validate, update the RAM copy, post CONFIG_EVENT_CHANGED
 * (default loop) with the parameter id, then schedule the debounced NVS
 * commit (CONFIG_STORE_DEBOUNCE_MS). ESP_ERR_INVALID_ARG on bad id/value. */
esp_err_t config_store_set_u16(config_param_id_t id, uint16_t value);
esp_err_t config_store_set_pct(config_param_id_t id, uint8_t pct);
esp_err_t config_store_set_bool(config_param_id_t id, bool value);
/* Colore per id: valido solo per CFG_PARAM_COLOR, CFG_PARAM_EYES_IDLE_COLOR
 * e CFG_PARAM_EYES_SCARE_COLOR (altrimenti ESP_ERR_INVALID_ARG). */
esp_err_t config_store_set_color_param(config_param_id_t id, uint8_t r,
                                       uint8_t g, uint8_t b);
/* Wrapper di set_color_param(CFG_PARAM_COLOR, ...): colore del look di
 * trigger del corpo. */
esp_err_t config_store_set_color(uint8_t r, uint8_t g, uint8_t b);
esp_err_t config_store_set_anim(config_param_id_t id, const char *name);

/* MQTT block (v2): same semantics as the typed setters. get_mqtt serves
 * the RAM copy; set_mqtt validates (strings NUL-terminated within their
 * buffer, port 1..65535), updates the RAM copy, posts
 * CONFIG_EVENT_CHANGED {CFG_PARAM_MQTT} and schedules the debounced NVS
 * commit. Identical input (memcmp) is a no-op. */
esp_err_t config_store_get_mqtt(config_mqtt_t *out);
esp_err_t config_store_set_mqtt(const config_mqtt_t *in);

/* Commit pending changes now; ESP_OK also when nothing is dirty. */
esp_err_t config_store_flush(void);

#ifdef __cplusplus
}
#endif
#endif /* CONFIG_STORE_H */
