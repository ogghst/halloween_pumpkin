---
titolo: Unit test su host
tipo: procedura
stato: verificato
aggiornato: 2026-10-25
fonti: [README.md, components/led_effects/host_tests/CMakeLists.txt]
correlate: [../componenti/led_effects.md, ../hardware/led-ws2812b.md, ../../../components/led_effects/host_tests/CMakeLists.txt, ../../../components/led_effects/host_tests/test_fx_color.c, ../../../components/led_effects/host_tests/test_fx_matrix.c]
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
- `test_fx_current.c`: stima corrente **3840 mA** su 64 pixel bianchi a scala 100, **1920 mA**
  a scala 50, **5120 mA** con canale RGBW, frame nero e argomenti invalidi a 0 mA.
- `test_fx_matrix.c`: mappatura serpentina di `led_fx_xy_index` — angoli (0,0)=0, (7,0)=7,
  (0,1)=15, (7,1)=8, (0,2)=16, (7,7)=56, (0,7)=63; fuori range (anche x=8/y=8 e negativi) → -1.

## Regola

- Cambia un algoritmo puro (colore/corrente/mappatura matrice) → estendere qui il test prima di flashare.
