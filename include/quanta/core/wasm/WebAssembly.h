/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WEBASSEMBLY_H
#define QUANTA_WEBASSEMBLY_H

#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Value.h"
#include <vector>
#include <memory>
#include <cstdint>
#include <string>

namespace Quanta {

class Context;
class ArrayBuffer;

/**
 * WebAssembly Value Types
 */
enum class WasmType : uint8_t {
    I32 = 0x7F,
    I64 = 0x7E,
    F32 = 0x7D,
    F64 = 0x7C,
};

/**
 * WebAssembly Memory
 */
class WasmMemory : public Object {
private:
    std::unique_ptr<ArrayBuffer> buffer_;
    uint32_t initial_pages_;
    uint32_t maximum_pages_;
    
    static constexpr uint32_t PAGE_SIZE = 65536;
    
public:
    explicit WasmMemory(uint32_t initial_pages, uint32_t maximum_pages = 65536);
    ~WasmMemory() override = default;
    
    bool grow(uint32_t delta_pages);
    uint32_t size() const;
    
    static Value constructor(Context& ctx, const std::vector<Value>& args);
    
    virtual bool is_wasm_memory() const { return true; }
};

/**
 * WebAssembly Module (compiled)
 */
class WasmModule : public Object {
public:
    enum class SectionId : uint8_t {
        Custom = 0,
        Type = 1,
        Import = 2,
        Function = 3,
        Table = 4,
        Memory = 5,
        Global = 6,
        Export = 7,
        Start = 8,
        Element = 9,
        Code = 10,
        Data = 11
    };

private:
    std::vector<uint8_t> binary_data_;
    bool is_compiled_;
    
    struct WasmSection {
        SectionId id;
        uint32_t size;
        std::vector<uint8_t> data;
    };
    
    std::vector<WasmSection> sections_;
    
    struct TypeSection {
        std::vector<std::vector<WasmType>> function_types;
    } type_section_;
    
    struct ImportSection {
        std::vector<std::string> imports;
    } import_section_;
    
    struct FunctionSection {
        std::vector<uint32_t> function_type_indices;
    } function_section_;
    
    struct CodeSection {
        std::vector<std::vector<uint8_t>> function_bodies;
    } code_section_;
    
    struct ExportSection {
        std::vector<std::pair<std::string, uint32_t>> exports;
    } export_section_;
    
public:
    explicit WasmModule(const std::vector<uint8_t>& binary_data);
    ~WasmModule() override = default;
    
    bool compile();
    bool is_compiled() const { return is_compiled_; }
    
    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value compile_static(Context& ctx, const std::vector<Value>& args);
    static Value validate(Context& ctx, const std::vector<Value>& args);
    
    virtual bool is_wasm_module() const { return true; }
    
private:
    bool parse_binary();
    bool parse_header();
    bool parse_sections();
    bool parse_section(WasmSection& section, const uint8_t*& ptr, const uint8_t* end);
    bool validate_module();
    
    bool parse_type_section(const std::vector<uint8_t>& data);
    bool parse_import_section(const std::vector<uint8_t>& data);
    bool parse_function_section(const std::vector<uint8_t>& data);
    bool parse_code_section(const std::vector<uint8_t>& data);
    bool parse_export_section(const std::vector<uint8_t>& data);
    
    uint32_t read_leb128_u32(const uint8_t*& ptr, const uint8_t* end);
    int32_t read_leb128_i32(const uint8_t*& ptr, const uint8_t* end);
    std::string read_string(const uint8_t*& ptr, const uint8_t* end);
};

/**
 * WASM Virtual Machine - Instruction Execution Engine
 */
class WasmVM {
public:
    enum class Opcode : uint8_t {
        UNREACHABLE = 0x00,
        NOP = 0x01,
        BLOCK = 0x02,
        LOOP = 0x03,
        IF = 0x04,
        ELSE = 0x05,
        END = 0x0B,
        BR = 0x0C,
        BR_IF = 0x0D,
        RETURN = 0x0F,
        CALL = 0x10,
        
        LOCAL_GET = 0x20,
        LOCAL_SET = 0x21,
        LOCAL_TEE = 0x22,
        GLOBAL_GET = 0x23,
        GLOBAL_SET = 0x24,
        
        I32_LOAD = 0x28,
        I64_LOAD = 0x29,
        F32_LOAD = 0x2A,
        F64_LOAD = 0x2B,
        I32_STORE = 0x36,
        I64_STORE = 0x37,
        F32_STORE = 0x38,
        F64_STORE = 0x39,
        
        I32_CONST = 0x41,
        I64_CONST = 0x42,
        F32_CONST = 0x43,
        F64_CONST = 0x44,
        
        I32_EQZ = 0x45,
        I32_EQ = 0x46,
        I32_NE = 0x47,
        I32_ADD = 0x6A,
        I32_SUB = 0x6B,
        I32_MUL = 0x6C,
        I32_DIV_S = 0x6D,
        I32_DIV_U = 0x6E,
        
        F32_ADD = 0x92,
        F32_SUB = 0x93,
        F32_MUL = 0x94,
        F32_DIV = 0x95,
        
        F64_ADD = 0xA0,
        F64_SUB = 0xA1,
        F64_MUL = 0xA2,
        F64_DIV = 0xA3
    };
    
    union WasmValue {
        int32_t i32;
        int64_t i64;
        float f32;
        double f64;
        
        WasmValue() : i64(0) {}
        WasmValue(int32_t v) : i32(v) {}
        WasmValue(int64_t v) : i64(v) {}
        WasmValue(float v) : f32(v) {}
        WasmValue(double v) : f64(v) {}
    };
    
    struct ExecutionFrame {
        std::vector<WasmValue> locals;
        std::vector<WasmValue> stack;
        const uint8_t* pc;
        const uint8_t* end;
    };
    
private:
    std::vector<ExecutionFrame> call_stack_;
    WasmMemory* memory_;
    
public:
    explicit WasmVM(WasmMemory* memory) : memory_(memory) {}
    
    Value execute_function(const std::vector<uint8_t>& bytecode, const std::vector<Value>& args);
    bool execute_instruction(ExecutionFrame& frame);
    
    bool handle_i32_const(ExecutionFrame& frame, const uint8_t*& pc);
    bool handle_i32_add(ExecutionFrame& frame);
    bool handle_i32_sub(ExecutionFrame& frame);
    bool handle_i32_mul(ExecutionFrame& frame);
    bool handle_f32_add(ExecutionFrame& frame);
    bool handle_f64_add(ExecutionFrame& frame);
    bool handle_local_get(ExecutionFrame& frame, const uint8_t*& pc);
    bool handle_local_set(ExecutionFrame& frame, const uint8_t*& pc);
    bool handle_return(ExecutionFrame& frame);
    
    uint32_t read_leb128_u32(const uint8_t*& pc, const uint8_t* end);
    int32_t read_leb128_i32(const uint8_t*& pc, const uint8_t* end);
};

/**
 * WebAssembly Instance (executable)
 */
class WasmInstance : public Object {
private:
    std::shared_ptr<WasmModule> module_;
    std::unique_ptr<WasmMemory> memory_;
    std::unique_ptr<WasmVM> vm_;
    
public:
    explicit WasmInstance(std::shared_ptr<WasmModule> module, Object* import_object = nullptr);
    ~WasmInstance() override = default;
    
    bool instantiate();
    Value call_exported_function(const std::string& name, const std::vector<Value>& args);
    
    static Value constructor(Context& ctx, const std::vector<Value>& args);
    
    virtual bool is_wasm_instance() const { return true; }
    
private:
    bool resolve_imports(Object* import_object);
};

/**
 * WebAssembly namespace object (global WebAssembly)
 */
namespace WebAssemblyAPI {
    void setup_webassembly(Context& ctx);
    
    Value compile(Context& ctx, const std::vector<Value>& args);
    Value instantiate(Context& ctx, const std::vector<Value>& args);
    Value validate(Context& ctx, const std::vector<Value>& args);
}

}

#endif
