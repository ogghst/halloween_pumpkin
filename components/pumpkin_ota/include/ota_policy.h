#pragma once
/* Politica di gate dell'OTA (pura, senza dipendenze ESP-IDF): decide se un
 * aggiornamento puo' partire e compone gli eventi JSON di stato.
 * Il layer IDF (pumpkin_ota.c) misura batteria/corrente LED/heap e passa
 * i valori qui; i limiti arrivano dal Kconfig del componente. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lunghezza massima URL di default (compreso il NUL): allineata al limite
 * pratico del comando MQTT/console. */
#define PUMPKIN_OTA_URL_MAX 512

/* Soglia batteria: sotto il 40% (se nota) l'update e' rifiutato. */
#define PUMPKIN_OTA_BATTERY_MIN_PCT 40

/* Ingresso della politica; warn_battery_unknown e' un campo di OUTPUT
 * impostato da pumpkin_ota_policy_check. */
typedef struct {
    /* misurato dal chiamante */
    const char *url;        /* NULL/vuota/non https:/troppo lunga -> bad_url */
    int battery_pct;        /* 0..100; -1 sconosciuta (consentita, warn) */
    uint32_t led_current_ma;   /* stima corrente rail LED */
    uint32_t free_heap;        /* heap libero */
    uint32_t max_block;        /* massima allocazione contigua */
    /* limiti */
    uint32_t min_free_heap;    /* heap libero e largest block minimi */
    uint32_t max_led_ma;       /* corrente LED oltre la quale si rifiuta */
    size_t url_max;            /* lunghezza massima URL (incluso NUL) */
    /* output */
    bool warn_battery_unknown; /* true se battery_pct era -1 */
} pumpkin_ota_policy_input_t;

/* Evento di stato da pubblicare su update/status. */
typedef struct {
    const char *event_str;  /* "started"|"progress"|"success"|"failed"|"rejected" */
    const char *detail;     /* motivo/errore, NULL se n/d */
    const char *installed;  /* versione installata (success), NULL se n/d */
    uint32_t free_heap;
    uint32_t max_block;
    int progress;           /* 0..100 */
    bool battery_unknown;   /* true -> aggiunge "battery":"unknown" */
} pumpkin_ota_event_t;

/* Gate completo: 0 consenti; -1 rifiuta e scrive il motivo in reason[]
 * ("bad_url"|"battery_low"|"led_full_light"|"heap_low").
 * Setta anche in->warn_battery_unknown (batteria -1: consentita ma
 * segnalata nell'evento started). */
int pumpkin_ota_policy_check(const pumpkin_ota_policy_input_t *in,
                             char *reason, size_t reason_len);

/* Confronto versioni: 0 se DIVERSE (ignorando una 'v' iniziale in ciascuna),
 * -1 se uguali o argomento NULL. */
int pumpkin_ota_policy_version_changed(const char *running, const char *incoming);

/* Evento JSON compatto (senza spazi) in out, NUL finale sempre scritto se
 * cap > 0. Ritorna la lunghezza scritta (escluso NUL); 0 su argomenti
 * invalidi o cap insufficiente (nessun troncamento parziale in output). */
size_t pumpkin_ota_policy_event_json(char *out, size_t cap,
                                     const pumpkin_ota_event_t *ev);
