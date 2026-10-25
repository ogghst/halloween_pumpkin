# Domande aperte

Tutto ciò che resta stima, non verificato o da decidere. Ogni voce rinvia alla pagina correlata.

1. **Porta seriale reale** — `/dev/ttyACM0` è quella attesa (il README dice "in genere"); confermare al primo collegamento fisico della board. → [procedure/flash-monitor.md](procedure/flash-monitor.md)
2. **Immunità VL53L1X alla luce solare diretta** — ipotesi di disturbo del ToF, nessuna misura prevista finché il sensore non è cablato. → [insidie/vl53l1x-luce-solare.md](insidie/vl53l1x-luce-solare.md)
3. **Light sleep + Wi-Fi attivo + RMT** — compatibilità da verificare: il rail LED usa RMT e power_mgr è una fase futura. → [insidie/light-sleep-wifi-rmt.md](insidie/light-sleep-wifi-rmt.md)
4. **Bilancio energetico ~11,5 Wh per 8 h** — solo stima da brief, da misurare a banco. → [misure/bilancio-energetico-stima.md](misure/bilancio-energetico-stima.md)
5. **Pin I2C per il VL53L1X** — non ancora definiti; la Fase 2 non è ancora pianificata in dettaglio. → [hardware/vl53l1x.md](hardware/vl53l1x.md)
6. **Celle 18650 effettive e consumo a riposo** — modello/capacità reali e corrente di riposo del boost ancora da determinare (stima consumo a riposo). → [hardware/alimentazione.md](hardware/alimentazione.md)
7. **Tipo di interruttore rail cablato** — confermare a banco low-side N-MOSFET vs high-side (nota in `docs/pinout.md`); il firmware resta identico (LED_EN attivo alto). → [decisioni/adr-002-nmosfet-rail-switch.md](decisioni/adr-002-nmosfet-rail-switch.md)
8. **File di progetto mancanti** — `AGENTS.md` viene creato con questo seed della wiki; `CLAUDE.md` e `prompts/fase*.md` non esistono: chiedere all'utente se crearli. → [log.md](log.md)
