---
titolo: Componente net_mgr
tipo: componente
stato: assunto
aggiornato: 2026-10-26
fonti: [fase4, components/net_mgr/include/net_mgr.h, components/net_mgr/net_mgr.c, components/net_mgr/net_backoff.c, components/net_mgr/net_backoff.h, components/net_mgr/Kconfig, components/net_mgr/host_tests (8 casi net_backoff, verdi 2026-10-26), components/net_mgr/idf_component.yml]
correlate: [wifi-prov.md, ../decisioni/adr-008-rete-provisioning.md, ../procedure/provisioning.md, ../insidie/light-sleep-wifi-rmt.md, ../domande-aperte.md]
---

# Componente `net_mgr`

Gestore del ciclo di vita della stazione Wi-Fi (`components/net_mgr/`),
consegnato con la Fase 4. **Stato**: logica pura del backoff verificata su
host (8 test verdi); l'integrazione esp32 (build, riconnessioni reali, mDNS)
non è ancora stata esercitata — vedi [domande aperte](../domande-aperte.md).

## Comportamento

- `net_mgr_start()`: idempotente; fa il setup one-time (`esp_netif_init`, loop
  default, netif STA default, `esp_wifi_init`) solo se assenti e **adotta**
  quelli già creati dall'app (contratto `wifi_prov`: al boot possiede il chip,
  vedi [ADR-008](../decisioni/adr-008-rete-provisioning.md)). Imposta
  l'hostname, scrive il `listen_interval` Kconfig nel `wifi_config_t`
  (round-trip: SSID/password
  restano quelle in NVS, mai lette/loggate), avvia e connette.
- **Eventi** su `NET_EVENT` (loop default): `NET_EVENT_UP {ip[16]}` al got-IP,
  `NET_EVENT_DOWN {reason, next_ms}` a ogni disconnessione con il ritardo del
  backoff già schedulato.
- **Backoff** (puro in `net_backoff.c`, host-testato): parte da 1000 ms,
  raddoppia a ogni tentativo, jitter deterministico **±15%** (rand_pct 0..99) e
  cap `CONFIG_PUMPKIN_NET_BACKOFF_MAX_MS` (default 60000 ms → 1→60 s).
- **Diagnostica**: timer periodico (`CONFIG_PUMPKIN_NET_DIAG_PERIOD_S`, default
  30 s) campiona rssi/canale, logga e aggiorna i TXT mDNS; `net_mgr_get_diag`
  serve una copia `{connected, rssi, channel, reconnects, ps_min_modem,
  listen_interval}` per console/HA.
- **mDNS** (componente gestito `espressif/mdns` ^1): al primo UP registra
  hostname+istanza e il servizio `_pumpkin._tcp:80` con TXT diagnostici
  `{fw, rssi, ch, reconnects}` (`fw` = fase firmware, oggi "4").
- **Hostname**: `<prefisso>-xxxxxx` (`CONFIG_PUMPKIN_NET_HOSTNAME_PREFIX`,
  default `pumpkin` + ultimi 3 byte del MAC STA in esadecimale minuscolo), both
  sul netif (DHCP) e su mDNS.
- **Power save**: `WIFI_PS_MIN_MODEM` allo start se
  `CONFIG_PUMPKIN_NET_PS_MIN_MODEM` (default y); `net_mgr_set_power_save`
  commuta MIN_MODEM/NONE a runtime per le prove di latenza a banco. Il
  `listen_interval` (beacon, 1..10, default 3) si scrive prima dello start e
  vale alla prossima associazione.
- `net_mgr_stop()`: cancella i timer, `mdns_free`, ferma la stazione; netif e
  `esp_wifi_init` restano attivi per un ri-start immediato (usato dal
  provisioning prima di ripartire).

## Integrazione nell'app `test_apps/net_prov`

- Start dopo il provisioning (su `PROV_EVENT_SUCCESS/TIMEOUT/FAILED`) o subito
  al boot con credenziali presenti; senza credenziali resta offline e il backoff
  riprova: nessun SSID/password nel codice dell'app.
- Coesistenza offline con radar (UART1 GPIO26/27 + OUT 25), LED (RMT GPIO5/4) e
  BLE NimBLE del provisioning: da verificare a banco
  ([domande aperte](../domande-aperte.md)).

Tag log del componente: `net` (eventi e diagnostica).
