---
titolo: Componente radar_ld2410
tipo: componente
stato: verificato
aggiornato: 2026-10-25
fonti: [components/radar_ld2410/include/radar_protocol.h, components/radar_ld2410/include/radar_trigger.h, components/radar_ld2410/include/radar_ld2410.h, components/radar_ld2410/Kconfig, components/radar_ld2410/host_tests (42 casi 2026-10-25), test_apps/radar_ld2410/main/main.c, test_apps/radar_ld2410/build/test_radar_ld2410.bin (0x43aa0 B 2026-10-25), Hi-Link protocollo v1.07]
correlate: [../hardware/hlk-ld2410b.md, ../decisioni/adr-006-pivot-esp32-d1-mini.md, ../insidie/ld2410b-protocollo-errata.md, ../insidie/gpio-da-evitare.md, ../procedure/host-tests.md, ../../../components/radar_ld2410/include/radar_ld2410.h]
---

# Componente `radar_ld2410`

Radar di presenza HLK-LD2410B su UART1 per la Fase 2 su ESP32 D1 mini: frame, trigger, eventi.
[ADR-006](../decisioni/adr-006-pivot-esp32-d1-mini.md) (board e pin) ·
[hardware](../hardware/hlk-ld2410b.md).

**Stato**: codice **verificato** (build esp32 della test app exit 0, zero warning + 42/42 test
host verdi); **hardware NON verificato** (modulo mai collegato: pagina hardware `assunto`).

## Architettura (quattro strati)

- `radar_parser.c` — **FSM pura** su flusso di byte: valida header, lunghezza, marker e coda
  (ACK `FD FC FB FA…`, report `F4 F3 F2 F1…`); si risincronizza su dati corrotti; contatori
  `frames_ok/frames_bad/bytes_dropped`; nessuna allocazione nel path RX.
- `radar_command.c` — **codec comandi** e transazioni sincrone con transport iniettato
  (host-testabile); `radar_command_session` incapsula la regola del protocollo: ogni comando di
  config viaggia dentro **ENTER_CONFIG (0x00FF) → comando → EXIT_CONFIG (0x00FE)**, EXIT sempre.
- `radar_trigger.c` — **logica trigger pura** con orologio iniettato (semantica sotto).
- `radar.c` — **integrazione**: task UART1 (buffer `RADAR_RX_BUFFER`), eventi su base
  `PRESENCE_EVENT`, boot sync, guasto/recovery, calibrazione.

## Eventi su `PRESENCE_EVENT`

`RADAR_EVENT_PRESENCE_ON/OFF`, `RADAR_EVENT_DISTANCE_UPDATED`, `RADAR_EVENT_RADAR_FAULT/OK`;
payload `radar_event_data_t` (presenza, statica, degradato, distanza mm, energia movimento).
- **Guasto**: silenzio UART oltre `RADAR_FAULT_TIMEOUT_MS` (2000 ms) → evento FAULT e degrado
  sul pin OUT (GPIO25); recovery con **backoff** → RADAR_OK; sensore assente all'avvio: no-op.

## Semantica del trigger

- Frame qualificante: bersaglio in movimento (o fermo, se `CONFIG_RADAR_STATIC_TRIGGER`) con
  distanza ≤ `trigger_mm` ed energia ≥ `trigger_energy`.
- **ON** dopo `confirm_frames` frame consecutivi (default 3); un frame non qualificante azzera.
- **Tenuta**: ogni frame con un bersaglio qualsiasi rinfresca il timer; **OFF** dopo `hold_ms`
  (default 3000) senza nessun bersaglio, o subito se il bersaglio in movimento esce oltre
  `trigger_mm + hysteresis_gates × gate_mm` (**isteresi**, default 1 cancello).
- Bersaglio **solo fermo** (persona immobile): nessun trigger ma `present_static` vero
  (presenza per HA senza nuovo spavento); uscire e rientrare riarma normalmente.

## Configurazione del modulo e persistenza

- **Boot sync write-only-if-different**: all'avvio legge parametri (0x0061) e risoluzione
  (0x00AB) e riscrive **solo** ciò che differisce (limiti 0x0060, sensibilità 0x0064).
  Persistito in **NVS namespace `radar`**: `trigger_mm` (u16), `sens` (blob 18 B: motion +
  static 0..8), `max_mov`/`max_st` (u8), `unmanned_s` (u16); `factoryreset` le cancella.
- **Calibrazione**: `radar_calibrate(secondi)` campiona in engineering mode l'energia per
  cancello su **stanza vuota** e propone soglia = massimo osservato + `RADAR_CAL_MARGIN`
  (default 10); **mai scritta senza** `radar_calibrate_apply` (0x0064 per cancello + NVS).
- **Distanze**: sul wire in **cm** (errata, Table 13), convertite in mm e **quantizzate al
  cancello** (750 mm default, 200 mm opzionale 0x00AA): la granularità è il cancello.

## Configurazione (Kconfig principali)

UART 1 con remap via `uart_set_pin` (nessun pin di default), baud 256000, GPIO 26 (RX, ← TX
radar) / 27 (TX, → RX radar) / 25 (OUT), OUT abilitato, RADAR_EN −1; trigger 3000 mm, energia
20, conferma 3 frame, tenuta 3000 ms, isteresi 1 cancello, static trigger no; cancelli 8/8,
no-one 5 s, fault 2000 ms, calibrazione margine 10 / 60 s.

## Comandi console e verifica (app di test `test_apps/radar_ld2410`)

`radar status` · `version` · `params` · `resolution` · `trigger <mm>` · `sensitivity <gate|all> <motion> <static>` ·
`limits <mov> <st> <durata_s>` · `engineering <on|off>` · `restart` · `factoryreset confirm` · `calibrate [secondi]` ·
`calibrate show` · `calibrate apply` · `csv <on|off>`. Sequenze: [procedura radar](../../procedura_radar.md).

- **Target** (2026-10-25): `idf.py -C test_apps/radar_ld2410 build` exit 0, **zero warning**;
  binario `test_radar_ld2410.bin` **0x43aa0 B** (~18% dello slot `ota_0` da 1536K, ~82% libero).
- **Host**: **42/42** (unity, un solo `main()` in `test_radar_parser.c`): 15 parser, 14 codec
  comandi/transazioni, 13 trigger. CTest: [host test](../procedure/host-tests.md).
- **Telemetria CSV**: `radar_state_t` esteso **additivamente** (`target_state`,
  `moving/static_distance_mm`, `static_energy`); task a **20 Hz** (50 ms) stampa solo su
  cambiamento riga; colonne `ms,state,mov_mm,mov_en,st_mm,st_en,det_mm,presence,static,eng,deg,fault`.
