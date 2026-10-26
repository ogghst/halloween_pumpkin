---
titolo: LD2420: buffer comandi 32 byte, scritture a blocchi
tipo: insidia
stato: verificato
aggiornato: 2026-10-25
fonti: [banco 2026-10-25 (ESP32 D1 mini + HLK-LD2420 fw v1.5.9): `radar calibrate apply` con 16 pair -> ESP_ERR_TIMEOUT, sensibilità singolo cancello OK; ENTER_CONFIG ACK buffer 0x0020; components/radar_ld2420/radar.c write_params chunked]
correlate: [../hardware/hlk-ld2420.md, ../componenti/radar_ld2420.md, ../hardware/hlk-ld2410b.md]
---

# LD2420: buffer comandi 32 byte — scritture oltre 5 pair tacciono

## Il sintomo

`radar calibrate apply` (2 WRITE_PARAMS da 16 pair, value 98 B) fallisce con
`ESP_ERR_TIMEOUT`: il modulo **non risponde mai** (nessun ACK, nessun NACK).
La scrittura di un singolo cancello (2 pair, value 14 B) invece funziona al
primo colpo, e le LETTURE da 16 nomi (value 34 B) passano regolarmente.

## La causa

L'ACK di ENTER_CONFIG del LD2420 dichiara **buffer 0x0020 = 32 byte** di dati
intra-frame. Un WRITE_PARAMS da N pair è `2 + 6·N` byte di value: 16 pair =
98 B, molto oltre il buffer dichiarato → il modulo **scarta in silenzio** il
comando oversize. Il documento Hi-Link mostra un esempio a 6 pair (38 B) che
supera già il buffer dichiarato: l'esempio del documento non è affidabile su
questo punto; il banco è autorevole (stato `verificato`).

## La regola nel codice

`write_params()` in `radar.c` spezza **ogni** scrittura in transazioni da
**al massimo 5 pair** (`2 + 30 = 32` byte = esattamente il buffer):
`radar calibrate apply`, `radar factoryreset`, la boot-sync delle soglie e
`radar sensitivity all` passano tutti da lì. Le letture multi-nome restano
interne (verificate funzionanti fino a 16 nomi a banco).

## Da ricordare

- Timeout su un WRITE_PARAMS senza risposta = prima ipotesi: comando oltre il
  buffer, non modulo morto (il modulo continua a streammare i frame energia).
- Le letture sembrano non risentire del limite: non fare supposizioni
  simmetriche lettura/scrittura.
