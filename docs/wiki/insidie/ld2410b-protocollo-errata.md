---
titolo: Errata protocollo HLK-LD2410B v1.07
tipo: insidia
stato: verificato
aggiornato: 2026-10-25
fonti: [Hi-Link "HLK-LD2410B — Serial communication protocol" v1.07 (2024-08-05), §2.2.5-2.2.6, §2.2.16-2.2.17, Table 7/8, §2.3.2 Table 13; components/radar_ld2420/include/radar_protocol.h]
correlate: [../hardware/hlk-ld2410b.md, ../componenti/radar_ld2420.md, ../domande-aperte.md]
---

# Errata del documento HLK-LD2410B v1.07

> **Interesse storico/di confronto (2026-10-25)**: queste errate riguardano il documento
> **LD2410B v1.07**, che il firmware **non implementa più** (dal 2026-10-25 usa il protocollo
> LD2420: [radar_ld2420](../componenti/radar_ld2420.md)). Restano utili come esempio di
> verifica contro gli **esempi byte** di un documento di protocollo e per un eventuale
> confronto con moduli LD2410 futuri.

Il documento ufficiale v1.07 contiene contraddizioni interne, verificate leggendo il testo
(stato `verificato` = il difetto è nel documento, non nel nostro banco). Le scelte qui sotto
documentavano il layer LD2410B del firmware, oggi rimosso e sostituito dal protocollo LD2420.

## a) Chiusura engineering: testo 0x0062, esempi 0x0063

- §2.2.6 ("Close engineering mode") dichiara nel testo **command word: 0x0062** — lo stesso
  comando di **apertura** §2.2.5 — ma i propri **esempi byte** usano `63 00` e l'ACK `63 01`.
- **Seguono i byte**: apertura `0x0062`, chiusura `0x0063`
  (`RADAR_CMD_ENG_ON`/`RADAR_CMD_ENG_OFF`). Conseguenza: mai copiare il comando dal testo del
  §2.2.6; un modulo reale che rispondesse come da testo è uno scenario da [domande
  aperte](../domande-aperte.md) (versione fw da leggere al primo collegamento).

## b) Default di risoluzione: testo §2.2.16 contro Table 7/8

- Table 8 definisce l'indice: **0x0000 = 0,75 m, 0x0001 = 0,2 m**; Table 7 dà come default di
  fabbrica la risoluzione **0,75 m**.
- Ma il testo §2.2.16 chiude con "The factory default value is **0x0001, or 0.75m**": il valore
  e l'etichetta sono in contraddizione tra loro (0x0001 sarebbe 0,2 m) e con le tabelle.
- Conferma indiretta: l'esempio di §2.2.17 (query risoluzione) legge ACK `…01 00` e lo commenta
  "currently set range resolution is **0.2m**", coerente con Table 8.
- **Table 8 è autorevole** (`RADAR_RESOLUTION_075`=0x0000, `RADAR_RESOLUTION_02`=0x0001); il
  default di fabbrica assunto è **0,75 m** (come Table 7), da confermare con `radar resolution`.

## c) Le distanze nei frame sono in cm, non mm

- Table 13/15 dichiarano esplicitamente "distance (**cm**)"; trattarle come mm gonfierebbe le
  distanze di 10× e romperebbe la soglia di trigger. Il parser converte in mm
  (`radar_data_frame_t.*_distance_mm`); tutto il resto del componente ragiona in mm.

## d) Massimo 8 cancelli sulla variante B

- La nota di pianificazione "tipicamente 9 cancelli" si riferiva ad altre varianti; il
  documento v1.07 per la B fissa **massimo 8** cancelli (§1.2.2, §2.2.16) e comanda limiti
  0x0060 nell'intervallo **2..8**. Il codice: `RADAR_MAX_GATES = 8`, vettori per-gate di 9
  elementi (0..8), range 2..8 nel Kconfig.

## Conseguenze pratiche

- Qualunque nuovo comando scritto a mano va verificato contro gli **esempi byte** del
  documento, mai contro la sola prosa (casi a e b).
- Le distanze mostrate in console sono mm ma **quantizzate al cancello**: un valore "strano"
  (es. sempre multiplo di 750) è la risoluzione attiva, non un bug.
- Se un modulo si comporta secondo la prosa errata (es. default 0x0001 = 0,75 m), sospettare
  prima la versione fw del modulo: registrarla in [hardware](../hardware/hlk-ld2410b.md).
