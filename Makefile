LOG_FILE = build/build.log
ERROR_LOG = build/errors.log

C_RESET = \033[0m
C_GREEN = \033[92m
C_BLUE = \033[94m
C_YELLOW = \033[93m
C_RED = \033[91m
C_CYAN = \033[96m

CXX = clang++
CXXFLAGS = -std=c++20 -Wall -O3 -march=native -mtune=native
CXXFLAGS += -DQUANTA_VERSION=\"0.9.3\"
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

# Directories
CORE_SRC = src/core
LEXER_SRC = src/lexer
PARSER_SRC = src/parser
BUILD_DIR = build
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
.PHONY: all clean debug release asan setup-deps heap-test shape-test

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
	@if [ ! -f "$(PCRE2_DIR)/config.h" ]; then \
	    cp "$(PCRE2_DIR)/config.h.generic" "$(PCRE2_DIR)/config.h"; \
	    echo "[OK] Generated config.h"; \
	fi
	@if [ ! -f "$(PCRE2_DIR)/pcre2.h" ]; then \
	    cp "$(PCRE2_DIR)/pcre2.h.generic" "$(PCRE2_DIR)/pcre2.h"; \
	    echo "[OK] Generated pcre2.h"; \
	fi
	@if [ ! -f "$(PCRE2_DIR)/pcre2_chartables.c" ]; then \
	    cp "$(PCRE2_DIR)/pcre2_chartables.c.dist" "$(PCRE2_DIR)/pcre2_chartables.c"; \
	    echo "[OK] Generated pcre2_chartables.c"; \
	fi

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
	@ar rcs $@ $^
	@echo "[OK] Library created: $@"

# Main console executable
$(BIN_DIR)/quanta$(EXE_EXT): $(CONSOLE_MAIN) $(LIBQUANTA) $(MIMALLOC_OVERRIDE_OBJ)
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
clean:
	@echo "[CLEAN] Cleaning build files..."
	@$(RM) $(BUILD_DIR)
	@echo "[OK] Clean completed"

# Prevent deletion of intermediate files
.PRECIOUS: $(OBJ_DIR)/%.o
