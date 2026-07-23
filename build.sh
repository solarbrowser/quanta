#!/usr/bin/env bash

set -uo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
DIVIDER="────────────────────────────────────────────────────────────"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="build"
OBJ_DIR="$BUILD_DIR/obj"
BIN_DIR="$BUILD_DIR/bin"
LOG_FILE="$BUILD_DIR/build.log"
ERROR_LOG="$BUILD_DIR/errors.log"

mkdir -p "$BUILD_DIR" "$OBJ_DIR" "$BIN_DIR"
: > "$ERROR_LOG"
printf '[%s] Build started\n' "$(date '+%Y-%m-%d %H:%M:%S')" > "$LOG_FILE"

WARNING_COUNT=0
COMPILED_FILES=0
BUILD_START=$(date +%s)

JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
JOB_DIR=$(mktemp -d)
JOB_SEQ=0
JOB_PIDS=()
trap 'rm -rf "$JOB_DIR"' EXIT

fail() {
    echo -e "${RED}✗ $1${NC}" >&2
    echo "[${SECONDS}s] BUILD FAILED: $1" >> "$LOG_FILE"
    exit 1
}

show_error() {
    local errfile="$1"
    echo
    echo -e "${RED}✗ Compile failed${NC}"
    echo
    local first
    first=$(grep -m1 -E ': error:' "$errfile")
    if [[ -n "$first" ]]; then
        echo "${first%%: error:*}"
        echo
        echo "error:${first#*: error:}"
    else
        sed -n '1,10p' "$errfile"
    fi
    echo
}

fail_compile() {
    local src="$1" errfile="$2"
    show_error "$errfile"
    echo "[$(date '+%H:%M:%S')] ERROR compiling $src" >> "$LOG_FILE"
    exit 1
}

# Runs a compiler invocation, folding its stderr into ERROR_LOG (and counting
# warnings) regardless of outcome; on failure, prints the clean error block
# and exits -- $1/$2 are the source name and a scratch file for this call's
# own stderr, everything after is the actual command.
run_compile() {
    local src="$1" errfile="$2"
    shift 2
    "$@" 2>"$errfile"
    local rc=$?
    if [[ -s "$errfile" ]]; then
        WARNING_COUNT=$((WARNING_COUNT + $(grep -c 'warning:' "$errfile" || true)))
        cat "$errfile" >> "$ERROR_LOG"
    fi
    if [[ $rc -ne 0 ]]; then
        fail_compile "$src" "$errfile"
    fi
    rm -f "$errfile"
}

# Runs "$@" (after the source name) in the background, capturing its exit
# code and stderr under $JOB_DIR for later, serial collection -- keeps
# ERROR_LOG writes and the warning/file counters race-free across a whole
# pool of concurrent compiles. Blocks once $JOBS are in flight by waiting on
# the oldest tracked PID specifically (`wait -n`, i.e. whichever finishes
# first, needs bash 4.3+ -- macOS ships bash 3.2, so this settles for FIFO
# draining instead: still a hard cap on concurrency, just not always the
# very next one to finish).
launch_job() {
    local src="$1"
    shift
    JOB_SEQ=$((JOB_SEQ + 1))
    local id="$JOB_SEQ"
    echo "$src" > "$JOB_DIR/$id.src"
    ( "$@" 2>"$JOB_DIR/$id.err"; echo $? > "$JOB_DIR/$id.rc" ) &
    JOB_PIDS+=("$!")
    if [ "${#JOB_PIDS[@]}" -ge "$JOBS" ]; then
        wait "${JOB_PIDS[0]}" 2>/dev/null
        JOB_PIDS=("${JOB_PIDS[@]:1}")
    fi
}

# Waits for every job launched since the last collect_jobs, then serially
# folds each one's stderr into ERROR_LOG, sums warnings, and -- if any job
# failed -- reports the first failure with the same clean error block as a
# non-parallel run and exits.
collect_jobs() {
    wait
    JOB_PIDS=()
    local f id rc src fail_src fail_err
    for f in "$JOB_DIR"/*.rc; do
        [[ -e "$f" ]] || continue
        id="${f##*/}"; id="${id%.rc}"
        rc=$(<"$f")
        src=$(<"$JOB_DIR/$id.src")
        if [[ -s "$JOB_DIR/$id.err" ]]; then
            WARNING_COUNT=$((WARNING_COUNT + $(grep -c 'warning:' "$JOB_DIR/$id.err" || true)))
            cat "$JOB_DIR/$id.err" >> "$ERROR_LOG"
        fi
        COMPILED_FILES=$((COMPILED_FILES + 1))
        if [[ "$rc" != "0" && -z "${fail_src:-}" ]]; then
            fail_src="$src"
            fail_err="$JOB_DIR/$id.err"
            rm -f "$f" "$JOB_DIR/$id.src"
        else
            rm -f "$f" "$JOB_DIR/$id.src" "$JOB_DIR/$id.err"
        fi
    done
    if [[ -n "${fail_src:-}" ]]; then
        fail_compile "$fail_src" "$fail_err"
    fi
}

# $2 is the caller's array VARIABLE NAME, not its value -- namerefs
# (`local -n`) need bash 4.3+, unavailable on macOS's stock bash 3.2, so
# this appends via eval instead. Safe: every caller passes one of this
# script's own hardcoded *_OBJECTS names, never anything derived from
# outside input.
compile_group() {
    local obj_subdir="$1" arr_name="$2"
    shift 2
    local src obj
    for src in "$@"; do
        obj="$OBJ_DIR/$obj_subdir/$(basename "${src%.cpp}").o"
        launch_job "$src" clang++ "${CXXFLAGS[@]}" "${INCLUDES[@]}" -c "$src" -o "$obj"
        eval "$arr_name+=(\"\$obj\")"
    done
    collect_jobs
}

setup_pcre2() {
    local pcre2_src="third_party/pcre2/src"
    local pcre2_configs="third_party/pcre2_configs"

    if [[ ! -f "$pcre2_src/pcre2.h.generic" ]]; then
        git submodule update --init --recursive third_party/pcre2 || fail "Failed to initialize PCRE2 submodule"
    fi
    if [[ ! -f "third_party/utf8proc/utf8proc.c" ]]; then
        git submodule update --init --recursive third_party/utf8proc || fail "Failed to initialize utf8proc submodule"
    fi
    if [[ ! -f "$pcre2_src/config.h" ]]; then
        cp "$pcre2_configs/config.h" "$pcre2_src/config.h"
    fi
    if [[ ! -f "$pcre2_src/pcre2.h" ]]; then
        cp "$pcre2_src/pcre2.h.generic" "$pcre2_src/pcre2.h"
    fi
    if [[ ! -f "$pcre2_src/pcre2_chartables.c" ]]; then
        cp "$pcre2_src/pcre2_chartables.c.dist" "$pcre2_src/pcre2_chartables.c"
    fi
}

phase() {
    local name="$1"; shift
    PHASE_NUM=$((PHASE_NUM + 1))
    local t0 t1 elapsed
    t0=$(date +%s)
    "$@"
    t1=$(date +%s)
    elapsed=$((t1 - t0))
    printf "[%d/%d] %-28s ${GREEN}✓${NC} %ss\n" "$PHASE_NUM" "$TOTAL_PHASES" "$name" "$elapsed"
}

phase_configure() {
    if ! command -v clang++ >/dev/null 2>&1; then fail "clang++ not found in PATH"; fi
    if ! command -v clang >/dev/null 2>&1; then fail "clang not found in PATH"; fi
    CLANG_VERSION="$(clang++ --version | head -n1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)"
    if command -v ld.lld >/dev/null 2>&1 || command -v lld >/dev/null 2>&1; then
        LTO_FLAGS=(-fuse-ld=lld -flto=thin)
    else
        LTO_FLAGS=()
    fi
    setup_pcre2
    mkdir -p \
        "$OBJ_DIR/core/engine" "$OBJ_DIR/core/engine/builtins" "$OBJ_DIR/core/gc" \
        "$OBJ_DIR/core/modules" "$OBJ_DIR/core/runtime" "$OBJ_DIR/core/vm" \
        "$OBJ_DIR/lexer" "$OBJ_DIR/parser" "$OBJ_DIR/parser/ast" \
        "$OBJ_DIR/pcre2" "$OBJ_DIR/utf8proc"
}

phase_thirdparty() {
    local src obj
    for src in "${PCRE2_SOURCES[@]}"; do
        obj="$OBJ_DIR/pcre2/$(basename "${src%.c}").o"
        launch_job "$src" clang "${PCRE2FLAGS[@]}" -c "$src" -o "$obj"
        PCRE2_OBJECTS+=("$obj")
    done
    obj="$OBJ_DIR/utf8proc/utf8proc.o"
    launch_job "third_party/utf8proc/utf8proc.c" clang "${UTF8PROC_FLAGS[@]}" \
        -c third_party/utf8proc/utf8proc.c -o "$obj"
    UTF8PROC_OBJECTS+=("$obj")
    collect_jobs
}

phase_engine() {
    compile_group core/engine CORE_ENGINE_OBJECTS "${CORE_ENGINE_SOURCES[@]}"
    compile_group core/engine/builtins BUILTIN_OBJECTS "${BUILTIN_SOURCES[@]}"
    compile_group core/modules CORE_MODULE_OBJECTS "${CORE_MODULE_SOURCES[@]}"
}

phase_gc() {
    compile_group core/gc CORE_GC_OBJECTS "${CORE_GC_SOURCES[@]}"
}

phase_runtime() {
    compile_group core/runtime CORE_RUNTIME_OBJECTS "${CORE_RUNTIME_SOURCES[@]}"
}

phase_frontend() {
    compile_group lexer LEXER_OBJECTS "${LEXER_SOURCES[@]}"
    compile_group parser PARSER_OBJECTS "${PARSER_SOURCES[@]}"
    compile_group parser/ast AST_OBJECTS "${AST_SOURCES[@]}"
}

phase_vm() {
    compile_group core/vm CORE_VM_OBJECTS "${CORE_VM_SOURCES[@]}"
}

phase_link() {
    local errfile
    errfile=$(mktemp)
    # bash 3.2 (macOS) treats a declared-but-empty array's "${arr[@]}" as unset
    # under set -u, even though its "${#arr[@]}" count works fine -- so LTO_FLAGS
    # (empty when lld isn't found) and STACK_FLAGS (always empty on POSIX; it's
    # build-windows.bat's linker-only /STACK counterpart) are appended via a
    # regular array instead of expanded inline.
    local link_args=("${CXXFLAGS[@]}" "${INCLUDES[@]}")
    [ "${#LTO_FLAGS[@]}" -gt 0 ] && link_args+=("${LTO_FLAGS[@]}")
    link_args+=(-DMAIN_EXECUTABLE -o "$BIN_DIR/quanta" console.cpp
        "${CORE_ENGINE_OBJECTS[@]}" "${BUILTIN_OBJECTS[@]}" "${CORE_GC_OBJECTS[@]}"
        "${CORE_MODULE_OBJECTS[@]}" "${CORE_RUNTIME_OBJECTS[@]}" "${CORE_VM_OBJECTS[@]}"
        "${LEXER_OBJECTS[@]}" "${PARSER_OBJECTS[@]}" "${AST_OBJECTS[@]}"
        "${PCRE2_OBJECTS[@]}" "${UTF8PROC_OBJECTS[@]}" "${LIBS[@]}")
    [ "${#STACK_FLAGS[@]}" -gt 0 ] && link_args+=("${STACK_FLAGS[@]}")
    run_compile "console.cpp (link)" "$errfile" clang++ "${link_args[@]}"
}

# ./build.sh heap-test -> build and run the GC heap unit tests, nothing else
if [[ "${1:-}" == "heap-test" ]]; then
    errfile=$(mktemp)
    if ! clang++ -std=c++20 -Wall -g -O1 -fsanitize=address,undefined -Iinclude \
        -o "$BIN_DIR/heap-test" \
        tests/gc/heap_test.cpp \
        src/core/gc/Heap.cpp src/core/gc/HeapBlock.cpp src/core/gc/BlockAllocator.cpp \
        2>"$errfile"; then
        show_error "$errfile"
        exit 1
    fi
    rm -f "$errfile"
    if ! ASAN_OPTIONS=detect_leaks=0 "$BIN_DIR/heap-test"; then
        fail "heap-test reported failures"
    fi
    echo -e "${GREEN}✓${NC} heap-test passed"
    exit 0
fi

CXXFLAGS=(
    -std=c++20
    -Wall
    -O3
    -march=native
    -mtune=native
    -DQUANTA_VERSION=\"0.9.0.71926\"
    -DPROMISE_STABILITY_FIXED
    -DNATIVE_BUILD
    -DUTF8PROC_STATIC
    -DNDEBUG
    -funroll-loops
    -finline-functions
    -fvectorize
    -fslp-vectorize
    -fomit-frame-pointer
    -fstrict-aliasing
    -fstrict-enums
    -pthread
)

INCLUDES=(-Iinclude -Ithird_party/pcre2/src -Ithird_party/utf8proc -Ithird_party/minicoro)

PCRE2FLAGS=(
    -O3
    -DPCRE2_CODE_UNIT_WIDTH=16
    -DHAVE_CONFIG_H
    -Ithird_party/pcre2/src
    -march=native
    -fomit-frame-pointer
)

UTF8PROC_FLAGS=(
    -O3
    -DUTF8PROC_STATIC
    -Ithird_party/utf8proc
    -march=native
    -fomit-frame-pointer
)

LIBS=(-pthread)
STACK_FLAGS=()

shopt -s nullglob

PCRE2_SOURCES=(
    third_party/pcre2/src/pcre2_auto_possess.c
    third_party/pcre2/src/pcre2_chartables.c
    third_party/pcre2/src/pcre2_chkdint.c
    third_party/pcre2/src/pcre2_compile.c
    third_party/pcre2/src/pcre2_compile_cgroup.c
    third_party/pcre2/src/pcre2_compile_class.c
    third_party/pcre2/src/pcre2_config.c
    third_party/pcre2/src/pcre2_context.c
    third_party/pcre2/src/pcre2_convert.c
    third_party/pcre2/src/pcre2_dfa_match.c
    third_party/pcre2/src/pcre2_error.c
    third_party/pcre2/src/pcre2_extuni.c
    third_party/pcre2/src/pcre2_find_bracket.c
    third_party/pcre2/src/pcre2_jit_compile.c
    third_party/pcre2/src/pcre2_maketables.c
    third_party/pcre2/src/pcre2_match.c
    third_party/pcre2/src/pcre2_match_data.c
    third_party/pcre2/src/pcre2_match_next.c
    third_party/pcre2/src/pcre2_newline.c
    third_party/pcre2/src/pcre2_ord2utf.c
    third_party/pcre2/src/pcre2_pattern_info.c
    third_party/pcre2/src/pcre2_script_run.c
    third_party/pcre2/src/pcre2_serialize.c
    third_party/pcre2/src/pcre2_string_utils.c
    third_party/pcre2/src/pcre2_study.c
    third_party/pcre2/src/pcre2_substitute.c
    third_party/pcre2/src/pcre2_substring.c
    third_party/pcre2/src/pcre2_tables.c
    third_party/pcre2/src/pcre2_ucd.c
    third_party/pcre2/src/pcre2_valid_utf.c
    third_party/pcre2/src/pcre2_xclass.c
)

CORE_ENGINE_SOURCES=(src/core/engine/*.cpp)
# mapfile/readarray needs bash 4.0+ (macOS's stock bash is 3.2) -- a NUL-
# delimited read loop is the portable equivalent.
BUILTIN_SOURCES=()
while IFS= read -r -d '' f; do BUILTIN_SOURCES+=("$f"); done < <(find src/core/engine/builtins -name '*.cpp' -print0 2>/dev/null)
CORE_GC_SOURCES=(src/core/gc/*.cpp)
CORE_MODULE_SOURCES=(src/core/modules/*.cpp)
CORE_RUNTIME_SOURCES=(src/core/runtime/*.cpp)
CORE_VM_SOURCES=(src/core/vm/*.cpp)
LEXER_SOURCES=(src/lexer/*.cpp)
PARSER_SOURCES=(src/parser/*.cpp)
AST_SOURCES=()
while IFS= read -r -d '' f; do AST_SOURCES+=("$f"); done < <(find src/parser/ast -name '*.cpp' -print0 2>/dev/null)

shopt -u nullglob

TOTAL_FILES=$((
    ${#PCRE2_SOURCES[@]} + 1 +
    ${#CORE_ENGINE_SOURCES[@]} +
    ${#BUILTIN_SOURCES[@]} +
    ${#CORE_GC_SOURCES[@]} +
    ${#CORE_MODULE_SOURCES[@]} +
    ${#CORE_RUNTIME_SOURCES[@]} +
    ${#CORE_VM_SOURCES[@]} +
    ${#LEXER_SOURCES[@]} +
    ${#PARSER_SOURCES[@]} +
    ${#AST_SOURCES[@]}
))

PCRE2_OBJECTS=()
UTF8PROC_OBJECTS=()
CORE_ENGINE_OBJECTS=()
BUILTIN_OBJECTS=()
CORE_GC_OBJECTS=()
CORE_MODULE_OBJECTS=()
CORE_RUNTIME_OBJECTS=()
CORE_VM_OBJECTS=()
LEXER_OBJECTS=()
PARSER_OBJECTS=()
AST_OBJECTS=()

TOTAL_PHASES=8
PHASE_NUM=0

echo "$DIVIDER"
phase "Configure project"   phase_configure
phase "Compile third-party" phase_thirdparty
phase "Compile engine"      phase_engine
phase "Compile GC"          phase_gc
phase "Compile runtime"     phase_runtime
phase "Compile front-end"   phase_frontend
phase "Compile VM"          phase_vm
phase "Link executable"     phase_link
echo "$DIVIDER"

BUILD_END=$(date +%s)
TOTAL_TIME=$((BUILD_END - BUILD_START))
# GNU stat (-c) vs BSD/macOS stat (-f) -- try GNU first, fall back to BSD.
SIZE_MB=$(( $(stat -c '%s' "$BIN_DIR/quanta" 2>/dev/null || stat -f '%z' "$BIN_DIR/quanta") / 1048576 ))
PLATFORM="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"

echo -e "${GREEN}✓ Build completed successfully${NC}"
echo
printf "Profile   : release\n"
printf "Platform  : %s\n" "$PLATFORM"
printf "Compiler  : clang %s\n" "$CLANG_VERSION"
printf "Jobs      : %s (parallel)\n" "$JOBS"
printf "Time      : %ss\n" "$TOTAL_TIME"
printf "Binary    : %s (%sMB)\n" "$BIN_DIR/quanta" "$SIZE_MB"
printf "Files     : %d/%d\n" "$COMPILED_FILES" "$TOTAL_FILES"
printf "Warnings  : %d\n" "$WARNING_COUNT"
printf "Errors    : 0\n"
printf "Logs      : %s, %s\n" "$LOG_FILE" "$ERROR_LOG"

echo "[$(date '+%H:%M:%S')] BUILD SUCCESSFUL" >> "$LOG_FILE"
echo "[$(date '+%H:%M:%S')] Compiled files: $COMPILED_FILES/$TOTAL_FILES" >> "$LOG_FILE"
