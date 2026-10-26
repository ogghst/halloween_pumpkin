---
titolo: Modulo radar HLK-LD2410B (protocollo storico)
tipo: hardware
stato: obsoleto
aggiornato: 2026-10-25
fonti: [Hi-Link "HLK-LD2410B — Human presence sensing module, Serial communication protocol" v1.07 (2024-08-05), estratto locale 2026-10-25; decisione utente variante B 2026-10-25; ADR-006 pivot D1 mini 2026-10-25 (pin 26/27/25; RADAR_EN −1); components/radar_ld2420/Kconfig (pin di progetto, storico)]
correlate: [hlk-ld2420.md, esp32-d1-mini.md, ../decisioni/adr-006-pivot-esp32-d1-mini.md, ../insidie/ld2410b-protocollo-errata.md, ../insidie/gpio-da-evitare.md, ../componenti/radar_ld2420.md, ../domande-aperte.md]
---

# Modulo radar HLK-LD2410B (protocollo storico)

> **Obsoleta (2026-10-25), due motivi**. 1) Il modulo fisico ha i pin **3V3, GND, OT1, RX, OT2**, che
> corrispondono a un **HLK-LD2420** — pagina attuale: [HLK-LD2420](hlk-ld2420.md). 2) Il firmware
> **non implementa più il protocollo LD2410B**: dal 2026-10-25 parla il protocollo LD2420
> ([radar_ld2420](../componenti/radar_ld2420.md)). Questa pagina resta come **riferimento storico
> del protocollo v1.07** e delle sue errate ([errata](../insidie/ld2410b-protocollo-errata.md));
> le sezioni sotto descrivono il previsto da protocollo, non il firmware attuale.

Radar a microonde 24 GHz per rilevamento presenza e distanza: era stato scelto come **sensore della
Fase 2** al posto del ToF VL53L1X, prima che l'identificazione dei pin rivelasse il modulo come
LD2420. **Variante B**, scelta dell'utente 2026-10-25. Un modulo LD2410B è **fisicamente assente
dal banco**: questa pagina descrive il previsto da protocollo v1.07, tutto da confermare a banco.

## Pinout del modulo (§1.1, Table 1 — assunto)

| Pin | Nome | Funzione |
| --- | ---- | -------- |
| 1 | OUT | uscita stato bersaglio: **alto = presenza, basso = nessuno** (default di fabbrica) |
| 2 | UART_Tx | seriale del modulo (verso l'ESP32) |
| 3 | UART_Rx | seriale di comando (dall'ESP32) |
| 4 | GND | massa |
| 5 | VCC | alimentazione **5 V** |

- Alimentazione 5 V con sorgente **>200 mA** (§1.2.1); livelli di I/O a **3,3 V**.
- Baud di fabbrica **256000, 8N1** (§2, Table 7).
- Assorbimento atteso **~80 mA continui**: assunto da datasheet, **da misurare**.

## Pin di progetto

| Segnale | GPIO | Direzione | Kconfig |
| ------- | ---- | --------- | ------- |
| RADAR_RX | 26 | ESP32 riceve | `RADAR_RX_GPIO` (26) ← UART_Tx radar |
| RADAR_TX | 27 | ESP32 trasmette | `RADAR_TX_GPIO` (27) → UART_Rx radar |
| OUT | 25 | ESP32 riceve | `RADAR_OUT_GPIO` (25), `RADAR_OUT_ENABLE` = y |
| RADAR_EN | — | opzionale | `RADAR_EN_GPIO` (default −1 = non cablato, proposta MOSFET in discussione) |

Pin decisi con l'[ADR-006](../decisioni/adr-006-pivot-esp32-d1-mini.md) per l'ESP32 classico (D1 mini; le
prime bozze usavano 12/13/11, vietati o a rischio sul classico). I vecchi pin I2C del ToF (GPIO8/9) qui
**non sono liberi**: sul classico sono flash SPI (GPIO6-11), motivo in
[GPIO da evitare](../insidie/gpio-da-evitare.md). UART1 con remap esplicito via `uart_set_pin` (nessun
pin di default usato); console di sistema: UART0 115200 via CP2102 su `/dev/ttyUSB0`. Cablaggio
dettagliato: [procedura radar](../../procedura_radar.md). Il firmware attuale usa gli stessi pin per il LD2420.

## Assunzioni aperte (verificare al primo collegamento)

- **Versione firmware del modulo**: da leggere via UART (`radar version`, comando 0x00A0); da cui dipende la validità dei dettagli del protocollo.
- **Polarità reale di OUT**: default di fabbrica alto = presente (§1.1; configurazione
  ausiliaria di fabbrica 0x00, §2.2.18) — da verificare misurando il pin.
- **Consumo 5 V** reale: [domande aperte](../domande-aperte.md).

## Cosa persiste nel modulo (protocollo v1.07)

| Parametro | Comando | Persistente |
| --------- | ------- | ----------- |
| Sensibilità per cancello | 0x0064 | **Sì** (§2.2.7) |
| Cancelli massimi + durata no-one | 0x0060 | **Sì** (§2.2.3) |
| Baud rate | 0x00A1 | **Sì**, effettivo dopo riavvio (§2.2.9) |
| Risoluzione cancello (0,75 m / 0,2 m) | 0x00AA | **Sì**, effettiva dopo riavvio (§2.2.16) |
| Modalità engineering | 0x0062 / 0x0063 | **No**: si spegne allo spegnimento (§2.2.5) |

Default di fabbrica (Table 7): cancelli massimi 8/8, durata no-one 5 s, baud 256000, risoluzione 0,75 m,
sensibilità motion 50/50/40/30/20/15/15/15/15 (cancelli 0–8) e static 40/40/30/30/20/20/20/20/20
(cancelli 2–8; i cancelli 0–1 statici non sono configurabili).

Le contraddizioni interne del documento v1.07 (comando engineering, default risoluzione, unità delle
distanze, numero cancelli) sono catalogate in [errata protocollo](../insidie/ld2410b-protocollo-errata.md).

## Stato

- **Assunto**: tutto quanto citato dal protocollo v1.07; il modulo non è mai stato alimentato né interrogato. Prima accensione: [procedura radar](../../procedura_radar.md).
