#!/usr/bin/env bash
# Profile-guided build: instrumented build, run the training corpus, rebuild with the merged profile.
# Regenerates tools/pgo/quanta.profdata, which `make` then applies by itself (PGO=0 turns that off).
#   tools/pgo/pgo.sh
#   PGO_EXTRA=dir tools/pgo/pgo.sh   also trains on every *.js in dir (the Octane programs, code.js, ...)
# Needs clang, llvm-profdata, and node/npm for the library part of the corpus (skipped without them).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
WORK=$ROOT/build-pgo
JOBS=${JOBS:-$(nproc)}
PROFDATA=${PROFDATA:-llvm-profdata}

mkdir -p "$WORK/raw" "$WORK/corpus"
rm -f "$WORK"/raw/*.profraw

make -j"$JOBS" BUILD_DIR=build-pgo/gen PGO_RAW_DIR="$WORK/raw"
GEN=$WORK/gen/bin/quanta

if command -v npm >/dev/null 2>&1; then
    if [ ! -d "$WORK/libs/node_modules" ]; then
        mkdir -p "$WORK/libs"
        (cd "$WORK/libs" && npm init -y >/dev/null 2>&1 &&
         npm install --no-audit --no-fund lodash moment marked handlebars acorn esprima immutable papaparse ejs mustache dayjs typescript@5 >/dev/null)
    fi
    python3 tools/pgo/corpus.py "$WORK/libs" "$WORK/corpus"
else
    echo "[pgo] npm not found: training without the library corpus"
fi

run() { LLVM_PROFILE_FILE="$WORK/raw/p-%p.profraw" "$GEN" "$1" >/dev/null 2>&1 || echo "[pgo] $1 failed"; }
for f in "$WORK"/corpus/*.js tools/pgo/corpus/*.js ${PGO_EXTRA:+"$PGO_EXTRA"/*.js}; do
    [ -f "$f" ] && run "$f"
done

"$PROFDATA" merge -o "$WORK/quanta.profdata" "$WORK"/raw/*.profraw
cp "$WORK/quanta.profdata" tools/pgo/quanta.profdata
echo "[pgo] wrote tools/pgo/quanta.profdata; run 'make clean && make' to build with it"
