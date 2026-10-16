#!/usr/bin/env bash
# Differential test of the baseline JIT: every program in a directory must print the same thing
# with the JIT off as with every function forced through it.
#
#   tools/jit-differential.sh [path/to/quanta] [directory of .js files]
#
# Defaults: build/bin/quanta and tests/comparison. Exits non-zero if anything differs, or if the
# JIT turns out not to be compiling anything at all (a process that cannot map executable memory
# leaves every function interpreted, which would make the comparison pass for the wrong reason).
set -u

QUANTA="${1:-build/bin/quanta}"
DIR="${2:-tests/comparison}"
TIMEOUT_SECONDS="${JIT_DIFF_TIMEOUT:-120}"

if [ ! -x "$QUANTA" ]; then echo "no such binary: $QUANTA" >&2; exit 2; fi
QUANTA="$(cd "$(dirname "$QUANTA")" && pwd)/$(basename "$QUANTA")"

run_with_timeout() {
    if command -v timeout >/dev/null 2>&1; then timeout "$TIMEOUT_SECONDS" "$@"
    elif command -v gtimeout >/dev/null 2>&1; then gtimeout "$TIMEOUT_SECONDS" "$@"
    else "$@"; fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1. The JIT has to be alive on this machine.
cat > "$WORK/probe.js" <<'JS'
function sum(n) { var s = 0; for (var i = 0; i < n; i++) { s += i * 2 - 1; } return s; }
console.log(sum(100000));
JS
QUANTA_JIT_LOG=1 QUANTA_JIT_THRESHOLD=1 QUANTA_JIT_MODE=0 "$QUANTA" "$WORK/probe.js" > "$WORK/probe.out" 2>&1
COMPILED="$(grep -o 'compiled=[0-9]*' "$WORK/probe.out" | tail -1 | cut -d= -f2)"
echo "JIT probe: compiled ${COMPILED:-0} function(s)"
if [ "${COMPILED:-0}" -lt 1 ]; then
    echo "FAIL: the JIT compiled nothing here (no executable memory, or unsupported target)" >&2
    cat "$WORK/probe.out" >&2
    exit 1
fi

# 2. Same output with and without it.
pass=0; fail=0; failed=""
for f in "$DIR"/*.js; do
    name="$(basename "$f")"
    ( cd "$DIR" && QUANTA_JIT=0 run_with_timeout "$QUANTA" "$name" > "$WORK/ref.out" 2>&1 )
    ( cd "$DIR" && QUANTA_JIT_THRESHOLD=1 QUANTA_JIT_MODE=0 run_with_timeout "$QUANTA" "$name" > "$WORK/jit.out" 2>&1 )
    if cmp -s "$WORK/ref.out" "$WORK/jit.out"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); failed="$failed $name"
        echo "--- DIFF: $name"
        diff "$WORK/ref.out" "$WORK/jit.out" | head -10
    fi
done

echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || { echo "failed:$failed" >&2; exit 1; }
