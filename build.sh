#!/usr/bin/env bash

set -uo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_header() {
    echo -e "${BLUE}===============================================================${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}===============================================================${NC}"
}

print_success() {
    echo -e "${GREEN}[OK] $1${NC}"
}

print_error() {
    echo -e "${RED}[ERROR] $1${NC}"
}

print_info() {
    echo -e "${YELLOW}[INFO] $1${NC}"
}

fail() {
    print_error "$1"
    echo
    echo "Recent errors:"
    if [[ -s "$ERROR_LOG" ]]; then
        tail -n 20 "$ERROR_LOG"
    else
        echo "(no compiler output captured)"
    fi
    echo
    echo "[${SECONDS}s] BUILD FAILED" >> "$LOG_FILE"
    exit 1
}

setup_pcre2() {
    local pcre2_src="third_party/pcre2/src"
    local pcre2_configs="third_party/pcre2_configs"

    if [[ ! -f "$pcre2_src/pcre2.h.generic" ]]; then
        print_info "Initializing PCRE2 submodule..."
        git submodule update --init --recursive third_party/pcre2 || fail "Failed to initialize PCRE2 submodule"
    fi

    if [[ ! -f "$pcre2_src/config.h" ]]; then
        cp "$pcre2_configs/config.h" "$pcre2_src/config.h"
        print_success "Installed config.h from pcre2_configs"
    fi

    if [[ ! -f "$pcre2_src/pcre2.h" ]]; then
        cp "$pcre2_src/pcre2.h.generic" "$pcre2_src/pcre2.h"
        print_success "Generated pcre2.h"
    fi

    if [[ ! -f "$pcre2_src/pcre2_chartables.c" ]]; then
        cp "$pcre2_src/pcre2_chartables.c.dist" "$pcre2_src/pcre2_chartables.c"
        print_success "Generated pcre2_chartables.c"
    fi
}

compile_cpp() {
    local src="$1"
    local out="$2"

    clang++ "${CXXFLAGS[@]}" "${INCLUDES[@]}" -c "$src" -o "$out" 2>>"$ERROR_LOG" || return 1
}

compile_c() {
    local src="$1"
    local out="$2"

    clang "${PCRE2FLAGS[@]}" -c "$src" -o "$out" 2>>"$ERROR_LOG" || return 1
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="build"
OBJ_DIR="$BUILD_DIR/obj"
BIN_DIR="$BUILD_DIR/bin"
LOG_FILE="$BUILD_DIR/build.log"
ERROR_LOG="$BUILD_DIR/errors.log"

mkdir -p "$BUILD_DIR" "$OBJ_DIR" "$BIN_DIR"
: > "$ERROR_LOG"
printf '[%(%Y-%m-%d %H:%M:%S)T] Build started\n' -1 > "$LOG_FILE"

print_header "Building Quanta with Clang"

if ! command -v clang++ >/dev/null 2>&1; then
    fail "clang++ not found in PATH"
fi

if ! command -v clang >/dev/null 2>&1; then
    fail "clang not found in PATH"
fi

CLANG_VERSION="$(clang++ --version | head -n 1)"
print_success "$CLANG_VERSION detected"
echo "[$(date '+%H:%M:%S')] Clang: $CLANG_VERSION" >> "$LOG_FILE"

if command -v ld.lld >/dev/null 2>&1 || command -v lld >/dev/null 2>&1; then
    LTO_FLAGS=(-fuse-ld=lld -flto=thin)
    print_info "LLD detected; ThinLTO enabled"
else
    LTO_FLAGS=()
    print_info "LLD not found; building without ThinLTO"
fi

setup_pcre2

mkdir -p \
    "$OBJ_DIR/core/engine" \
    "$OBJ_DIR/core/gc" \
    "$OBJ_DIR/core/modules" \
    "$OBJ_DIR/core/runtime" \
    "$OBJ_DIR/lexer" \
    "$OBJ_DIR/parser" \
    "$OBJ_DIR/pcre2" \
    "$OBJ_DIR/utf8proc"

CXXFLAGS=(
    -std=c++17
    -Wall
    -O3
    -march=native
    -mtune=native
    -DQUANTA_VERSION=\"0.1.0\"
    -DPROMISE_STABILITY_FIXED
    -DNATIVE_BUILD
    -DUTF8PROC_STATIC
    -funroll-loops
    -finline-functions
    -fvectorize
    -fslp-vectorize
    -fomit-frame-pointer
    -fstrict-aliasing
    -fstrict-enums
    -pthread
)

INCLUDES=(-Iinclude -Ithird_party/pcre2/src -Ithird_party/utf8proc)

PCRE2FLAGS=(
    -O3
    -DPCRE2_CODE_UNIT_WIDTH=8
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
CORE_GC_SOURCES=(src/core/gc/*.cpp)
CORE_MODULE_SOURCES=(src/core/modules/*.cpp)
CORE_RUNTIME_SOURCES=(src/core/runtime/*.cpp)
LEXER_SOURCES=(src/lexer/*.cpp)
PARSER_SOURCES=(src/parser/*.cpp)

shopt -u nullglob

TOTAL_FILES=$((
    ${#PCRE2_SOURCES[@]} +
    ${#CORE_ENGINE_SOURCES[@]} +
    ${#CORE_GC_SOURCES[@]} +
    ${#CORE_MODULE_SOURCES[@]} +
    ${#CORE_RUNTIME_SOURCES[@]} +
    ${#LEXER_SOURCES[@]} +
    ${#PARSER_SOURCES[@]}
))

COMPILED_FILES=0
FAILED_FILES=0
PCRE2_OBJECTS=()
UTF8PROC_OBJECTS=()
CORE_ENGINE_OBJECTS=()
CORE_GC_OBJECTS=()
CORE_MODULE_OBJECTS=()
CORE_RUNTIME_OBJECTS=()
LEXER_OBJECTS=()
PARSER_OBJECTS=()

echo
print_header "Compilation Phase"

echo "[0/4] Compiling PCRE2..."
echo "[$(date '+%H:%M:%S')] === PCRE2 ===" >> "$LOG_FILE"
for src in "${PCRE2_SOURCES[@]}"; do
    obj="$OBJ_DIR/pcre2/$(basename "${src%.c}").o"
    printf '  [%d/%d] %s\n' $((COMPILED_FILES + 1)) "$TOTAL_FILES" "$(basename "$src")"
    if ! compile_c "$src" "$obj"; then
        echo "[$(date '+%H:%M:%S')] ERROR compiling $src" >> "$LOG_FILE"
        ((FAILED_FILES++))
        fail "PCRE2 compilation failed at $src"
    fi
    PCRE2_OBJECTS+=("$obj")
    ((COMPILED_FILES++))
done
print_success "PCRE2 + JIT compiled"

echo
echo "[0/4] Compiling utf8proc..."
echo "[$(date '+%H:%M:%S')] === UTF8PROC ===" >> "$LOG_FILE"
obj="$OBJ_DIR/utf8proc/utf8proc.o"
if ! clang "${UTF8PROC_FLAGS[@]}" -c third_party/utf8proc/utf8proc.c -o "$obj" 2>>"$ERROR_LOG"; then
    echo "[$(date '+%H:%M:%S')] ERROR compiling utf8proc.c" >> "$LOG_FILE"
    fail "utf8proc compilation failed"
fi
UTF8PROC_OBJECTS+=("$obj")
print_success "utf8proc compiled"

echo
echo "[1/4] Compiling core engine modules..."
echo "[$(date '+%H:%M:%S')] === CORE ENGINE ===" >> "$LOG_FILE"
for src in "${CORE_ENGINE_SOURCES[@]}"; do
    obj="$OBJ_DIR/core/engine/$(basename "${src%.cpp}").o"
    printf '  [%d/%d] %s\n' $((COMPILED_FILES + 1)) "$TOTAL_FILES" "$(basename "$src")"
    if ! compile_cpp "$src" "$obj"; then
        echo "[$(date '+%H:%M:%S')] ERROR compiling $src" >> "$LOG_FILE"
        ((FAILED_FILES++))
        fail "Compilation failed at $src"
    fi
    CORE_ENGINE_OBJECTS+=("$obj")
    ((COMPILED_FILES++))
done

echo
echo "[1/4] Compiling garbage collector..."
echo "[$(date '+%H:%M:%S')] === GARBAGE COLLECTOR ===" >> "$LOG_FILE"
for src in "${CORE_GC_SOURCES[@]}"; do
    obj="$OBJ_DIR/core/gc/$(basename "${src%.cpp}").o"
    printf '  [%d/%d] %s\n' $((COMPILED_FILES + 1)) "$TOTAL_FILES" "$(basename "$src")"
    if ! compile_cpp "$src" "$obj"; then
        echo "[$(date '+%H:%M:%S')] ERROR compiling $src" >> "$LOG_FILE"
        ((FAILED_FILES++))
        fail "Compilation failed at $src"
    fi
    CORE_GC_OBJECTS+=("$obj")
    ((COMPILED_FILES++))
done

echo
echo "[1/4] Compiling core modules..."
echo "[$(date '+%H:%M:%S')] === CORE MODULES ===" >> "$LOG_FILE"
for src in "${CORE_MODULE_SOURCES[@]}"; do
    obj="$OBJ_DIR/core/modules/$(basename "${src%.cpp}").o"
    printf '  [%d/%d] %s\n' $((COMPILED_FILES + 1)) "$TOTAL_FILES" "$(basename "$src")"
    if ! compile_cpp "$src" "$obj"; then
        echo "[$(date '+%H:%M:%S')] ERROR compiling $src" >> "$LOG_FILE"
        ((FAILED_FILES++))
        fail "Compilation failed at $src"
    fi
    CORE_MODULE_OBJECTS+=("$obj")
    ((COMPILED_FILES++))
done

echo
echo "[2/4] Compiling runtime library..."
echo "[$(date '+%H:%M:%S')] === RUNTIME ===" >> "$LOG_FILE"
for src in "${CORE_RUNTIME_SOURCES[@]}"; do
    obj="$OBJ_DIR/core/runtime/$(basename "${src%.cpp}").o"
    printf '  [%d/%d] %s\n' $((COMPILED_FILES + 1)) "$TOTAL_FILES" "$(basename "$src")"
    if ! compile_cpp "$src" "$obj"; then
        echo "[$(date '+%H:%M:%S')] ERROR compiling $src" >> "$LOG_FILE"
        ((FAILED_FILES++))
        fail "Compilation failed at $src"
    fi
    CORE_RUNTIME_OBJECTS+=("$obj")
    ((COMPILED_FILES++))
done

echo
echo "[3/4] Compiling lexer..."
echo "[$(date '+%H:%M:%S')] === LEXER ===" >> "$LOG_FILE"
for src in "${LEXER_SOURCES[@]}"; do
    obj="$OBJ_DIR/lexer/$(basename "${src%.cpp}").o"
    printf '  [%d/%d] %s\n' $((COMPILED_FILES + 1)) "$TOTAL_FILES" "$(basename "$src")"
    if ! compile_cpp "$src" "$obj"; then
        echo "[$(date '+%H:%M:%S')] ERROR compiling $src" >> "$LOG_FILE"
        ((FAILED_FILES++))
        fail "Compilation failed at $src"
    fi
    LEXER_OBJECTS+=("$obj")
    ((COMPILED_FILES++))
done

echo
echo "[4/4] Compiling parser..."
echo "[$(date '+%H:%M:%S')] === PARSER ===" >> "$LOG_FILE"
for src in "${PARSER_SOURCES[@]}"; do
    obj="$OBJ_DIR/parser/$(basename "${src%.cpp}").o"
    printf '  [%d/%d] %s\n' $((COMPILED_FILES + 1)) "$TOTAL_FILES" "$(basename "$src")"
    if ! compile_cpp "$src" "$obj"; then
        echo "[$(date '+%H:%M:%S')] ERROR compiling $src" >> "$LOG_FILE"
        ((FAILED_FILES++))
        fail "Compilation failed at $src"
    fi
    PARSER_OBJECTS+=("$obj")
    ((COMPILED_FILES++))
done

echo
print_header "Linking Phase"
echo "[LINK] Creating executable with ThinLTO..."
echo "[$(date '+%H:%M:%S')] === LINKING ===" >> "$LOG_FILE"

if ! clang++ "${CXXFLAGS[@]}" "${INCLUDES[@]}" "${LTO_FLAGS[@]}" -DMAIN_EXECUTABLE \
    -o "$BIN_DIR/quanta" \
    console.cpp \
    "${CORE_ENGINE_OBJECTS[@]}" \
    "${CORE_GC_OBJECTS[@]}" \
    "${CORE_MODULE_OBJECTS[@]}" \
    "${CORE_RUNTIME_OBJECTS[@]}" \
    "${LEXER_OBJECTS[@]}" \
    "${PARSER_OBJECTS[@]}" \
    "${PCRE2_OBJECTS[@]}" \
    "${UTF8PROC_OBJECTS[@]}" \
    "${LIBS[@]}" \
    "${STACK_FLAGS[@]}" \
    2>>"$ERROR_LOG"; then
    fail "Linking failed"
fi

print_success "Executable created"
echo "[$(date '+%H:%M:%S')] Linking successful" >> "$LOG_FILE"

echo
print_header "Build Success"
echo
echo "  [OK] Files compiled: $COMPILED_FILES / $TOTAL_FILES"
echo "  [OK] Output: $BIN_DIR/quanta"
if [[ -f "$BIN_DIR/quanta" ]]; then
    SIZE_MB=$(( $(stat -c '%s' "$BIN_DIR/quanta") / 1048576 ))
    echo "  [OK] Size: ${SIZE_MB}MB"
fi
echo "  [OK] Optimizations: O3 + ThinLTO + AVX2"
echo
echo "  Build log: $LOG_FILE"
echo "  Error log: $ERROR_LOG"
echo
echo "[$(date '+%H:%M:%S')] BUILD SUCCESSFUL" >> "$LOG_FILE"
echo "[$(date '+%H:%M:%S')] Compiled files: $COMPILED_FILES/$TOTAL_FILES" >> "$LOG_FILE"