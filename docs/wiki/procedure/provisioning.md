---
titolo: Provisioning Wi-Fi e parametri MQTT
tipo: procedura
stato: assunto
aggiornato: 2026-10-26
fonti: [fase4, components/wifi_prov/include/wifi_prov.h, components/wifi_prov/include/prov_mqtt_codec.h, app Espressif "ESP BLE Provisioning" (Play Store/App Store), esp_prov in ESP-IDF v5.4.4]
correlate: [../componenti/wifi-prov.md, ../componenti/net-mgr.md, ../decisioni/adr-008-rete-provisioning.md, flash-monitor.md, ../domande-aperte.md]
---

# Provisioning Wi-Fi e parametri MQTT

Procedura di banco per la Fase 4, con l'app `test_apps/net_prov` flashata sulla
board. **Non ancora eseguita**: ogni passaggio va spuntato al primo banco.

## Provisioning da telefono (BLE, app Espressif)

1. Flash + monitor: `idf.py -C test_apps/net_prov build flash monitor`
   ([procedura](flash-monitor.md)). Al primo boot senza credenziali il log
   `prov` mostra transport, service name, PoP e heap baseline; overlay LED =
   pulsazione blu lenta.
2. Apri **ESP BLE Provisioning** → *Provision New Device* → *No QR code*.
3. Seleziona il dispositivo **`PUMPKIN-XXXXXX`** (gli ultimi 6 caratteri = 3
   byte finali del MAC, uguali per hostname e mDNS: `pumpkin-xxxxxx` minuscolo).
4. Security: **Security 1**, PoP **`pump-XXXXXX`** (leggila dal log: stessi 3
   byte del service name, maiuscolo).
5. Scegli la rete Wi-Fi e inserisci la password (nessuna credenziale nel
   firmware o nella wiki). Al successo: evento `PROV_EVENT_SUCCESS` con SSID,
   overlay verde (3 lampi), `net_mgr_start()` e connessione.
6. I parametri MQTT (broker/user/pass/tls) si passano **solo via TLV**
   dall'endpoint `mqtt`, oppure a banco col comando `prov mqtt` (vedi sotto):
   la password MQTT finisce in NVS via `config_store`, mai nel codice.

## Alternative da console/host

- **`esp_prov`** (in `$IDF_PATH/tools/esp_prov`): transport **ble** — richiede
  il pacchetto Python `bleak` sull'host — oppure transport **softap** dopo
  `idf.py menuconfig` con `PUMPKIN_PROV_SCHEME_SOFTAP` (l'ESP32 espone un AP
  `PUMPKIN-XXXXXX`).
- **Console di banco** (senza telefono): `prov mqtt host=X port=N user=U
  pass=P tls=0|1` costruisce il TLV, lo passa a `wifi_prov_handle_mqtt_tlv`
  (stesso percorso dell'endpoint) e stampa l'esito + i valori merged (password
  mascherata); `cfg_get mqtt` rilegge dallo store.

## Formato TLV dell'endpoint `mqtt`

| campo | type | len   | value                   |
|-------|------|-------|-------------------------|
| host  | 1    | 0..63 | stringa NUL-terminata   |
| port  | 2    | 2     | u16 LE                  |
| user  | 3    | 0..31 | stringa NUL-terminata   |
| pass  | 4    | 0..63 | stringa NUL-terminata   |
| tls   | 5    | 1     | u8, 0 oppure 1          |

Primo byte del payload = versione (1). Record con type sconosciuto scartati e
si prosegue; stringhe sempre NUL-terminate. Dettagli nella pagina del
[componente](../componenti/wifi-prov.md).

## Checklist di banco (Fase 4)

- [ ] **Riconnessioni**: togli e riaccendi il router 5 volte; ogni volta la
      riconnessione entro 2 min, backoff visibile nei log `net`
      (1→60 s ±15%), evento `NET_EVENT_DOWN/UP` e overlay rosso/verde.
- [ ] **Reset credenziali**: pressione lunga 5 s su GPIO32 → ri-provisioning
      (overlay blu); parametri MQTT ancora presenti (`cfg_get mqtt`).
- [ ] **Latenza**: ping dall'host con `net ps min` vs `net ps max` e
      `net listen 1` vs `net listen 10`; annotare le differenze.
- [ ] **Corrente a riposo** con Wi-Fi connesso (ps min modem): confronto con il
      [bilancio](../misure/bilancio-energetico-stima.md).
- [ ] **Nessun glitch LED** durante BLE attivo + traffico di rete (effetti in
      ciclo mentre l'app provisioning è connessa).
- [ ] **mDNS visibile sulla LAN**: `avahi-browse -r _pumpkin._tcp` (o
      equivalente) mostra hostname, TXT `fw/rssi/ch/reconnects`.
