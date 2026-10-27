/* Pure core of the config_store component: field sanitizing, version
 * migration and the fixed-layout v1 serialization. No IDF dependency
 * beyond esp_err.h; host-tested in host_tests/.
 *
 * v1 blob layout (little-endian, no padding; the "version" field is NOT
 * part of the blob, the NVS key "ver" carries it separately):
 *   u16 trigger_mm, u16 cooldown_s, u16 trigger_duration_s,
 *   char[16] trigger_anim, char[16] idle_anim,
 *   u8 idle_brightness_pct, u8 max_brightness_pct, u8 auto_mode,
 *   u8 color_r, u8 color_g, u8 color_b, u8 brightness_pct, u8 power_on
 * Total 46 bytes.
 *
 * v2 blob layout: the 46 v1 bytes above stay IDENTICAL in the head, the
 * mqtt block is appended in tail (163 bytes, 209 total):
 *   [46..109]  char[64] mqtt.host
 *   [110..111] u16 LE    mqtt.port
 *   [112..143] char[32]  mqtt.user
 *   [144..207] char[64]  mqtt.pass
 *   [208]      u8 0/1    mqtt.tls
 * config_deserialize accepts both lengths: a legacy v1 blob deserializes
 * with a zeroed mqtt block, then config_migrate (1 -> 2) fills the safe
 * defaults (host/user/pass "", port 1883, tls false).
 */
#include <string.h>

#include "esp_err.h"

#include "config_store.h"

/* Fixed length of the legacy v1 blob, in bytes. */
#define V1_BLOB_SIZE (3u * 2u + 2u * (size_t)CONFIG_ANIM_NAME_LEN + 8u)

/* v2: head v1 + blocco mqtt (host[64] + port u16 + user[32] + pass[64] +
 * tls u8), 209 byte. */
#define V2_BLOB_SIZE (V1_BLOB_SIZE + (size_t)CONFIG_MQTT_HOST_LEN + 2u +                      \
                      (size_t)CONFIG_MQTT_USER_LEN + (size_t)CONFIG_MQTT_PASS_LEN + 1u)

/* v3: blob v2 + coda occhi (enabled u8 + 2x rgb + effect[16] + 2 pct +
 * follow u8), 26 byte; totale 235. */
#define V3_BLOB_SIZE (V2_BLOB_SIZE + 1u + 6u + (size_t)CONFIG_ANIM_NAME_LEN + 3u)

size_t config_serialized_size(void)
{
    return V3_BLOB_SIZE; /* 209 (v2) + 26 (coda occhi) = 235 */
}

/* --- sanitize --- */

static bool clamp_u16(uint16_t *v, uint16_t lo, uint16_t hi)
{
    if (*v >= lo && *v <= hi)
        return false;
    *v = (*v < lo) ? lo : hi;
    return true;
}

static bool clamp_u8(uint8_t *v, uint8_t lo, uint8_t hi)
{
    if (*v >= lo && *v <= hi)
        return false;
    *v = (*v < lo) ? lo : hi;
    return true;
}

/* Trim a fixed-capacity string in place: keep the printable prefix, cut
 * at the cap-1 usable characters (the buffer holds the NUL). An empty
 * string stays empty: sanitize has no defaults to substitute, that is
 * the IDF layer's job (config_store_init). Returns true when the string
 * was corrected. */
static bool sanitize_str(char *s, size_t cap)
{
    size_t len = 0;
    while (len < cap && s[len] != '\0') {
        unsigned char c = (unsigned char)s[len];
        if (c < 0x20u || c > 0x7eu)
            break; /* first non-printable ends the usable string */
        len++;
    }
    if (len >= cap) {
        s[cap - 1] = '\0'; /* was unterminated */
        return true;
    }
    if (s[len] != '\0') {
        s[len] = '\0'; /* cut at the first non-printable */
        return true;
    }
    return false;
}

static bool sanitize_anim(char *name)
{
    return sanitize_str(name, CONFIG_ANIM_NAME_LEN);
}

esp_err_t config_sanitize(pumpkin_config_t *cfg)
{
    if (cfg == NULL)
        return ESP_ERR_INVALID_ARG;

    bool fixed = false;
    fixed |= clamp_u16(&cfg->trigger_mm, 100, 10000);
    fixed |= clamp_u16(&cfg->cooldown_s, 0, 3600);
    fixed |= clamp_u16(&cfg->trigger_duration_s, 1, 3600);
    fixed |= sanitize_anim(cfg->trigger_anim);
    fixed |= sanitize_anim(cfg->idle_anim);
    fixed |= clamp_u8(&cfg->idle_brightness_pct, 0, 100);
    fixed |= clamp_u8(&cfg->max_brightness_pct, 5, 100);
    fixed |= clamp_u8(&cfg->brightness_pct, 0, 100);
    /* mqtt block (v2): strings like the anims; port range is 1..65535
     * and an u16 already caps the top, so only 0 is out of range and
     * maps to the default; tls normalizes to a clean 0/1. */
    fixed |= sanitize_str(cfg->mqtt.host, CONFIG_MQTT_HOST_LEN);
    fixed |= sanitize_str(cfg->mqtt.user, CONFIG_MQTT_USER_LEN);
    fixed |= sanitize_str(cfg->mqtt.pass, CONFIG_MQTT_PASS_LEN);
    if (cfg->mqtt.port == 0) {
        cfg->mqtt.port = 1883;
        fixed = true;
    }
    uint8_t tls = cfg->mqtt.tls ? 1u : 0u;
    if ((uint8_t)cfg->mqtt.tls != tls) {
        cfg->mqtt.tls = tls != 0;
        fixed = true;
    }
    /* zona occhi (v3): pct clampate 0..100, nome effetto ripulito come
     * le anim, bool normalizzati 0/1 come tls. */
    fixed |= clamp_u8(&cfg->eyes_idle_brightness_pct, 0, 100);
    fixed |= clamp_u8(&cfg->eyes_scare_brightness_pct, 0, 100);
    fixed |= sanitize_anim(cfg->eyes_idle_effect);
    uint8_t eyes_enabled = cfg->eyes_enabled ? 1u : 0u;
    if ((uint8_t)cfg->eyes_enabled != eyes_enabled) {
        cfg->eyes_enabled = eyes_enabled != 0;
        fixed = true;
    }
    uint8_t eyes_follow = cfg->eyes_follow_fsm ? 1u : 0u;
    if ((uint8_t)cfg->eyes_follow_fsm != eyes_follow) {
        cfg->eyes_follow_fsm = eyes_follow != 0;
        fixed = true;
    }
    return fixed ? ESP_ERR_INVALID_STATE : ESP_OK;
}

/* --- migrate --- */

esp_err_t config_migrate(pumpkin_config_t *cfg)
{
    if (cfg == NULL)
        return ESP_ERR_INVALID_ARG;
    if (cfg->version > PUMPKIN_CONFIG_VERSION)
        return ESP_ERR_NOT_SUPPORTED;

    while (cfg->version < PUMPKIN_CONFIG_VERSION) {
        switch (cfg->version) {
        case 1:
            /* v1 -> v2: the mqtt block did not exist. Zero the whole
             * block (deterministic strings) then fill the fixed safe
             * defaults; provisioning sets real values later. */
            memset(&cfg->mqtt, 0, sizeof(cfg->mqtt));
            cfg->mqtt.port = 1883;
            break;
        case 2:
            /* v2 -> v3: la zona occhi non esisteva. Default hardcoded
             * UGUALI ai simboli Kconfig PUMPKIN_EYES_* (enabled=y,
             * idle_color 255/120/0, scare_color 255/0/0,
             * idle_effect="breathe", idle_brightness_pct=40,
             * scare_brightness_pct=100, follow_fsm=y): se cambi uno,
             * cambia anche l'altro. */
            memset(cfg->eyes_idle_effect, 0, sizeof(cfg->eyes_idle_effect));
            memcpy(cfg->eyes_idle_effect, "breathe", sizeof("breathe"));
            cfg->eyes_enabled = true;
            cfg->eyes_idle_r = 255;
            cfg->eyes_idle_g = 120;
            cfg->eyes_idle_b = 0;
            cfg->eyes_scare_r = 255;
            cfg->eyes_scare_g = 0;
            cfg->eyes_scare_b = 0;
            cfg->eyes_idle_brightness_pct = 40;
            cfg->eyes_scare_brightness_pct = 100;
            cfg->eyes_follow_fsm = true;
            break;
        case 0:
            /* Blob pre-versioning (or absent "ver" key): the v1 fields
             * were already present, nothing to transform. Chain future
             * steps as `case <stored version>:` ABOVE this one, each
             * filling the fields its target version introduced. */
            break;
        default:
            /* Intermediate version this build does not know. */
            return ESP_ERR_NOT_SUPPORTED;
        }
        cfg->version++;
    }
    return ESP_OK;
}

/* --- serialize / deserialize --- */

static void put_u16(uint8_t **p, uint16_t v)
{
    (*p)[0] = (uint8_t)(v & 0xffu);
    (*p)[1] = (uint8_t)(v >> 8);
    *p += 2;
}

static uint16_t get_u16(const uint8_t **p)
{
    uint16_t v = (uint16_t)((uint16_t)(*p)[0] | ((uint16_t)(*p)[1] << 8));
    *p += 2;
    return v;
}

void config_serialize(const pumpkin_config_t *cfg, void *buf, size_t len)
{
    if (cfg == NULL || buf == NULL || len < config_serialized_size())
        return;

    uint8_t *p = buf;
    put_u16(&p, cfg->trigger_mm);
    put_u16(&p, cfg->cooldown_s);
    put_u16(&p, cfg->trigger_duration_s);
    memcpy(p, cfg->trigger_anim, CONFIG_ANIM_NAME_LEN);
    p += CONFIG_ANIM_NAME_LEN;
    memcpy(p, cfg->idle_anim, CONFIG_ANIM_NAME_LEN);
    p += CONFIG_ANIM_NAME_LEN;
    *p++ = cfg->idle_brightness_pct;
    *p++ = cfg->max_brightness_pct;
    *p++ = cfg->auto_mode ? 1u : 0u;
    *p++ = cfg->color_r;
    *p++ = cfg->color_g;
    *p++ = cfg->color_b;
    *p++ = cfg->brightness_pct;
    *p++ = cfg->power_on ? 1u : 0u;
    /* v2 tail: mqtt block, appended after the byte-identical v1 head */
    memcpy(p, cfg->mqtt.host, CONFIG_MQTT_HOST_LEN);
    p += CONFIG_MQTT_HOST_LEN;
    put_u16(&p, cfg->mqtt.port);
    memcpy(p, cfg->mqtt.user, CONFIG_MQTT_USER_LEN);
    p += CONFIG_MQTT_USER_LEN;
    memcpy(p, cfg->mqtt.pass, CONFIG_MQTT_PASS_LEN);
    p += CONFIG_MQTT_PASS_LEN;
    *p++ = cfg->mqtt.tls ? 1u : 0u;
    /* v3 tail: zona occhi, appesa dopo il blocco v2 byte-identico */
    *p++ = cfg->eyes_enabled ? 1u : 0u;
    *p++ = cfg->eyes_idle_r;
    *p++ = cfg->eyes_idle_g;
    *p++ = cfg->eyes_idle_b;
    *p++ = cfg->eyes_scare_r;
    *p++ = cfg->eyes_scare_g;
    *p++ = cfg->eyes_scare_b;
    memcpy(p, cfg->eyes_idle_effect, CONFIG_ANIM_NAME_LEN);
    p += CONFIG_ANIM_NAME_LEN;
    *p++ = cfg->eyes_idle_brightness_pct;
    *p++ = cfg->eyes_scare_brightness_pct;
    *p++ = cfg->eyes_follow_fsm ? 1u : 0u;
}

esp_err_t config_deserialize(pumpkin_config_t *cfg, const void *buf, size_t len)
{
    if (cfg == NULL || buf == NULL)
        return ESP_ERR_INVALID_ARG;
    /* All or nothing: a blob of the wrong size is treated as corrupt by
     * the caller; cfg is left untouched here. Valid lengths: v3 (235),
     * v2 (209, eyes tail zeroed for migration) and the legacy v1 (46). */
    if (len != config_serialized_size() && len != V2_BLOB_SIZE &&
        len != V1_BLOB_SIZE)
        return ESP_ERR_INVALID_SIZE;

    const uint8_t *p = buf;
    cfg->trigger_mm = get_u16(&p);
    cfg->cooldown_s = get_u16(&p);
    cfg->trigger_duration_s = get_u16(&p);
    memcpy(cfg->trigger_anim, p, CONFIG_ANIM_NAME_LEN);
    p += CONFIG_ANIM_NAME_LEN;
    memcpy(cfg->idle_anim, p, CONFIG_ANIM_NAME_LEN);
    p += CONFIG_ANIM_NAME_LEN;
    cfg->idle_brightness_pct = *p++;
    cfg->max_brightness_pct = *p++;
    cfg->auto_mode = *p++ != 0;
    cfg->color_r = *p++;
    cfg->color_g = *p++;
    cfg->color_b = *p++;
    cfg->brightness_pct = *p++;
    cfg->power_on = *p++ != 0;

    if (len == V1_BLOB_SIZE) {
        /* Legacy blob: no mqtt block on flash. Zero it so the struct is
         * fully defined regardless of the caller's initial state; the
         * following config_migrate fills the v1 -> v2 defaults. */
        memset(&cfg->mqtt, 0, sizeof(cfg->mqtt));
        return ESP_OK;
    }

    memcpy(cfg->mqtt.host, p, CONFIG_MQTT_HOST_LEN);
    p += CONFIG_MQTT_HOST_LEN;
    cfg->mqtt.port = get_u16(&p);
    memcpy(cfg->mqtt.user, p, CONFIG_MQTT_USER_LEN);
    p += CONFIG_MQTT_USER_LEN;
    memcpy(cfg->mqtt.pass, p, CONFIG_MQTT_PASS_LEN);
    p += CONFIG_MQTT_PASS_LEN;
    cfg->mqtt.tls = *p++ != 0;

    if (len == V2_BLOB_SIZE) {
        /* Blob v2: nessuna coda occhi su flash. Azzero la coda per
         * avere una struct interamente definita a prescindere dallo
         * stato del chiamante; la config_migrate seguente (v2 -> v3)
         * la riempie con i default. */
        memset(cfg->eyes_idle_effect, 0, CONFIG_ANIM_NAME_LEN);
        cfg->eyes_enabled = false;
        cfg->eyes_idle_r = 0;
        cfg->eyes_idle_g = 0;
        cfg->eyes_idle_b = 0;
        cfg->eyes_scare_r = 0;
        cfg->eyes_scare_g = 0;
        cfg->eyes_scare_b = 0;
        cfg->eyes_idle_brightness_pct = 0;
        cfg->eyes_scare_brightness_pct = 0;
        cfg->eyes_follow_fsm = false;
        return ESP_OK;
    }

    /* v3 tail: zona occhi, agli offset documentati nell'header */
    cfg->eyes_enabled = *p++ != 0;
    cfg->eyes_idle_r = *p++;
    cfg->eyes_idle_g = *p++;
    cfg->eyes_idle_b = *p++;
    cfg->eyes_scare_r = *p++;
    cfg->eyes_scare_g = *p++;
    cfg->eyes_scare_b = *p++;
    memcpy(cfg->eyes_idle_effect, p, CONFIG_ANIM_NAME_LEN);
    p += CONFIG_ANIM_NAME_LEN;
    cfg->eyes_idle_brightness_pct = *p++;
    cfg->eyes_scare_brightness_pct = *p++;
    cfg->eyes_follow_fsm = *p++ != 0;
    /* version is deliberately untouched: the "ver" key owns it */
    return ESP_OK;
}
