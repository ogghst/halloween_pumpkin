/* Nucleo puro della salute di sistema: solo libc, nessuna dipendenza IDF.
 * I motivi di reset usano i valori letterali di esp_reset_reason_t
 * (esp_system.h, IDF v5.4): 0 UNKNOWN, 1 POWERON, 2 EXT, 3 SW, 4 PANIC,
 * 5 INT_WDT, 6 TASK_WDT, 7 WDT, 8 DEEPSLEEP, 9 BROWNOUT, 10 SDIO, 11 USB,
 * 12 JTAG, 13 EFUSE, 14 PWR_GLITCH, 15 CPU_LOCKUP. */
#include <stdio.h>
#include <string.h>

#include "sys_health_core.h"

bool sys_health_core_should_confirm(const sys_health_flags_t *f,
                                    bool image_pending)
{
    return image_pending && f != NULL && f->wifi_up && f->mqtt_connected &&
           f->sensor_ok && f->fsm_ok;
}

const char *sys_health_core_reset_reason_str(int reason)
{
    switch (reason) {
    case 1:  return "POWERON";
    case 3:  return "SW";
    case 4:  return "PANIC";
    case 5:  return "INT_WDT";
    case 6:  return "TASK_WDT";
    case 7:  return "WDT";
    case 8:  return "DEEPSLEEP";
    case 9:  return "BROWNOUT";
    default: return "UNKNOWN";
    }
}

size_t sys_health_core_stability_json(char *out, size_t cap,
                                      const sys_health_sample_t *s)
{
    if (out == NULL || cap == 0 || s == NULL)
        return 0;
    out[0] = '\0';
    int n = snprintf(out, cap,
                     "{\"up\":%u,\"free\":%u,\"min\":%u,\"maxblk\":%u,"
                     "\"wr\":%u,\"mr\":%u,\"rst\":%u,\"v\":\"%s\"}",
                     (unsigned)s->uptime_s, (unsigned)s->free_heap,
                     (unsigned)s->min_heap, (unsigned)s->max_block,
                     (unsigned)s->wifi_reconn, (unsigned)s->mqtt_reconn,
                     (unsigned)s->resets,
                     s->version != NULL ? s->version : "");
    if (n < 0)
        return 0;
    return strlen(out);
}

size_t sys_health_core_tasks_json(char *out, size_t cap,
                                  const sys_health_task_hwm_t *tasks, size_t n)
{
    if (out == NULL || cap == 0)
        return 0;
    out[0] = '\0';
    int w = snprintf(out, cap, "{\"t\":[");
    if (w < 0)
        return 0;
    size_t used = (size_t)w;
    if (used >= cap - 1)
        return cap - 1; /* cap minuscolo: solo header troncato, mai oltre cap */

    if (tasks != NULL) {
        for (size_t i = 0; i < n; i++) {
            const char *name = tasks[i].name != NULL ? tasks[i].name : "";
            int need = snprintf(NULL, 0, "%s{\"n\":\"%s\",\"h\":%u}",
                                i == 0 ? "" : ",", name,
                                (unsigned)tasks[i].hwm);
            if (need < 0)
                break;
            /* serve spazio per l'elemento + chiusura "]}" + NUL finale */
            if (used + (size_t)need > cap - 3)
                break; /* tronca l'array: i task oltre il cap restano fuori */
            w = snprintf(out + used, cap - used, "%s{\"n\":\"%s\",\"h\":%u}",
                         i == 0 ? "" : ",", name, (unsigned)tasks[i].hwm);
            if (w < 0)
                break;
            used += (size_t)w;
        }
    }
    (void)snprintf(out + used, cap - used, "]}");
    return strlen(out);
}
