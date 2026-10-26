---
titolo: IRAM colmo con BLE NimBLE + Wi-Fi sull'ESP32 classico
tipo: insidia
stato: verificato
aggiornato: 2026-10-26
fonti: [build 2026-10-26 test_apps/net_prov: link fallito con `iram0_0_seg
  overflowed by 3180 bytes`; fix in test_apps/net_prov/sdkconfig.defaults;
  idf.py size post-fix: IRAM 121974/131072 (93%), 9098 B liberi]
correlate: [../decisioni/adr-008-rete-provisioning.md, ../componenti/wifi-prov.md, ../componenti/net-mgr.md]
---

# IRAM colmo: BLE NimBLE + Wi-Fi + mDNS sull'ESP32 classico

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

In `test_apps/net_prov/sdkconfig.defaults`:

```
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n      # provisioning = solo periferica
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n
CONFIG_ESP_WIFI_IRAM_OPT=n           # percorsi caldi Wi-Fi in flash
```

Verifica dopo il fix: **link OK, IRAM al 93% (9098 B liberi)**, binario
1,19 MB sullo slot da 1536K. Il costo di `ESP_WIFI_IRAM_OPT=n` è una
throughput/latenza Wi-Fi leggermente peggiore (cache miss sui percorsi caldi):
irrilevante per la telemetria MQTT della zucca.

## Da ricordare

- Quando questa pila approderà nell'app principale (Fase 5): portarsi dietro
  gli stessi knob, e ripartire da `idf.py size` se si aggiunge un componente.
- Con IRAM al 93% ogni aggiunta sostanziale (protocolli, stack extra) va
  misurata subito con `idf.py size`, non scoperta al link.
- Non disattivare `CONFIG_ESP_WIFI_IRAM_OPT` nell'app che NON ha BLE: lì
  l'IRAM avanza e il default è la scelta giusta.
