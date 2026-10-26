---
titolo: Modulo radar HLK-LD2420
tipo: hardware
stato: verificato
aggiornato: 2026-10-25
fonti: [scritte a bordo riportate dall'utente 2026-10-25 (3V3, GND, OT1, RX, OT2); ESPHome "LD2420 24GHz mmWave Radar Sensor" (tabelle pinout per firmware, consultata 2026-10-25); ADR-006 pivot D1 mini 2026-10-25 (pin 26/27/25); protocollo Hi-Link HLK-LD2420 (doc tradotto, 2026-10-25); adattamento firmware al LD2420 2026-10-25 (componente radar_ld2420)]
correlate: [hlk-ld2410b.md, esp32-d1-mini.md, ../decisioni/adr-006-pivot-esp32-d1-mini.md, ../insidie/gpio-da-evitare.md, ../componenti/radar_ld2420.md, ../domande-aperte.md]
---

# Modulo radar HLK-LD2420

**Identificazione del modulo fisico (2026-10-25)**: i pin presenti sul modulo riportati
dall'utente — **3V3, GND, OT1, RX, OT2** — corrispondono al pinout dell'**HLK-LD2420**
(sensore di presenza/movimento 24 GHz Hi-Link), **non** al pinout dell'HLK-LD2410B
(VCC 5 V, OUT, UART_Tx, UART_Rx, GND). Il firmware **parla il protocollo LD2420 dal
2026-10-25** (componente [radar_ld2420](../componenti/radar_ld2420.md)); la pagina
[HLK-LD2410B](hlk-ld2410b.md) resta solo il riferimento storico del protocollo v1.07.
Fonte pinout: [documentazione ESPHome LD2420](https://esphome.io/components/sensor/ld2420) (consultata 2026-10-25).

## Pinout (assunto, da confermare leggendo la versione firmware)

**Il ruolo di OT1 e OT2 dipende dalla versione del firmware del modulo** (fonte ESPHome):

| Pin | fw < 1.5.3 | fw ≥ 1.5.3 |
| --- | ---------- | ----------- |
| 1 — 3V3 | VCC (3,3 V) | VCC (3,3 V) |
| 2 — GND | GND | GND |
| 3 — OT1 | **uscita presenza** (Presence Signal Output) | **TX seriale** (verso ESP32 RX) |
| 4 — RX | RX seriale (dal ESP32 TX) | RX seriale (dal ESP32 TX) |
| 5 — OT2 | **TX seriale** (verso ESP32 RX) | **uscita presenza** |

- Baud: **256000** per fw < 1.5.3, **115200** per fw ≥ 1.5.3.
- Conseguenza pratica: **prima di cablare OT1/OT2 in modo definitivo va letta la versione
  firmware** (sonda UART da PC, o prova per ipotesi e verifica dell'uscita presenza).

## Cablaggio su ESP32 D1 mini (pin ADR-006 invariati)

| Pin modulo | Segnale ESP32 | GPIO | Note |
| ---------- | ------------- | ---- | ---- |
| 3V3 | alimentazione 3,3 V | pin 3V3 board (LDO) | **MAI il boost a 5 V**: pin a 3,3 V |
| GND | GND comune | — | |
| RX | RADAR_TX | GPIO27 | ingresso seriale del modulo |
| OT2 (fw <1.5.3) / OT1 (fw ≥1.5.3) | RADAR_RX | GPIO26 | dati seriali dal modulo |
| OT1 (fw <1.5.3) / OT2 (fw ≥1.5.3) | RADAR_OUT | GPIO25 | uscita presenza (polarità da verificare) |

- Filtro alimentazione: ferrite o LC + 100 nF ceramico vicino al modulo (100 µF elettrolitico
  consigliato se alimentato dal LDO di board).
- Il carico passa dal boost al **LDO 3,3 V**, che alimenta anche l'ESP32: il consumo del radar
  su 3,3 V è **da misurare** ([domande aperte](../domande-aperte.md)); il bilancio va ricalcolato
  ([bilancio](../misure/bilancio-energetico-stima.md)).

## Protocollo e differenze dal LD2410B

- Il LD2420 è un sensore di **presenza/movimento** con **16 cancelli di energia** (soglie
  trigger "move" e keep "still" per cancello, u32 0..65535) e riporta una **distanza** (parola
  "range" del frame, cm sul wire): non c'è la separazione movimento/fermo con distanze
  dedicate del LD2410B e **nessun comando di risoluzione cancello**.
- L'inviluppo dei frame comando (FD FC FB FA … 04 03 02 01) è lo stesso; **comandi e frame
  dati sono diversi**: 0x00FF/0x00FE config, 0x0000 versione, 0x0068 restart, 0x0008/0x0007
  parametri, 0x0012 modalità seriale.
- **Firmware adattato (2026-10-25)**: il firmware implementa ora il protocollo LD2420
  (componente [radar_ld2420](../componenti/radar_ld2420.md)); la vecchia nota "il firmware
  parla LD2410B e non dialoga" non è più vera. I criteri di Fase 2 con distanze 1/2/3 m vanno
  ridimensionati sul modulo: la distanza esiste (range), ma la mappa cancello→distanza è
  da verificare ([domande aperte](../domande-aperte.md)).
- **Nota fw ≤ 1.5.3 ora gestita dal firmware**: le versioni vecchie non trasmettono frame
  energia (solo righe testo running mode) e usano un baud diverso: il firmware le gestisce
  con **probe baud automatico 115200/256000** e **parsing delle righe testo**, scegliendo la
  modalità seriale dopo la lettura della versione.

## Stato

- **Verificato a banco (2026-10-25)**: modulo alimentato e dialogante — firmware **v1.5.9**,
  baud **115200** (fw ≥1.5.3 → seriale su **OT1** → GPIO26, presenza su **OT2** → GPIO25,
  RX modulo → GPIO27); protocollo funzionante in modalità energia (migliaia di frame ok,
  0 errati); calibrazione applicata e rilettura identica; boot senza riscritture
  ([componente](../componenti/radar_ld2420.md), [log](../log.md)).
- **Codice verificato** (2026-10-25): 50/50 test host, build esp32 zero warning, banco senza
  alimentazione del sensore (guasto/degrado/backoff) — [componente](../componenti/radar_ld2420.md).
- **Ancora da verificare**: polarità dell'uscita presenza (OT2), mappa cancello→distanza,
  trigger reale con avvicinamento, 30 min falsi positivi, consumo 3,3 V
  ([domande aperte](../domande-aperte.md)).
- Cablaggio, comandi e calibrazione: [procedura radar](../../procedura_radar.md), ora
  interamente sul protocollo LD2420 del firmware.
