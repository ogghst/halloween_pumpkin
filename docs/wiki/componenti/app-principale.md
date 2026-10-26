---
titolo: App principale e app di test
tipo: componente
stato: assunto
aggiornato: 2026-10-26
fonti: [main/main.c (846 righe, Fase 7), CMakeLists.txt (PROJECT_VER), test_apps/led_effects/main/main.c, test_apps/net_prov/main/main.c, test_apps/ha_mqtt, test_apps/pumpkin_fsm, test_apps/radar_ld2420, build di integrazione 2026-10-26]
correlate: [led_effects.md, net-mgr.md, wifi-prov.md, ha-mqtt.md, pumpkin-ota.md, sys-health.md, ../decisioni/adr-009-ota-rollback.md, ../procedure/flash-monitor.md, ../procedure/provisioning.md, ../../../docs/architettura.md, ../../../docs/troubleshooting.md, ../../../main/main.c]
---

# App principale e app di test

## App principale (`main/`, Fase 7)

[main.c](../../../main/main.c) è l'**app di produzione completa**: wiring di
config_store + led_effects + pumpkin_fsm + radar + wifi_prov + net_mgr +
ha_entities/ha_mqtt più **pumpkin_ota** e **sys_health**.

- **Boot** (ordine): NVS → event loop → config_store (default dal Kconfig) →
  led_fx → radar → FSM → ha_entities (registro + handler + trasporto pronto) →
  pumpkin_ota → sys_health → handler CONFIG/FSM/PROV/NET dell'app → sync
  `trigger_mm` store→radar → init rete (**netif STA creato UNA volta qui**,
  `net_mgr_start` lo adotta) → decisione provisioning vs `net_mgr_start` →
  console REPL (`pumpkin>`).
- **Nessun provider di batteria**: `pumpkin_ota` tratta la batteria come
  sconosciuta (-1) e consente l'update con `"battery":"unknown"`
  ([ADR-009](../decisioni/adr-009-ota-rollback.md)).
- **MQTT**: avvio su `NET_EVENT_UP`, stop su `NET_EVENT_DOWN`; il comando
  `set/update` arriva ad `ha_entities`, che chiama `pumpkin_ota_start`.
- **Console**: `net`, `prov` (start/stop/mqtt), `cfg_get`, `cfg_flush`,
  `fsm_state`, `fsm_mode`, `ha` (status/announce/remove/ca/pub), `heap`,
  `ota <url> [versione]`, `ota_status`, più i comandi di `sys_health`
  (`sysinfo`, `stability`, `tasks`, `heaptrace`). Nessuna credenziale stampata.
- **Build di integrazione 2026-10-26**: zero warning; `pumpkin.bin`
  1.400.352 B su slot 1984K (31,1% libero), IRAM 95,8% (5.482 B liberi),
  DRAM statica 51,4%. **Banco (OTA, provisioning reale, 24 h) pendente**:
  [report di stabilità](../../../docs/report_stabilita.md).
- `PROJECT_VER` da `git describe` (CMakeLists radice): alimenta `sw_version`
  del device HA, `update/state` e il gate `same_version`.

## App di test (`test_apps/`)

Build: `idf.py -C test_apps/<nome> build flash monitor` — [procedura](../procedure/flash-monitor.md).

- [**led_effects**](../../../test_apps/led_effects/main/main.c): ciclo
  automatico dei 6 effetti (10 s) + console `fx`, `oneshot`, `color`, `bright`,
  `speed`, `power`, `pixel`/`pixel off`, `stats`, `current`, `scale`
  (tabella completa nel [README](../../../README.md)).
- **net_prov** (Fase 4): catena offline + provisioning/rete; console `net`,
  `prov`, `cfg_get`, `cfg_flush`, `fsm_state`, `fsm_mode`, `heap`.
- **pumpkin_fsm** (Fase 3): banco FSM con sync `trigger_mm`; console
  `fsm`, `fsm_mode`, `fsm_scare`, `fsm_light`, `fsm_anim`, `cfg_get`,
  `cfg_set`, `cfg_flush`.
- **radar_ld2420** (Fase 2): console radar completa (status, calibrate,
  factoryreset, csv...), dettagli in [radar_ld2420](radar_ld2420.md).
- **ha_mqtt** (Fase 5): catena completa Fase 5 con console `ha status|
  announce|remove|ca|pub`; guida [docs/mqtt.md](../../mqtt.md).

Le app di test restano come banchi monofunzione; il riferimento per la zucca
vera è ora l'app principale.

## Comandi rapidi

- Build/flash app principale: `idf.py -p /dev/ttyUSB0 flash monitor`.
- Logica pura su host: [host test](../procedure/host-tests.md).
- Troubleshooting e comandi console: [troubleshooting](../../../docs/troubleshooting.md).
