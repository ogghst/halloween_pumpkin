---
titolo: Tick FreeRTOS a 100 Hz vs 60 fps
tipo: insidia
stato: verificato
aggiornato: 2026-10-25
fonti: [README.md, sdkconfig.defaults]
correlate: [../componenti/led_effects.md, ../../../sdkconfig.defaults, ../../../README.md]
---

# Tick FreeRTOS a 100 Hz insufficiente per i 60 fps

## Il problema

- Il task `anim` di `led_effects` usa `vTaskDelayUntil` con periodo **~16,7 ms** (60 fps).
- Con il tick FreeRTOS di default a **100 Hz** la risoluzione è di 10 ms: il periodo da 16,7 ms
  non è rispettabile e il rendering perde il ritmo.

## La soluzione già applicata

- `CONFIG_FREERTOS_HZ=1000` in [sdkconfig.defaults](../../../sdkconfig.defaults)
  (radice del progetto e app di test) — verificato in Fase 1.
- Il `sdkconfig` generato è gitignored: modificare i defaults **non** tocca un `sdkconfig`
  esistente (serve rigenerarlo, es. con `idf.py set-target esp32s3`).

## Da ricordare

- Anche altri task futuri con timing fine (sensori, debounce, animazioni) beneficiano del tick a 1 kHz.
- Non abbassare `CONFIG_FREERTOS_HZ` per risparmiare CPU senza rivedere tutti i periodi.
