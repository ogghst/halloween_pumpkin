---
titolo: Modulo radar HLK-LD2420
tipo: hardware
stato: assunto
aggiornato: 2026-10-25
fonti: [scritte a bordo riportate dall'utente 2026-10-25 (3V3, GND, OT1, RX, OT2); ESPHome "LD2420 24GHz mmWave Radar Sensor" (tabelle pinout per firmware, consultata 2026-10-25); ADR-006 pivot D1 mini 2026-10-25 (pin 26/27/25)]
correlate: [hlk-ld2410b.md, esp32-d1-mini.md, ../decisioni/adr-006-pivot-esp32-d1-mini.md, ../insidie/gpio-da-evitare.md, ../componenti/radar_ld2410.md, ../domande-aperte.md]
---

# Modulo radar HLK-LD2420

**Identificazione del modulo fisico (2026-10-25)**: i pin presenti sul modulo riportati
dall'utente — **3V3, GND, OT1, RX, OT2** — corrispondono al pinout dell'**HLK-LD2420**
(sensore di presenza/movimento 24 GHz Hi-Link), **non** al pinout dell'HLK-LD2410B
(VCC 5 V, OUT, UART_Tx, UART_Rx, GND). La pagina [HLK-LD2410B](hlk-ld2410b.md) resta il
riferimento del **protocollo implementato dal firmware**, ma l'identificazione hardware
è questa. Fonte pinout: [documentazione ESPHome LD2420]
(https://esphome.io/components/sensor/ld2420) (consultata 2026-10-25).

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

## Differenze sostanziali dal LD2410B (impatto su Fase 2)

- Il LD2420 è un sensore di **presenza/movimento**: **non fornisce la distanza continua del
  bersaglio** tipica del LD2410B (distanza movimento/fermo in cm). ESPHome espone uno stato
  `has_target` e valori legati ai **cancelli di energia** (con soglie per cancello, calibrazione
  del rumore di fondo, timeout presenza); una stima di distanza legata ai cancelli esiste ma è
  **da verificare sul protocollo**, non è equivalente alla distanza in cm del 2410.
- I **criteri di accettazione di Fase 2 basati sulla distanza** (1/2/3 m, cancello 0,75 m) non
  sono verificabili su questo modulo così come specificati: decisione aperta tra **adattare il
  firmware al protocollo LD2420** o **sostituire il modulo con un vero LD2410B**
  ([domande aperte](../domande-aperte.md)).
- L'inviluppo dei frame comando (FD FC FB FA … 04 03 02 01) è lo stesso; **comandi e frame
  dati sono diversi**: il firmware attuale (layer protocollo LD2410B v1.07) **non dialoga**
  con questo modulo finché non adattato.

## Stato

- **Assunto**: identificazione dai pin (forte, ma la conferma definitiva è leggere la versione
  firmware al primo collegamento); ruoli OT1/OT2 e baud dipendono dal firmware.
- Cablaggio dettagliato: [procedura radar](../../procedura_radar.md) — sezione cablaggio
  aggiornata a questo modulo; le sezioni protocollo/calibrazione si riferiscono al layer
  LD2410B del firmware.
