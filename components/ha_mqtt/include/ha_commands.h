#pragma once
/* Parsing/validazione comandi in ingresso (puro).
 * payload malformati, tipi errati o fuori range -> HA_CMD_IGNORED con motivo in err. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ha_registry.h"

#define HA_CMD_PAYLOAD_MAX 512
/* Specchi dei range di config_store (config_store_core.c sanitize) */
#define HA_CMD_TRIGGER_MM_MIN 100
#define HA_CMD_TRIGGER_MM_MAX 10000
#define HA_CMD_COOLDOWN_S_MIN 0
#define HA_CMD_COOLDOWN_S_MAX 3600

typedef enum {
    HA_TARGET_NONE = 0,
    HA_TARGET_LIGHT,
    HA_TARGET_AUTO,
    HA_TARGET_TRIGGER_MM,
    HA_TARGET_COOLDOWN_S,
    HA_TARGET_TRIGGER_ANIM,
    HA_TARGET_SCARE,
    /* Fase 8: occhi (in coda, come i tipi di ha_registry). */
    HA_TARGET_EYES,           /* light del motore occhi (schema JSON) */
    HA_TARGET_EYES_IDLE_COLOR,  /* colore riposo + eventuale eyes_enabled */
    HA_TARGET_EYES_SCARE_COLOR, /* colore spavento + eventuale eyes_enabled */
    HA_TARGET_EYES_ENABLED,   /* ON/OFF */
    HA_TARGET_EYES_FOLLOW,    /* ON/OFF: occhi seguono la FSM */
} ha_cmd_target_t;

typedef struct {
    ha_cmd_target_t target;
    /* HA_TARGET_LIGHT / HA_TARGET_EYES */
    bool light_on;                    /* campo "state": ON=true */
    bool has_brightness; int brightness; /* 0..255 */
    bool has_color; uint8_t r, g, b;
    bool has_effect; char effect[HA_ANIM_NAME_MAX];
    /* HA_TARGET_EYES_IDLE_COLOR / HA_TARGET_EYES_SCARE_COLOR: campo "state"
     * opzionale (assente = eyes_enabled non toccato). */
    bool has_state;
    /* HA_TARGET_AUTO / HA_TARGET_SCARE / HA_TARGET_EYES_* (ON/OFF) */
    bool onoff;                       /* ON=true / OFF=false / PRESS=true */
    /* HA_TARGET_TRIGGER_MM / HA_TARGET_COOLDOWN_S */
    int32_t number;
    /* HA_TARGET_TRIGGER_ANIM */
    char anim[HA_ANIM_NAME_MAX];
} ha_cmd_t;

typedef enum {
    HA_CMD_OK = 0,
    HA_CMD_IGNORED = -1, /* scartato: log + nessuna azione */
    HA_CMD_ERROR = -2,   /* argomenti funzione invalidi */
} ha_cmd_result_t;

ha_cmd_result_t ha_cmd_parse(const char *key, const char *payload, size_t len,
                             const char *const *options, size_t options_len,
                             ha_cmd_t *out, char *err, size_t err_len);
