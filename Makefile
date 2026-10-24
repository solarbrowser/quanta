BUILD_DIR ?= build
LOG_FILE = $(BUILD_DIR)/build.log
ERROR_LOG = $(BUILD_DIR)/errors.log

C_RESET = \033[0m
C_GREEN = \033[92m
C_BLUE = \033[94m
C_YELLOW = \033[93m
C_RED = \033[91m
C_CYAN = \033[96m

CXX = clang++
CXXFLAGS = -std=c++20 -Wall -O3 -march=native -mtune=native
CXXFLAGS += -DQUANTA_VERSION=\"2.0.0.alpha\"
CXXFLAGS += -DPROMISE_STABILITY_FIXED -DNATIVE_BUILD -DUTF8PROC_STATIC
CXXFLAGS += -funroll-loops -finline-functions
CXXFLAGS += -fvectorize -fslp-vectorize
CXXFLAGS += -fomit-frame-pointer
CXXFLAGS += -fstrict-aliasing -fstrict-enums
# The stack canary is a mitigation for overflowing a stack buffer, and the
# interpreter and runtime have none to overflow -- their arrays are sized from
# the chunk the compiler just produced. It is kept where bytes the engine did
# not produce are first handled: the lexer, the parser and their buffers,
# which are cold once a script is running and so cost nothing to protect.
#
# This is a deliberate trade, not an oversight, and it is not something a test
# run can vouch for: a mitigation only matters once there is a bug to exploit,
# so a green suite says nothing about it either way. What the split rests on is
# structural -- past the parser, nothing is sized from bytes this engine did
# not produce. The one exception is VM::run's register bank, indexed by
# operands our own compiler emitted, and that is covered by
# validate_chunk_registers instead (Bytecode.cpp), which rejects a bad operand
# where it is introduced rather than noticing the damage at return. Reverting
# is one line: drop -fno-stack-protector.
CXXFLAGS += -fno-stack-protector
# Applied per-directory below, to the lexer and parser objects only.
HARDEN_FLAGS = -fstack-protector-strong
CXXFLAGS += -pthread

LTO_FLAGS = -fuse-ld=lld -flto=thin

# Whole-program ThinLTO: LTO_FLAGS alone only covers console.cpp, so the engine's own objects were never optimised
# across translation units. Measured -5% cycles on code.js and -11% on markdown.js, nothing lost elsewhere; full LTO
# gave the same. Linux only for now (the archive needs llvm-ar). The embedding archive is pre-linked into one machine-code
# object so a program with a different toolchain can still link it.
AR = ar
USE_LTO = 0
ifeq ($(shell uname -s),Linux)
CXXFLAGS += -flto=thin
AR = llvm-ar
USE_LTO = 1
endif

# Profile-guided optimization (tools/pgo/pgo.sh regenerates the profile): PGO_RAW_DIR builds an instrumented
# binary that writes its counters there, PGO_PROFILE builds with a merged profile.
# The checked-in profile is applied unless PGO=0; it only steers inlining and code layout, so a function the
# current source no longer matches is simply left unprofiled.
PGO ?= 1
PGO_RAW_DIR ?=
PGO_PROFILE ?=
ifneq ($(PGO),0)
ifeq ($(PGO_RAW_DIR),)
ifeq ($(PGO_PROFILE),)
PGO_PROFILE := $(wildcard tools/pgo/quanta.profdata)
endif
endif
endif
ifneq ($(PGO_RAW_DIR),)
CXXFLAGS += -fprofile-generate=$(PGO_RAW_DIR) -DQUANTA_PGO_INSTRUMENT
LTO_FLAGS += -fprofile-generate=$(PGO_RAW_DIR)
endif
ifneq ($(PGO_PROFILE),)
CXXFLAGS += -fprofile-use=$(PGO_PROFILE) -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
endif

DEBUG_FLAGS = -g -DDEBUG -O0 -DQUANTA_VALIDATE_BYTECODE

# On top of the default -O3 set, not -O0/DEBUG_FLAGS: some GC-rooting bugs
# only reproduce under -O3's register allocation, never under -O0/-O1.
# use-after-return is off because it is incompatible with the collector, not
# because it is noisy: it moves a function's locals off the real stack into a
# heap-allocated fake frame, and the conservative stack scan that finds our
# roots only walks the real stack. Every local holding the sole reference to a
# cell becomes invisible, so live objects get swept and the damage surfaces
# far away as corrupted string bytes -- under GC stress the same script
# returned a different answer on every run, with nothing wrong in the engine.
ASAN_FLAGS = -g -fsanitize=address,undefined -fno-omit-frame-pointer \
             -fsanitize-address-use-after-return=never -DQUANTA_VALIDATE_BYTECODE

PCRE2_DIR = third_party/pcre2/src
PCRE2_CFLAGS = -O3 -DPCRE2_CODE_UNIT_WIDTH=16 -DHAVE_CONFIG_H -I$(PCRE2_DIR) -march=native -fomit-frame-pointer

MIMALLOC_DIR = third_party/mimalloc
# One object for the whole library: src/static.c includes every other source.
# No MI_MALLOC_OVERRIDE: the C library's own malloc is left alone and
# only C++ operator new is redirected -- see src/core/runtime/MiMalloc.cpp.
#
# Compiled as C, which is what upstream builds by default and what its own
# sources are written against -- a few of them hold an _Atomic in a way that
# is C and not C++. Only the Windows build compiles it as C++, and only
# because it has to; see build-windows.bat.
MIMALLOC_CFLAGS = -O3 -DNDEBUG -I$(MIMALLOC_DIR)/include -fomit-frame-pointer
MIMALLOC_OBJS = $(OBJ_DIR)/mimalloc/static.o
# Named on the link line rather than left to the archive. Replacing operator
# new is only a replacement if the object that does it is actually linked, and
# an archive member is pulled in only to resolve something undefined -- which
# operator new never is, because libstdc++ already defines it. Left to -lquanta
# this object is silently dropped and the program keeps the standard allocator.
MIMALLOC_OVERRIDE_OBJ = $(OBJ_DIR)/core/runtime/MiMalloc.o

UTF8PROC_DIR = third_party/utf8proc
UTF8PROC_CFLAGS = -O3 -DUTF8PROC_STATIC -I$(UTF8PROC_DIR) -march=native -fomit-frame-pointer
UTF8PROC_SRCS = $(UTF8PROC_DIR)/utf8proc.c
UTF8PROC_OBJS = $(OBJ_DIR)/utf8proc/utf8proc.o
PCRE2_SRCS = \
    $(PCRE2_DIR)/pcre2_auto_possess.c \
    $(PCRE2_DIR)/pcre2_chartables.c \
    $(PCRE2_DIR)/pcre2_chkdint.c \
    $(PCRE2_DIR)/pcre2_compile.c \
    $(PCRE2_DIR)/pcre2_compile_cgroup.c \
    $(PCRE2_DIR)/pcre2_compile_class.c \
    $(PCRE2_DIR)/pcre2_config.c \
    $(PCRE2_DIR)/pcre2_context.c \
    $(PCRE2_DIR)/pcre2_convert.c \
    $(PCRE2_DIR)/pcre2_dfa_match.c \
    $(PCRE2_DIR)/pcre2_error.c \
    $(PCRE2_DIR)/pcre2_extuni.c \
    $(PCRE2_DIR)/pcre2_find_bracket.c \
    $(PCRE2_DIR)/pcre2_jit_compile.c \
    $(PCRE2_DIR)/pcre2_maketables.c \
    $(PCRE2_DIR)/pcre2_match.c \
    $(PCRE2_DIR)/pcre2_match_data.c \
    $(PCRE2_DIR)/pcre2_match_next.c \
    $(PCRE2_DIR)/pcre2_newline.c \
    $(PCRE2_DIR)/pcre2_ord2utf.c \
    $(PCRE2_DIR)/pcre2_pattern_info.c \
    $(PCRE2_DIR)/pcre2_script_run.c \
    $(PCRE2_DIR)/pcre2_serialize.c \
    $(PCRE2_DIR)/pcre2_string_utils.c \
    $(PCRE2_DIR)/pcre2_study.c \
    $(PCRE2_DIR)/pcre2_substitute.c \
    $(PCRE2_DIR)/pcre2_substring.c \
    $(PCRE2_DIR)/pcre2_tables.c \
    $(PCRE2_DIR)/pcre2_ucd.c \
    $(PCRE2_DIR)/pcre2_valid_utf.c \
    $(PCRE2_DIR)/pcre2_xclass.c
PCRE2_OBJS = $(PCRE2_SRCS:$(PCRE2_DIR)/%.c=$(OBJ_DIR)/pcre2/%.o)

INCLUDES = -Iinclude -I$(PCRE2_DIR) -I$(UTF8PROC_DIR) -Ithird_party/minicoro -I$(MIMALLOC_DIR)/include

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

# Platform-specific settings
ifeq ($(UNAME_S),Windows)
    LIBS = -lws2_32 -lpowrprof -lsetupapi -lwinmm -lole32 -lshell32
    STACK_FLAGS = -Wl,/stack:67108864
    LDFLAGS = -fuse-ld=lld
    EXE_EXT = .exe
    RM = rm -rf
    MKDIR_P = powershell -Command "New-Item -ItemType Directory -Force -Path"
else ifeq ($(UNAME_S),Linux)
    LIBS =
    STACK_FLAGS = -Wl,-z,stack-size=16777216
    LDFLAGS =
    EXE_EXT =
    RM = rm -rf
    MKDIR_P = mkdir -p
else ifeq ($(UNAME_S),Darwin)
    LIBS =
    STACK_FLAGS =
    LDFLAGS =
    EXE_EXT =
    RM = rm -rf
    MKDIR_P = mkdir -p
else
    LIBS = -lws2_32 -lpowrprof -lsetupapi -lwinmm -lole32 -lshell32
    STACK_FLAGS = -Wl,/stack:67108864
    LDFLAGS = -fuse-ld=lld
    EXE_EXT = .exe
    RM = rm -rf
    MKDIR_P = powershell -Command "New-Item -ItemType Directory -Force -Path"
endif

# ICU4X: the Intl implementation. Off by default; `make ICU4X=1` builds third_party/quanta_icu (the C API of the
# ICU4X submodule, as a static library with no data inside) and links it in. The locale data it formats from is a
# separate file, built by `make icu-data` and loaded when the first Intl object is made (see tools/icu/README.md).
ICU4X ?= 0
ICU4X_LIB = third_party/quanta_icu/target/release/libquanta_icu.a
ifeq ($(ICU4X),1)
CXXFLAGS += -DQUANTA_ICU4X
INCLUDES += -Ithird_party/icu4x/ffi/capi/bindings/cpp
LIBS += $(ICU4X_LIB) -lpthread -ldl -lm
endif

$(ICU4X_LIB): FORCE
	@cargo build --release --manifest-path third_party/quanta_icu/Cargo.toml

# The locale data for Intl: every CLDR locale and every ICU4X data marker, as one blob (about 10 MB).
ICU4X_DATA ?= $(BUILD_DIR)/quanta-icu.postcard
icu-data:
	@$(MKDIR_P) $(dir $(ICU4X_DATA))
	@cargo run --release --features unstable --manifest-path third_party/icu4x/provider/icu4x-datagen/Cargo.toml -- \
	    --format blob --markers all --locales full --include-collations search-all -W -o $(ICU4X_DATA)
	@echo "[OK] ICU data: $(ICU4X_DATA)"

# Directories
CORE_SRC = src/core
LEXER_SRC = src/lexer
PARSER_SRC = src/parser
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin

rwildcard = $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2)$(filter $(subst *,%,$2),$d))

# Core source files
CORE_SOURCES = $(call rwildcard,$(CORE_SRC)/,*.cpp)
LEXER_SOURCES = $(call rwildcard,$(LEXER_SRC)/,*.cpp)
PARSER_SOURCES = $(call rwildcard,$(PARSER_SRC)/,*.cpp)

# Object files
CORE_OBJECTS = $(CORE_SOURCES:$(CORE_SRC)/%.cpp=$(OBJ_DIR)/core/%.o)
LEXER_OBJECTS = $(LEXER_SOURCES:$(LEXER_SRC)/%.cpp=$(OBJ_DIR)/lexer/%.o)
PARSER_OBJECTS = $(PARSER_SOURCES:$(PARSER_SRC)/%.cpp=$(OBJ_DIR)/parser/%.o)

ALL_OBJECTS = $(CORE_OBJECTS) $(LEXER_OBJECTS) $(PARSER_OBJECTS) $(PCRE2_OBJS) $(UTF8PROC_OBJS) $(MIMALLOC_OBJS)

# Header dependency tracking (-MMD): a header edit rebuilds every dependent TU.
-include $(CORE_OBJECTS:.o=.d) $(LEXER_OBJECTS:.o=.d) $(PARSER_OBJECTS:.o=.d)

# Static library
LIBQUANTA = $(BUILD_DIR)/libquanta.a

# Console main source
CONSOLE_MAIN = console.cpp

# Main targets
.PHONY: all clean clean-all debug release asan setup-deps heap-test shape-test lib embed-test icu-data

# Bare `make` builds release directly (no separate opt-in step needed).
.DEFAULT_GOAL := release

setup-deps:
	@if [ ! -f "$(PCRE2_DIR)/pcre2.h.generic" ]; then \
	    echo "[INFO] Initializing PCRE2 submodule..."; \
	    git submodule update --init third_party/pcre2; \
	fi
	@if [ ! -f "$(MIMALLOC_DIR)/src/static.c" ]; then \
	    echo "[INFO] Initializing mimalloc submodule..."; \
	    git submodule update --init third_party/mimalloc; \
	fi
	@if [ ! -f "$(PCRE2_DIR)/pcre2.h" ]; then \
	    cp "$(PCRE2_DIR)/pcre2.h.generic" "$(PCRE2_DIR)/pcre2.h"; \
	    echo "[OK] Generated pcre2.h"; \
	fi
	@if [ ! -f "$(PCRE2_DIR)/pcre2_chartables.c" ]; then \
	    cp "$(PCRE2_DIR)/pcre2_chartables.c.dist" "$(PCRE2_DIR)/pcre2_chartables.c"; \
	    echo "[OK] Generated pcre2_chartables.c"; \
	fi

# PCRE2's config decides whether it has Unicode and JIT support, so the copy
# kept in this repo is the one that has to be in place -- the generic template
# next to it enables neither, and regexes then fail to compile as "invalid".
# FORCE makes the recipe run every time; the file only changes (and the PCRE2
# objects only rebuild) when what is there differs.
.PHONY: FORCE
FORCE:
$(PCRE2_DIR)/config.h: third_party/pcre2_configs/config.h FORCE | setup-deps
	@if ! cmp -s "third_party/pcre2_configs/config.h" "$@"; then \
	    cp "third_party/pcre2_configs/config.h" "$@"; \
	    echo "[OK] Installed PCRE2 config.h (Unicode and JIT support)"; \
	fi

$(PCRE2_OBJS): $(PCRE2_DIR)/config.h

all: setup-deps build_header $(LIBQUANTA) $(BIN_DIR)/quanta$(EXE_EXT) build_footer

build_header:
	@echo ""
	@echo "==============================================================="
	@echo "  Building Quanta ($(UNAME_S))"
	@echo "==============================================================="
	@echo ""
	@mkdir -p $(BUILD_DIR)
	@date '+[%Y-%m-%d %H:%M:%S] Build started' > $(LOG_FILE) 2>/dev/null || echo "Build started" > $(LOG_FILE)

build_footer:
	@echo ""
	@echo "==============================================================="
	@echo "  Build Success!"
	@echo "==============================================================="
	@echo ""
	@echo "  [OK] Executable: $(BIN_DIR)/quanta$(EXE_EXT)"
	@echo "  [OK] Optimizations: O3 + ThinLTO + AVX2"
	@echo "  [OK] Build log: $(LOG_FILE)"
	@echo ""
	@date '+[%Y-%m-%d %H:%M:%S] Build completed' >> $(LOG_FILE) 2>/dev/null || echo "Build completed" >> $(LOG_FILE)

# Static library for embedding
$(LIBQUANTA): $(ALL_OBJECTS)
	@echo "[LIB] Creating Quanta static library..."
	@echo "[LIB] Creating static library" >> $(LOG_FILE)
	@$(AR) rcs $@ $^
	@echo "[OK] Library created: $@"

# Embedding: the engine without console.cpp, in $(BUILD_DIR)/lib. Unlike
# $(LIBQUANTA) above (which only the console executable links, and which is why
# MiMalloc.o can sit in it), this archive leaves the operator-new replacement out:
# it changes allocation for the whole process, which is the embedding program's
# decision, and an archive member nothing references is dropped by the linker
# anyway. It is shipped beside the archive as a plain object, which a link line
# always includes. Mirrors `./build.sh lib`.
EMBED_LIB_DIR = $(BUILD_DIR)/lib
EMBED_LIB = $(EMBED_LIB_DIR)/libquanta.a
EMBED_OVERRIDE = $(EMBED_LIB_DIR)/quanta_mimalloc_override.o

lib: setup-deps $(EMBED_LIB) $(EMBED_OVERRIDE)
	@echo "[OK] Library: $(EMBED_LIB)"

$(EMBED_LIB): $(filter-out $(MIMALLOC_OVERRIDE_OBJ),$(ALL_OBJECTS))
	@$(MKDIR_P) $(EMBED_LIB_DIR)
	@$(RM) $@
ifeq ($(USE_LTO),1)
	@$(CXX) $(CXXFLAGS) -fuse-ld=lld -nostdlib -no-pie -Wl,-r -o $(EMBED_LIB_DIR)/quanta_all.o $^
	@$(AR) rcs $@ $(EMBED_LIB_DIR)/quanta_all.o
	@$(RM) $(EMBED_LIB_DIR)/quanta_all.o
else
	@$(AR) rcs $@ $^
endif

$(EMBED_OVERRIDE): $(MIMALLOC_OVERRIDE_OBJ)
	@$(MKDIR_P) $(EMBED_LIB_DIR)
	@cp $< $@

# Acceptance tests for the embedding surface, linked against the archive the
# way an embedder would.
embed-test: lib
	@$(MKDIR_P) $(BIN_DIR)
	@echo "[TEST] Building embed-test..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(BIN_DIR)/embed-test tests/embed/embed_test.cpp $(EMBED_LIB) $(LIBS)
	@$(BIN_DIR)/embed-test

# Main console executable
$(BIN_DIR)/quanta$(EXE_EXT): $(CONSOLE_MAIN) $(LIBQUANTA) $(MIMALLOC_OVERRIDE_OBJ) $(if $(filter 1,$(ICU4X)),$(ICU4X_LIB))
	@$(MKDIR_P) $(BIN_DIR)
	@echo "[LINK] Linking with ThinLTO..."
	@echo "[LINK] Creating executable" >> $(LOG_FILE)
	@$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) $(LTO_FLAGS) -DMAIN_EXECUTABLE -o $@ $< $(MIMALLOC_OVERRIDE_OBJ) -L$(BUILD_DIR) -lquanta $(LIBS) $(STACK_FLAGS) 2>> $(ERROR_LOG) || (echo "[ERROR] Linking failed" >> $(LOG_FILE) && exit 1)
	@echo "[OK] Executable built: $@"

# Object file compilation - create directories and compile
$(OBJ_DIR)/core/%.o: $(CORE_SRC)/%.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "[BUILD] Compiling core: $<"
	@echo "[BUILD] $<" >> $(LOG_FILE)
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@ 2>> $(ERROR_LOG) || (echo "[ERROR] Failed: $<" >> $(LOG_FILE) && exit 1)

$(OBJ_DIR)/lexer/%.o: $(LEXER_SRC)/%.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "[BUILD] Compiling lexer: $<"
	@echo "[BUILD] $<" >> $(LOG_FILE)
	@$(CXX) $(CXXFLAGS) $(HARDEN_FLAGS) $(INCLUDES) -MMD -MP -c $< -o $@ 2>> $(ERROR_LOG) || (echo "[ERROR] Failed: $<" >> $(LOG_FILE) && exit 1)

$(OBJ_DIR)/parser/%.o: $(PARSER_SRC)/%.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "[BUILD] Compiling parser: $<"
	@echo "[BUILD] $<" >> $(LOG_FILE)
	@$(CXX) $(CXXFLAGS) $(HARDEN_FLAGS) $(INCLUDES) -MMD -MP -c $< -o $@ 2>> $(ERROR_LOG) || (echo "[ERROR] Failed: $<" >> $(LOG_FILE) && exit 1)

$(OBJ_DIR)/pcre2/%.o: $(PCRE2_DIR)/%.c
	@$(MKDIR_P) $(dir $@)
	@clang $(PCRE2_CFLAGS) -c $< -o $@ 2>> $(ERROR_LOG) || (echo "[ERROR] Failed: $<" >> $(LOG_FILE) && exit 1)

$(OBJ_DIR)/utf8proc/utf8proc.o: $(UTF8PROC_DIR)/utf8proc.c
	@$(MKDIR_P) $(dir $@)
	@clang $(UTF8PROC_CFLAGS) -c $< -o $@ 2>> $(ERROR_LOG) || (echo "[ERROR] Failed: $<" >> $(LOG_FILE) && exit 1)

$(OBJ_DIR)/mimalloc/static.o: $(MIMALLOC_DIR)/src/static.c
	@$(MKDIR_P) $(dir $@)
	@clang $(MIMALLOC_CFLAGS) -c $< -o $@ 2>> $(ERROR_LOG) || (echo "[ERROR] Failed: $<" >> $(LOG_FILE) && exit 1)

# Debug build
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: all

# Release build: strips asserts on top of the default optimization set.
release: CXXFLAGS += -DNDEBUG
release: all

# ASan+UBSan build for GC/memory-safety verification. Run the resulting
# binary with ASAN_OPTIONS=detect_stack_use_after_return=0 -- ASan's fake-
# stack relocates locals off the real stack, which our conservative GC
# stack scan can't see, causing false-positive premature collections
# (confirmed: both the "inc is not a function" and object-literal SEGV
# bugs vanish with this option set, at 15x their normal repro iteration
# count). Real release builds never have ASan, so this is a test-harness
# setting, not a product bug.
# Release speed with the bytecode validator on, so a full conformance run can
# check every chunk the compiler emits without paying for a sanitizer.
validate: CXXFLAGS += -DQUANTA_VALIDATE_BYTECODE
validate: all

asan: CXXFLAGS += $(ASAN_FLAGS)
# mimalloc's global operator new/delete override and ASan's own allocator
# interception both want to own the same symbols; linked together, some
# allocations end up on the side the other doesn't track, which ASan then
# reports as a heap-buffer-overflow on a "wild pointer" it has no allocation
# record for -- an allocator collision, not a bug in anything Quanta itself
# allocates. Found chasing what looked like a real corruption in Environment's
# call-frame allocation (code.js under this target, 100% reproducible): the
# same crash persisted across every candidate fix in that code, but vanished
# for good the moment this object was left off the link line, on the same
# binary otherwise. Left out of this target only -- the normal build still
# gets mimalloc.
asan: MIMALLOC_OVERRIDE_OBJ :=
asan: all
	@echo ""
	@echo "  [NOTE] Run with: ASAN_OPTIONS=detect_stack_use_after_return=0 $(BIN_DIR)/quanta$(EXE_EXT) <script.js>"

# GC heap unit tests (standalone; compiles only the gc/ sources)
HEAP_TEST_SRCS = tests/gc/heap_test.cpp \
                 src/core/gc/Heap.cpp \
                 src/core/gc/HeapBlock.cpp \
                 src/core/gc/BlockAllocator.cpp

heap-test: $(HEAP_TEST_SRCS)
	@mkdir -p $(BIN_DIR)
	@echo "[TEST] Building heap-test..."
	@$(CXX) -std=c++20 -Wall -g -O1 -fsanitize=address,undefined -Iinclude \
		-o $(BIN_DIR)/heap-test $(HEAP_TEST_SRCS)
	@ASAN_OPTIONS=detect_leaks=0 $(BIN_DIR)/heap-test  # the heap leaks chunks/snapshots at exit by design

# Shape unit tests (standalone; no GC/Object dependency)
SHAPE_TEST_SRCS = tests/runtime/shape_test.cpp \
                  src/core/runtime/Shape.cpp \
                  src/core/runtime/SmallMapPool.cpp

shape-test: $(SHAPE_TEST_SRCS)
	@mkdir -p $(BIN_DIR)
	@echo "[TEST] Building shape-test..."
	@$(CXX) -std=c++20 -Wall -g -O1 -fsanitize=address,undefined -Iinclude \
		-o $(BIN_DIR)/shape-test $(SHAPE_TEST_SRCS)
	@$(BIN_DIR)/shape-test

# Clean
# The Intl data blob takes a quarter of an hour to make, so a clean keeps it (make clean-all removes it too).
clean:
	@echo "[CLEAN] Cleaning build files..."
	@if [ -f $(ICU4X_DATA) ]; then mv $(ICU4X_DATA) $(BUILD_DIR).icu-keep; fi
	@$(RM) $(BUILD_DIR)
	@if [ -f $(BUILD_DIR).icu-keep ]; then $(MKDIR_P) $(dir $(ICU4X_DATA)) && mv $(BUILD_DIR).icu-keep $(ICU4X_DATA); fi
	@echo "[OK] Clean completed"

clean-all:
	@$(RM) $(BUILD_DIR)
	@echo "[OK] Clean completed, Intl data removed too"

# Prevent deletion of intermediate files
.PRECIOUS: $(OBJ_DIR)/%.o
