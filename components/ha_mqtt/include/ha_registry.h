#pragma once
/* Registro entita' HA: tabella estesa a runtime dalle fasi successive.
 * Puro: nessuna dipendenza ESP-IDF. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HA_KEY_MAX 24
#define HA_ANIM_NAME_MAX 16
#define HA_ENTITY_MAX 24

typedef enum {
    HA_ET_LIGHT = 0,        /* light schema JSON (brightness+rgb+effect) */
    HA_ET_BINARY_SENSOR,
    HA_ET_SENSOR,
    HA_ET_SWITCH,
    HA_ET_NUMBER,
    HA_ET_SELECT,
    HA_ET_BUTTON,
    HA_ET_UPDATE,           /* update firmware (title + latest_version_topic) */
    /* In coda (non dopo HA_ET_LIGHT): gli id di tipo non viaggiano sul filo,
     * accodare evita di rinumerare i tipi esistenti e mantiene i diff minimi. */
    HA_ET_LIGHT_RGB,        /* light schema JSON, solo rgb: senza brightness/effect */
} ha_entity_type_t;

typedef enum {
    HA_CAT_NONE = 0,
    HA_CAT_CONFIG,
    HA_CAT_DIAGNOSTIC,
} ha_entity_category_t;

typedef struct {
    const char *key;          /* chiave stabile nel topic e unique_id */
    const char *name;         /* nome visibile (HA lo prefissa col device) */
    ha_entity_type_t type;
    ha_entity_category_t category;
    const char *unit;         /* sensor/number, NULL se n/d */
    const char *device_class; /* NULL se n/d */
    bool retained;            /* stato retained sul broker */
    bool has_command;         /* esiste pumpkin/<id>/set/<key> */
    const char *const *options; /* light: effect_list; select: options */
    size_t options_len;
    int32_t num_min, num_max, num_step; /* number */
} ha_entity_desc_t;

int ha_registry_reset(void);
int ha_registry_add(const ha_entity_desc_t *d); /* 0 ok; -1 arg invalido o pieno */
size_t ha_registry_count(void);
const ha_entity_desc_t *ha_registry_at(size_t i); /* NULL fuori range */
