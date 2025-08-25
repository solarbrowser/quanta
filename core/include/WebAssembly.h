/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include <cstdint>
#include <functional>

namespace Quanta {

// Forward declarations
class Value;

//=============================================================================
// WebAssembly (WASM) Support - Near-Native Performance
// 
// Complete WebAssembly implementation for ultra-fast execution:
// - WASM binary parsing and validation
// - High-performance WASM runtime
// - JavaScript/WASM interoperability
// - Memory management integration
// - SIMD WASM instructions support
// - JIT compilation of WASM to native code
//=============================================================================

//=============================================================================
// WASM Value Types and Instructions
//=============================================================================

enum class WASMValueType : uint8_t {
    I32 = 0x7F,
    I64 = 0x7E,
    F32 = 0x7D,
    F64 = 0x7C,
    V128 = 0x7B,    // SIMD vector type
    FUNCREF = 0x70,
    EXTERNREF = 0x6F
};

enum class WASMOpcode : uint8_t {
    // Control flow
    UNREACHABLE = 0x00,
    NOP = 0x01,
    BLOCK = 0x02,
    LOOP = 0x03,
    IF = 0x04,
    ELSE = 0x05,
    END = 0x0B,
    BR = 0x0C,
    BR_IF = 0x0D,
    BR_TABLE = 0x0E,
    RETURN = 0x0F,
    CALL = 0x10,
    CALL_INDIRECT = 0x11,
    
    // Parametric instructions
    DROP = 0x1A,
    SELECT = 0x1B,
    
    // Variable instructions
    LOCAL_GET = 0x20,
    LOCAL_SET = 0x21,
    LOCAL_TEE = 0x22,
    GLOBAL_GET = 0x23,
    GLOBAL_SET = 0x24,
    
    // Memory instructions
    I32_LOAD = 0x28,
    I64_LOAD = 0x29,
    F32_LOAD = 0x2A,
    F64_LOAD = 0x2B,
    I32_STORE = 0x36,
    I64_STORE = 0x37,
    F32_STORE = 0x38,
    F64_STORE = 0x39,
    
    // Numeric instructions
    I32_CONST = 0x41,
    I64_CONST = 0x42,
    F32_CONST = 0x43,
    F64_CONST = 0x44,
    
    // I32 operations
    I32_ADD = 0x6A,
    I32_SUB = 0x6B,
    I32_MUL = 0x6C,
    I32_DIV_S = 0x6D,
    I32_DIV_U = 0x6E,
    
    // F32 operations
    F32_ADD = 0x92,
    F32_SUB = 0x93,
    F32_MUL = 0x94,
    F32_DIV = 0x95,
    F32_SQRT = 0x91,
    
    // F64 operations
    F64_ADD = 0xA0,
    F64_SUB = 0xA1,
    F64_MUL = 0xA2,
    F64_DIV = 0xA3,
    F64_SQRT = 0x9F
};

//=============================================================================
// WASM Value - Runtime value representation
//=============================================================================

class WASMValue {
public:
    WASMValueType type;
    
    union {
        int32_t i32;
        int64_t i64;
        float f32;
        double f64;
        uint8_t v128[16]; // SIMD vector
        void* ref;
    } data;
    
    WASMValue() : type(WASMValueType::I32) { data.i32 = 0; }
    WASMValue(int32_t val) : type(WASMValueType::I32) { data.i32 = val; }
    WASMValue(int64_t val) : type(WASMValueType::I64) { data.i64 = val; }
    WASMValue(float val) : type(WASMValueType::F32) { data.f32 = val; }
    WASMValue(double val) : type(WASMValueType::F64) { data.f64 = val; }
    
    // Conversion to JavaScript Value
    Value to_js_value() const;
    static WASMValue from_js_value(const Value& val);
    
    // Type checking
    bool is_i32() const { return type == WASMValueType::I32; }
    bool is_i64() const { return type == WASMValueType::I64; }
    bool is_f32() const { return type == WASMValueType::F32; }
    bool is_f64() const { return type == WASMValueType::F64; }
    bool is_v128() const { return type == WASMValueType::V128; }
};

//=============================================================================
// WASM Memory - Linear memory implementation
//=============================================================================

class WASMMemory {
private:
    std::vector<uint8_t> memory_;
    uint32_t page_size_;        // 64KB pages
    uint32_t max_pages_;
    uint32_t current_pages_;
    
    static constexpr uint32_t WASM_PAGE_SIZE = 65536; // 64KB

public:
    WASMMemory(uint32_t initial_pages, uint32_t max_pages = UINT32_MAX);
    ~WASMMemory();
    
    // Memory operations
    bool grow(uint32_t delta_pages);
    uint32_t size() const { return current_pages_; }
    uint32_t byte_size() const { return current_pages_ * WASM_PAGE_SIZE; }
    
    // Load operations
    int32_t load_i32(uint32_t offset) const;
    int64_t load_i64(uint32_t offset) const;
    float load_f32(uint32_t offset) const;
    double load_f64(uint32_t offset) const;
    
    // Store operations
    void store_i32(uint32_t offset, int32_t value);
    void store_i64(uint32_t offset, int64_t value);
    void store_f32(uint32_t offset, float value);
    void store_f64(uint32_t offset, double value);
    
    // Raw memory access
    uint8_t* get_memory_ptr() { return memory_.data(); }
    const uint8_t* get_memory_ptr() const { return memory_.data(); }
    
    // Bounds checking
    bool is_valid_offset(uint32_t offset, uint32_t size) const;
    void check_bounds(uint32_t offset, uint32_t size) const;
};

//=============================================================================
// WASM Function - Function representation
//=============================================================================

struct WASMFunctionType {
    std::vector<WASMValueType> params;
    std::vector<WASMValueType> results;
    
    bool matches(const WASMFunctionType& other) const;
    std::string to_string() const;
};

class WASMFunction {
public:
    uint32_t type_index;
    std::vector<WASMValueType> locals;
    std::vector<uint8_t> code;
    
    // JIT compilation data
    void* compiled_code;
    bool is_compiled;
    uint64_t execution_count;
    
    WASMFunction(uint32_t type_idx) 
        : type_index(type_idx), compiled_code(nullptr), 
          is_compiled(false), execution_count(0) {}
    
    // Execution
    std::vector<WASMValue> execute(const std::vector<WASMValue>& args, class WASMModule* module);
    
    // JIT compilation
    bool compile_to_native();
    void* get_compiled_code() const { return compiled_code; }
};

//=============================================================================
// WASM Table - Function reference table
//=============================================================================

class WASMTable {
private:
    std::vector<WASMFunction*> elements_;
    uint32_t max_size_;
    WASMValueType element_type_;

public:
    WASMTable(uint32_t initial_size, uint32_t max_size, WASMValueType type);
    ~WASMTable();
    
    // Table operations
    WASMFunction* get_function(uint32_t index) const;
    void set_function(uint32_t index, WASMFunction* func);
    bool grow(uint32_t delta_size);
    uint32_t size() const { return elements_.size(); }
    
    // Call indirect support
    std::vector<WASMValue> call_indirect(uint32_t index, const std::vector<WASMValue>& args, 
                                       WASMFunctionType expected_type, class WASMModule* module);
};

//=============================================================================
// WASM Global - Global variable
//=============================================================================

class WASMGlobal {
public:
    WASMValueType type;
    bool is_mutable;
    WASMValue value;
    
    WASMGlobal(WASMValueType t, bool mutable_flag, const WASMValue& initial_value)
        : type(t), is_mutable(mutable_flag), value(initial_value) {}
    
    WASMValue get() const { return value; }
    void set(const WASMValue& new_value);
};

//=============================================================================
// WASM Module - Complete module representation
//=============================================================================

class WASMModule {
private:
    // Module sections
    std::vector<WASMFunctionType> types_;
    std::vector<std::unique_ptr<WASMFunction>> functions_;
    std::vector<std::unique_ptr<WASMTable>> tables_;
    std::vector<std::unique_ptr<WASMMemory>> memories_;
    std::vector<std::unique_ptr<WASMGlobal>> globals_;
    
    // Imports and exports
    std::unordered_map<std::string, uint32_t> exports_;
    std::unordered_map<std::string, std::function<std::vector<WASMValue>(const std::vector<WASMValue>&)>> imports_;
    
    // Module metadata
    std::string module_name_;
    bool is_instantiated_;
    
    // Performance tracking
    uint64_t total_function_calls_;
    uint64_t total_execution_time_ns_;

public:
    WASMModule(const std::string& name = "");
    ~WASMModule();
    
    // Module building
    uint32_t add_type(const WASMFunctionType& type);
    uint32_t add_function(std::unique_ptr<WASMFunction> func);
    uint32_t add_table(std::unique_ptr<WASMTable> table);
    uint32_t add_memory(std::unique_ptr<WASMMemory> memory);
    uint32_t add_global(std::unique_ptr<WASMGlobal> global);
    
    // Exports
    void add_export(const std::string& name, uint32_t index);
    WASMFunction* get_exported_function(const std::string& name);
    WASMMemory* get_exported_memory(const std::string& name);
    
    // Imports
    void add_import(const std::string& name, std::function<std::vector<WASMValue>(const std::vector<WASMValue>&)> func);
    
    // Access to module components
    const WASMFunctionType& get_type(uint32_t index) const { return types_[index]; }
    WASMFunction* get_function(uint32_t index) const { return functions_[index].get(); }
    WASMTable* get_table(uint32_t index) const { return tables_[index].get(); }
    WASMMemory* get_memory(uint32_t index) const { return memories_[index].get(); }
    WASMGlobal* get_global(uint32_t index) const { return globals_[index].get(); }
    
    // Module instantiation
    bool instantiate();
    bool is_instantiated() const { return is_instantiated_; }
    
    // Function execution
    std::vector<WASMValue> call_function(const std::string& name, const std::vector<WASMValue>& args);
    std::vector<WASMValue> call_function(uint32_t index, const std::vector<WASMValue>& args);
    
    // Performance
    void print_performance_stats() const;
    uint64_t get_total_function_calls() const { return total_function_calls_; }
};

//=============================================================================
// WASM Parser - Binary format parser
//=============================================================================

class WASMParser {
private:
    const uint8_t* data_;
    size_t size_;
    size_t position_;
    
    // Magic numbers and version
    static constexpr uint32_t WASM_MAGIC = 0x6D736100; // "\0asm"
    static constexpr uint32_t WASM_VERSION = 1;

public:
    WASMParser(const uint8_t* data, size_t size);
    ~WASMParser();
    
    // Main parsing function
    std::unique_ptr<WASMModule> parse();
    
    // Section parsing
    bool parse_type_section(WASMModule* module);
    bool parse_import_section(WASMModule* module);
    bool parse_function_section(WASMModule* module);
    bool parse_table_section(WASMModule* module);
    bool parse_memory_section(WASMModule* module);
    bool parse_global_section(WASMModule* module);
    bool parse_export_section(WASMModule* module);
    bool parse_code_section(WASMModule* module);
    bool parse_data_section(WASMModule* module);
    
    // Utility functions
    uint32_t read_u32();
    uint64_t read_u64();
    float read_f32();
    double read_f64();
    std::string read_string();
    WASMValueType read_value_type();
    
    // LEB128 decoding
    uint32_t read_leb128_u32();
    int32_t read_leb128_i32();
    uint64_t read_leb128_u64();
    int64_t read_leb128_i64();
    
    // Validation
    bool validate_magic_and_version();
    bool has_more_data() const { return position_ < size_; }
};

//=============================================================================
// WASM Interpreter - High-performance interpreter
//=============================================================================

class WASMInterpreter {
private:
    WASMModule* module_;
    std::vector<WASMValue> operand_stack_;
    std::vector<WASMValue> locals_;
    
    // Execution context
    struct ExecutionFrame {
        WASMFunction* function;
        uint32_t return_address;
        uint32_t locals_start;
        std::vector<WASMValueType> result_types;
    };
    
    std::vector<ExecutionFrame> call_stack_;
    
    // Performance optimization
    bool jit_enabled_;
    std::unordered_map<WASMFunction*, void*> compiled_functions_;

public:
    WASMInterpreter(WASMModule* module);
    ~WASMInterpreter();
    
    // Main execution function
    std::vector<WASMValue> execute_function(WASMFunction* func, const std::vector<WASMValue>& args);
    
    // Instruction execution
    bool execute_instruction(WASMOpcode opcode, const uint8_t*& code_ptr);
    
    // Stack operations
    void push(const WASMValue& value) { operand_stack_.push_back(value); }
    WASMValue pop();
    WASMValue peek(size_t offset = 0) const;
    
    // Control flow
    void call_function(uint32_t func_index);
    void return_from_function();
    
    // Memory operations
    WASMValue load_from_memory(WASMValueType type, uint32_t offset);
    void store_to_memory(WASMValueType type, uint32_t offset, const WASMValue& value);
    
    // JIT compilation
    void enable_jit() { jit_enabled_ = true; }
    void disable_jit() { jit_enabled_ = false; }
    bool compile_function(WASMFunction* func);
    
    // Performance monitoring
    void print_execution_stats() const;
};

//=============================================================================
// WASM JIT Compiler - Native code generation
//=============================================================================

class WASMJITCompiler {
private:
    std::unordered_map<WASMFunction*, void*> compiled_functions_;
    
    // Compilation statistics
    uint64_t total_compilations_;
    uint64_t total_compile_time_ns_;
    uint64_t compiled_function_calls_;

public:
    WASMJITCompiler();
    ~WASMJITCompiler();
    
    // Compilation
    bool compile_function(WASMFunction* func, const WASMFunctionType& type);
    void* get_compiled_function(WASMFunction* func);
    bool is_compiled(WASMFunction* func) const;
    
    // Code generation for specific instructions
    void emit_i32_add();
    void emit_i32_sub();
    void emit_i32_mul();
    void emit_f32_add();
    void emit_f32_mul();
    void emit_memory_load(WASMValueType type);
    void emit_memory_store(WASMValueType type);
    
    // Optimization
    void optimize_function(WASMFunction* func);
    void apply_simd_optimizations(WASMFunction* func);
    
    // Performance
    void print_compilation_stats() const;
    double get_average_compile_time() const;
    
    // Singleton access
    static WASMJITCompiler& get_instance();
};

//=============================================================================
// WASM JavaScript Integration
//=============================================================================

namespace WASMJavaScriptIntegration {
    // Module loading
    std::unique_ptr<WASMModule> compile_wasm_module(const std::vector<uint8_t>& wasm_bytes);
    std::unique_ptr<WASMModule> compile_wasm_from_file(const std::string& filename);
    
    // JavaScript API
    Value create_wasm_instance(const std::vector<uint8_t>& wasm_bytes);
    Value call_wasm_function(WASMModule* module, const std::string& function_name, const std::vector<Value>& args);
    
    // Memory sharing
    Value create_wasm_memory_view(WASMMemory* memory);
    bool copy_js_array_to_wasm_memory(const std::vector<Value>& js_array, WASMMemory* memory, uint32_t offset);
    std::vector<Value> copy_wasm_memory_to_js_array(WASMMemory* memory, uint32_t offset, uint32_t length);
    
    // Performance optimization
    void enable_wasm_simd_optimization();
    void enable_wasm_jit_compilation();
    void set_wasm_optimization_level(int level);
    
    // Utilities
    void print_wasm_module_info(const WASMModule* module);
    void print_wasm_performance_stats();
    
    // Engine integration
    void initialize_wasm_runtime();
    void shutdown_wasm_runtime();
}

} // namespace Quanta