# Domande aperte

Tutto ciò che resta stima, non verificato o da decidere. Ogni voce rinvia alla pagina correlata.

1. **Immunità VL53L1X alla luce solare diretta** — ipotesi di disturbo del ToF, nessuna misura prevista finché il sensore non è cablato. → [insidie/vl53l1x-luce-solare.md](insidie/vl53l1x-luce-solare.md)
2. **Light sleep + Wi-Fi attivo + RMT** — compatibilità da verificare: il rail LED usa RMT e power_mgr è una fase futura. → [insidie/light-sleep-wifi-rmt.md](insidie/light-sleep-wifi-rmt.md)
3. **Bilancio energetico ~11,5 Wh per 8 h** — solo stima da brief, da misurare a banco. → [misure/bilancio-energetico-stima.md](misure/bilancio-energetico-stima.md)
4. **Pin I2C per il VL53L1X** — non ancora definiti; la Fase 2 non è ancora pianificata in dettaglio. → [hardware/vl53l1x.md](hardware/vl53l1x.md)
5. **Celle 18650 effettive e consumo a riposo** — modello/capacità reali e corrente di riposo del boost ancora da determinare (stima consumo a riposo). → [hardware/alimentazione.md](hardware/alimentazione.md)
6. **Tipo di interruttore rail cablato** — confermare a banco low-side N-MOSFET vs high-side (nota in `docs/pinout.md`); il firmware resta identico (LED_EN attivo alto). → [decisioni/adr-002-nmosfet-rail-switch.md](decisioni/adr-002-nmosfet-rail-switch.md)
7. **File di progetto mancanti** — `AGENTS.md` viene creato con questo seed della wiki; `CLAUDE.md` e `prompts/fase*.md` non esistono: chiedere all'utente se crearli. → [log.md](log.md)
8. **Mappatura serpentina della matrice 8×8** — l'indice `y*8 + (y pari ? x : 7-x)` con origine alto-sinistra è un **assunto hardware**: verificare a banco col comando `pixel` (angoli e primi pixel delle righe 0/1); se l'ordine fisico differisce si corregge solo la mappatura software. → [hardware/led-ws2812b.md](hardware/led-ws2812b.md)
9. **Identificazione del modulo radar** — i pin fisici (3V3, GND, OT1, RX, OT2) corrispondono a un **HLK-LD2420**, non a un LD2410B: confermare al primo collegamento leggendo la versione firmware (256000 baud per fw <1.5.3, 115200 per fw ≥1.5.3). → [hardware/hlk-ld2420.md](hardware/hlk-ld2420.md)
10. **Ruolo di OT1/OT2 e baud** — scambiati dalla firmware 1.5.3 (<1.5.3: OT1 = uscita presenza, OT2 = TX seriale; ≥1.5.3: invertiti): determinare dalla versione firmware prima di cablare in modo definitivo. Polarità dell'uscita presenza (assunto: alto = presente): verificare misurando il pin. → [hardware/hlk-ld2420.md](hardware/hlk-ld2420.md)
11. **Adattamento protocollo o sostituzione modulo** — il firmware attuale parla LD2410B v1.07 e NON dialoga con il LD2420 (presenza senza distanza bersaglio: i criteri di Fase 2 con distanze 1/2/3 m non sono verificabili su questo modulo): adattare il layer protocollo al LD2420 o sostituire il modulo con un vero LD2410B? → [hardware/hlk-ld2420.md](hardware/hlk-ld2420.md), [componenti/radar_ld2410.md](componenti/radar_ld2410.md)
12. **Consumo del radar su 3,3 V** — da misurare (alimentato dal LDO insieme all'ESP32, verifica anche della capacità dell'LDO) per chiudere il bilancio. → [misure/bilancio-energetico-stima.md](misure/bilancio-energetico-stima.md)
13. **Immunità EMI boost→radar** — il boost a pieno carico (rainbow 100%) disturba il radar (ora su LDO 3,3 V)? Test 30 min a stanza vuota previsto dalla procedura. → [procedura radar](../procedura_radar.md)

## Risolte

1. **Porta seriale reale** — **risolta il 2026-10-25**: collegata la board, il bridge è un
   **CP2102** su **`/dev/ttyUSB0`** (verificato con esptool; la vecchia attesa era
   `/dev/ttyACM0` via USB-CDC sulla board S3 mai ricevuta). → [hardware/esp32-d1-mini.md](hardware/esp32-d1-mini.md)
