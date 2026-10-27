/* Stub freertos/FreeRTOS.h per gli host test: solo cio' che usa
 * config_store.c. L'header reale arriva da ESP-IDF nei build su target. */
#ifndef STUB_FREERTOS_FREE_RTOS_H
#define STUB_FREERTOS_FREE_RTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;

#define portMAX_DELAY ((TickType_t)0xffffffffUL)

#endif /* STUB_FREERTOS_FREE_RTOS_H */
