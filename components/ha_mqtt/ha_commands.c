/* Parsing/validazione comandi in ingresso (pura).
 * I payload non sono NUL-terminati: i campi scalari vengono copiati in un
 * buffer temporaneo; il JSON light e' parsato con cJSON_ParseWithLength. */
#include "ha_commands.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static void set_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static bool in_options(const char *const *options, size_t options_len, const char *v)
{
    if (options == NULL || v == NULL)
        return false;
    for (size_t i = 0; i < options_len; i++) {
        if (options[i] != NULL && strcmp(options[i], v) == 0)
            return true;
    }
    return false;
}

/* Copia truncata (sicura) di src in dst[HA_ANIM_NAME_MAX]. */
static void copy_anim(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > HA_ANIM_NAME_MAX - 1)
        n = HA_ANIM_NAME_MAX - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool is_integral_in_range(const cJSON *item, double min, double max)
{
    if (!cJSON_IsNumber(item))
        return false;
    double v = item->valuedouble;
    return v == (double)(int)v && v >= min && v <= max;
}

static ha_cmd_result_t parse_light(const char *label, const char *payload, size_t len,
                                   const char *const *options, size_t options_len,
                                   ha_cmd_t *out, char *err, size_t err_len)
{
    if (len == 0 || len > HA_CMD_PAYLOAD_MAX) {
        set_err(err, err_len, "%s: payload vuoto o oltre %d byte", label,
                HA_CMD_PAYLOAD_MAX);
        return HA_CMD_IGNORED;
    }

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL) {
        set_err(err, err_len, "%s: JSON malformato", label);
        return HA_CMD_IGNORED;
    }
    if (!cJSON_IsObject(root)) {
        set_err(err, err_len, "%s: JSON non e' un oggetto", label);
        cJSON_Delete(root);
        return HA_CMD_IGNORED;
    }

    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsString(state) || state->valuestring == NULL) {
        set_err(err, err_len, "%s: campo state mancante o non stringa", label);
        cJSON_Delete(root);
        return HA_CMD_IGNORED;
    }
    if (strcmp(state->valuestring, "ON") == 0)
        out->light_on = true;
    else if (strcmp(state->valuestring, "OFF") == 0)
        out->light_on = false;
    else {
        set_err(err, err_len, "%s: state non valido (attesi ON/OFF)", label);
        cJSON_Delete(root);
        return HA_CMD_IGNORED;
    }

    cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (b != NULL) {
        if (!is_integral_in_range(b, 0, 255)) {
            set_err(err, err_len, "%s: brightness %.0f fuori range [0..255]",
                    label, cJSON_IsNumber(b) ? b->valuedouble : 0.0);
            cJSON_Delete(root);
            return HA_CMD_IGNORED;
        }
        out->has_brightness = true;
        out->brightness = (int)b->valuedouble;
    }

    cJSON *color = cJSON_GetObjectItemCaseSensitive(root, "color");
    if (color != NULL) {
        cJSON *cr = cJSON_GetObjectItemCaseSensitive(color, "r");
        cJSON *cg = cJSON_GetObjectItemCaseSensitive(color, "g");
        cJSON *cb = cJSON_GetObjectItemCaseSensitive(color, "b");
        if (!is_integral_in_range(cr, 0, 255) || !is_integral_in_range(cg, 0, 255) ||
            !is_integral_in_range(cb, 0, 255)) {
            set_err(err, err_len, "%s: colore r/g/b fuori range [0..255]", label);
            cJSON_Delete(root);
            return HA_CMD_IGNORED;
        }
        out->has_color = true;
        out->r = (uint8_t)cr->valuedouble;
        out->g = (uint8_t)cg->valuedouble;
        out->b = (uint8_t)cb->valuedouble;
    }

    cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "effect");
    if (e != NULL) {
        if (!cJSON_IsString(e) || e->valuestring == NULL ||
            !in_options(options, options_len, e->valuestring)) {
            set_err(err, err_len, "%s: effect non in lista", label);
            cJSON_Delete(root);
            return HA_CMD_IGNORED;
        }
        copy_anim(out->effect, e->valuestring);
        out->has_effect = true;
    }

    cJSON_Delete(root);
    return HA_CMD_OK;
}

/* Chiavi eyes_idle_color/eyes_scare_color: JSON con "state" ON/OFF e/o
 * "color" r/g/b, tutti e due opzionali (payload "{}" = nessuna modifica). */
static ha_cmd_result_t parse_eyes_color(const char *key, const char *payload,
                                        size_t len, ha_cmd_t *out,
                                        char *err, size_t err_len)
{
    if (len == 0 || len > HA_CMD_PAYLOAD_MAX) {
        set_err(err, err_len, "%s: payload vuoto o oltre %d byte", key,
                HA_CMD_PAYLOAD_MAX);
        return HA_CMD_IGNORED;
    }

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL) {
        set_err(err, err_len, "%s: JSON malformato", key);
        return HA_CMD_IGNORED;
    }
    if (!cJSON_IsObject(root)) {
        set_err(err, err_len, "%s: JSON non e' un oggetto", key);
        cJSON_Delete(root);
        return HA_CMD_IGNORED;
    }

    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (state != NULL) {
        if (!cJSON_IsString(state) || state->valuestring == NULL) {
            set_err(err, err_len, "%s: state non stringa", key);
            cJSON_Delete(root);
            return HA_CMD_IGNORED;
        }
        if (strcmp(state->valuestring, "ON") == 0)
            out->onoff = true;
        else if (strcmp(state->valuestring, "OFF") != 0) {
            set_err(err, err_len, "%s: state non valido (attesi ON/OFF)", key);
            cJSON_Delete(root);
            return HA_CMD_IGNORED;
        }
        out->has_state = true;
    }

    cJSON *color = cJSON_GetObjectItemCaseSensitive(root, "color");
    if (color != NULL) {
        cJSON *cr = cJSON_GetObjectItemCaseSensitive(color, "r");
        cJSON *cg = cJSON_GetObjectItemCaseSensitive(color, "g");
        cJSON *cb = cJSON_GetObjectItemCaseSensitive(color, "b");
        if (!is_integral_in_range(cr, 0, 255) || !is_integral_in_range(cg, 0, 255) ||
            !is_integral_in_range(cb, 0, 255)) {
            set_err(err, err_len, "%s: colore r/g/b fuori range [0..255]", key);
            cJSON_Delete(root);
            return HA_CMD_IGNORED;
        }
        out->has_color = true;
        out->r = (uint8_t)cr->valuedouble;
        out->g = (uint8_t)cg->valuedouble;
        out->b = (uint8_t)cb->valuedouble;
    }

    cJSON_Delete(root);
    return HA_CMD_OK;
}

/* Campione decimale puro: payload non NUL-terminato, copia in buffer locale. */
static ha_cmd_result_t parse_scalar(const char *key, const char *payload, size_t len,
                                    int32_t min, int32_t max, int32_t *out_v,
                                    char *err, size_t err_len)
{
    char buf[32];
    if (len == 0 || len >= sizeof buf) {
        set_err(err, err_len, "%s: payload vuoto o troppo lungo", key);
        return HA_CMD_IGNORED;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';

    errno = 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0' || errno == ERANGE) {
        set_err(err, err_len, "%s: payload non numerico intero", key);
        return HA_CMD_IGNORED;
    }
    if (v < (long)INT32_MIN || v > (long)INT32_MAX) {
        set_err(err, err_len, "%s %ld fuori range [%ld..%ld]", key, v, (long)min, (long)max);
        return HA_CMD_IGNORED;
    }
    if (v < (long)min || v > (long)max) {
        set_err(err, err_len, "%s %ld fuori range [%ld..%ld]", key, v, (long)min, (long)max);
        return HA_CMD_IGNORED;
    }
    *out_v = (int32_t)v;
    return HA_CMD_OK;
}

/* Target derivato dalla chiave: il dispatch di ha_entities commuta su
 * cmd.target, che deve essere impostato per OGNI chiave nota (significativo
 * solo con risultato HA_CMD_OK). */
static ha_cmd_target_t target_for_key(const char *key)
{
    if (strcmp(key, "light") == 0)
        return HA_TARGET_LIGHT;
    if (strcmp(key, "auto") == 0)
        return HA_TARGET_AUTO;
    if (strcmp(key, "trigger_mm") == 0)
        return HA_TARGET_TRIGGER_MM;
    if (strcmp(key, "cooldown_s") == 0)
        return HA_TARGET_COOLDOWN_S;
    if (strcmp(key, "trigger_anim") == 0)
        return HA_TARGET_TRIGGER_ANIM;
    if (strcmp(key, "scare") == 0)
        return HA_TARGET_SCARE;
    if (strcmp(key, "eyes") == 0)
        return HA_TARGET_EYES;
    if (strcmp(key, "eyes_idle_color") == 0)
        return HA_TARGET_EYES_IDLE_COLOR;
    if (strcmp(key, "eyes_scare_color") == 0)
        return HA_TARGET_EYES_SCARE_COLOR;
    if (strcmp(key, "eyes_enabled") == 0)
        return HA_TARGET_EYES_ENABLED;
    if (strcmp(key, "eyes_follow_fsm") == 0)
        return HA_TARGET_EYES_FOLLOW;
    return HA_TARGET_NONE;
}

ha_cmd_result_t ha_cmd_parse(const char *key, const char *payload, size_t len,
                             const char *const *options, size_t options_len,
                             ha_cmd_t *out, char *err, size_t err_len)
{
    if (out == NULL)
        return HA_CMD_ERROR;
    memset(out, 0, sizeof *out);
    if (key == NULL) {
        set_err(err, err_len, "chiave NULL");
        return HA_CMD_ERROR;
    }
    if (payload == NULL) {
        set_err(err, err_len, "%s: payload NULL", key);
        return HA_CMD_ERROR;
    }

    out->target = target_for_key(key);

    if (strcmp(key, "light") == 0)
        return parse_light("light", payload, len, options, options_len, out, err, err_len);

    /* Stesso schema JSON di light, ma effect validato contro le opzioni
     * occhi (i 5 persistenti passati dal chiamante, oneshot esclusi). */
    if (strcmp(key, "eyes") == 0)
        return parse_light("eyes", payload, len, options, options_len, out, err, err_len);

    if (strcmp(key, "eyes_idle_color") == 0 || strcmp(key, "eyes_scare_color") == 0)
        return parse_eyes_color(key, payload, len, out, err, err_len);

    if (strcmp(key, "auto") == 0 || strcmp(key, "eyes_enabled") == 0 ||
        strcmp(key, "eyes_follow_fsm") == 0) {
        if (len == 2 && memcmp(payload, "ON", 2) == 0) {
            out->onoff = true;
            return HA_CMD_OK;
        }
        if (len == 3 && memcmp(payload, "OFF", 3) == 0)
            return HA_CMD_OK;
        set_err(err, err_len, "%s: payload non valido (attesi ON/OFF)", key);
        return HA_CMD_IGNORED;
    }

    if (strcmp(key, "trigger_mm") == 0)
        return parse_scalar(key, payload, len, HA_CMD_TRIGGER_MM_MIN,
                            HA_CMD_TRIGGER_MM_MAX, &out->number, err, err_len);

    if (strcmp(key, "cooldown_s") == 0)
        return parse_scalar(key, payload, len, HA_CMD_COOLDOWN_S_MIN,
                            HA_CMD_COOLDOWN_S_MAX, &out->number, err, err_len);

    if (strcmp(key, "trigger_anim") == 0) {
        /* Confronto senza NUL: l'opzione deve essere esattamente lunga len. */
        const char *match = NULL;
        if (options != NULL) {
            for (size_t i = 0; i < options_len; i++) {
                if (options[i] != NULL && strlen(options[i]) == len &&
                    memcmp(options[i], payload, len) == 0) {
                    match = options[i];
                    break;
                }
            }
        }
        if (match == NULL) {
            set_err(err, err_len, "trigger_anim: valore non in lista");
            return HA_CMD_IGNORED;
        }
        copy_anim(out->anim, match);
        return HA_CMD_OK;
    }

    if (strcmp(key, "scare") == 0) {
        if (len == 5 && memcmp(payload, "PRESS", 5) == 0) {
            out->onoff = true;
            return HA_CMD_OK;
        }
        set_err(err, err_len, "scare: payload non valido (atteso PRESS)");
        return HA_CMD_IGNORED;
    }

    set_err(err, err_len, "chiave sconosciuta: %s", key);
    return HA_CMD_IGNORED;
}
