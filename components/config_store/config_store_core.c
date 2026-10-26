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
 */
#include <string.h>

#include "esp_err.h"

#include "config_store.h"

size_t config_serialized_size(void)
{
    return 3u * 2u + 2u * (size_t)CONFIG_ANIM_NAME_LEN + 8u;
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

/* Trim an anim name in place: keep the printable prefix, cut at the 15
 * usable characters (buffer is 16 with the NUL). An empty name stays
 * empty: sanitize has no defaults to substitute, that is the IDF layer's
 * job (config_store_init). Returns true when the name was corrected. */
static bool sanitize_anim(char *name)
{
    size_t len = 0;
    while (len < CONFIG_ANIM_NAME_LEN && name[len] != '\0') {
        unsigned char c = (unsigned char)name[len];
        if (c < 0x20u || c > 0x7eu)
            break; /* first non-printable ends the usable name */
        len++;
    }
    if (len >= CONFIG_ANIM_NAME_LEN) {
        name[CONFIG_ANIM_NAME_LEN - 1] = '\0'; /* was unterminated */
        return true;
    }
    if (name[len] != '\0') {
        name[len] = '\0'; /* cut at the first non-printable */
        return true;
    }
    return false;
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
}

esp_err_t config_deserialize(pumpkin_config_t *cfg, const void *buf, size_t len)
{
    if (cfg == NULL || buf == NULL)
        return ESP_ERR_INVALID_ARG;
    /* All or nothing: a blob of the wrong size is treated as corrupt by
     * the caller; cfg is left untouched here. */
    if (len != config_serialized_size())
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
    /* version is deliberately untouched: the "ver" key owns it */
    return ESP_OK;
}
