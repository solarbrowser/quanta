# =============================================================================
# Quanta JavaScript Engine v0.0.2
# Clean Build System
# =============================================================================

# Compiler and optimization flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O3 -fPIC -march=native -mtune=native
CXXFLAGS += -DQUANTA_VERSION="0.0.2"
CXXFLAGS += -DPROMISE_STABILITY_FIXED -DNATIVE_BUILD
CXXFLAGS += -funroll-loops -finline-functions
CXXFLAGS += -ftree-vectorize -ftree-loop-vectorize
CXXFLAGS += -msse4.2 -mavx
CXXFLAGS += -faggressive-loop-optimizations
CXXFLAGS += -fomit-frame-pointer
CXXFLAGS += -pthread

DEBUG_FLAGS = -g -DDEBUG -O0
INCLUDES = -Icore/include -Ilexer/include -Iparser/include

# Platform-specific libraries and stack size
ifeq ($(OS),Windows_NT)
    LIBS = -lws2_32 -lpowrprof -lsetupapi -lwinmm -lole32 -lshell32
    # Increase stack size to 64MB for deep recursion support
    STACK_FLAGS = -Wl,--stack,67108864
else
    LIBS =
    # Linux stack size flag
    STACK_FLAGS = -Wl,-z,stack-size=16777216
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
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\builtin mkdir $(subst /,\,$(OBJ_DIR))\core\builtin >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\context mkdir $(subst /,\,$(OBJ_DIR))\core\context >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\global mkdir $(subst /,\,$(OBJ_DIR))\core\global >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\collections mkdir $(subst /,\,$(OBJ_DIR))\core\collections >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\memory mkdir $(subst /,\,$(OBJ_DIR))\core\memory >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\datatypes mkdir $(subst /,\,$(OBJ_DIR))\core\datatypes >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\runtime mkdir $(subst /,\,$(OBJ_DIR))\core\runtime >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\compiler mkdir $(subst /,\,$(OBJ_DIR))\core\compiler >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\utils mkdir $(subst /,\,$(OBJ_DIR))\core\utils >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\gc mkdir $(subst /,\,$(OBJ_DIR))\core\gc >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\engine mkdir $(subst /,\,$(OBJ_DIR))\core\engine >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\value mkdir $(subst /,\,$(OBJ_DIR))\core\value >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\core\object mkdir $(subst /,\,$(OBJ_DIR))\core\object >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\lexer mkdir $(subst /,\,$(OBJ_DIR))\lexer >nul 2>&1)
$(shell if not exist $(subst /,\,$(OBJ_DIR))\parser mkdir $(subst /,\,$(OBJ_DIR))\parser >nul 2>&1)
$(shell if not exist $(subst /,\,$(BIN_DIR)) mkdir $(subst /,\,$(BIN_DIR)) >nul 2>&1)
else
$(shell mkdir -p $(OBJ_DIR) $(OBJ_DIR)/core $(OBJ_DIR)/core/platform $(OBJ_DIR)/core/builtin $(OBJ_DIR)/core/context $(OBJ_DIR)/core/global $(OBJ_DIR)/core/collections $(OBJ_DIR)/core/memory $(OBJ_DIR)/core/datatypes $(OBJ_DIR)/core/runtime $(OBJ_DIR)/core/compiler $(OBJ_DIR)/core/utils $(OBJ_DIR)/core/gc $(OBJ_DIR)/core/engine $(OBJ_DIR)/core/value $(OBJ_DIR)/core/object $(OBJ_DIR)/lexer $(OBJ_DIR)/parser $(BIN_DIR))
endif

# Source files (exclude experimental files and problematic files that cause compilation issues)
EXCLUDED_FILES = $(CORE_SRC)/AdaptiveOptimizer.cpp $(CORE_SRC)/AdvancedDebugger.cpp $(CORE_SRC)/AdvancedJIT.cpp $(CORE_SRC)/SIMD.cpp $(CORE_SRC)/LockFree.cpp $(CORE_SRC)/NativeFFI.cpp $(CORE_SRC)/NUMAMemoryManager.cpp $(CORE_SRC)/CPUOptimization.cpp $(CORE_SRC)/ShapeOptimization.cpp $(CORE_SRC)/RealJIT.cpp $(CORE_SRC)/NativeCodeGenerator.cpp $(CORE_SRC)/SpecializedNodes.cpp $(CORE_SRC)/JIT.cpp $(CORE_SRC)/UltimatePatternDetector.cpp

# Core optimization files that still exist
CORE_OPTIMIZATIONS = $(CORE_SRC)/FastBytecode.cpp
# Memory sources excluding problematic files
MEMORY_SOURCES = $(filter-out core/memory/src/NUMAMemoryManager.cpp, $(wildcard core/memory/src/*.cpp))

CORE_SOURCES = $(filter-out $(EXCLUDED_FILES), $(wildcard $(CORE_SRC)/*.cpp)) $(CORE_SRC)/platform/NativeAPI.cpp $(CORE_SRC)/platform/APIRouter.cpp $(wildcard core/builtin/src/*.cpp) $(wildcard core/context/src/*.cpp) $(wildcard core/global/src/*.cpp) $(wildcard core/collections/src/*.cpp) $(MEMORY_SOURCES) $(wildcard core/datatypes/src/*.cpp) $(wildcard core/runtime/src/*.cpp) $(wildcard core/compiler/src/*.cpp) $(wildcard core/utils/src/*.cpp) $(wildcard core/engine/src/*.cpp) $(wildcard core/value/src/*.cpp) $(wildcard core/object/src/*.cpp) $(CORE_OPTIMIZATIONS)
ifneq ($(OS),Windows_NT) 
    CORE_SOURCES += $(CORE_SRC)/platform/LinuxNativeAPI.cpp
endif
LEXER_SOURCES = $(wildcard $(LEXER_SRC)/*.cpp)
PARSER_SOURCES = $(wildcard $(PARSER_SRC)/*.cpp)

# Object files
CORE_OBJECTS = $(patsubst $(CORE_SRC)/%.cpp,$(OBJ_DIR)/core/%.o,$(filter $(CORE_SRC)/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/builtin/src/%.cpp,$(OBJ_DIR)/core/builtin/%.o,$(filter core/builtin/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/context/src/%.cpp,$(OBJ_DIR)/core/context/%.o,$(filter core/context/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/global/src/%.cpp,$(OBJ_DIR)/core/global/%.o,$(filter core/global/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/collections/src/%.cpp,$(OBJ_DIR)/core/collections/%.o,$(filter core/collections/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/memory/src/%.cpp,$(OBJ_DIR)/core/memory/%.o,$(filter core/memory/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/datatypes/src/%.cpp,$(OBJ_DIR)/core/datatypes/%.o,$(filter core/datatypes/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/runtime/src/%.cpp,$(OBJ_DIR)/core/runtime/%.o,$(filter core/runtime/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/compiler/src/%.cpp,$(OBJ_DIR)/core/compiler/%.o,$(filter core/compiler/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/utils/src/%.cpp,$(OBJ_DIR)/core/utils/%.o,$(filter core/utils/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/engine/src/%.cpp,$(OBJ_DIR)/core/engine/%.o,$(filter core/engine/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/value/src/%.cpp,$(OBJ_DIR)/core/value/%.o,$(filter core/value/src/%.cpp,$(CORE_SOURCES))) \
               $(patsubst core/object/src/%.cpp,$(OBJ_DIR)/core/object/%.o,$(filter core/object/src/%.cpp,$(CORE_SOURCES)))
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
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DMAIN_EXECUTABLE -o $@ $< -L$(BUILD_DIR) -lquanta $(LIBS) $(STACK_FLAGS)
	@echo "[OK] Quanta console built: $@"

# Object file compilation
$(OBJ_DIR)/core/%.o: $(CORE_SRC)/%.cpp
	@echo "[BUILD] Compiling core: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/core/platform/%.o: $(CORE_SRC)/platform/%.cpp
	@echo "[BUILD] Compiling platform: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/core/builtin/%.o: core/builtin/src/%.cpp
	@echo "[BUILD] Compiling builtin: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/builtin/include -c $< -o $@

$(OBJ_DIR)/core/context/%.o: core/context/src/%.cpp
	@echo "[BUILD] Compiling context: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/context/include -c $< -o $@

$(OBJ_DIR)/core/global/%.o: core/global/src/%.cpp
	@echo "[BUILD] Compiling global: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/global/include -c $< -o $@

$(OBJ_DIR)/core/collections/%.o: core/collections/src/%.cpp
	@echo "[BUILD] Compiling collections: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/collections/include -c $< -o $@

$(OBJ_DIR)/core/memory/%.o: core/memory/src/%.cpp
	@echo "[BUILD] Compiling memory: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/memory/include -c $< -o $@

$(OBJ_DIR)/core/datatypes/%.o: core/datatypes/src/%.cpp
	@echo "[BUILD] Compiling datatypes: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/datatypes/include -c $< -o $@

$(OBJ_DIR)/core/runtime/%.o: core/runtime/src/%.cpp
	@echo "[BUILD] Compiling runtime: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/runtime/include -c $< -o $@

$(OBJ_DIR)/core/compiler/%.o: core/compiler/src/%.cpp
	@echo "[BUILD] Compiling compiler: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/compiler/include -c $< -o $@

$(OBJ_DIR)/core/utils/%.o: core/utils/src/%.cpp
	@echo "[BUILD] Compiling utils: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/utils/include -c $< -o $@

$(OBJ_DIR)/core/gc/%.o: core/gc/src/%.cpp
	@echo "[BUILD] Compiling gc: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/gc/include -c $< -o $@

$(OBJ_DIR)/core/engine/%.o: core/engine/src/%.cpp
	@echo "[BUILD] Compiling engine: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/engine/include -c $< -o $@

$(OBJ_DIR)/core/value/%.o: core/value/src/%.cpp
	@echo "[BUILD] Compiling value: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/value/include -c $< -o $@

$(OBJ_DIR)/core/object/%.o: core/object/src/%.cpp
	@echo "[BUILD] Compiling object: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/object/include -c $< -o $@

$(OBJ_DIR)/core/engine/%.o: core/engine/src/%.cpp
	@echo "[BUILD] Compiling engine: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/engine/include -c $< -o $@

$(OBJ_DIR)/core/value/%.o: core/value/src/%.cpp
	@echo "[BUILD] Compiling value: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Icore/value/include -c $< -o $@

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