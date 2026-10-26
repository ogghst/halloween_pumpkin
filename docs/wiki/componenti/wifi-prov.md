---
titolo: Componente wifi_prov
tipo: componente
stato: assunto
aggiornato: 2026-10-26
fonti: [fase4, components/wifi_prov/include/wifi_prov.h, components/wifi_prov/include/prov_mqtt_codec.h, components/wifi_prov/prov_mqtt_codec.c, components/wifi_prov/prov_pop.c, contratto slice D 2026-10-26]
correlate: [net-mgr.md, ../decisioni/adr-008-rete-provisioning.md, ../procedure/provisioning.md, config_store.md, ../domande-aperte.md]
---

# Componente `wifi_prov`

Provisioning Wi-Fi e parametri MQTT (`components/wifi_prov/`), consegnato con la
Fase 4: avvolge il provisioning manager ESP-IDF (`wifi_provisioning`) con le
convenzioni pumpkin. **Stato**: codec TLV puro host-testato; integrazione esp32
e provisioning reale da telefono **da eseguire a banco** (procedura in
[provisioning](../procedure/provisioning.md)).

## Identità e sicurezza

- **BLE NimBLE** di default, **SoftAP** in alternativa via Kconfig
  (`PUMPKIN_PROV_SCHEME_BLE` / `PUMPKIN_PROV_SCHEME_SOFTAP`).
- Security **1** con PoP derivata dal MAC: `pump-XXXXXX` (ultimi 3 byte del MAC
  STA in esadecimale MAIUSCOLO, `prov_pop_from_mac`). Service name
  `PUMPKIN-XXXXXX` (`PUMPKIN_PROV_SERVICE_PREFIX` + stessi 3 byte).
- **Assunzione di minaccia dichiarata**: il MAC è pubblico → la PoP è debole
  contro un attaccante locale motivato; è una comodità di pairing per una
  zucca in giardino, non una difesa —
  [ADR-008](../decisioni/adr-008-rete-provisioning.md).
- Timeout unico di **600 s** (`PUMPKIN_PROV_TIMEOUT_S`) → `PROV_EVENT_TIMEOUT`
  e ritorno a idle (l'app riprova comunque la rete).

## Endpoint custom `mqtt` (TLV)

Formato del payload (little-endian, nessun padding):

| byte                | contenuto                                        |
|---------------------|--------------------------------------------------|
| 0                   | versione (1; altro → `ESP_ERR_INVALID_ARG`)      |
| poi, a record       | type u8, len u16 LE, value[len]                  |

Type: 1 host, 2 port (u16 LE, len 2), 3 user, 4 pass, 5 tls (u8 0/1, len 1).
Type sconosciuto → scartato e si prosegue; record oltre buffer/troncato →
`ESP_ERR_INVALID_ARG`; stringhe sempre NUL-terminate (truncate to fit); campi
assenti → `has_*` false. `wifi_prov_handle_mqtt_tlv`: decode → merge sui
valori correnti → `config_store_set_mqtt` (evento `CFG_PARAM_MQTT` + commit
debounce); con `out`/`out_len` non NULL scrive un ack TLV coi valori correnti
completi (`*out_len` in ingresso = dimensione del buffer). Il codec puro vive in
`prov_mqtt_codec.c` ed è usabile da console per i test di banco (comando
`prov mqtt`).

## Ciclo di vita e rilascio BT

- `wifi_prov_app_init()`: idempotente; richiede dall'app NVS pronto, netif STA
  default e `esp_wifi_init` (il chip è dell'app, vedi
  [ADR-008](../decisioni/adr-008-rete-provisioning.md)); logga la baseline di
  heap su tag `prov`; sullo schema SoftAP libera subito la memoria BT; avvia il
  poll del pulsante.
- `wifi_prov_begin()`: prima di avviare chiama `net_mgr_stop()` +
  `esp_wifi_restore()` (credenziali Wi-Fi azzerate; i parametri **MQTT restano**
  in `config_store`); `ESP_ERR_INVALID_STATE` se attivo; su BLE con BT già
  rilasciato log + `esp_restart()` (al riavvio il boot rifà la scelta con le
  credenziali ora in NVS).
- Eventi `PROV_EVENT` sul loop default: `STARTED`, `SUCCESS {ssid[33]}`,
  `TIMEOUT`, `FAILED` (payload solo su SUCCESS).
- **Rilascio BTDM**: su BLE avviene allo stop del manager (memoria restituita al
  heap), su SoftAP subito all'init; **permanente per la sessione** → un
  re-provisioning BLE dopo il primo richiede il reboot (comportamento volato).
  Heap prima/dopo loggato; verifica a banco in
  [domande aperte](../domande-aperte.md).
- **Pulsante GPIO32** (RTC GPIO, riciclabile per il futuro deep sleep), pull-up
  attivo basso, pressione lunga 5 s (`PUMPKIN_PROV_BTN_GPIO`/`_HOLD_MS`) = reset
  credenziali Wi-Fi + ri-provisioning.

Tag log del componente: `prov`.
