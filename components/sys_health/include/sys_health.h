#pragma once
/* Salute di sistema Fase 7: conferma rollback, statistiche reset,
 * telemetria stabilita' e watchdog. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct {
    bool wifi_up;
    bool mqtt_connected;
    bool sensor_ok;
    bool fsm_ok;          /* FSM in IDLE o OFF */
} sys_health_flags_t;
esp_err_t sys_health_init(void);
void sys_health_notify_sensor_ok(void);
esp_err_t sys_health_register_console(void);
sys_health_flags_t sys_health_get_flags(void);
uint32_t sys_health_reset_count(void);
uint32_t sys_health_wifi_reconnections(void);
uint32_t sys_health_mqtt_reconnections(void);
