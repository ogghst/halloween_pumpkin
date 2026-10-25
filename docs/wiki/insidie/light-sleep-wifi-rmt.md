---
titolo: Light sleep, Wi-Fi e RMT
tipo: insidia
stato: ipotesi
aggiornato: 2026-10-25
fonti: [brief utente 2026-10-25]
correlate: [../hardware/led-ws2812b.md, ../decisioni/adr-003-alimentazione-separata.md, ../decisioni/adr-004-trigger-mqtt-contratto.md]
---

# Light sleep + Wi-Fi attivo + RMT: convivenza da verificare

## L'ipotesi

- power_mgr (fase futura) vorrà usare il **light sleep** dell'ESP32-S3 per risparmiare batteria,
  mantenendo il Wi-Fi attivo per MQTT ([ADR-004](../decisioni/adr-004-trigger-mqtt-contratto.md)).
- Il rail LED è pilotato da **RMT** ([striscia LED](../hardware/led-ws2812b.md)): la sospensione
  automatica può fermare o corrompere una periferica in emissione. Comportamento **non
  verificato** su questa combinazione: nessun codice power_mgr nel repo.

## Come uscire dall'ipotesi

1. Prototipo power_mgr: misurare consumo con/senza light sleep con Wi-Fi e MQTT attivi.
2. Verificare che un frame LED resti corretto quando il sistema rientra dal sleep.
3. Se incompatibile: valutare deep sleep con risveglio esterno (es. ToF) invece del light sleep.

## Correlati

- [ADR-003](../decisioni/adr-003-alimentazione-separata.md): a riposo il boost resta spento,
  l'ESP32 no — il risparmio si gioca tutto sul profilo dell'ESP32.
