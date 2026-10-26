#pragma once
/* Aggiornamento firmware via HTTPS OTA (esp_https_ota ADVANCED) per la
 * zucca: gate di politica (batteria/LED/heap), controllo versione contro
 * l'immagine scaricata, eventi su update/status.
 *
 * Il successo NON marca l'immagine valida: con il rollback abilitato
 * l'immagine resta ESP_OTA_IMG_PENDING_VERIFY fino alla validazione
 * post-reboot fatta da sys_health ( conferma dopo l'auto-test ). */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Prepara il modulo (idempotente). Nessun lavoro pesante. */
esp_err_t pumpkin_ota_init(void);

/* Sorgente del livello batteria per il gate: ritorna 0..100 o -1 se
 * sconosciuta (consentita). Chiamata a ogni tentativo di avvio. */
void pumpkin_ota_set_battery_provider(int (*fn)(void));

/* Avvia l'aggiornamento dall'URL HTTPS: gate immediato nel chiamante,
 * poi download/flash in un task dedicato (una istanza per volta).
 * version/release_summary sono metadati informativi del comando; la
 * decisione di versione usa il descrittore dell'immagine scaricata.
 * Ritorna ESP_ERR_INVALID_STATE al rifiuto della politica (evento
 * "rejected" gia' pubblicato con il motivo) o se un update e' gia'
 * in corso ("busy"). */
esp_err_t pumpkin_ota_start(const char *url, const char *version,
                            const char *release_summary);

/* true tra l'accettazione di uno start e il termine (riavvio/errore). */
bool pumpkin_ota_is_running(void);

/* Stato per la console: attivita', ultimo esito, contatori. */
void pumpkin_ota_status(char *out, size_t out_len);
