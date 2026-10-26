---
titolo: ADR-008 — Rete e provisioning della Fase 4
tipo: decisione
stato: assunto
aggiornato: 2026-10-26
fonti: [fase4, components/net_mgr/include/net_mgr.h, components/wifi_prov/include/wifi_prov.h, components/wifi_prov/include/prov_mqtt_codec.h, components/led_effects/include/led_fx.h, components/config_store/include/config_store.h, brief Fase 4 2026-10-26]
correlate: [../componenti/net-mgr.md, ../componenti/wifi-prov.md, ../componenti/config_store.md, ../componenti/led_effects.md, adr-007-fsm-config-store.md, ../procedure/provisioning.md]
---

# ADR-008 — Rete e provisioning della Fase 4

## Contesto

La Fase 4 porta Wi-Fi, provisioning BLE e parametri MQTT (Fase 5) su ESP32 D1
mini. Vincoli: nessuna credenziale nei sorgenti né nei log; slot app 1536 KB
(con BLE NimBLE incluso); la FSM e il radar esistono già e devono coesistere;
il broker MQTT (Fase 5) ha bisogno di host/port/user/pass/tls configurabili
senza ricompilare.

## Scelta

- **Due componenti separati**: `wifi_prov` (provisioning manager + endpoint
  MQTT + pulsante di reset) e `net_mgr` (connettere e restare connessi: eventi
  UP/DOWN, backoff 1→60 s ±15%, diagnostica, mDNS). Nessuno dei due logga
  SSID/password.
- **BLE NimBLE default, SoftAP via Kconfig**; classic BT spento
  (`CONFIG_BT_CLASSIC_ENABLED=n`, `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`) per
  stare nello slot da 1536 KB.
- **Security 1 con PoP derivata dal MAC** (`pump-XXXXXX`), service name
  `PUMPKIN-XXXXXX`: zero segreti nel firmware. **Assunzione di minaccia
  dichiarata**: il MAC è pubblico, quindi la PoP è debole contro un attaccante
  locale motivato — accettata per il contesto (zucca in giardino).
- **Parametri MQTT via endpoint custom TLV `mqtt`**: campi opzionali (merge sui
  valori correnti), codec puro host-testato, destinazione unica `config_store`
  v2 (`config_mqtt_t`, blob 209 B) — resta valevole la fonte unica di
  [ADR-007](adr-007-fsm-config-store.md).
- **Init del chip all'app**: l'app di banco crea una volta sola netif STA
  default + `esp_wifi_init` (contratto `wifi_prov`); `net_mgr_start()` li
  adotta. Il provisioning serve come reset: `wifi_prov_begin()` chiama
  `net_mgr_stop()` + `esp_wifi_restore()` (Wi-Fi azzerato, MQTT conservato).
- **Feedback via overlay LED, non via FSM**: lo stato rete/provisioning compare
  come overlay `led_fx_overlay_set` fuso con max() sopra l'effetto corrente
  (dopo gamma/luminosità, cap 48/255 per canale). La FSM non sa nulla della
  rete: separazione netta fra presentazione (overlay) e comportamento (stati).
- **Rilascio BTDM permanente per sessione**: dopo il provisioning BLE la
  memoria BT torna allo heap e **non torna più** finché non si riavvia; un
  re-provisioning BLE richiede il reboot (`wifi_prov_begin` logga e riavvia).
  Comportamento voluto: il reset via pulsante GPIO32 (RTC, 5 s, attivo basso)
  riparte dal boot pulito.
- **Timeout provisioning 10 min** → ritorno a idle; l'app riparte comunque con
  `net_mgr_start()` (offline senza credenziali, backoff in atterraggio).

## Alternative scartate

- **SoftAP di default**: obbliga a cambiare rete Wi-Fi sul telefono e non
  rileva automaticamente il nuovo dispositivo; resta disponibile via Kconfig
  come ripiego quando il BLE dà problemi al banco.
- **QR code provisioning** (scheme standard con payload CSV): richiede di
  fabbricare/stampare il payload con chiave inclusa; la PoP dal MAC basta per
  l'uso reale e non aggiunge segreti da gestire.
- **Parametri MQTT in NVS separata del provisioning** (chiavi dedicate): due
  fonti di verità con la Fase 5 che dovrebbe scrivere in due posti; vince il
  blocco `config_mqtt_t` in `config_store` (migrate v1→v2 già pronto).
- **FSM informata dello stato di rete** (nuovi eventi/stati): accoppierebbe
  comportamento e connettività per un beneficio solo estetico; gli overlay
  coprono il caso senza toccare la macchina a stati.
- **`wifi_prov` owner di netif/esp_wifi**: la sequenza di boot del manager
  (prima il provisioning, poi la rete) richiederebbe init/deinit incrociati
  col gestore di rete; più semplice l'init one-time all'app e l'adozione da
  parte di `net_mgr`.

## Conseguenze

- La Fase 5 (`ha_mqtt`) legge il broker **solo** da `config_store_get_mqtt` e
  riceve gli aggiornamenti dall'evento `CFG_PARAM_MQTT`.
- La coesistenza BLE + radar UART + RMT e i tempi reali (riconnessioni, heap
  BTDM, glitch) restano **da verificare a banco**:
  [checklist](../procedure/provisioning.md) e
  [domande aperte](../domande-aperte.md).
