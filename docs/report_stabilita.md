# Report di stabilità — Fase 7

**Stato: ATTREZZATO, NON ESEGUITO.** La strumentazione di stabilità è a bordo e la
build di integrazione è verde, ma **nessuna prova è stata eseguita**: la board non
era collegata. Tutti i numeri di misura di questo documento vanno riempiti al banco;
finché non lo sono valgono solo i dati di build qui sotto (2026-10-26).

## Configurazione build (integrazione 2026-10-26)

| Voce | Valore |
| --- | --- |
| Binario | `pumpkin.bin` **1.400.352 B** (0x155e20) su slot **1984K** → **31,1% libero** |
| IRAM | 125.590 / 131.072 → **95,8%**, 5.482 B liberi (con TLS + OTA a bordo) |
| DRAM statica | **51,4%** |
| Bootloader | 0x6600 (26.112 B), ~9% libero |
| Host test | **10 suite verdi** |
| Warning build | zero (main e componenti con `-Wall -Wextra -Werror`) |
| Target | esp32 (D1 mini), ESP-IDF v5.4.4, `PROJECT_VER` da `git describe` |

Config sicurezza/stabilità in `sdkconfig.defaults`: rollback bootloader attivo
(`BOOTLOADER_APP_ROLLBACK_ENABLE=y`), task watchdog 5 s + interrupt WDT, panic con
reboot, core dump su flash (ELF, CRC32), heap tracing standalone.

## Strumentazione a bordo

- **Telemetria MQTT ogni 60 s** (task `sys_health`, Kconfig `SYS_HEALTH_PERIOD_S`):
  `pumpkin/<id>/status/stability` (uptime, heap free/min, max block, riconnessioni
  Wi-Fi/MQTT, contatore reset, versione) e `status/tasks` (high water mark per
  task). Non retained.
- **`status/reset`** retained a ogni (ri)connessione MQTT: contatore di accensioni
  e motivo dell'ultimo reset (NVS namespace `pumpkin_sys`).
- **Heap tracing** standalone (default nel repo): `heaptrace start|stop|dump [n]`
  da console per verificare l'assenza di allocazioni nei loop caldi.
- **Console**: `sysinfo`, `stability`, `tasks`, `heap`, `heaptrace`, `ota_status`,
  `fsm_state` (elenco completo in [`troubleshooting.md`](troubleshooting.md)).
- **Heap minimo**: `esp_get_minimum_free_heap_size()` nel campione di stabilità
  (minimo storico da boot, chiave per le perdite).
- **Contatori riconnessioni**: Wi-Fi (`NET_EVENT_UP`) e MQTT (ogni connessione),
  esposti in `stability`/`sysinfo`.
- **Test di carico**: `tools/soak_mqtt.py` (trigger simulati su `set/scare`,
  osservatore `update/status`/`status/*`/`availability`, report CSV).

## Procedura 24 ore (da banco)

Preparazione: alimentazione **da rete** (non batteria), broker attivo, CA
caricata, board provisionata, `mosquitto_sub -v -t 'pumpkin/#'` in log su file,
`python3 tools/soak_mqtt.py watch --csv stabilita.csv` attivo.

1. 24 h di funzionamento continuo in AUTO (look idle, `cooldown_s` default).
2. **3 riavvii del router** spalmati nel periodo (attesa riconnessione e ripresa
   telemetria; backoff atteso 1→60 s ±15%).
3. **2 riavvii del broker** (attesa riconnessione esp_mqtt ~10 s e ripubblicazione
   discovery + `status/reset` retained aggiornato).
4. **100 trigger simulati** con `python3 tools/soak_mqtt.py trigger --count 100`
   (intervallo consigliato ≥ 2 s per lasciare scadere lo scare/cooldown).
5. Campionamento ogni ora: `sysinfo` da console (o leggere `status/stability`).
6. A fine corsa: `python3 tools/soak_mqtt.py report --csv stabilita.csv`.

**Criteri PASS**:

- **0 reset inattesi**: il contatore `rst` cresce solo dei reset comandati
  (OTA/alimentazione); nessun WDT o panic (motivi 4/5/6 in `status/reset`).
- **Heap minimo stabile**: `min` (minimo storico) si assesta e non cala tra inizio
  e fine corsa; nessuna tendenza alla diminuzione → nessuna perdita.
- **Riconnessioni con recovery automatico**: ogni caduta Wi-Fi/broker è seguita
  da riconnessione e ripresa di availability/telemetria senza intervento.
- Radar e FSM vivi: eventi di presenza regolari, nessun FAULT permanente.

## Risultati (TEMPLATE DA RIEMPIRE AL BANCO)

### Heap

| Campione | free heap | heap min (storico) | max block | Note |
| --- | --- | --- | --- | --- |
| t=0 | | | | |
| t=1 h | | | | |
| ... | | | | |
| t=24 h | | | | |

### Stack per task (hwm, da `status/tasks` / `tasks`)

| Task | hwm [B] inizio | hwm [B] fine | Note |
| --- | --- | --- | --- |
| `anim` | | | |
| `radar` | | | |
| `fsm` | | | |
| `ha_mqtt` | | | |
| `sys_health` | | | |
| `pumpkin_ota` | (solo se update) | | |
| console/IDLE | | | |

### Riconnessioni e reset

| Evento | Numero | Recovery automatico (sì/no, tempo) | Note |
| --- | --- | --- | --- |
| Wi-Fi down (riavvii router ×3) | | | |
| Broker down (riavvii ×2) | | | |
| Reset totali (`rst` inizio → fine) | | | |
| Reset inattesi (WDT/panic) | | | |

## Checklist banco passo-passo

1. [ ] Flash iniziale: `idf.py -p /dev/ttyUSB0 flash monitor`.
2. [ ] Provisioning BLE dall'app (PoP `pump-XXXXXX` dal log) + TLV `mqtt`.
3. [ ] CA caricata (`ha ca begin` + righe + commit automatico; `ha ca status`).
4. [ ] Discovery visibile in HA (12 entità) — screenshot.
5. [ ] OTA end-to-end: build nuova → app desktop (server HTTPS + CA via seriale +
       `set/update`) → `success` → reboot → `ota_state=VALID` in `sysinfo`.
6. [ ] Rollback reale: installare un'immagine che non conferma (es. broker
       irraggiungibile al riavvio) → rollback al riavvio successivo →
       `update/state` torna alla versione precedente.
7. [ ] 100 trigger simulati (`soak_mqtt.py trigger`): FSM in TRIGGERED/COOLDOWN
       coerente, eventi `event/trigger` sul sub.
8. [ ] Avvio corsa 24 h (log MQTT su file + `soak_mqtt.py watch --csv`).
9. [ ] Riavvii router ×3 e broker ×2 durante la corsa.
10. [ ] Estrazione report (`soak_mqtt.py report`), tabelle qui sopra riempite.
11. [ ] Coredump: verificare assenza (nessun panic atteso); se presente, estrarre
       con `espcoredump.py -c coredump info build/pumpkin.elf`.

## Criteri di accettazione Fase 7 — stato attuale

| Criterio | Stato |
| --- | --- |
| Build integrazione verde, zero warning, numeri flash/IRAM within budget | **fatto** (31,1% slot libero, IRAM 95,8% con 5.482 B liberi, DRAM statica 51,4%) |
| 10 suite host verdi | **fatto** (2026-10-26) |
| Wiki e documentazione aggiornate (ADR-009, componenti OTA/sys_health, README, guide) | **fatto** (questa consegna) |
| App desktop per OTA locale (server HTTPS, CA via seriale) | **fatto nel repo**, da esercitare con la board |
| Script di carico/osservazione `tools/soak_mqtt.py` | **fatto nel repo**, da usare in corsa |
| OTA da entità update di Home Assistant reale | **pendente** (da banco) |
| Rollback con immagine che non conferma | **pendente** (da banco) |
| Corsa 24 h con criteri PASS | **pendente** (da banco) |
| README usabile da zero (setup → flash → provisioning → OTA) | **fatto lato repo** (percorso non ancora percorso fisicamente da zero) |
