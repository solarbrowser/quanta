/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Value.h"
#include "quanta/Context.h"
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <memory>

namespace Quanta {

class ASTNode;


enum class BytecodeInstruction : uint8_t {
    LOAD_CONST = 0x01,      
    LOAD_VAR = 0x02,        
    STORE_VAR = 0x03,       
    LOAD_GLOBAL = 0x04,     
    STORE_GLOBAL = 0x05,    
    
    LOAD_PROP = 0x10,       
    STORE_PROP = 0x11,      
    LOAD_ELEMENT = 0x12,    
    STORE_ELEMENT = 0x13,   
    
    ADD = 0x20,            
    SUB = 0x21,            
    MUL = 0x22,             
    DIV = 0x23,             
    MOD = 0x24,             
    NEG = 0x25,            
    
    EQ = 0x30,
    NEQ = 0x31,
    LT = 0x32,
    LE = 0x33,
    GT = 0x34,
    GE = 0x35,
    STRICT_EQ = 0x36,
    STRICT_NEQ = 0x37,
    
    AND = 0x40,            
    OR = 0x41,              
    NOT = 0x42,             
    
    JUMP = 0x50,           
    JUMP_TRUE = 0x51,      
    JUMP_FALSE = 0x52,     
    CALL = 0x53,           
    RETURN = 0x54,        
    THROW = 0x55,          
    
    NEW_OBJECT = 0x60,     
    NEW_ARRAY = 0x61,      
    NEW_FUNCTION = 0x62,    
    
    POP = 0x70,            
    DUP = 0x71,             
    SWAP = 0x72,            
    
    NOP = 0x80,            
    HALT = 0x81,            
    DEBUG = 0x82,           
    
    TYPEOF = 0x90,         
    INSTANCEOF = 0x91,    
    
    FAST_ADD_INT = 0xA0,    
    FAST_ADD_NUM = 0xA1,   
    FAST_PROP_LOAD = 0xA2, 
    FAST_CALL = 0xA3,       
    FAST_LOOP = 0xA4,      
};


struct BytecodeOperand {
    enum Type : uint8_t {
        IMMEDIATE,     
        REGISTER,      
        CONSTANT,       
        OFFSET          
    };
    
    Type type;
    uint32_t value;
    
    BytecodeOperand(Type t, uint32_t v) : type(t), value(v) {}
};



struct BytecodeOp {
    BytecodeInstruction instruction;
    std::vector<BytecodeOperand> operands;
    uint32_t source_line;     
    
    BytecodeOp(BytecodeInstruction inst) 
        : instruction(inst), source_line(0) {}
    
    BytecodeOp(BytecodeInstruction inst, std::vector<BytecodeOperand> ops)
        : instruction(inst), operands(std::move(ops)), source_line(0) {}
};



class BytecodeFunction {
public:
    std::vector<BytecodeOp> instructions;
    std::vector<Value> constants;         
    std::vector<std::string> variables;    
    uint32_t register_count;               
    uint32_t parameter_count;             
    std::string function_name;            
    
    std::unordered_map<uint32_t, uint32_t> hot_spots;  
    bool is_optimized;                    
    uint32_t optimization_level;
    BytecodeFunction(const std::string& name = "")
        : register_count(0), parameter_count(0), function_name(name),
          is_optimized(false), optimization_level(0) {}
    
    void emit(BytecodeInstruction inst) {
        instructions.emplace_back(inst);
    }
    
    void emit(BytecodeInstruction inst, std::vector<BytecodeOperand> operands) {
        instructions.emplace_back(inst, std::move(operands));
    }
    
    uint32_t add_constant(const Value& value) {
        constants.push_back(value);
        return static_cast<uint32_t>(constants.size() - 1);
    }
    
    uint32_t add_variable(const std::string& name) {
        variables.push_back(name);
        return static_cast<uint32_t>(variables.size() - 1);
    }
};


class BytecodeCompiler {
public:
    BytecodeCompiler();
    ~BytecodeCompiler();
    
    std::unique_ptr<BytecodeFunction> compile(ASTNode* ast, const std::string& function_name = "");
    
    void optimize_bytecode(BytecodeFunction* function, uint32_t level = 1);
    
    void set_optimization_enabled(bool enabled) { optimization_enabled_ = enabled; }
    bool is_optimization_enabled() const { return optimization_enabled_; }
    
private:
    bool optimization_enabled_;
    uint32_t next_register_;
    
    void compile_node(ASTNode* node, BytecodeFunction* function);
    void compile_node_simple(ASTNode* node, BytecodeFunction* function);
    void compile_expression(ASTNode* node, BytecodeFunction* function);
    void compile_statement(ASTNode* node, BytecodeFunction* function);
    
    void constant_folding_pass(BytecodeFunction* function);
    void dead_code_elimination_pass(BytecodeFunction* function);
    void peephole_optimization_pass(BytecodeFunction* function);
    void hot_path_optimization_pass(BytecodeFunction* function);
    
    uint32_t allocate_register() { return next_register_++; }
    void reset_registers() { next_register_ = 0; }
};




class BytecodeVM {
public:
    BytecodeVM();
    ~BytecodeVM();
    
    Value execute(BytecodeFunction* function, Context& context, const std::vector<Value>& args = {});
    
    void enable_profiling(bool enabled) { profiling_enabled_ = enabled; }
    void record_execution(BytecodeFunction* function, uint32_t pc);
    
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
    std::vector<Value> stack_;           
    std::vector<Value> registers_;         
    bool profiling_enabled_;
    VMStats stats_;
    
    struct PropertyCache {
        std::string property_name;
        Value cached_value;
        uint64_t access_count;
        
        PropertyCache() : access_count(0) {}
    };
    std::unordered_map<uint32_t, PropertyCache> property_cache_;
    
    Value execute_fast_add(const Value& left, const Value& right);
    Value execute_fast_property_load(const Value& object, const std::string& property, uint32_t cache_key);
    
    void execute_instruction(const BytecodeOp& op, BytecodeFunction* function, Context& context, uint32_t& pc);
    void execute_instruction_simple(const BytecodeOp& op, BytecodeFunction* function, Context& context, uint32_t& pc);
    
    void push(const Value& value) { stack_.push_back(value); }
    Value pop() { 
        if (stack_.empty()) return Value();
        Value v = stack_.back(); 
        stack_.pop_back(); 
        return v; 
    }
    Value peek() const { return stack_.empty() ? Value() : stack_.back(); }
};



class BytecodeJITBridge {
public:
    static bool should_jit_compile(BytecodeFunction* function);
    static bool compile_to_machine_code(BytecodeFunction* function);
    
    static constexpr uint32_t JIT_COMPILE_THRESHOLD = 50;
    static constexpr uint32_t HOT_SPOT_THRESHOLD = 10;    
};

}
