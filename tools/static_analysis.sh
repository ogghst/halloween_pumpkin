#!/usr/bin/env bash
# static_analysis.sh — analisi statica del firmware pumpkin (cppcheck + GCC -fanalyzer).
#
# Uso: ./tools/static_analysis.sh
# Requisiti: ambiente ESP-IDF disponibile (la script fa source di ~/esp/esp-idf/export.sh
# se idf.py non e' gia' nel PATH).
#
# Report salvati in /tmp/pumpkin_static_analysis/<timestamp>/:
#   - cppcheck_report.txt      (se cppcheck e' installato)
#   - fanalyzer_build.log      (configurazione + compilazione -fanalyzer dei TU nostri)
#   - fanalyzer_findings.txt   (solo i diagnostic sui sorgenti nostri: components/ + main/)
#
# EXIT CODE: 0 se l'analisi gira (i finding vanno triati a mano: bug reale -> fix,
# falso positivo -> documento); 1 solo se l'infrastruttura fallisce (configurazione
# o compilazione di un TU interrotta, ambiente ESP-IDF mancante).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

STAMP="$(date +%Y%m%d_%H%M%S)"
REPORT_DIR="/tmp/pumpkin_static_analysis/$STAMP"
mkdir -p "$REPORT_DIR"

# Ambiente ESP-IDF
if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        # shellcheck disable=SC1091
        . "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
    fi
fi
if ! command -v idf.py >/dev/null 2>&1; then
    echo "ERRORE: idf.py non disponibile (export.sh non trovato?)"
    exit 1
fi

echo "== Analisi statica pumpkin — report in $REPORT_DIR =="

# --- (a) cppcheck (opzionale) ---
N_CPPCHECK="n/d"
if command -v cppcheck >/dev/null 2>&1; then
    cppcheck --enable=warning,performance,portability --std=c11 \
        --suppress=missingIncludeSystem --inline-suppr \
        -I components/*/include \
        components main \
        > "$REPORT_DIR/cppcheck_report.txt" 2>&1
    N_CPPCHECK="$(grep -c 'warning:' "$REPORT_DIR/cppcheck_report.txt" || true)"
    echo "[cppcheck] $N_CPPCHECK warning -> $REPORT_DIR/cppcheck_report.txt"
else
    echo "[cppcheck] NON installato: salto. Per installarlo:"
    echo "    sudo apt install cppcheck"
    echo "  (poi rilanciare questa script)"
fi

# --- (b) GCC -fanalyzer sui TU nostri (components/ + main/) ---
# Vincoli verificati su IDF v5.4.4:
#  1. l'ambiente CFLAGS NON viene letto da idf.py (nessun ENV{CFLAGS} in
#     tools/cmake): l'unico modo per iniettare flag in tutta la configurazione
#     e' la cache CMake: idf.py -D "CMAKE_C_FLAGS=..." reconfigure. Il
#     toolchain-esp32.cmake appende i suoi flag al valore cache, quindi il
#     -fanalyzer sopravvive. Serve configurazione da zero (rm -rf build).
#  2. eseguire i target-oggetto via ninja in una build fresca trascina il
#     grafo completo (~850 step, mbedtls compreso): si analizzano quindi
#     SOLO i TU dei nostri 10 componenti + main, RIESEGUENDO le righe di
#     compilazione lette da build/compile_commands.json (stesse flag e
#     include del build reale, output su /dev/null, nessuna dipendenza IDF).
#  3. -fanalyzer segnala falsi positivi su header mbedtls terzi (bn_mul.h)
#     toccati transitivamente dai nostri TU: con -Werror per-target diventano
#     errori fatali. Per la SOLA configurazione di analisi si degrada SOLO
#     quella categoria in coda ai flag target (l'ultimo flag vince); le altre
#     categorie di analyzer restano errori. Ripristino con trap.
echo "[fanalyzer] configurazione pulita con -D CMAKE_C_FLAGS=\"-fanalyzer ...\" ..."
T0=$(date +%s)
rm -rf build

sed -i 's/-Wall -Wextra -Werror)/-Wall -Wextra -Werror -Wno-error=analyzer-out-of-bounds)/' \
    components/*/CMakeLists.txt main/CMakeLists.txt
restore_flags() {
    sed -i 's/ -Wno-error=analyzer-out-of-bounds)/)/' \
        components/*/CMakeLists.txt main/CMakeLists.txt
}
trap restore_flags EXIT

if ! idf.py -D "CMAKE_C_FLAGS=-fanalyzer -Wno-analyzer-too-complex" reconfigure \
        > "$REPORT_DIR/fanalyzer_build.log" 2>&1; then
    restore_flags
    trap - EXIT
    echo "[fanalyzer] CONFIGURAZIONE FALLITA: vedi $REPORT_DIR/fanalyzer_build.log"
    exit 1
fi
if ! grep -q '"command"' build/compile_commands.json 2>/dev/null; then
    restore_flags
    trap - EXIT
    echo "[fanalyzer] ERRORE: compile_commands.json mancante (CONFIGURE fallita?)"
    exit 1
fi

echo "[fanalyzer] riesecuzione dei TU nostri da compile_commands.json ..."
python3 - "$ROOT" >> "$REPORT_DIR/fanalyzer_build.log" 2>&1 <<'PY'
import json, subprocess, sys
root = sys.argv[1]
with open(root + "/build/compile_commands.json") as fh:
    entries = json.load(fh)
n = 0
fails = 0
for e in entries:
    f = e["file"]
    if "managed_components" in f:
        continue
    if not ("/components/" in f or "/main/" in f):
        continue
    # ultimo -o vince: l'oggetto analizzato va su /dev/null, il build tree
    # resta intatto (una build normale lo rigenera').
    cmd = e["command"] + " -o /dev/null"
    r = subprocess.run(cmd, shell=True, cwd=e["directory"])
    n += 1
    if r.returncode != 0:
        fails += 1
print(f"[fanalyzer-python] TU analizzati: {n}, falliti: {fails}")
sys.exit(1 if fails else 0)
PY
RC_PY=$?
echo "[fanalyzer] analisi completata in $(( $(date +%s) - T0 )) s"
restore_flags
trap - EXIT
if [ "$RC_PY" -ne 0 ]; then
    echo "[fanalyzer] COMPILAZIONE FALLITA su qualche TU: vedi $REPORT_DIR/fanalyzer_build.log"
    exit 1
fi

# Diagnostic solo sui NOSTRI sorgenti (esclude managed_components e componenti IDF)
grep -E 'warning:' "$REPORT_DIR/fanalyzer_build.log" \
    | grep -E "$ROOT/(components|main)/" \
    | grep -v 'managed_components' \
    > "$REPORT_DIR/fanalyzer_findings.txt" || true
N_FAN="$(wc -l < "$REPORT_DIR/fanalyzer_findings.txt")"
echo "[fanalyzer] $N_FAN diagnostic sui nostri file -> $REPORT_DIR/fanalyzer_findings.txt"
echo "[fanalyzer] conferma propagazione flag:"
if grep -q -- '-fanalyzer' build/compile_commands.json; then
    echo "  -fanalyzer presente nei comandi dei TU (compile_commands.json)"
else
    echo "  ATTENZIONE: -fanalyzer NON presente in compile_commands.json"
fi

echo ""
echo "== RIASSUNTO =="
echo "  cppcheck warning : $N_CPPCHECK"
echo "  -fanalyzer (nostri file): $N_FAN"
echo "  Report: $REPORT_DIR/"
exit 0
