# pumpkin_desktop — pannello desktop per la zucca di Halloween

Applicazione desktop **portatile** in Python con GUI Tkinter (solo stdlib) per gestire
la scheda della zucca: configurazione, build ESP-IDF, flash USB, monitor seriale e OTA.

## Requisiti

- Python 3.8+ con `tkinter` (su Debian/Ubuntu: `sudo apt install python3-tk`).
  Se `python3 -c 'import tkinter'` fallisce, l'app non si avvia, ma il selftest
  headless continua a funzionare (il modulo GUI viene importato in modo lazy):
  `python3 pumpkin_desktop.py --selftest`.
- `openssl` nella PATH: serve solo alla prima generazione del certificato HTTPS
  del server OTA (`cert.pem` + `key.pem`).
- Dipendenze **opzionali** (vedi `requisiti.txt`):
  - `pyserial` → abilita la scheda *Monitor* e l'invio della CA via seriale;
  - `paho-mqtt` → abilita pubblicazione MQTT e viewer eventi nella scheda *OTA*.
  Se mancano, le funzioni corrispondenti sono disabilitate con messaggio chiaro:
  nessun traceback, il resto dell'app funziona.

```bash
pip3 install pyserial paho-mqtt   # oppure: pip3 install -r requisiti.txt
```

## Avvio

```bash
cd tools/desktop
python3 pumpkin_desktop.py
```

L'app non scrive nulla fuori dalla propria cartella: `config.json`, `monitor.log`,
`cert.pem` e `key.pem` nascono accanto allo script.

## Funzioni (una scheda ciascuna)

### Impostazioni
- Percorso del repo (default: directory padre di `tools/desktop`) e di ESP-IDF
  (default `~/esp/esp-idf`), porta USB con autodetect `/dev/ttyUSB*` e `/dev/ttyACM*`
  (bottone *Aggiorna*), baud console (115200) e flash (921600).
- Broker MQTT (host/porta, default `127.0.0.1:1883`). Utente e password opzionali:
  restano **solo in memoria**, non vengono salvati su disco.
- Device id della zucca (es. `a1b2c3`); se vuoto l'app lo chiede all'atto OTA.
- Porta server HTTPS locale (8443) e soglia di margine per lo slot OTA (15%).
- *Salva* scrive `config.json` accanto allo script.

### Build
- *Compila* esegue `source <idf>/export.sh && idf.py -B <repo>/build build` nella
  repo root, con output in streaming sul log (nessun freeze della GUI).
  Al termine legge `idf.py size` e mostra: dimensione binario, % dello slot ota_0
  usata e margine % (slot assunto = 1984 KiB), con avviso se sotto soglia.
- *Pulisci build* esegue `idf.py -B <repo>/build fullclean`.

### Flash
- *Flash*: `idf.py -B <repo>/build -p <porta> -b <baud flash> flash`.
- *Cancella flash*: `erase-flash`, con conferma (cancella anche la NVS:
  provisioning e configurazione compresi).

### Monitor
- Terminale seriale 115200 8N1: lettura in thread, invio righe dalla casella
  *Comando* (es. `ha status`, `ca status`, `ota <url>`), opzione di log su
  `monitor.log`. Se pyserial manca, la scheda resta disabilitata con messaggio.

### OTA
1. **Server HTTPS locale**: *Avvia server* genera (una volta sola) il certificato
   self-signed con `openssl` e serve `<repo>/build/pumpkin.bin` su
   `https://<ip-LAN>:<porta>/pumpkin.bin`. L'IP è ricavato dal routing locale
   (fallback `127.0.0.1`). *Ferma server* chiude tutto.
2. **Invia CA al dispositivo (seriale)**: avvia il monitor se serve, invia
   `ca begin`, le righe di `cert.pem` e attende il commit automatico del firmware
   (alla riga `END CERTIFICATE`); poi invia `ca status` e mostra la risposta.
   È così che il dispositivo si fida del server OTA.
3. **Pubblica disponibile**: publish retained della versione (stringa semplice) su
   `pumpkin/<id>/update/latest`.
4. **Lancia OTA**: publish di `{"url","version","release_summary","title"}` su
   `pumpkin/<id>/set/update`; l'URL deve essere **https** (viene validato).
5. **Viewer eventi**: iscrizione a `update/status`, `status/reset`,
   `status/stability`, `status/tasks` e `availability`; tabella con l'ultimo
   messaggio ricevuto per topic.

## Sequenza OTA consigliata

1. *Impostazioni*: verifica porta, broker, device id → *Salva*.
2. *Build*: *Compila*.
3. *OTA*: *Avvia server*.
4. *OTA*: *Invia CA al dispositivo (seriale)* (una volta per certificato).
5. *OTA*: compila versione + note → *Pubblica disponibile* → *Lancia OTA*.

## Verifica headless (senza GUI né scheda)

```bash
python3 -m py_compile pumpkin_desktop.py
python3 pumpkin_desktop.py --selftest
```

Il selftest copre: roundtrip di config.json, payload `set/update` con validazione
https-only, costruzione comandi build/flash/erase/clean, parser dell'output di
`idf.py size` (su un esempio incastonato nel test).

## Limiti e note

- Operazioni sul dispositivo (flash, seriale, MQTT, OTA vere) richiedono la scheda
  collegata e un broker attivo: il selftest non le copre.
- Lo slot OTA è assunto da **1984 KiB** per il calcolo di margine.
- Il certificato è self-signed: il dispositivo lo accetta solo dopo l'invio della
  CA via seriale (punto 2 della sequenza).
- Le credenziali MQTT, se compilate, vengono usate per la connessione ma **mai
  salvate** in `config.json`.
- Il browser/hardware non è coinvolto: tutta la verifica locale è comandabile da
  riga di comando come sopra.
