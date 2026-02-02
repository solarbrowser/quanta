# =============================================================================
# Quanta JavaScript Engine v0.1.0
# Universal Build System (Windows/Linux/macOS)
# =============================================================================

# Build logging
LOG_FILE = build/build.log
ERROR_LOG = build/errors.log

# Terminal colors (ANSI escape codes)
C_RESET = \033[0m
C_GREEN = \033[92m
C_BLUE = \033[94m
C_YELLOW = \033[93m
C_RED = \033[91m
C_CYAN = \033[96m

# Compiler and optimization flags (Clang-optimized)
CXX = clang++
CXXFLAGS = -std=c++17 -Wall -O3 -fPIC -march=native -mtune=native
CXXFLAGS += -DQUANTA_VERSION="0.1.0"
CXXFLAGS += -DPROMISE_STABILITY_FIXED -DNATIVE_BUILD

# Clang-specific optimizations
CXXFLAGS += -funroll-loops -finline-functions
CXXFLAGS += -fvectorize -fslp-vectorize
CXXFLAGS += -msse4.2 -mavx -mavx2
CXXFLAGS += -fomit-frame-pointer
CXXFLAGS += -fstrict-aliasing -fstrict-enums
CXXFLAGS += -flto=thin
CXXFLAGS += -pthread

# NOTE: -ffast-math breaks denormal number handling (flushes them to zero)
# Removed for IEEE 754 compliance.
# CXXFLAGS += -ffast-math
# CXXFLAGS += -fno-signed-zeros -fno-trapping-math

DEBUG_FLAGS = -g -DDEBUG -O0
INCLUDES = -Iinclude

# Platform detection
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

# Core source files - recursively find all in subdirectories
CORE_SOURCES = $(wildcard $(CORE_SRC)/*/*.cpp) $(wildcard $(CORE_SRC)/*/*/*.cpp)
LEXER_SOURCES = $(wildcard $(LEXER_SRC)/*.cpp)
PARSER_SOURCES = $(wildcard $(PARSER_SRC)/*.cpp)

# Object files
CORE_OBJECTS = $(CORE_SOURCES:$(CORE_SRC)/%.cpp=$(OBJ_DIR)/core/%.o)
LEXER_OBJECTS = $(LEXER_SOURCES:$(LEXER_SRC)/%.cpp=$(OBJ_DIR)/lexer/%.o)
PARSER_OBJECTS = $(PARSER_SOURCES:$(PARSER_SRC)/%.cpp=$(OBJ_DIR)/parser/%.o)

ALL_OBJECTS = $(CORE_OBJECTS) $(LEXER_OBJECTS) $(PARSER_OBJECTS)

# Static library
LIBQUANTA = $(BUILD_DIR)/libquanta.a

# Console main source
CONSOLE_MAIN = console.cpp

# Main targets
.PHONY: all clean debug release

all: build_header $(LIBQUANTA) $(BIN_DIR)/quanta$(EXE_EXT) build_footer

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
$(BIN_DIR)/quanta$(EXE_EXT): $(CONSOLE_MAIN) $(LIBQUANTA)
	@$(MKDIR_P) $(BIN_DIR)
	@echo "[LINK] Linking with ThinLTO..."
	@echo "[LINK] Creating executable" >> $(LOG_FILE)
	@$(CXX) $(CXXFLAGS) $(INCLUDES) $(LDFLAGS) -DMAIN_EXECUTABLE -o $@ $< -L$(BUILD_DIR) -lquanta $(LIBS) $(STACK_FLAGS) 2>> $(ERROR_LOG) || (echo "[ERROR] Linking failed" >> $(LOG_FILE) && exit 1)
	@echo "[OK] Executable built: $@"

# Object file compilation - create directories and compile
$(OBJ_DIR)/core/%.o: $(CORE_SRC)/%.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "[BUILD] Compiling core: $<"
	@echo "[BUILD] $<" >> $(LOG_FILE)
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@ 2>> $(ERROR_LOG) || (echo "[ERROR] Failed: $<" >> $(LOG_FILE) && exit 1)

$(OBJ_DIR)/lexer/%.o: $(LEXER_SRC)/%.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "[BUILD] Compiling lexer: $<"
	@echo "[BUILD] $<" >> $(LOG_FILE)
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@ 2>> $(ERROR_LOG) || (echo "[ERROR] Failed: $<" >> $(LOG_FILE) && exit 1)

$(OBJ_DIR)/parser/%.o: $(PARSER_SRC)/%.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "[BUILD] Compiling parser: $<"
	@echo "[BUILD] $<" >> $(LOG_FILE)
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@ 2>> $(ERROR_LOG) || (echo "[ERROR] Failed: $<" >> $(LOG_FILE) && exit 1)

# Debug build
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: all

# Release build
release: CXXFLAGS += -DNDEBUG -O3 -flto
release: all

# Clean
clean:
	@echo "[CLEAN] Cleaning build files..."
	@$(RM) $(BUILD_DIR)
	@echo "[OK] Clean completed"

# Prevent deletion of intermediate files
.PRECIOUS: $(OBJ_DIR)/%.o
