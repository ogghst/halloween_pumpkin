# Domande aperte

Tutto ciò che resta stima, non verificato o da decidere. Ogni voce rinvia alla pagina correlata.

1. **Immunità VL53L1X alla luce solare diretta** — ipotesi di disturbo del ToF (sensore rimandato a una fase successiva: la Fase 2 usa il radar HLK-LD2420), nessuna misura prevista finché il sensore non è cablato. → [insidie/vl53l1x-luce-solare.md](insidie/vl53l1x-luce-solare.md)
2. **Light sleep + Wi-Fi attivo + RMT** — compatibilità da verificare: il rail LED usa RMT e power_mgr è una fase futura. → [insidie/light-sleep-wifi-rmt.md](insidie/light-sleep-wifi-rmt.md)
3. **Bilancio energetico ~11,5 Wh per 8 h** — solo stima da brief, da misurare a banco. → [misure/bilancio-energetico-stima.md](misure/bilancio-energetico-stima.md)
4. **Pin I2C per il VL53L1X** — non ancora definiti né serviti: il sensore è rimandato a una fase successiva (la Fase 2 usa il radar HLK-LD2420); sul classico GPIO8/9 sono flash SPI. → [hardware/vl53l1x.md](hardware/vl53l1x.md)
5. **Celle 18650 effettive e consumo a riposo** — modello/capacità reali e corrente di riposo del boost ancora da determinare (stima consumo a riposo). → [hardware/alimentazione.md](hardware/alimentazione.md)
6. **Tipo di interruttore rail cablato** — confermare a banco low-side N-MOSFET vs high-side (nota in `docs/pinout.md`); il firmware resta identico (LED_EN attivo alto). → [decisioni/adr-002-nmosfet-rail-switch.md](decisioni/adr-002-nmosfet-rail-switch.md)
7. **File di progetto mancanti** — `AGENTS.md` viene creato con questo seed della wiki; `CLAUDE.md` e `prompts/fase*.md` non esistono: chiedere all'utente se crearli. → [log.md](log.md)
8. **Mappatura serpentina della matrice 8×8** — l'indice `y*8 + (y pari ? x : 7-x)` con origine alto-sinistra è un **assunto hardware**: verificare a banco col comando `pixel` (angoli e primi pixel delle righe 0/1); se l'ordine fisico differisce si corregge solo la mappatura software. → [hardware/led-ws2812b.md](hardware/led-ws2812b.md)
9. **Ruolo dell'uscita presenza (OT2) e polarità** — i ruoli OT1 (seriale) / OT2 (presenza) sono CONFERMATI a banco (fw v1.5.9 ≥1.5.3, 115200); resta da verificare la polarità dell'uscita presenza (assunto: alto = presente) misurando il pin con un bersaglio. → [hardware/hlk-ld2420.md](hardware/hlk-ld2420.md)
10. **Consumo del radar su 3,3 V** — da misurare (alimentato dal LDO insieme all'ESP32, verifica anche della capacità dell'LDO) per chiudere il bilancio. → [misure/bilancio-energetico-stima.md](misure/bilancio-energetico-stima.md)
11. **Immunità EMI boost→radar** — il boost a pieno carico (rainbow 100%) disturba il radar (ora su LDO 3,3 V)? Test 30 min a stanza vuota previsto dalla procedura. → [procedura radar](../procedura_radar.md)

## Risolte

1. **Porta seriale reale** — **risolta il 2026-10-25**: collegata la board, il bridge è un
   **CP2102** su **`/dev/ttyUSB0`** (verificato con esptool; la vecchia attesa era
   `/dev/ttyACM0` via USB-CDC sulla board S3 mai ricevuta). → [hardware/esp32-d1-mini.md](hardware/esp32-d1-mini.md)
2. **Adattamento protocollo o sostituzione modulo** — **risolta il 2026-10-25**: firmware
   **adattato al protocollo LD2420** (componente rinominato `radar_ld2420`; 50/50 test host,
   build esp32 zero warning, banco senza alimentazione del sensore: guasto/degrado/backoff
   verificati). → [hardware/hlk-ld2420.md](hardware/hlk-ld2420.md), [componenti/radar_ld2420.md](componenti/radar_ld2420.md)
