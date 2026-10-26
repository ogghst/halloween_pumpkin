#pragma once
/* Nucleo puro (host-testabile) della salute di sistema: decisione di conferma
 * rollback, mapping dei motivi di reset e serializzazione JSON della
 * telemetria. Nessuna dipendenza da ESP-IDF: i motivi di reset seguono la
 * numerazione di esp_reset_reason_t (esp_system.h, IDF v5.4). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sys_health.h" /* sys_health_flags_t: contratto pubblico */

#ifdef __cplusplus
extern "C" {
#endif

/* Campione di telemetria stabilita' (riempito dal glue IDF). */
typedef struct {
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t min_heap;
    uint32_t max_block;
    uint32_t wifi_reconn;
    uint32_t mqtt_reconn;
    uint32_t resets;
    const char *version;
} sys_health_sample_t;

/* Nome e high water mark dello stack (byte) di un task. */
typedef struct {
    const char *name;
    uint32_t hwm;
} sys_health_task_hwm_t;

/* true SOLO con immagine in pending verify E tutte e quattro le flag vere
 * (Wi-Fi su, MQTT connesso, sensore ok, FSM in IDLE o OFF).
 * La conferma una-sola-volta (marcare l'immagine valida e non ripetere la
 * verifica) e' responsabilita' del chiamante, che legge lo stato OTA e
 * smette di chiamare quando l'immagine non e' piu' in pending. */
bool sys_health_core_should_confirm(const sys_health_flags_t *f,
                                    bool image_pending);

/* Stringa statica (nessuna allocazione) per il motivo di reset secondo la
 * numerazione esp_reset_reason_t: 1 POWERON, 3 SW, 4 PANIC, 5 INT_WDT,
 * 6 TASK_WDT, 7 WDT, 8 DEEPSLEEP, 9 BROWNOUT; UNKNOWN per tutto il resto. */
const char *sys_health_core_reset_reason_str(int reason);

/* {"up":..,"free":..,"min":..,"maxblk":..,"wr":..,"mr":..,"rst":..,"v":".."}
 * JSON compatto: con input tipici ~100 B, limite di progetto 400 B.
 * Scrive sempre il NUL finale (se cap > 0) e non sfora mai il cap.
 * Ritorna i byte scritti senza NUL (cap-1 in caso di troncamento), 0 su
 * argomenti nulli. */
size_t sys_health_core_stability_json(char *out, size_t cap,
                                      const sys_health_sample_t *s);

/* {"t":[{"n":"nome","h":123},..]}: se serve, tronca l'array in coda per
 * chiudere "]}" entro il cap (limite di progetto 512 B, cap del publish
 * MQTT). Ritorna i byte scritti senza NUL, 0 su argomenti nulli. */
size_t sys_health_core_tasks_json(char *out, size_t cap,
                                  const sys_health_task_hwm_t *tasks, size_t n);

#ifdef __cplusplus
}
#endif
