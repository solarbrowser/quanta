/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include "Context.h"
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <memory>

namespace Quanta {

// Forward declarations
class ASTNode;

//=============================================================================
// Bytecode Instructions - High-performance Optimization
//=============================================================================

enum class BytecodeInstruction : uint8_t {
    // Load/Store Operations
    LOAD_CONST = 0x01,      // Load constant value
    LOAD_VAR = 0x02,        // Load variable
    STORE_VAR = 0x03,       // Store variable
    LOAD_GLOBAL = 0x04,     // Load global variable
    STORE_GLOBAL = 0x05,    // Store global variable
    
    // Property Operations
    LOAD_PROP = 0x10,       // Load object property
    STORE_PROP = 0x11,      // Store object property
    LOAD_ELEMENT = 0x12,    // Load array element
    STORE_ELEMENT = 0x13,   // Store array element
    
    // Arithmetic Operations
    ADD = 0x20,             // Addition
    SUB = 0x21,             // Subtraction
    MUL = 0x22,             // Multiplication
    DIV = 0x23,             // Division
    MOD = 0x24,             // Modulus
    NEG = 0x25,             // Negation
    
    // Comparison Operations
    EQ = 0x30,              // Equality (==)
    NEQ = 0x31,             // Inequality (!=)
    LT = 0x32,              // Less than (<)
    LE = 0x33,              // Less than or equal (<=)
    GT = 0x34,              // Greater than (>)
    GE = 0x35,              // Greater than or equal (>=)
    STRICT_EQ = 0x36,       // Strict equality (===)
    STRICT_NEQ = 0x37,      // Strict inequality (!==)
    
    // Logical Operations
    AND = 0x40,             // Logical AND
    OR = 0x41,              // Logical OR
    NOT = 0x42,             // Logical NOT
    
    // Control Flow
    JUMP = 0x50,            // Unconditional jump
    JUMP_TRUE = 0x51,       // Jump if true
    JUMP_FALSE = 0x52,      // Jump if false
    CALL = 0x53,            // Function call
    RETURN = 0x54,          // Return from function
    THROW = 0x55,           // Throw exception
    
    // Object Operations
    NEW_OBJECT = 0x60,      // Create new object
    NEW_ARRAY = 0x61,       // Create new array
    NEW_FUNCTION = 0x62,    // Create new function
    
    // Stack Operations
    POP = 0x70,             // Pop from stack
    DUP = 0x71,             // Duplicate top of stack
    SWAP = 0x72,            // Swap top two stack elements
    
    // Special Operations
    NOP = 0x80,             // No operation
    HALT = 0x81,            // Halt execution
    DEBUG = 0x82,           // Debug breakpoint
    
    // Type Operations
    TYPEOF = 0x90,          // typeof operator
    INSTANCEOF = 0x91,      // instanceof operator
    
    // Hot Path Optimizations
    FAST_ADD_INT = 0xA0,    // Fast integer addition
    FAST_ADD_NUM = 0xA1,    // Fast number addition
    FAST_PROP_LOAD = 0xA2,  // Fast property load with inline cache
    FAST_CALL = 0xA3,       // Fast function call
    FAST_LOOP = 0xA4,       // Fast loop iteration
};

//=============================================================================
// Bytecode Operand Types
//=============================================================================

struct BytecodeOperand {
    enum Type : uint8_t {
        IMMEDIATE,      // Immediate value
        REGISTER,       // Virtual register
        CONSTANT,       // Constant pool index
        OFFSET          // Jump offset
    };
    
    Type type;
    uint32_t value;
    
    BytecodeOperand(Type t, uint32_t v) : type(t), value(v) {}
};

//=============================================================================
// Bytecode Instruction Structure
//=============================================================================

struct BytecodeOp {
    BytecodeInstruction instruction;
    std::vector<BytecodeOperand> operands;
    uint32_t source_line;       // For debugging
    
    BytecodeOp(BytecodeInstruction inst) 
        : instruction(inst), source_line(0) {}
    
    BytecodeOp(BytecodeInstruction inst, std::vector<BytecodeOperand> ops)
        : instruction(inst), operands(std::move(ops)), source_line(0) {}
};

//=============================================================================
// Bytecode Function - Advanced Compilation Unit
//=============================================================================

class BytecodeFunction {
public:
    std::vector<BytecodeOp> instructions;
    std::vector<Value> constants;          // Constant pool
    std::vector<std::string> variables;    // Variable names
    uint32_t register_count;               // Number of virtual registers
    uint32_t parameter_count;              // Number of parameters
    std::string function_name;             // Function name for debugging
    
    // Optimization metadata
    std::unordered_map<uint32_t, uint32_t> hot_spots;  // PC -> execution count
    bool is_optimized;                     // Whether this function is optimized
    uint32_t optimization_level;           // 0=None, 1=Basic, 2=Advanced, 3=Maximum
    
    BytecodeFunction(const std::string& name = "")
        : register_count(0), parameter_count(0), function_name(name),
          is_optimized(false), optimization_level(0) {}
    
    // Add instruction
    void emit(BytecodeInstruction inst) {
        instructions.emplace_back(inst);
    }
    
    void emit(BytecodeInstruction inst, std::vector<BytecodeOperand> operands) {
        instructions.emplace_back(inst, std::move(operands));
    }
    
    // Add constant to pool
    uint32_t add_constant(const Value& value) {
        constants.push_back(value);
        return static_cast<uint32_t>(constants.size() - 1);
    }
    
    // Add variable
    uint32_t add_variable(const std::string& name) {
        variables.push_back(name);
        return static_cast<uint32_t>(variables.size() - 1);
    }
};

//=============================================================================
// Bytecode Compiler - AST to Bytecode Translation
//=============================================================================

class BytecodeCompiler {
public:
    BytecodeCompiler();
    ~BytecodeCompiler();
    
    // Compile AST to bytecode
    std::unique_ptr<BytecodeFunction> compile(ASTNode* ast, const std::string& function_name = "");
    
    // Optimization passes
    void optimize_bytecode(BytecodeFunction* function, uint32_t level = 1);
    
    // Enable/disable optimizations
    void set_optimization_enabled(bool enabled) { optimization_enabled_ = enabled; }
    bool is_optimization_enabled() const { return optimization_enabled_; }
    
private:
    bool optimization_enabled_;
    uint32_t next_register_;
    
    // Compilation methods
    void compile_node(ASTNode* node, BytecodeFunction* function);
    void compile_node_simple(ASTNode* node, BytecodeFunction* function);
    void compile_expression(ASTNode* node, BytecodeFunction* function);
    void compile_statement(ASTNode* node, BytecodeFunction* function);
    
    // Optimization passes
    void constant_folding_pass(BytecodeFunction* function);
    void dead_code_elimination_pass(BytecodeFunction* function);
    void peephole_optimization_pass(BytecodeFunction* function);
    void hot_path_optimization_pass(BytecodeFunction* function);
    
    // Helper methods
    uint32_t allocate_register() { return next_register_++; }
    void reset_registers() { next_register_ = 0; }
};

//=============================================================================
// Bytecode Virtual Machine - High-Performance Execution Engine
//=============================================================================

class BytecodeVM {
public:
    BytecodeVM();
    ~BytecodeVM();
    
    // Execute bytecode function
    Value execute(BytecodeFunction* function, Context& context, const std::vector<Value>& args = {});
    
    // Hot spot detection and optimization
    void enable_profiling(bool enabled) { profiling_enabled_ = enabled; }
    void record_execution(BytecodeFunction* function, uint32_t pc);
    
    // Performance statistics
    struct VMStats {
        uint64_t instructions_executed;
        uint64_t function_calls;
        uint64_t optimized_paths_taken;
        uint64_t cache_hits;
        uint64_t cache_misses;
        
        VMStats() : instructions_executed(0), function_calls(0), 
                   optimized_paths_taken(0), cache_hits(0), cache_misses(0) {}
    };
    
    const VMStats& get_stats() const { return stats_; }
    void reset_stats() { stats_ = VMStats(); }
    
private:
    std::vector<Value> stack_;             // Execution stack
    std::vector<Value> registers_;         // Virtual registers
    bool profiling_enabled_;
    VMStats stats_;
    
    // Inline cache for property access
    struct PropertyCache {
        std::string property_name;
        Value cached_value;
        uint64_t access_count;
        
        PropertyCache() : access_count(0) {}
    };
    std::unordered_map<uint32_t, PropertyCache> property_cache_;
    
    // Fast path execution methods
    Value execute_fast_add(const Value& left, const Value& right);
    Value execute_fast_property_load(const Value& object, const std::string& property, uint32_t cache_key);
    
    // Instruction execution
    void execute_instruction(const BytecodeOp& op, BytecodeFunction* function, Context& context, uint32_t& pc);
    void execute_instruction_simple(const BytecodeOp& op, BytecodeFunction* function, Context& context, uint32_t& pc);
    
    // Stack manipulation
    void push(const Value& value) { stack_.push_back(value); }
    Value pop() { 
        if (stack_.empty()) return Value();
        Value v = stack_.back(); 
        stack_.pop_back(); 
        return v; 
    }
    Value peek() const { return stack_.empty() ? Value() : stack_.back(); }
};

//=============================================================================
// Bytecode Integration with JIT System
//=============================================================================

class BytecodeJITBridge {
public:
    // Convert hot bytecode functions to JIT-compiled machine code
    static bool should_jit_compile(BytecodeFunction* function);
    static bool compile_to_machine_code(BytecodeFunction* function);
    
    // Performance thresholds
    static constexpr uint32_t JIT_COMPILE_THRESHOLD = 50;  // Execute 50 times before JIT
    static constexpr uint32_t HOT_SPOT_THRESHOLD = 10;     // Hot spot detection
};

} // namespace Quanta