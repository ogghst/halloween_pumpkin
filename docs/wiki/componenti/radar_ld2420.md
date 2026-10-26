---
titolo: Componente radar_ld2420
tipo: componente
stato: verificato
aggiornato: 2026-10-25
fonti: [components/radar_ld2420/include/radar_protocol.h, components/radar_ld2420/include/radar_trigger.h, components/radar_ld2420/include/radar_ld2420.h, components/radar_ld2420/Kconfig, components/radar_ld2420/host_tests (50 casi 2026-10-25), test_apps/radar_ld2420/main/main.c, protocollo Hi-Link HLK-LD2420 (doc tradotto, 2026-10-25), ESPHome ld2420 (consultato 2026-10-25)]
correlate: [../hardware/hlk-ld2420.md, ../hardware/hlk-ld2410b.md, ../decisioni/adr-006-pivot-esp32-d1-mini.md, ../insidie/ld2410b-protocollo-errata.md, ../procedure/host-tests.md, ../domande-aperte.md]
---

# Componente `radar_ld2420`

Radar di presenza HLK-LD2420 su UART1 per la Fase 2 su ESP32 D1 mini. Il firmware è stato **adattato al protocollo LD2420 il 2026-10-25** dopo l'identificazione del modulo fisico (pin 3V3/GND/OT1/RX/OT2); il layer LD2410B v1.07 è stato sostituito (storico: [HLK-LD2410B](../hardware/hlk-ld2410b.md)). [ADR-006](../decisioni/adr-006-pivot-esp32-d1-mini.md) (board e pin) · [hardware](../hardware/hlk-ld2420.md).

**Stato**: codice **verificato** (50/50 test host, build esp32 zero warning, banco senza alimentazione del sensore: guasto/degrado/backoff); **hardware DA VERIFICARE** con modulo alimentato (versione fw, ruoli OT1/OT2, distanze reali).

## Architettura (quattro strati)

- `radar_parser.c` — **FSM pura** su flusso di byte: **frame energia** `F4 F3 F2 F1` (len 0x0023: presenza + range + 16 energie a 16 bit, coda `F8 F7 F6 F5`), ACK comandi e **righe testo running mode** (fw ≤ 1.5.3: CRLF con `ON`/`OFF` e `Range XXXX` in cm, nessun frame energia); risincronizzazione su dati corrotti; contatori `frames_ok/frames_bad/bytes_dropped/lines_parsed`; nessuna allocazione nel path RX.
- `radar_command.c` — **codec comandi LD2420** con transport iniettato: ENTER_CONFIG 0x00FF / EXIT_CONFIG 0x00FE, versione 0x0000, restart 0x0068, lettura parametri 0x0008 (N nomi → N u32), scrittura 0x0007 (coppie nome+u32: cancelli 0x0000/0x0001, timeout 0x0004, soglie move 0x0010+g e still 0x0020+g), modalità seriale 0x0012; `radar_command_session` incapsula ENTER → passo → **EXIT sempre**, anche in errore.
- `radar_trigger.c` — **logica trigger pura** presence/distance con orologio iniettato.
- `radar.c` — **integrazione**: task UART (poll 20 ms, buffer `RADAR_RX_BUFFER`), probe baud, boot sync, eventi su base `PRESENCE_EVENT`, guasto/recovery, calibrazione, NVS.

## Avvio: probe baud, modalità seriale, boot sync

- **Probe baud automatico** (`RADAR_BAUD` 0, default): prova **115200 e 256000** (budget 400 ms a tentativo) finché il modulo risponde alla lettura versione; UART1 con remap esplicito via `uart_set_pin`.
- **Modalità seriale**: fw numerico ≥ 1.5.4 ("v1.5.34" → 1534) → **frame energia** (0x0012); fw ≤ 1.5.3 → **righe testo running mode**: la versione vecchia è gestita dal firmware (probe + parsing testo), non è un caso speciale manuale.
- **Boot sync write-only-if-different**: legge i parametri (0x0008) e riscrive **solo** ciò che differisce dal desiderato; modulo assente o muto: no-op safe (percorso guasto/backoff).

## Eventi su `PRESENCE_EVENT`

`RADAR_EVENT_PRESENCE_ON/OFF`, `RADAR_EVENT_DISTANCE_UPDATED`, `RADAR_EVENT_RADAR_FAULT/OK`; payload `radar_event_data_t` (presenza, degradato, distanza mm) — **invariati** rispetto al vecchio layer LD2410B.

- **Guasto**: silenzio UART oltre `RADAR_FAULT_TIMEOUT_MS` (2000 ms) → evento FAULT e **degrado**: presenza proseguita dal pin OUT (GPIO25) con `degraded=true`; verificato a banco (2026-10-25) con sensore non alimentato: FAULT a ~2 s, console viva e graceful.
- **Recovery**: tentativi ciclici con **backoff esponenziale 1 s → raddoppio → cap 30 s** (1→2→4→8→16→30 s verificati a banco); un probe riuscito ripristina `RADAR_OK`. Bug ms-vs-us nel backoff trovato al banco e corretto.

## Semantica del trigger (presence/distance)

- Frame qualificante: presenza vera con **distanza non nulla** ≤ `trigger_mm` (distanza 0 = modulo senza range: mai qualificante); **ON** dopo `confirm_frames` frame consecutivi (default 3), un frame non qualificante azzera.
- **Tenuta**: ogni frame con presenza rinfresca il timer; **OFF** dopo `hold_ms` (3000 ms) senza presenza, o subito oltre `trigger_mm + hysteresis_mm` (500 mm, isteresi).
- Nessun concetto di bersaglio fermo separato (non esiste nel LD2420): solo presenza+distanza.

## Configurazione del modulo e persistenza

- **NVS namespace `radar`**: `trigger_mm` (u16), `min_gate`/`max_gate` (u8), `unmanned_s` (u16), `move_thresh`/`still_thresh` (**blob 64 B** = 16×u32); le chiavi legacy del vecchio protocollo LD2410 (`sens`, `max_mov`, `max_st`) sono **cancellate all'avvio**.
- **Calibrazione**: `radar_calibrate(secondi)` campiona (async, flag `calibrating`) l'energia per cancello su **stanza vuota** in modalità energia e propone soglia = **picco × percentuale Kconfig** (`RADAR_CAL_MOVE_PCT` 250 %, `RADAR_CAL_STILL_PCT` 225 %); **mai scritta senza** `radar_calibrate_apply` (0x0007 + NVS). Richiede fw ≥ 1.5.4.
- **Distanze**: sul wire in **cm**, convertite in mm; i 16 cancelli (0..15) non hanno comando di risoluzione: la mappa cancello→distanza è del modulo, da verificare a banco.
- **Factoryreset**: il LD2420 **non ha comando di reset**: `radar_factory_reset` riscrive la **tabella di fabbrica ESPHome** (min 1, max 12, timeout 120 s + banche soglie default) e cancella la NVS; solo console, dietro `radar factoryreset confirm`.

## Configurazione (Kconfig principali)

UART 1, baud 0 = probe automatico, GPIO 26 (RX) / 27 (TX) / 25 (OUT), OUT abilitato, RADAR_EN −1; trigger 3000 mm (range 200–8000), conferma 3 frame, tenuta 3000 ms, isteresi 500 mm, cancelli min 1 / max 12 (default di fabbrica), no-one 5 s, fault 2000 ms, calibrazione 250 % / 225 % / 60 s.

## Comandi console e verifica (app di test `test_apps/radar_ld2420`)

`radar status` · `version` · `params` · `trigger <mm>` · `sensitivity <gate|all> <move> <still>` (u32 0..65535) · `limits <min> <max> <s>` · `engineering <on|off>` (energia/testo; **niente `resolution`**) · `restart` · `factoryreset confirm` · `calibrate [secondi]` · `calibrate show` · `calibrate apply` · `csv <on|off>`. Sequenze: [procedura radar](../../procedura_radar.md).

- **Host** (2026-10-25): **50/50** (unity, un solo `main()` in `test_radar_parser.c`): 20 parser, 19 codec, 11 trigger. CTest: [host test](../procedure/host-tests.md).
- **Build esp32** della test app: exit 0, **zero warning**.
- **Banco senza alimentazione del sensore** (2026-10-25): FAULT ~2 s, `degraded=true`, recovery con backoff 1→2→4→8→16→30 s; il resto richiede il modulo alimentato ([domande aperte](../domande-aperte.md)).
- **CSV**: colonne `ms,mode,pres,dist_mm,latched,fault` + 16 energie quando trasmesse (`gates_valid`); task a **20 Hz**, stampa solo su cambiamento riga.
