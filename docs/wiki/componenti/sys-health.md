---
titolo: Componente sys_health
tipo: componente
stato: assunto
aggiornato: 2026-10-26
fonti: [components/sys_health/sys_health.c, components/sys_health/sys_health_core.c, components/sys_health/include/*.h, components/sys_health/Kconfig, components/sys_health/host_tests (verdi 2026-10-26), sdkconfig.defaults]
correlate: [../decisioni/adr-009-ota-rollback.md, pumpkin-ota.md, ha-mqtt.md, app-principale.md, ../procedure/host-tests.md, ../../../docs/report_stabilita.md, ../../../docs/troubleshooting.md]
---

# Componente `sys_health`

Salute di sistema (`components/sys_health/`, Fase 7): conferma post-OTA
(rollback), statistiche di reset su NVS, telemetria di stabilità su MQTT,
task watchdog e heap tracing. Decisioni in [ADR-009](../decisioni/adr-009-ota-rollback.md).

**Stato**: nucleo puro host-testato, build verde; la telemetria reale (heap,
riconnessioni, tempi) è **da raccogliere al banco** — template nel
[report di stabilità](../../../docs/report_stabilita.md).

## Architettura

- **`sys_health_core.c` — puro (host-testato)**: `should_confirm` (true SOLO
  con immagine in pending verify E le quattro flag vere),
  `reset_reason_str` (mapping `esp_reset_reason_t`: 1 POWERON, 3 SW, 4 PANIC,
  5 INT_WDT, 6 TASK_WDT, 7 WDT, 8 DEEPSLEEP, 9 BROWNOUT), JSON
  `stability_json` (~100 B, cap 400) e `tasks_json` (troncamento in coda
  entro 512 B).
- **`sys_health.c` — glue IDF**: eventi (NET/FSM/PRESENCE) aggiornano le
  flag, listener MQTT via `ha_mqtt_on_connected_add`, task `sys_health`
  (4096 B, prio 3) che pubblica ogni `SYS_HEALTH_PERIOD_S` (default 60)
  attendendo a passi da 1 s con feed del watchdog.

## Conferma rollback (con pumpkin_ota)

A ogni cambio flag, a ogni periodo e a ogni (ri)connessione MQTT: se
l'immagine corrente è `PENDING_VERIFY` e valgono **Wi-Fi su + MQTT connesso +
sensore radar ok + FSM in IDLE o OFF**, chiama
`esp_ota_mark_app_valid_cancel_rollback` e logga `immagine <ver> confermata
valida: rollback annullato`. Immagini in altri stati: mai toccate (la conferma
è una sola: PENDING→VALID toglie la condizione). Con
`BOOTLOADER_APP_ROLLBACK_ENABLE=n` tutto il percorso è compilato fuori.

## Telemetria e topic

| Topic | Retained | Contenuto |
| --- | --- | --- |
| `status/stability` | no | ogni 60 s: `{"up","free","min","maxblk","wr","mr","rst","v"}` (uptime, heap libero/minimo storico, max block, riconnessioni, reset, versione) |
| `status/tasks` | no | ogni 60 s: `{"t":[{"n":"anim","h":1234},...]}` (nome + high water mark, cap 24 task) |
| `status/reset` | sì | a ogni (ri)connessione MQTT: `{"count","reason","reason_str","v"}` |

(Topic completi `pumpkin/<id>/status/...`.) Contatore accensioni e motivo
dell'ultimo reset in **NVS namespace `pumpkin_sys`** (`resets`/`reason`); senza
NVS il contatore resta in RAM.

## Flag e API

`sys_health_flags_t`: `wifi_up` (NET UP/DOWN), `mqtt_connected`,
`sensor_ok` (RADAR_OK/FAULT e `sys_health_notify_sensor_ok`, idempotente),
`fsm_ok` (IDLE o OFF). Lettura con `sys_health_get_flags`; contatori con
`sys_health_reset_count`/`_wifi_reconnections`/`_mqtt_reconnections`.

## Console e Kconfig

- `sysinfo` (versione, `ota_state`, partizione, reset, flag, riconnessioni,
  heap), `stability` (JSON di stability), `tasks` (JSON task),
  `heaptrace start|stop|dump [n]` (heap tracing standalone, default attivo nel
  repo, buffer 200 record).
- `SYS_HEALTH_PERIOD_S` (60, range 10–3600), `SYS_HEALTH_TASK_WDT` (y: attesa
  a passi da 1 s per alimentare il watchdog senza stringare il periodo).

## Host test e cosa NON fare

`components/sys_health/host_tests`: decisione di conferma (combinazioni di
flag), mapping motivi di reset, JSON con troncamento cap.

- **Non confermare l'immagine OTA fuori dal criterio a quattro flag** (ADR-009):
  la conferma prematura disarma il rollback.
- **Nessun lavoro lento nelle callback evento**: il publish avviene nel task di
  telemetria, i listener MQTT girano nel worker di `ha_mqtt`.
- **Non fidarsi della telemetria pubblicata se MQTT è giù**: `telemetry_publish`
  salta il periodo senza connessione (informazione persa, non ritardata).

Tag log del componente: `sys_health`.
