# =============================================================================
# Quanta JavaScript Engine v0.0.2
# Clean Build System
# =============================================================================

# Compiler and optimization flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O3 -fPIC -march=native -mtune=native
CXXFLAGS += -DQUANTA_VERSION="0.0.2"
CXXFLAGS += -DPROMISE_STABILITY_FIXED -DNATIVE_BUILD
CXXFLAGS += -ffast-math -funroll-loops -finline-functions
CXXFLAGS += -ftree-vectorize -ftree-loop-vectorize
CXXFLAGS += -msse4.2 -mavx
CXXFLAGS += -faggressive-loop-optimizations
CXXFLAGS += -fomit-frame-pointer
CXXFLAGS += -pthread

DEBUG_FLAGS = -g -DDEBUG -O0
INCLUDES = -Icore/include -Ilexer/include -Iparser/include

# Platform-specific libraries
ifeq ($(OS),Windows_NT)
    LIBS = -lws2_32 -lpowrprof -lsetupapi -lwinmm -lole32 -lshell32
else
    LIBS =
endif

# Directories
CORE_SRC = core/src
LEXER_SRC = lexer/src
PARSER_SRC = parser/src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin

# Create directories
ifeq ($(OS),Windows_NT)
$(shell if not exist $(subst /,\,$(OBJ_DIR)) mkdir $(subst /,\,$(OBJ_DIR)) >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core mkdir $(subst /,\,$(OBJ_DIR))\core >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\platform mkdir $(subst /,\,$(OBJ_DIR))\core\platform >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\PhotonCore mkdir $(subst /,\,$(OBJ_DIR))\core\PhotonCore >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\lexer mkdir $(subst /,\,$(OBJ_DIR))\lexer >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\parser mkdir $(subst /,\,$(OBJ_DIR))\parser >nul 2>&1)
$(shell if not exist $(subst /,\,$(BIN_DIR)) mkdir $(subst /,\,$(BIN_DIR)) >nul 2>&1)
else
$(shell mkdir -p $(OBJ_DIR) $(OBJ_DIR)/core $(OBJ_DIR)/core/platform $(OBJ_DIR)/core/PhotonCore $(OBJ_DIR)/lexer $(OBJ_DIR)/parser $(BIN_DIR))
endif

# Source files (exclude experimental files and problematic files that cause compilation issues)
EXCLUDED_FILES = $(CORE_SRC)/AdaptiveOptimizer.cpp $(CORE_SRC)/AdvancedDebugger.cpp $(CORE_SRC)/AdvancedJIT.cpp $(CORE_SRC)/SIMD.cpp $(CORE_SRC)/LockFree.cpp $(CORE_SRC)/WebAssembly.cpp $(CORE_SRC)/NativeFFI.cpp $(CORE_SRC)/NUMAMemoryManager.cpp $(CORE_SRC)/CPUOptimization.cpp $(CORE_SRC)/ShapeOptimization.cpp $(CORE_SRC)/RealJIT.cpp

# High-performance Optimizations (NEW!) - HIGH PERFORMANCE MODE!
HIGH_PERF_OPTIMIZATIONS_NEW = $(CORE_SRC)/FastBytecode.cpp $(CORE_SRC)/HighPerformance.cpp $(CORE_SRC)/AdvancedObjectOptimizer.cpp
HIGH_PERF_OPTIMIZATIONS = $(CORE_SRC)/OptimizedLoop.cpp
CORE_SOURCES = $(filter-out $(EXCLUDED_FILES), $(wildcard $(CORE_SRC)/*.cpp)) $(wildcard $(CORE_SRC)/PhotonCore/*.cpp) $(CORE_SRC)/platform/NativeAPI.cpp $(CORE_SRC)/platform/APIRouter.cpp $(HIGH_PERF_OPTIMIZATIONS) $(HIGH_PERF_OPTIMIZATIONS_NEW)
ifneq ($(OS),Windows_NT) 
    CORE_SOURCES += $(CORE_SRC)/platform/LinuxNativeAPI.cpp
endif
LEXER_SOURCES = $(wildcard $(LEXER_SRC)/*.cpp)
PARSER_SOURCES = $(wildcard $(PARSER_SRC)/*.cpp)

# Object files
CORE_OBJECTS = $(CORE_SOURCES:$(CORE_SRC)/%.cpp=$(OBJ_DIR)/core/%.o)
LEXER_OBJECTS = $(LEXER_SOURCES:$(LEXER_SRC)/%.cpp=$(OBJ_DIR)/lexer/%.o)
PARSER_OBJECTS = $(PARSER_SOURCES:$(PARSER_SRC)/%.cpp=$(OBJ_DIR)/parser/%.o)

ALL_OBJECTS = $(CORE_OBJECTS) $(LEXER_OBJECTS) $(PARSER_OBJECTS)

# Static library
LIBQUANTA = $(BUILD_DIR)/libquanta.a

# Main targets
.PHONY: all clean debug release

all: $(LIBQUANTA) $(BIN_DIR)/quanta

# Console main source
CONSOLE_MAIN = console.cpp

# Static library for embedding
$(LIBQUANTA): $(ALL_OBJECTS)
	@echo "[LIB] Creating Quanta static library..."
	ar rcs $@ $^
	@echo "[OK] Library created: $@"

# Main console executable
$(BIN_DIR)/quanta: $(CONSOLE_MAIN) $(LIBQUANTA)
	@echo "[BUILD] Building Quanta JavaScript console..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DMAIN_EXECUTABLE -o $@ $< -L$(BUILD_DIR) -lquanta $(LIBS)
	@echo "[OK] Quanta console built: $@"

# Object file compilation
$(OBJ_DIR)/core/%.o: $(CORE_SRC)/%.cpp
	@echo "[BUILD] Compiling core: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/core/platform/%.o: $(CORE_SRC)/platform/%.cpp
	@echo "[BUILD] Compiling platform: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lexer/%.o: $(LEXER_SRC)/%.cpp
	@echo "[BUILD] Compiling lexer: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/parser/%.o: $(PARSER_SRC)/%.cpp
	@echo "[BUILD] Compiling parser: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Debug build
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: all

# Release build
release: CXXFLAGS += -DNDEBUG -O3 -flto
release: all

# Clean
clean:
	@echo "[CLEAN] Cleaning build files..."
ifeq ($(OS),Windows_NT)
	if exist $(subst /,\,$(BUILD_DIR)) rmdir /s /q $(subst /,\,$(BUILD_DIR)) >nul 2>&1
else
	rm -rf $(BUILD_DIR)/*
endif
	@echo "[OK] Clean completed"

# Prevent deletion of intermediate files
.PRECIOUS: $(OBJ_DIR)/%.o