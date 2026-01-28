# =============================================================================
# Quanta JavaScript Engine v0.1.0
# Universal Build System (Windows/Linux/macOS)
# =============================================================================

# Compiler and optimization flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O3 -fPIC -march=native -mtune=native
CXXFLAGS += -DQUANTA_VERSION="0.1.0"
CXXFLAGS += -DPROMISE_STABILITY_FIXED -DNATIVE_BUILD
CXXFLAGS += -funroll-loops -finline-functions -finline-limit=1000
CXXFLAGS += -ftree-vectorize -ftree-loop-vectorize
CXXFLAGS += -msse4.2 -mavx
CXXFLAGS += -faggressive-loop-optimizations
CXXFLAGS += -fomit-frame-pointer
# NOTE: -ffast-math breaks denormal number handling (flushes them to zero)
# which causes ES1 conformance test failures. Removed for IEEE 754 compliance.
# CXXFLAGS += -ffast-math
# CXXFLAGS += -fno-signed-zeros -fno-trapping-math
CXXFLAGS += -pthread

DEBUG_FLAGS = -g -DDEBUG -O0
INCLUDES = -Iinclude

# Platform detection
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

# Platform-specific settings
ifeq ($(UNAME_S),Windows)
    LIBS = -lws2_32 -lpowrprof -lsetupapi -lwinmm -lole32 -lshell32
    STACK_FLAGS = -Wl,--stack,67108864
    EXE_EXT = .exe
    RM = rm -rf
    MKDIR_P = powershell -Command "New-Item -ItemType Directory -Force -Path"
else ifeq ($(UNAME_S),Linux)
    LIBS =
    STACK_FLAGS = -Wl,-z,stack-size=16777216
    EXE_EXT =
    RM = rm -rf
    MKDIR_P = mkdir -p
else ifeq ($(UNAME_S),Darwin)
    LIBS =
    STACK_FLAGS =
    EXE_EXT =
    RM = rm -rf
    MKDIR_P = mkdir -p
else
    LIBS = -lws2_32 -lpowrprof -lsetupapi -lwinmm -lole32 -lshell32
    STACK_FLAGS = -Wl,--stack,67108864
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

all: $(LIBQUANTA) $(BIN_DIR)/quanta$(EXE_EXT)

# Static library for embedding
$(LIBQUANTA): $(ALL_OBJECTS)
	@echo "[LIB] Creating Quanta static library..."
	@ar rcs $@ $^
	@echo "[OK] Library created: $@"

# Main console executable
$(BIN_DIR)/quanta$(EXE_EXT): $(CONSOLE_MAIN) $(LIBQUANTA)
	@$(MKDIR_P) $(BIN_DIR)
	@echo "[BUILD] Building Quanta JavaScript console..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -DMAIN_EXECUTABLE -o $@ $< -L$(BUILD_DIR) -lquanta $(LIBS) $(STACK_FLAGS)
	@echo "[OK] Quanta console built: $@"

# Object file compilation - create directories and compile
$(OBJ_DIR)/core/%.o: $(CORE_SRC)/%.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "[BUILD] Compiling core: $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lexer/%.o: $(LEXER_SRC)/%.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "[BUILD] Compiling lexer: $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/parser/%.o: $(PARSER_SRC)/%.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "[BUILD] Compiling parser: $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

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
