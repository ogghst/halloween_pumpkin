---
titolo: Componente pumpkin_ota
tipo: componente
stato: assunto
aggiornato: 2026-10-26
fonti: [components/pumpkin_ota/pumpkin_ota.c, components/pumpkin_ota/ota_policy.c, components/pumpkin_ota/include/*.h, components/pumpkin_ota/Kconfig, components/pumpkin_ota/host_tests (verdi 2026-10-26), docs/report_stabilita.md (numeri build)]
correlate: [../decisioni/adr-009-ota-rollback.md, sys-health.md, ha-mqtt.md, app-principale.md, ../insidie/iram-nimble-wifi.md, ../../../docs/troubleshooting.md, ../../../docs/report_stabilita.md]
---

# Componente `pumpkin_ota`

Aggiornamento firmware via **HTTPS OTA** (`components/pumpkin_ota/`, Fase 7):
gate di politica, controllo versione, eventi su `update/status`, installazione
con **rollback abilitato**. Le decisioni di progetto sono in
[ADR-009](../decisioni/adr-009-ota-rollback.md).

**Stato**: politica pura host-testata, build verde (IRAM 95,8% misurata in
integrazione 2026-10-26); **nessun OTA reale ancora esercitato a banco** —
download, conferma post-reboot e rollback da verificare
([report di stabilità](../../../docs/report_stabilita.md)).

## Architettura

- **`ota_policy.c` — puro (host-testato)**: `policy_check` (gate
  `bad_url`/`battery_low`/`led_full_light`/`heap_low`, output
  `warn_battery_unknown`), `policy_version_changed` (0 = diverse, `v` iniziale
  ignorata), `policy_event_json` (evento compatto < 512 B). Nessuna dipendenza
  ESP-IDF: i valori li misura il chiamante.
- **`pumpkin_ota.c` — layer IDF**: esp_https_ota **ADVANCED** (begin/perform/
  finish), CA letta con `ha_mqtt_ca_get` (verifica condivisa col trasporto
  MQTT, non disabilitabile), eventi su `update/status` via `ha_mqtt_publish`
  (non retained), task `pumpkin_ota` (8192 B, prio 5) creato solo durante
  l'update, iscritto al task watchdog (feed a ogni iterazione).

## API

`pumpkin_ota_init` (idempotente); `pumpkin_ota_set_battery_provider(fn)`
(0..100 o -1 = sconosciuta); `pumpkin_ota_start(url, version, release_summary)`
(gate immediato nel chiamante, poi download in task; `ESP_ERR_INVALID_STATE`
al rifiuto — evento `rejected` già pubblicato col motivo — o se un update è in
corso `busy`); `pumpkin_ota_is_running`; `pumpkin_ota_status(out, len)`
(contatori starts/rejects, ultimo esito).

## Gate (soglie)

| Gate | Rifiuto | Soglia |
| --- | --- | --- |
| URL | `bad_url` | assente/vuota, non `https://`, > 512 B (`PUMPKIN_OTA_URL_MAX`) |
| Batteria | `battery_low` | < 40% se nota; -1 sconosciuta → **consentita** (`"battery":"unknown"`) |
| LED | `led_full_light` | stima corrente rail > `PUMPKIN_OTA_MAX_LED_CURRENT_MA` (default 500 mA) |
| Heap | `heap_low` | libero o largest block < `PUMPKIN_OTA_MIN_FREE_HEAP` (default 64 KB) |
| Occupato | `busy` | un aggiornamento alla volta |
| Versione | `same_version` | immagine identica alla in esecuzione (confronto su `esp_app_desc`) |
| CA | `no_ca` | CA assente/illeggibile: rifiutato prima del download |

## Flusso e eventi

1. `started` → `progress` ogni 10% → `success` (attesa 3 s flush MQTT →
   `esp_restart`) oppure `failed` (errore/download incompleto) / `rejected`.
2. `esp_https_ota_finish` lascia l'immagine **PENDING_VERIFY**: **il componente
   NON marca mai l'immagine valida**; la conferma post-reboot è di
   [sys_health](sys-health.md).
3. Payload evento (JSON): `event`, `detail` (motivo/errore), `installed`,
   `free_heap`, `max_block`, `progress`, `battery:"unknown"` se era sconosciuta.

## Kconfig e host test

- `PUMPKIN_OTA_MIN_FREE_HEAP` (65536), `PUMPKIN_OTA_MAX_LED_CURRENT_MA` (500,
  da `led_fx_estimate_current_ma`), `PUMPKIN_OTA_TASK_WDT` (y: download
  bloccato > 5 s → riavvio → rollback se non confermata).
- `components/pumpkin_ota/host_tests` (pattern CTest del repo,
  [procedura](../procedure/host-tests.md)): gate con tutte le combinazioni di
  soglia, confronto versioni, serializzazione eventi e troncamento cap.

## Cosa NON fare

- **Non chiamare `esp_ota_mark_app_valid` dal percorso OTA**: tocca solo a
  `sys_health` dopo l'auto-test post-reboot (ADR-009).
- **Non abbassare né bypassare la verifica del certificato**: la CA è la stessa
  del MQTT; senza CA si rifiuta, mai fallback HTTP.
- **Non trattare `version`/`release_summary` come decisione di versione**: conta
  solo il descrittore dell'immagine scaricata.

Tag log del componente: `pumpkin_ota`.
