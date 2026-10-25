---
titolo: App principale e app di test
tipo: componente
stato: verificato
aggiornato: 2026-10-25
fonti: [main/main.c, test_apps/led_effects/main/main.c, README.md]
correlate: [led_effects.md, ../procedure/flash-monitor.md, ../procedure/host-tests.md, ../../../main/main.c, ../../../test_apps/led_effects/main/main.c]
---

# App principale e app di test

## App principale (`main/`)

- [main.c](../../../main/main.c) è ancora lo **skeleton della Fase 0**: `app_main` logga il boot
  e resta in loop con `vTaskDelay` di 10 s.
- Le fasi successive (`tof_sensor`, `pumpkin_fsm`, `power_mgr`, `ha_mqtt`+`config_store`, `wifi_prov`)
  verranno agganciate qui, una alla volta con pianificazione dedicata.

## App di test LED (`test_apps/led_effects/`)

- [main.c](../../../test_apps/led_effects/main/main.c): all'avvio **cicla automaticamente i 6 effetti**
  (10 s ciascuno, `scare` riprodotto come one-shot) e stampa periodicamente statistiche e stima corrente.
- Console UART: `fx`, `oneshot`, `color`, `bright`, `speed`, `power` (rail on/off), `pixel` (`pixel <x> <y> <r> <g> <b>` per il pixel test sulla matrice, `pixel off` per rilasciare), `stats`, `current`, `scale`.
- Ha un proprio `sdkconfig.defaults` con `CONFIG_FREERTOS_HZ=1000` (necessario per i 60 fps).

## Comandi rapidi

- Build/flash: `idf.py -C test_apps/led_effects build flash monitor` — [procedura](../procedure/flash-monitor.md).
- Logica pura su host: [host test](../procedure/host-tests.md).
