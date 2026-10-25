---
titolo: Unit test su host
tipo: procedura
stato: verificato
aggiornato: 2026-10-25
fonti: [README.md, components/led_effects/host_tests/CMakeLists.txt]
correlate: [../componenti/led_effects.md, ../../../components/led_effects/host_tests/CMakeLists.txt, ../../../components/led_effects/host_tests/test_fx_color.c]
---

# Host test (logica pura, senza hardware)

Il componente separa la logica pura (colore, stima corrente) dal layer IDF: si testa su host
con CMake + Unity + CTest.

## Comandi

```bash
cmake -S components/led_effects/host_tests -B components/led_effects/host_tests/build
cmake --build components/led_effects/host_tests/build
ctest --test-dir components/led_effects/host_tests/build --output-on-failure
```

- Richiede **`IDF_PATH` nell'ambiente** (il CMake compila `unity.c` da `$IDF_PATH`):
  prima dei comandi, `source ~/esp/esp-idf/export.sh`.

## Cosa copre oggi

- [test_fx_color.c](../../../components/led_effects/host_tests/test_fx_color.c): estremi e monotonia
  gamma, sin endpoints, primarie HSV, sweep hue completo contro riferimento float
  (tolleranza 16: sei regioni da 43 count, drift fino a ~15 LSB), LUT di uscita con cap 60%.
- `test_fx_current.c`: stima corrente 1920 mA su 32 pixel bianchi, 2560 mA con canale RGBW,
  scala 50%, frame nero e argomenti invalidi a 0 mA.

## Regola

- Cambia un algoritmo puro (colore/corrente) → estendere qui il test prima di flashare.
