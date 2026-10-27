/* Costruzione payload JSON discovery e stati (pura, cJSON).
 * Tutte le stringhe topic/unique_id sono composte in buffer malloc esatti:
 * nessuna possibilita' di troncamento. */
#include "ha_payloads.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

/* Concatena a+b+c in un buffer malloc esatto; NULL su out-of-memory. */
static char *join3(const char *a, const char *b, const char *c)
{
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char *out = malloc(la + lb + lc + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    memcpy(out + la + lb, c, lc);
    out[la + lb + lc] = '\0';
    return out;
}

/* Copia malloc di un buffer gia' formattato (strlen calcolato una volta). */
static char *dup_str(const char *s)
{
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, s, n + 1);
    return out;
}

static cJSON *add_string_array(cJSON *comp, const char *name,
                               const char *const *options, size_t options_len)
{
    cJSON *arr = cJSON_AddArrayToObject(comp, name);
    if (arr == NULL)
        return NULL;
    for (size_t i = 0; i < options_len; i++) {
        if (options[i] == NULL)
            continue;
        cJSON *item = cJSON_CreateString(options[i]);
        if (item == NULL || cJSON_AddItemToArray(arr, item) != 1)
            return NULL;
    }
    return arr;
}

char *ha_payload_discovery(const ha_device_info_t *dev, const char *base_topic,
                           const char *availability_topic)
{
    if (dev == NULL || dev->device_id == NULL || dev->name == NULL ||
        dev->manufacturer == NULL || dev->model == NULL || dev->sw_version == NULL ||
        dev->origin_name == NULL || dev->origin_sw == NULL ||
        base_topic == NULL || availability_topic == NULL)
        return NULL;

    char *uid_prefix = join3("pumpkin_", dev->device_id, "");
    if (uid_prefix == NULL)
        return NULL;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        free(uid_prefix);
        return NULL;
    }

    /* device */
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON *identifiers = device ? cJSON_AddArrayToObject(device, "identifiers") : NULL;
    cJSON *ident_item = identifiers ? cJSON_CreateString(uid_prefix) : NULL;
    if (device == NULL || identifiers == NULL || ident_item == NULL ||
        cJSON_AddItemToArray(identifiers, ident_item) != 1 ||
        cJSON_AddStringToObject(device, "name", dev->name) == NULL ||
        cJSON_AddStringToObject(device, "manufacturer", dev->manufacturer) == NULL ||
        cJSON_AddStringToObject(device, "model", dev->model) == NULL ||
        cJSON_AddStringToObject(device, "sw_version", dev->sw_version) == NULL)
        goto fail;

    /* origin */
    cJSON *origin = cJSON_AddObjectToObject(root, "origin");
    if (origin == NULL ||
        cJSON_AddStringToObject(origin, "name", dev->origin_name) == NULL ||
        cJSON_AddStringToObject(origin, "sw", dev->origin_sw) == NULL)
        goto fail;

    /* availability */
    cJSON *availability = cJSON_AddArrayToObject(root, "availability");
    cJSON *av0 = availability ? cJSON_CreateObject() : NULL;
    if (availability == NULL || av0 == NULL ||
        cJSON_AddItemToArray(availability, av0) != 1 ||
        cJSON_AddStringToObject(av0, "topic", availability_topic) == NULL)
        goto fail;

    /* components */
    cJSON *components = cJSON_AddObjectToObject(root, "components");
    if (components == NULL)
        goto fail;

    for (size_t i = 0; i < ha_registry_count(); i++) {
        const ha_entity_desc_t *d = ha_registry_at(i);
        cJSON *comp = cJSON_AddObjectToObject(components, d->key);
        /* update: topic dedicati fuori dal pattern state/<key> (contratto:
         * state_topic update/state + latest_version_topic update/latest). */
        char *state_topic = (d->type == HA_ET_UPDATE)
                                ? join3(base_topic, "/update/state", "")
                                : join3(base_topic, "/state/", d->key);
        char *latest_topic = (d->type == HA_ET_UPDATE)
                                 ? join3(base_topic, "/update/latest", "")
                                 : NULL;
        char *cmd_topic = d->has_command ? join3(base_topic, "/set/", d->key) : NULL;
        char *uid_mid = join3("pumpkin_", dev->device_id, "_");
        char *uid = uid_mid ? join3(uid_mid, d->key, "") : NULL;

        if (comp == NULL || state_topic == NULL ||
            (d->type == HA_ET_UPDATE && latest_topic == NULL) ||
            (d->has_command && cmd_topic == NULL) || uid == NULL)
            goto fail_loop;

        cJSON_AddStringToObject(comp, "unique_id", uid);
        /* name: senza questo campo HA nomina le entita' col default generico
         * ("MQTT JSON light", "MQTT switch", ...). Il nome umano del
         * registro e' la fonte (HA lo prefissa col nome del device). */
        cJSON_AddStringToObject(comp, "name", d->name);
        /* state_topic omesso per i button; command_topic solo se ha comandi. */
        if (d->type != HA_ET_BUTTON)
            cJSON_AddStringToObject(comp, "state_topic", state_topic);
        if (d->has_command)
            cJSON_AddStringToObject(comp, "command_topic", cmd_topic);
        if (latest_topic != NULL)
            cJSON_AddStringToObject(comp, "latest_version_topic", latest_topic);

        switch (d->type) {
        case HA_ET_LIGHT:
            cJSON_AddStringToObject(comp, "platform", "light");
            cJSON_AddStringToObject(comp, "schema", "json");
            cJSON_AddBoolToObject(comp, "brightness", true);
            cJSON_AddBoolToObject(comp, "rgb", true);
            cJSON_AddBoolToObject(comp, "effect", true);
            if (d->options != NULL && add_string_array(comp, "effect_list",
                                                       d->options, d->options_len) == NULL)
                goto fail_loop;
            break;
        case HA_ET_LIGHT_RGB:
            /* Solo colore: il colore e' un parametro di config (idle/scare),
             * non un look: niente brightness, effect o effect_list. */
            cJSON_AddStringToObject(comp, "platform", "light");
            cJSON_AddStringToObject(comp, "schema", "json");
            cJSON_AddBoolToObject(comp, "rgb", true);
            break;
        case HA_ET_BINARY_SENSOR:
            cJSON_AddStringToObject(comp, "platform", "binary_sensor");
            cJSON_AddStringToObject(comp, "device_class",
                                    d->device_class != NULL ? d->device_class : "occupancy");
            break;
        case HA_ET_SENSOR:
            cJSON_AddStringToObject(comp, "platform", "sensor");
            if (d->device_class != NULL)
                cJSON_AddStringToObject(comp, "device_class", d->device_class);
            if (d->unit != NULL)
                cJSON_AddStringToObject(comp, "unit_of_measurement", d->unit);
            cJSON_AddStringToObject(comp, "state_class", "measurement");
            break;
        case HA_ET_SWITCH:
            cJSON_AddStringToObject(comp, "platform", "switch");
            break;
        case HA_ET_NUMBER:
            cJSON_AddStringToObject(comp, "platform", "number");
            cJSON_AddNumberToObject(comp, "min", (double)d->num_min);
            cJSON_AddNumberToObject(comp, "max", (double)d->num_max);
            cJSON_AddNumberToObject(comp, "step", (double)d->num_step);
            cJSON_AddStringToObject(comp, "mode", "box");
            if (d->unit != NULL)
                cJSON_AddStringToObject(comp, "unit_of_measurement", d->unit);
            break;
        case HA_ET_SELECT:
            cJSON_AddStringToObject(comp, "platform", "select");
            if (d->options != NULL && add_string_array(comp, "options",
                                                       d->options, d->options_len) == NULL)
                goto fail_loop;
            break;
        case HA_ET_BUTTON:
            cJSON_AddStringToObject(comp, "platform", "button");
            cJSON_AddStringToObject(comp, "payload_press", "PRESS");
            break;
        case HA_ET_UPDATE:
            cJSON_AddStringToObject(comp, "platform", "update");
            cJSON_AddStringToObject(comp, "title", "Firmware zucca");
            break;
        default:
            goto fail_loop;
        }

        if (d->category == HA_CAT_CONFIG)
            cJSON_AddStringToObject(comp, "entity_category", "config");
        else if (d->category == HA_CAT_DIAGNOSTIC)
            cJSON_AddStringToObject(comp, "entity_category", "diagnostic");

        free(state_topic);
        free(cmd_topic);
        free(latest_topic);
        free(uid_mid);
        free(uid);
        continue;

fail_loop:
        free(state_topic);
        free(cmd_topic);
        free(latest_topic);
        free(uid_mid);
        free(uid);
        goto fail;
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(uid_prefix);
    return out;

fail:
    cJSON_Delete(root);
    free(uid_prefix);
    return NULL;
}

char *ha_payload_light(bool on, bool has_brightness, int brightness,
                       bool has_color, uint8_t r, uint8_t g, uint8_t b,
                       bool has_effect, const char *effect)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return NULL;

    cJSON_AddStringToObject(root, "state", on ? "ON" : "OFF");
    if (has_brightness)
        cJSON_AddNumberToObject(root, "brightness", brightness);
    if (has_color) {
        cJSON *color = cJSON_AddObjectToObject(root, "color");
        if (color == NULL)
            goto fail;
        cJSON_AddNumberToObject(color, "r", r);
        cJSON_AddNumberToObject(color, "g", g);
        cJSON_AddNumberToObject(color, "b", b);
    }
    if (has_effect)
        cJSON_AddStringToObject(root, "effect", effect != NULL ? effect : "");

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;

fail:
    cJSON_Delete(root);
    return NULL;
}

char *ha_payload_event_trigger(int32_t version, int64_t ts_ms, int32_t distance_mm,
                               const char *anim)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return NULL;

    cJSON_AddNumberToObject(root, "v", (double)version);
    cJSON *ts = cJSON_AddNumberToObject(root, "ts", 0.0);
    /* cJSON IDF non ha SetInt64: SetNumberValue scrive valuedouble (esatto
     * fino a 2^53 ms, oltre l'uptime possibile) e valueint. */
    if (ts != NULL)
        cJSON_SetNumberValue(ts, (double)ts_ms);
    cJSON_AddNumberToObject(root, "distance_mm", (double)distance_mm);
    cJSON_AddStringToObject(root, "anim", anim != NULL ? anim : "");

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

const char *ha_payload_onoff(bool on)
{
    return on ? "ON" : "OFF";
}

char *ha_payload_i32(int32_t v)
{
    char buf[16];
    int n = snprintf(buf, sizeof buf, "%d", (int)v);
    return n > 0 ? dup_str(buf) : NULL;
}

char *ha_payload_u32(uint32_t v)
{
    char buf[16];
    int n = snprintf(buf, sizeof buf, "%u", (unsigned)v);
    return n > 0 ? dup_str(buf) : NULL;
}

char *ha_payload_f32(float v, int decimals)
{
    if (decimals < 0)
        decimals = 0;
    if (decimals > 7)
        decimals = 7;
    /* Locale C: separatore decimale sempre '.'. */
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%.*f", decimals, (double)v);
    return n > 0 ? dup_str(buf) : NULL;
}

char *ha_payload_text(const char *v)
{
    if (v == NULL)
        return NULL;
    return dup_str(v);
}
