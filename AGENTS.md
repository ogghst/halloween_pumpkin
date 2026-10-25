# AGENTS.md

Istruzioni operative per agenti che lavorano su `halloween_pumpkin`: firmware ESP-IDF per una zucca di Halloween con ESP32-S3 DevKitC-1, matrice LED 8×8 (64 WS2812B in un'unica catena DIN a serpentina), sensore ToF VL53L1X e integrazione MQTT/Home Assistant.

## Ambiente e build

Setup, build, flash e test sono documentati in [README.md](README.md). Punti chiave:

- ESP-IDF v5.4.4 in `~/esp/esp-idf`; in ogni nuova shell: `source ~/esp/esp-idf/export.sh`.
- Target `esp32s3`, flash 8 MB, partizioni OTA dual-slot (`partitions.csv`), `CONFIG_FREERTOS_HZ=1000`.
- Regola utente: una fase alla volta; a fine fase, commit + report e attendere l'OK prima della fase successiva.

## Wiki di progetto

La conoscenza operativa vive in `docs/wiki/` (indice: `docs/wiki/index.md`). Il codice dice *cosa* fa il sistema; la wiki dice *perché*, cosa è stato provato, cosa è risultato vero e cosa non fare. Non duplica codice né commenti: li collega.

### Quando leggere

- A inizio di ogni compito: leggi `docs/wiki/index.md`, poi solo le pagine pertinenti al compito (componente toccato, insidie, decisioni correlate).

### Quando scrivere

Nello stesso commit del codice o in un commit `wiki:` subito dopo:

- Decisione di progetto presa o cambiata → ADR in `docs/wiki/decisioni/`.
- Comportamento non ovvio, bug difficile o limitazione hardware → pagina in `docs/wiki/insidie/`.
- Misura eseguita → pagina in `docs/wiki/misure/` con condizioni, strumento, data e valori.
- Assunzione confermata o smentita → aggiorna lo `stato` nel frontmatter della pagina e `docs/wiki/domande-aperte.md`.
- Fase completata → aggiorna le pagine dei componenti toccati e aggiungi una voce a `docs/wiki/log.md`. L'aggiornamento della wiki fa parte della definizione di "fatto" di ogni fase.

### Come scrivere

- Aggiorna la pagina esistente invece di crearne una duplicata: cerca prima nell'indice.
- Ogni modifica aggiorna: `aggiornato` nel frontmatter, `docs/wiki/index.md` e una riga in `docs/wiki/log.md`.
- Cita la provenienza (commit, file, misura) nel frontmatter (`fonti`). Niente affermazioni `stato: verificato` senza fonte: se non è verificato, marca `assunto` o `ipotesi`.
- Contraddizione tra pagine o tra wiki e codice → non scegliere in silenzio: segnalala, risolvila con la fonte più affidabile e annota la correzione in `docs/wiki/log.md`. Le pagine superate passano a `stato: obsoleto` con link alla pagina che le sostituisce, senza cancellarle.

### Cosa non scrivere

- Segreti, credenziali, token, password Wi-Fi o MQTT.
- Descrizioni riga per riga del codice o copie di Kconfig ed header.
- Speculazioni presentate come fatti.

### Controllo

- Prima di ogni commit che tocca `docs/wiki/`: `python3 tools/wiki_lint.py` (deve terminare senza errori).
- Hook git `pre-commit` che lancia il lint: opzionale, installare solo su richiesta esplicita.
