---
titolo: IRAM colmo con BLE NimBLE + Wi-Fi sull'ESP32 classico
tipo: insidia
stato: verificato
aggiornato: 2026-10-26
fonti: [build 2026-10-26 test_apps/net_prov: link fallito con `iram0_0_seg
  overflowed by 3180 bytes`; fix in test_apps/net_prov/sdkconfig.defaults;
  idf.py size post-fix: IRAM 121974/131072 (93%), 9098 B liberi;
  idf.py size app principale Fase 7 (TLS+OTA): IRAM 125590/131072 (95,8%),
  5482 B liberi]
correlate: [../decisioni/adr-008-rete-provisioning.md, ../decisioni/adr-009-ota-rollback.md, ../componenti/wifi-prov.md, ../componenti/net-mgr.md, ../componenti/pumpkin-ota.md]
---

# IRAM colmo: BLE NimBLE + Wi-Fi + mDNS + TLS sull'ESP32 classico

## Il sintomo

La app di banco `test_apps/net_prov` (led + fsm + radar + Wi-Fi + provisioning
BLE + mDNS) compila con zero warning ma **fallisce al link**:

```
section `.iram0.text' will not fit in region `iram0_0_seg'
region `iram0_0_seg' overflowed by 3180 bytes
```

Non è un problema di flash (il binario sta nel suo slot): è **IRAM**, che
sull'ESP32 classico è fissa a 128 KB e non ha PSRAM a cui spostare nulla.

## La causa

- Il BLE (controller + NimBLE) e il Wi-Fi si portano ciascuno una quota
  importante di codice in IRAM: con `CONFIG_ESP_WIFI_IRAM_OPT=y` (default)
  i percorsi caldi Wi-Fi finiscono in IRAM per la throughput.
- Ruoli NimBLE inutilizzati (central, observer) aggiungono codice e IRAM.
- mDNS e le app del progetto completano il quadro: insieme si supera il
  segmento di ~3 KB.

## La regola nel codice

In `test_apps/net_prov/sdkconfig.defaults` e in `sdkconfig.defaults` (root):

```
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n      # provisioning = solo periferica
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n
CONFIG_ESP_WIFI_IRAM_OPT=n           # percorsi caldi Wi-Fi in flash
```

Verifiche dopo il fix:

- **`test_apps/net_prov` (Fase 4)**: link OK, IRAM al **93%** (9098 B liberi),
  binario 1,19 MB sullo slot da 1536K.
- **App principale (Fase 7, con TLS esp_mqtt + OTA a bordo)**: IRAM
  **125.590/131.072 = 95,8%, 5.482 B liberi**; `pumpkin.bin` 1.400.352 B su
  slot 1984K (31,1% libero), DRAM statica 51,4%.

Il costo di `ESP_WIFI_IRAM_OPT=n` è una throughput/latenza Wi-Fi leggermente
peggiore (cache miss sui percorsi caldi): irrilevante per la telemetria MQTT
della zucca.

## Da ricordare

- **IRAM resta il vincolo più stretto del progetto**: al 95,8% (5,4 KB liberi)
  con BLE+Wi-Fi+mDNS+TLS+OTA, ogni aggiunta sostanziale (protocolli, stack
  extra) va misurata subito con `idf.py size`, non scoperta al link.
- Il margine di Fase 7 (~5,4 KB) è assunto e non ancora provato sul campo
  (board non collegata): un'aggiunta che passa il link ma mangia la cache di
  IRAM in runtime non è esclusa — la corsa 24 h del
  [report di stabilità](../../../docs/report_stabilita.md) è anche questo test.
- Non disattivare `CONFIG_ESP_WIFI_IRAM_OPT` nell'app che NON ha BLE: lì
  l'IRAM avanza e il default è la scelta giusta.
