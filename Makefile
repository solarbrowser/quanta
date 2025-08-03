# Quanta JavaScript Engine - Build System
# Modern ES2023+ JavaScript engine for Solar browser

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -fPIC
DEBUG_FLAGS = -g -DDEBUG -O0
INCLUDES = -Icore/include -Ilexer/include -Iparser/include -Iruntime/include -Ivm/include -Istdlib/include

# Directories
CORE_SRC = core/src
LEXER_SRC = lexer/src
PARSER_SRC = parser/src
RUNTIME_SRC = runtime/src
VM_SRC = vm/src
STDLIB_SRC = stdlib/src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin
TESTS_DIR = tests
EXAMPLES_DIR = examples

# Create directories
$(shell mkdir -p $(OBJ_DIR)/{core,lexer,parser,runtime,vm,stdlib} $(BIN_DIR))

# Source files (NodeJS now works on Windows with minimal headers)
CORE_SOURCES = $(wildcard $(CORE_SRC)/*.cpp)
LEXER_SOURCES = $(wildcard $(LEXER_SRC)/*.cpp)
PARSER_SOURCES = $(wildcard $(PARSER_SRC)/*.cpp)
RUNTIME_SOURCES = $(wildcard $(RUNTIME_SRC)/*.cpp)
VM_SOURCES = $(wildcard $(VM_SRC)/*.cpp)
STDLIB_SOURCES = $(wildcard $(STDLIB_SRC)/*.cpp)

# Object files
CORE_OBJECTS = $(CORE_SOURCES:$(CORE_SRC)/%.cpp=$(OBJ_DIR)/core/%.o)
LEXER_OBJECTS = $(LEXER_SOURCES:$(LEXER_SRC)/%.cpp=$(OBJ_DIR)/lexer/%.o)
PARSER_OBJECTS = $(PARSER_SOURCES:$(PARSER_SRC)/%.cpp=$(OBJ_DIR)/parser/%.o)
RUNTIME_OBJECTS = $(RUNTIME_SOURCES:$(RUNTIME_SRC)/%.cpp=$(OBJ_DIR)/runtime/%.o)
VM_OBJECTS = $(VM_SOURCES:$(VM_SRC)/%.cpp=$(OBJ_DIR)/vm/%.o)
STDLIB_OBJECTS = $(STDLIB_SOURCES:$(STDLIB_SRC)/%.cpp=$(OBJ_DIR)/stdlib/%.o)

ALL_OBJECTS = $(CORE_OBJECTS) $(LEXER_OBJECTS) $(PARSER_OBJECTS) $(RUNTIME_OBJECTS) $(VM_OBJECTS) $(STDLIB_OBJECTS)

# Static library
LIBQUANTA = $(BUILD_DIR)/libquanta.a

# Main targets
.PHONY: all clean debug release tests examples docs format lint

all: $(LIBQUANTA) $(BIN_DIR)/quanta

# Stage builds
stage1: $(BIN_DIR)/stage1
stage2: $(BIN_DIR)/stage2

# Enhanced console (optional)
enhanced: $(BIN_DIR)/quanta-enhanced

# Static library for embedding
$(LIBQUANTA): $(ALL_OBJECTS)
	@echo "📦 Creating Quanta static library..."
	ar rcs $@ $^
	@echo "✅ Library created: $@"

# Main console executable
$(BIN_DIR)/quanta: console.cpp $(LIBQUANTA)
	@echo "🔨 Building Quanta JavaScript console..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -L$(BUILD_DIR) -lquanta
	@echo "✅ Quanta console built: $@"

# Enhanced console with readline support
$(BIN_DIR)/quanta-enhanced: console.cpp $(LIBQUANTA)
	@echo "🔨 Building enhanced Quanta console with readline..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DUSE_READLINE -o $@ $< -L$(BUILD_DIR) -lquanta -lreadline
	@echo "✅ Enhanced Quanta console built: $@"

# Individual stage executables for testing
$(BIN_DIR)/stage1: simple_main.cpp $(LEXER_OBJECTS) $(CORE_OBJECTS)
	@echo "🔨 Building Stage 1 (Lexer)..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^
	@echo "✅ Stage 1 built: $@"

$(BIN_DIR)/stage2: stage2_main.cpp $(LIBQUANTA)
	@echo "🔨 Building Stage 2 (Parser)..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -L$(BUILD_DIR) -lquanta
	@echo "✅ Stage 2 built: $@"

# Component builds
lexer: $(LEXER_OBJECTS)
	@echo "✅ Lexer component built"

parser: $(PARSER_OBJECTS) $(LEXER_OBJECTS)
	@echo "✅ Parser component built"

vm: $(VM_OBJECTS)
	@echo "✅ Virtual machine built"

runtime: $(RUNTIME_OBJECTS)
	@echo "✅ Runtime system built"

stdlib: $(STDLIB_OBJECTS)
	@echo "✅ Standard library built"

core: $(CORE_OBJECTS)
	@echo "✅ Core engine built"

# Object file compilation
$(OBJ_DIR)/core/%.o: $(CORE_SRC)/%.cpp
	@echo "🔨 Compiling core: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/lexer/%.o: $(LEXER_SRC)/%.cpp
	@echo "🔨 Compiling lexer: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/parser/%.o: $(PARSER_SRC)/%.cpp
	@echo "🔨 Compiling parser: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/runtime/%.o: $(RUNTIME_SRC)/%.cpp
	@echo "🔨 Compiling runtime: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/vm/%.o: $(VM_SRC)/%.cpp
	@echo "🔨 Compiling VM: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/stdlib/%.o: $(STDLIB_SRC)/%.cpp
	@echo "🔨 Compiling stdlib: $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Debug build
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: all
	@echo "🐛 Debug build completed"

# Release build
release: CXXFLAGS += -DNDEBUG -O3 -flto
release: all
	@echo "🚀 Release build completed"

# Tests
tests: $(LIBQUANTA)
	@echo "🧪 Building and running tests..."
	$(MAKE) -C $(TESTS_DIR) all
	@echo "✅ Tests completed"

# Examples
examples: $(LIBQUANTA)
	@echo "📖 Building examples..."
	$(MAKE) -C $(EXAMPLES_DIR) all
	@echo "✅ Examples built"

# Documentation
docs:
	@echo "📚 Generating documentation..."
	doxygen Doxyfile 2>/dev/null || echo "⚠️  Doxygen not found, skipping docs"

# Code formatting
format:
	@echo "🎨 Formatting code..."
	find . -name "*.cpp" -o -name "*.h" | xargs clang-format -i 2>/dev/null || echo "⚠️  clang-format not found"

# Linting
lint:
	@echo "🔍 Running static analysis..."
	cppcheck --enable=all --std=c++17 --quiet \
		$(CORE_SRC) $(LEXER_SRC) $(PARSER_SRC) $(RUNTIME_SRC) $(VM_SRC) $(STDLIB_SRC) \
		2>/dev/null || echo "⚠️  cppcheck not found"

# Benchmarks
bench: release
	@echo "⚡ Running performance benchmarks..."
	./$(BIN_DIR)/quanta --benchmark

# Clean
clean:
	@echo "🧹 Cleaning build files..."
	rm -rf $(BUILD_DIR)/*
	@echo "✅ Clean completed"

# Help
help:
	@echo "🔧 Quanta JavaScript Engine Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all      - Build complete engine (default)"
	@echo "  enhanced - Build console with readline (arrow keys, history)"
	@echo "  lexer    - Build lexer component"
	@echo "  parser   - Build parser component" 
	@echo "  vm       - Build virtual machine"
	@echo "  runtime  - Build runtime system"
	@echo "  stdlib   - Build standard library"
	@echo "  core     - Build core engine"
	@echo "  debug    - Debug build with symbols"
	@echo "  release  - Optimized release build"
	@echo "  tests    - Build and run tests"
	@echo "  examples - Build example programs"
	@echo "  docs     - Generate documentation"
	@echo "  format   - Format source code"
	@echo "  lint     - Run static analysis"
	@echo "  bench    - Run benchmarks"
	@echo "  clean    - Remove build files"
	@echo "  help     - Show this help"

# Prevent deletion of intermediate files
.PRECIOUS: $(OBJ_DIR)/%.o