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
