---
titolo: ADR-001 — ESP-IDF v5.4 LTS
tipo: decisione
stato: verificato
aggiornato: 2026-10-25
fonti: [README.md, brief utente 2026-10-25]
correlate: [../hardware/esp32-s3-devkitc1.md, ../procedure/flash-monitor.md, ../procedure/host-tests.md, ../../../sdkconfig.defaults]
---

# ADR-001 — Usare ESP-IDF v5.4 LTS

## Contesto

Progetto firmware ESP32-S3 a più fasi (LED, ToF, FSM, power, MQTT, provisioning) su Ubuntu 24.04:
serve una base stabile e riproducibile, non un target in movimento.

## Scelta

- **ESP-IDF v5.4.4 (LTS)**, installata nativamente in `~/esp/esp-idf` (clone shallow del tag),
  tool in `~/.espressif`; ambiente per shell con `source ~/esp/esp-idf/export.sh`.
- **Verificato**: installata e usata con successo in Fase 0 (skeleton, build `esp32s3`) e Fase 1 (`led_effects`).

## Alternative scartate

- Branch `master` o rilasci non LTS: API in continuo movimento, rischio di rotture a metà progetto
  senza guadagni per un firmware di questo taglio.

## Conseguenze

- Ogni shell di build richiede l'export dell'ambiente (non persistente).
- Le dipendenze dei componenti restano su `idf >= 5.4` (es. `espressif/led_strip` 3.0.1).
- Aggiornamenti solo verso rilasci LTS successivi, con pianificazione e verifica dedicata.
- La configurazione di progetto vive in [sdkconfig.defaults](../../../sdkconfig.defaults).
