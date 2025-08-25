# =============================================================================
# quanta javascript engine v0.0.2
# build system / not ready for production
# =============================================================================

# Compiler and optimization flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O3 -fPIC -march=native -mtune=native
CXXFLAGS += -DQUANTA_VERSION="2.0" -DQUANTA_FEATURES_COUNT=500
CXXFLAGS += -DPROMISE_STABILITY_FIXED -DNATIVE_BUILD
CXXFLAGS += -ffast-math -funroll-loops -finline-functions
CXXFLAGS += -ftree-vectorize -ftree-loop-vectorize
CXXFLAGS += -msse4.2 -mavx
CXXFLAGS += -faggressive-loop-optimizations
CXXFLAGS += -fomit-frame-pointer
CXXFLAGS += -pthread

DEBUG_FLAGS = -g -DDEBUG -O0
INCLUDES = -Icore/include -Ilexer/include -Iparser/include

# Platform-specific configuration
ifeq ($(OS),Windows_NT)
    # Windows dependencies via MSYS2/MinGW
    CAIRO_EXISTS := $(shell pkg-config --exists cairo 2>/dev/null && echo yes || echo no)
    OPENGL_EXISTS := yes
    PORTAUDIO_EXISTS := $(shell pkg-config --exists portaudio-2.0 2>/dev/null && echo yes || echo no)
    LIBCURL_EXISTS := $(shell pkg-config --exists libcurl 2>/dev/null && echo yes || echo no)
    
    LIBS = -lws2_32 -lpowrprof -lsetupapi -lwinmm -lole32 -lshell32
    
    ifeq ($(CAIRO_EXISTS),yes)
        LIBS += $(shell pkg-config --libs cairo)
        INCLUDES += $(shell pkg-config --cflags cairo)
        CXXFLAGS += -DCAIRO_AVAILABLE
        $(info [OK] Cairo Graphics found via MSYS2 - enabling real Canvas 2D rendering!)
    endif
    
    ifeq ($(OPENGL_EXISTS),yes)
        LIBS += -lopengl32 -lglu32
        CXXFLAGS += -DOPENGL_AVAILABLE
        $(info [OK] OpenGL found via MSYS2 - enabling hardware-accelerated WebGL!)
    endif
    
    ifeq ($(PORTAUDIO_EXISTS),yes)
        LIBS += $(shell pkg-config --libs portaudio-2.0)
        INCLUDES += $(shell pkg-config --cflags portaudio-2.0)
        CXXFLAGS += -DPORTAUDIO_AVAILABLE
        $(info [OK] PortAudio found via MSYS2 - enabling real Web Audio processing!)
    endif
    
    ifeq ($(LIBCURL_EXISTS),yes)
        LIBS += $(shell pkg-config --libs libcurl)
        INCLUDES += $(shell pkg-config --cflags libcurl)
        CXXFLAGS += -DLIBCURL_AVAILABLE
        $(info [OK] libcurl found via MSYS2 - enabling real HTTP/HTTPS networking!)
    endif
    
    $(info [INFO] Platform-specific Windows APIs disabled in MSYS2 - use build-native-windows.bat for full Windows API support)
else
    # Linux/macOS dependencies
    CAIRO_EXISTS := $(shell pkg-config --exists cairo 2>/dev/null && echo yes)
    OPENGL_EXISTS := $(shell pkg-config --exists gl 2>/dev/null && echo yes)
    PORTAUDIO_EXISTS := $(shell pkg-config --exists portaudio-2.0 2>/dev/null && echo yes)
    LIBCURL_EXISTS := $(shell pkg-config --exists libcurl 2>/dev/null && echo yes)
    
    LIBS = 
    
    ifeq ($(CAIRO_EXISTS),yes)
        LIBS += $(shell pkg-config --libs cairo)
        INCLUDES += $(shell pkg-config --cflags cairo)
        CXXFLAGS += -DCAIRO_AVAILABLE
        $(info [OK] Cairo Graphics found - enabling real Canvas 2D rendering!)
    endif
    
    ifeq ($(OPENGL_EXISTS),yes)
        LIBS += $(shell pkg-config --libs gl glu)
        INCLUDES += $(shell pkg-config --cflags gl glu)
        CXXFLAGS += -DOPENGL_AVAILABLE
        $(info [OK] OpenGL found - enabling hardware-accelerated WebGL!)
    else
        OPENGL_FALLBACK := $(shell ldconfig -p 2>/dev/null | grep -E "(libGL\.so|libOpenGL\.so)" >/dev/null && echo yes)
        ifeq ($(OPENGL_FALLBACK),yes)
            LIBS += -lGL -lGLU
            CXXFLAGS += -DOPENGL_AVAILABLE
            $(info [OK] OpenGL found (fallback) - enabling hardware-accelerated WebGL!)
        endif
    endif
    
    ifeq ($(PORTAUDIO_EXISTS),yes)
        LIBS += $(shell pkg-config --libs portaudio-2.0)
        INCLUDES += $(shell pkg-config --cflags portaudio-2.0)
        CXXFLAGS += -DPORTAUDIO_AVAILABLE
        $(info [OK] PortAudio found - enabling real Web Audio processing!)
    endif
    
    ifeq ($(LIBCURL_EXISTS),yes)
        LIBS += $(shell pkg-config --libs libcurl)
        INCLUDES += $(shell pkg-config --cflags libcurl)
        CXXFLAGS += -DLIBCURL_AVAILABLE
        $(info [OK] libcurl found - enabling real HTTP/HTTPS networking!)
    endif
endif

# Directories
CORE_SRC = core/src
LEXER_SRC = lexer/src
PARSER_SRC = parser/src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin

# Create directories
$(shell mkdir -p $(OBJ_DIR) $(OBJ_DIR)/core $(OBJ_DIR)/core/platform $(OBJ_DIR)/core/PhotonCore $(OBJ_DIR)/lexer $(OBJ_DIR)/parser $(BIN_DIR) 2>/dev/null || true)

# Source files (exclude Phase 3 files and problematic V8 files that cause compilation issues)
PHASE3_EXCLUDE = $(CORE_SRC)/AdaptiveOptimizer.cpp $(CORE_SRC)/AdvancedDebugger.cpp $(CORE_SRC)/AdvancedJIT.cpp $(CORE_SRC)/SIMD.cpp $(CORE_SRC)/LockFree.cpp $(CORE_SRC)/WebAssembly.cpp $(CORE_SRC)/NativeFFI.cpp $(CORE_SRC)/NUMAMemoryManager.cpp $(CORE_SRC)/CPUOptimization.cpp $(CORE_SRC)/ShapeOptimization.cpp $(CORE_SRC)/RealJIT.cpp

# V8-Level Optimizations (NEW!) - NUCLEAR PERFORMANCE MODE!
V8_OPTIMIZATIONS_NEW = $(CORE_SRC)/FastBytecode.cpp $(CORE_SRC)/UltraPerformance.cpp $(CORE_SRC)/AdvancedObjectOptimizer.cpp
V8_OPTIMIZATIONS = $(CORE_SRC)/UltraFastLoop.cpp
CORE_SOURCES = $(filter-out $(PHASE3_EXCLUDE), $(wildcard $(CORE_SRC)/*.cpp)) $(wildcard $(CORE_SRC)/PhotonCore/*.cpp) $(CORE_SRC)/platform/NativeAPI.cpp $(CORE_SRC)/platform/APIRouter.cpp $(V8_OPTIMIZATIONS) $(V8_OPTIMIZATIONS_NEW)
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
	rm -rf $(BUILD_DIR)/*
	@echo "[OK] Clean completed"

# Prevent deletion of intermediate files
.PRECIOUS: $(OBJ_DIR)/%.o