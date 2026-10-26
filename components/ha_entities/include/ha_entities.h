#pragma once
#include "esp_err.h"

/* Inizializza registro entita', registra handler eventi (radar/fsm/config/net)
   e prepara il trasporto ha_mqtt. L'avvio effettivo avviene su NET_EVENT_UP. */
esp_err_t ha_entities_init(void);
/* Pubblica discovery + snapshot stato completo (chiama on_connected interno). */
esp_err_t ha_entities_announce(void);
/* Rimuove il dispositivo da HA: payload vuoto retained sul topic discovery
   + availability offline. */
esp_err_t ha_entities_remove(void);
/* Statistiche/contatori per la console. */
void ha_entities_status(char *out, size_t out_len);
