# Diario della wiki

Diario append-only: le nuove voci si aggiungono in coda, nel formato `## AAAA-MM-GG — argomento`, con bullet `fatto — stato`.

## 2026-10-25 — Seed della wiki

- Creata la struttura `hardware/`, `componenti/`, `decisioni/`, `misure/`, `procedure/`, `insidie/` con 19 pagine seed più i 4 file radice — fatto.
- Fatti hardware e firmware trascritti da README, docs/pinout.md e sorgenti della Fase 1, con stato `verificato` — fatto.
- Assunzioni e ipotesi (alimentazione, VL53L1X, ADR-003/004/005, bilancio energetico, light sleep) catalogate con stato onesto — fatto.
- Domande aperte minime registrate in [domande-aperte.md](domande-aperte.md) — fatto.

## 2026-10-25 — Conversione a matrice LED 8×8 (64 pixel)

- Hardware aggiornato da striscia 32× a **matrice 8×8 = 64× WS2812B** (un'unica catena DIN):
  pagina [hardware](hardware/led-ws2812b.md) riscritta, conteggio 64 nel Kconfig — fatto.
- Mappatura **serpentina** (origine alto-sinistra, `index = y*8 + (y pari ? x : 7-x)`) trascritta
  come assunto hardware; verifica a banco col comando `pixel` registrata tra le
  [domande aperte](domande-aperte.md) — fatto.
- [Componente led_effects](componenti/led_effects.md): `led_fx_xy_index` (in `fx_matrix.c`),
  pixel test (`led_fx_pixel_test`/`led_fx_pixel_test_off`) e stima corrente su 64 px
  (bianco pieno 3840 mA a scala 100, RGBW 5120 mA) — fatto.
- Valori attesi di corrente raddoppiati su pinout, misure, ADR-002/003 e alimentazione:
  piena luce ~3,84 A a cap 100%, ~2,3 A a cap 60% — fatto.
- Stima brief ~11,5 Wh segnata come anteriore alla conversione, da ricalcolare dopo misura — fatto.

## 2026-10-25 — Fase 2: radar presenza HLK-LD2410B

- Componente `radar_ld2410` su UART1 (parser FSM, codec comandi, logica trigger puri + integrazione):
  **build e 42 test host verdi**, zero warning — fatto.
- Protocollo Hi-Link v1.07 trascritto nel contratto: sessione ENTER_CONFIG/EXIT_CONFIG obbligatoria,
  limiti 0x0060 (cancelli 2..8, durata persistente), sensibilità 0x0064 (persistente), versione fw
  0x00A0, engineering 0x0062/0x0063 (non persistente), risoluzione 0x00AB (default 0,75 m) — fatto.
- Errata del documento v1.07 (chiusura engineering 0x0063, default risoluzione Table 8, distanze in
  cm, max 8 cancelli) catalogati in
  [insidie](insidie/ld2410b-protocollo-errata.md) e seguiti dal codice — fatto.
- Pagine nuove: [hardware HLK-LD2410B](hardware/hlk-ld2410b.md) (pin GPIO12/13/11, pinout, cosa
  persiste) e [componente radar_ld2410](componenti/radar_ld2420.md) — fatto.
- Procedura di installazione e calibrazione: [docs/procedura_radar.md](../procedura_radar.md)
  (posizionamento, cablaggio 5 V a monte di LED_EN, calibrazione stanza vuota, test 30 min con
  rainbow 100%, hot-plug UART) — fatto.
- Hardware **non collegato**: versione fw del modulo, polarità OUT, consumo ~80 mA assunto e
  immunità EMI boost→radar registrati in [domande aperte](domande-aperte.md); verifica a banco
  rimandata — fatto (assunto).
- [Bilancio energetico](misure/bilancio-energetico-stima.md) aggiornato con il radar come carico
  assunto del boost (~3,2 Wh per 8 h) e proposta MOSFET RADAR_EN — fatto (assunto).

## 2026-10-25 — Pivot hardware: da DevKitC-1 S3 a ESP32 D1 mini (ESP32 classico)

- Board fisicamente al banco e verificata con **esptool**: chip ESP32-D0WD-V3 rev v3.1,
  flash **4 MB**, bridge **CP2102** su **`/dev/ttyUSB0`** (console UART0 115200), MAC
  e4:65:b8:71:7e:50 — fatto. Nuova pagina [hardware](hardware/esp32-d1-mini.md); pagina S3
  segnata **obsoleta** (board mai ricevuta) — fatto.
- **[ADR-006](decisioni/adr-006-pivot-esp32-d1-mini.md)**: target `esp32` in
  sdkconfig.defaults e app di test; alternative scartate attendere/riacquistare una S3 — fatto.
- **Partizioni 4 MB** dual-OTA: nvs 16K, otadata 8K, phy_init 4K, `ota_0`/`ota_1` **1536K**,
  ~0,9 MB di riserva (partitions.csv) — fatto.
- **Radar spostato su GPIO 26 (RX) / 27 (TX) / 25 (OUT)** nel Kconfig: i vecchi 12/13/11 sono
  vietati o a rischio sul classico (12 strapping tensione flash, 11 flash SPI); pagine hardware,
  componente, procedura radar e [GPIO da evitare](insidie/gpio-da-evitare.md) riscritte per il
  classico (sezione storica S3 conservata) — fatto.
- LED_DATA GPIO5 (strapping SDIO, benigno: rail spento al boot) e LED_EN GPIO4 invariati;
  stime ADR-002/003 invariate (logica 3,3 V, rail 5 V) — fatto.
- Porta seriale confermata: domanda chiusa in
  [domande aperte](domande-aperte.md) (sezione Risolte); procedure flash/monitor aggiornate a
  `set-target esp32` + `/dev/ttyUSB0` — fatto.
- Pin header D1 mini confermati dall'utente sulle scritte a bordo: **4, 5, 25, 26, 27**
  presenti (tutti i pin di progetto) — [board](hardware/esp32-d1-mini.md) aggiornata, assunto
  chiuso — fatto.

## 2026-10-25 — Fase 2: completamento radar su esp32 (D1 mini)

- Pin radar **GPIO 26 (RX) / 27 (TX) / 25 (OUT)**, `RADAR_EN` non cablato (−1), per
  [ADR-006](decisioni/adr-006-pivot-esp32-d1-mini.md); UART1 con remap via `uart_set_pin` — fatto.
- App di test `test_apps/radar_ld2410` completata: 14 comandi console (`radar status/version/params/resolution/trigger/sensitivity/limits/engineering/restart/factoryreset confirm/calibrate[/show|apply]/csv`) + task CSV a 20 Hz su cambiamento riga — fatto.
- **Build esp32 della test app: exit 0, zero warning**; binario `test_radar_ld2410.bin` 0x43aa0 B (~18% dello slot `ota_0` da 1536K); **42/42 test host** (15 parser, 14 codec comandi, 13 trigger) — fatto.
- Persistenza **NVS namespace `radar`**: `trigger_mm` u16, `sens` blob 18 B, `max_mov`/`max_st` u8, `unmanned_s` u16; boot sync write-only-if-different; `radar_state_t` esteso additivamente per le colonne CSV — fatto.
- Hardware rimandato al banco: pagina [hardware](hardware/hlk-ld2410b.md) resta `assunto` (fw del modulo da leggere, polarità OUT); pagina e protocollo con errata v1.07 in [insidie](insidie/ld2410b-protocollo-errata.md) — fatto (assunto).

## 2026-10-25 — Identificazione modulo radar: HLK-LD2420 (non LD2410B)

- L'utente riporta le scritte a bordo del modulo: **3V3, GND, OT1, RX, OT2** — è il pinout
  dell'**HLK-LD2420** (presenza/movimento, alimentazione 3,3 V), non dell'LD2410B (5 V, OUT,
  UART_Tx/Rx). Identificazione forte ma da confermare leggendo la versione firmware al primo
  collegamento — fatto (assunto).
- Ruolo di OT1/OT2 e baud **dipendono dalla versione firmware** (<1.5.3: OT1 = uscita presenza,
  OT2 = TX seriale, 256000 baud; ≥1.5.3: invertiti, 115200) — fonte ESPHome LD2420,
  consultata 2026-10-25.
- Pagine: nuova [hardware HLK-LD2420](hardware/hlk-ld2420.md); [HLK-LD2410B]
  (hardware/hlk-ld2410b.md) → `stato: obsoleto` come hardware, resta il riferimento del
  protocollo implementato dal firmware; [procedura radar](../procedura_radar.md) con cablaggio
  riscritto (3,3 V dal LDO, mai il boost 5 V) e misura consumo su 3,3 V; [alimentazione]
  (hardware/alimentazione.md) e [bilancio](misure/bilancio-energetico-stima.md) aggiornati
  (voce ~3,2 Wh/8 h ritirata finché non misurata); [domande aperte](domande-aperte.md)
  riscritte — fatto.
- **Decisione aperta**: adattare il firmware al protocollo LD2420 (presenza senza distanza
  bersaglio: i criteri di Fase 2 con distanze 1/2/3 m non sono verificabili su questo modulo)
  o sostituire il modulo con un vero LD2410B — [domande aperte](domande-aperte.md).

## 2026-10-25 — Adattamento del firmware al protocollo LD2420

- Componente rinominato `radar_ld2410` → **`radar_ld2420`** (sorgenti, test app
  `test_apps/radar_ld2420`, pagina wiki): il firmware ora **parla il protocollo LD2420** —
  comandi 0x00FF/0x00FE, 0x0000, 0x0068, 0x0008/0x0007, 0x0012; 16 cancelli con soglie
  trigger/keep u32; frame energia F4F3F2F1. Fonti: doc Hi-Link LD2420 tradotto + ESPHome
  ld2420, consultati 2026-10-25 — fatto.
- **Fw ≤ 1.5.3 gestito dal firmware**: nessun frame energia, solo righe testo running mode e
  baud diverso → **probe baud automatico 115200/256000** e parsing delle righe testo;
  **boot sync write-only-if-different** — fatto.
- **Calibrazione** = picco di rumore × percentuale Kconfig (move 250 %, still 225 %) con
  proposta + apply esplicito; **factoryreset** = riscrittura della tabella di fabbrica ESPHome
  (il LD2420 non ha comando di reset); NVS `radar`: `trigger_mm`, `min_gate`/`max_gate`,
  `unmanned_s`, `move_thresh`/`still_thresh` blob 64 B, chiavi legacy LD2410 cancellate
  all'avvio — fatto.
- Eventi `PRESENCE_EVENT` **invariati**; trigger presence/distance puro (conferma, tenuta,
  isteresi); console aggiornata: sensibilità u32, niente `resolution`, limits = min/max gate +
  timeout s — fatto.
- **Verifica**: **50/50 test host** (20 parser, 19 codec, 11 trigger), **build esp32 zero
  warning**; **banco senza alimentazione dei sensori**: FAULT latched a ~2 s con evento,
  `degraded=true`, console viva e graceful, recovery con **backoff esponenziale
  1→2→4→8→16→30 s** — bug ms-vs-us nel backoff trovato sul banco (diagnosi via log) e
  corretto — fatto.
- Wiki allineata: pagina componente riscritta e rinominata [radar_ld2420]
  (componenti/radar_ld2420.md); [HLK-LD2420](hardware/hlk-ld2420.md) aggiornata (protocollo ora
  implementato, nota fw ≤ 1.5.3 gestita); [HLK-LD2410B](hardware/hlk-ld2410b.md) e [errata]
  (insidie/ld2410b-protocollo-errata.md) passano a riferimento storico;
  [procedura radar](../procedura_radar.md) rettificata; domanda 11 spostata in Risolte
  ([domande aperte](domande-aperte.md)) — fatto.
- **Resta da verificare con il modulo alimentato**: versione fw, ruoli OT1/OT2 e baud,
  polarità presenza, consumo 3,3 V, mappa cancello→distanza, EMI — [domande aperte]
  (domande-aperte.md) — fatto (assunto).

## 2026-10-25 — Server MQTT locale per i test (Mosquitto)

- L'utente ha installato sul PC di sviluppo un broker **Mosquitto 2.0.18** (servizio attivo
  e abilitato al boot) per provare i messaggi MQTT della Fase 5.
- Verificato: in ascolto SOLO su localhost (127.0.0.1/::1:1883, default di Mosquitto 2.0):
  la board ESP32 non può connettersi finché non si aggiunge un listener esposto con
  autenticazione in `/etc/mosquitto/conf.d/` (procedura in [server MQTT](procedure/mosquitto.md);
  richiede sudo dell'utente). Client CLI `mosquitto_pub`/`mosquitto_sub` NON ancora
  installati (`mosquitto-clients`): roundtrip locale da eseguire dopo l'installazione.
- Pagine: nuova [procedura server MQTT](procedure/mosquitto.md), riga in [index](index.md) — fatto.

## 2026-10-25 — Banco: modulo alimentato, protocollo verificato, calibrazione applicata

- Modulo **HLK-LD2420 alimentato e dialogante**: firmware **v1.5.9**, baud **115200**
  (fw ≥1.5.3 → seriale su **OT1**, presenza su **OT2**: cablaggio OT1→GPIO26,
  OT2→GPIO25, RX modulo→GPIO27 confermato a banco) — fatto.
- [Domanda 9 chiusa](domande-aperte.md) (identificazione: LD2420 confermato, fw v1.5.9);
  domanda 10: ruoli OT1/OT2 confermati, polarità presenza ancora da verificare — fatto (parziale).
- **Insidia trovata a banco**: buffer comandi del modulo = 32 byte → scritture da
  16 pair scartate in silenzio (timeout); [fix: scritture chunked ≤5 pair]
  (insidie/ld2420-buffer-comandi.md) — fatto.
- **Calibrazione applicata e verificata**: 60 s a stanza vuota, 568 campioni;
  soglie trigger/keep per cancello (picco×250%/225%) scritte con ACK verificato e
  **rilette identiche dal modulo**; riparto dopo reset: `module parameters already
  in sync, no writes` (boot non riscrive) — fatto.
- Verifiche a banco: FAULT+degrado a silenzio (sensori non alimentati), backoff
  esponenziale 1→30 s, console viva, 3055+ frame ok / 0 errati in modalità energia — fatto.
- Resta al prossimo banco: verifica presenza reale (avvicinamento, trigger 500 ms),
  30 min falsi positivi (10 con rainbow 100%), consumo 3,3 V — fatto (rimandato).

## 2026-10-26 — Fase 3: pumpkin_fsm e config_store

- Componenti `config_store` (parametri utente: NVS namespace `pumpkin`, debounce commit
  2000 ms, evento `CONFIG_EVENT_CHANGED` con `config_change_t{id}`, sanitize + migrate v1)
  e `pumpkin_fsm` (stati IDLE/TRIGGERED/COOLDOWN/MANUAL/OFF: core puro con clock
  iniettato, coda unica + task proprietario, esp_timer one-shot, comandi
  `fsm_cmd_set_mode/set_light/set_effect/scare`) — **test host verdi** (13 + 27 casi,
  2026-10-26) — fatto.
- **[ADR-007](decisioni/adr-007-fsm-config-store.md)** (nuovo): `config_store` fonte
  unica di verità con sync `trigger_mm` verso il radar (che resta con la sua copia NVS
  `radar`); FSM→`led_effects` solo via `fsm_output_t`; clock iniettato per i test host;
  boot OFF/IDLE da `power_on` persistito; semantica scare (ritorno a MANUAL se parte da
  lì, altrimenti IDLE) e cooldown col look idle e `PRESENCE_ON` ignorato — fatto.
- **[docs/fsm.md](../fsm.md)** (nuova, fuori wiki): diagramma Mermaid degli stati,
  tabella delle transizioni, tempi (trigger 8 s, cooldown 30 s, timer one-shot con
  cancel esplicito), modello di configurazione, semantica dei comandi, politica di boot
  e sequenza di verifica a banco con i comandi console dell'app `test_apps/pumpkin_fsm`
  (`fsm`, `fsm_mode`, `fsm_scare`, `fsm_light`, `fsm_anim`, `cfg_get`, `cfg_set`,
  `cfg_flush`) — fatto.
- Pagine nuove: [componente pumpkin_fsm](componenti/pumpkin_fsm.md), [componente
  config_store](componenti/config_store.md); tre nuove domande aperte (doppia
  persistenza di `trigger_mm`, validazione dei nomi effetto differita al consumo, tempi
  e persistenza da banco) — fatto.
- **Da verificare a banco** (assunto): tempi reali su board, persistenza dopo reboot,
  debounce NVS su flash reale, cablaggio radar→FSM→LED — fatto (assunto).

## 2026-10-26 — Fase 4 in corso: rete e provisioning

- Componenti della Fase 4: **`net_mgr`** (Wi-Fi STA: eventi `NET_EVENT_UP/DOWN`,
  backoff 1→60 s ±15% host-testato, hostname+mDNS `_pumpkin._tcp` con TXT
  diagnostici, power save/listen interval runtime) e **`wifi_prov`** (BLE
  NimBLE default / SoftAP via Kconfig, Security 1 con PoP `pump-XXXXXX` dal
  MAC, endpoint TLV `mqtt`, pulsante GPIO32, timeout 10 min, rilascio BTDM
  permanente) — fatto.
- **`config_store` esteso a v2**: blocco `config_mqtt_t` (blob 209 B, campi
  host/port/user/pass/tls) con `get_mqtt`/`set_mqtt`, migrate v1→v2, **18 test
  host verdi** — fatto.
- **`led_effects`**: overlay di stato (`fx_overlay.c`, PROV/CONNECTING/NET_UP/
  NET_DOWN, fusione max dopo gamma, auto-clear) senza toccare la FSM — fatto.
- **[ADR-008](decisioni/adr-008-rete-provisioning.md)** (nuovo): divisione
  wifi_prov/net_mgr, PoP dal MAC con assunzione di minaccia dichiarata, init
  del chip all'app, overlay invece di stati FSM, MQTT solo in config_store —
  fatto.
- **App di banco `test_apps/net_prov`** (nuova): catena offline dell'app
  `pumpkin_fsm` + rete; init netif/wifi all'app, decisione boot provisioning vs
  rete, console `net`/`prov`/`cfg_get`/`fsm_state`/`fsm_mode`/`heap` — fatto
  (build esp32 e banco **pending**).
- Pagine nuove: [net-mgr](componenti/net-mgr.md), [wifi-prov](componenti/wifi-prov.md),
  [provisioning](procedure/provisioning.md) (con checklist di banco), ADR-008;
  aggiornate [config_store](componenti/config_store.md) (v2),
  [led_effects](componenti/led_effects.md) (overlay),
  [app-principale](componenti/app-principale.md) e
  [domande aperte](domande-aperte.md) — fatto.

## 2026-10-26 — Fase 4: build verde e lezione IRAM

- **Build esp32 dell'app di banco `test_apps/net_prov`: exit 0, zero
  warning**; binario 1,19 MB sullo slot `ota_0` da 1536K; **IRAM al 93%**
  (9098 B liberi), DRAM ~40% — fatto.
- Prima build fallita al link: **IRAM overflow di 3180 byte** con BLE+Wi-Fi+
  mDNS; risolta coi knob documentati in
  [insidie/iram-nimble-wifi.md](insidie/iram-nimble-wifi.md)
  (`ESP_WIFI_IRAM_OPT=n`, ruoli NimBLE solo-periferica) — fatto.
- Suite host completa: **135 test verdi** (config_store 18, led_effects 20,
  net_mgr 8, wifi_prov 12, pumpkin_fsm 27, radar_ld2420 50); lint wiki a
  zero errori; nessuna credenziale nei sorgenti (grep) — fatto.
- Al banco la [checklist](procedure/provisioning.md): provisioning da
  telefono, 5 riconnessioni entro 2 min, reset GPIO32, latenza ps/listen,
  corrente a riposo con Wi-Fi, glitch LED durante BLE, mDNS sulla LAN —
  fatto (rimandato: board non collegata).
