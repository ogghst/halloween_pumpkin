---
titolo: App principale e app di test
tipo: componente
stato: verificato
aggiornato: 2026-10-26
fonti: [main/main.c, test_apps/led_effects/main/main.c, test_apps/net_prov/main/main.c, test_apps/ha_mqtt (Fase 5), README.md]
correlate: [led_effects.md, net-mgr.md, wifi-prov.md, ha-mqtt.md, ../procedure/flash-monitor.md, ../procedure/provisioning.md, ../../../main/main.c, ../../../test_apps/led_effects/main/main.c, ../../../test_apps/net_prov/main/main.c]
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

## App di banco rete+provisioning (`test_apps/net_prov/`, Fase 4)

- [main.c](../../../test_apps/net_prov/main/main.c): catena offline identica
  all'app `pumpkin_fsm` (config_store v2 + led_effects + radar + FSM) più la
  parte rete: l'app possiede l'init one-time del chip (netif STA default +
  `esp_wifi_init`, contratto [wifi_prov](wifi-prov.md)) e decide al boot fra
  provisioning (senza credenziali) e `net_mgr_start()`.
- `sdkconfig.defaults` proprio: BLE NimBLE on, classic BT off, 1 connessione
  max; `partitions.csv` copia locale del root (slot OTA 2×1536K).
- Nessuna credenziale nel codice: SSID/password arrivano solo da provisioning
  ([procedura](../procedure/provisioning.md)); overlay LED su eventi PROV/NET.
- Console: `net` (diag + hostname; `net ps min|max`, `net listen <1..10>`,
  `net reconnect`), `prov` (stato/heap vs baseline; `prov start/stop`;
  `prov mqtt host=X port=N user=U pass=P tls=0|1` via TLV), `cfg_get [mqtt]`,
  `cfg_flush`, `fsm_state`, `fsm_mode`, `heap`.

## App di banco MQTT (`test_apps/ha_mqtt/`, Fase 5)

- Catena completa (config_store + led + radar + FSM + rete) più `ha_mqtt`/`ha_entities`:
  avvio MQTT su `NET_EVENT_UP`, stop su `NET_EVENT_DOWN`; `PROJECT_VER 0.5.0` →
  `sw_version` del device HA.
- Console: tutti i comandi di `net_prov` più `ha status`, `ha announce`
  (ripubblica discovery + stato), `ha remove`, `ha ca clear|begin|+<riga>|status`,
  `ha pub <topic> <payload>`.
- Guida MQTT e checklist di banco: [docs/mqtt.md](../../mqtt.md) (fuori wiki) e
  [ha_mqtt](ha-mqtt.md).

## Comandi rapidi

- Build/flash: `idf.py -C test_apps/led_effects build flash monitor` — [procedura](../procedure/flash-monitor.md).
- Logica pura su host: [host test](../procedure/host-tests.md).
