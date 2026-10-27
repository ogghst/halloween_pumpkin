/* Stub freertos/semphr.h per gli host test: mutex finto senza
 * concorrenza (i test sono single-thread). L'header reale arriva da
 * ESP-IDF nei build su target. */
#ifndef STUB_FREERTOS_SEMPHR_H
#define STUB_FREERTOS_SEMPHR_H

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t ticks);
int xSemaphoreGive(SemaphoreHandle_t lock);
void vSemaphoreDelete(SemaphoreHandle_t lock);

#endif /* STUB_FREERTOS_SEMPHR_H */
