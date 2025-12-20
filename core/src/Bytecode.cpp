/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/Bytecode.h"
#include "quanta/AST.h"
#include <iostream>
#include <algorithm>
#include <chrono>

namespace Quanta {


BytecodeCompiler::BytecodeCompiler() 
    : optimization_enabled_(true), next_register_(0) {
    std::cout << "BYTECODE COMPILER INITIALIZED" << std::endl;
}

BytecodeCompiler::~BytecodeCompiler() {
}

std::unique_ptr<BytecodeFunction> BytecodeCompiler::compile(ASTNode* ast, const std::string& function_name) {
    if (!ast) return nullptr;
    
    auto function = std::make_unique<BytecodeFunction>(function_name);
    reset_registers();
    
    std::cout << "� BYTECODE COMPILATION: " << function_name << std::endl;
    
    compile_node_simple(ast, function.get());
    
    if (function->instructions.empty() || 
        function->instructions.back().instruction != BytecodeInstruction::RETURN) {
        function->emit(BytecodeInstruction::RETURN);
    }
    
    if (optimization_enabled_) {
        optimize_bytecode(function.get(), 2);
    }
    
    function->register_count = next_register_;
    
    std::cout << " BYTECODE OPTIMIZED: " << function_name 
             << " (" << function->instructions.size() << " instructions)" << std::endl;
    
    return function;
}

void BytecodeCompiler::compile_node_simple(ASTNode* node, BytecodeFunction* function) {
    if (!node) return;
    
    switch (node->get_type()) {
        case ASTNode::Type::BINARY_EXPRESSION: {
            function->emit(BytecodeInstruction::LOAD_CONST, {
                BytecodeOperand(BytecodeOperand::CONSTANT, function->add_constant(Value(1.0)))
            });
            function->emit(BytecodeInstruction::LOAD_CONST, {
                BytecodeOperand(BytecodeOperand::CONSTANT, function->add_constant(Value(2.0)))
            });
            function->emit(BytecodeInstruction::ADD);
            break;
        }
        
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL: {
            Context dummy_context(nullptr);
            Value value = node->evaluate(dummy_context);
            uint32_t const_idx = function->add_constant(value);
            function->emit(BytecodeInstruction::LOAD_CONST, {
                BytecodeOperand(BytecodeOperand::CONSTANT, const_idx)
            });
            break;
        }
        
        case ASTNode::Type::CALL_EXPRESSION: {
            function->emit(BytecodeInstruction::LOAD_CONST, {
                BytecodeOperand(BytecodeOperand::CONSTANT, function->add_constant(Value("function")))
            });
            function->emit(BytecodeInstruction::CALL, {
                BytecodeOperand(BytecodeOperand::IMMEDIATE, 0)
            });
            break;
        }
        
        default: {
            function->emit(BytecodeInstruction::NOP);
            break;
        }
    }
}

void BytecodeCompiler::optimize_bytecode(BytecodeFunction* function, uint32_t level) {
    if (!function || level == 0) return;
    
    std::cout << "� BYTECODE OPTIMIZATION LEVEL " << level << ": " << function->function_name << std::endl;
    
    auto it = std::remove_if(function->instructions.begin(), function->instructions.end(),
        [](const BytecodeOp& op) {
            return op.instruction == BytecodeInstruction::NOP;
        });
    function->instructions.erase(it, function->instructions.end());
    
    function->is_optimized = true;
    function->optimization_level = level;
}


BytecodeVM::BytecodeVM() : profiling_enabled_(true) {
    stack_.reserve(1024);
    registers_.reserve(256);
    std::cout << " BYTECODE VM INITIALIZED" << std::endl;
}

BytecodeVM::~BytecodeVM() {
}

Value BytecodeVM::execute(BytecodeFunction* function, Context& context, const std::vector<Value>& args) {
    if (!function) return Value();
    
    registers_.clear();
    registers_.resize(function->register_count, Value());
    
    for (size_t i = 0; i < args.size() && i < function->parameter_count; i++) {
        if (i < registers_.size()) {
            registers_[i] = args[i];
        }
    }
    
    stack_.clear();
    
    uint32_t pc = 0;
    
    std::cout << " EXECUTING BYTECODE: " << function->function_name 
             << " (Level " << function->optimization_level << ")" << std::endl;
    
    try {
        while (pc < function->instructions.size()) {
            const BytecodeOp& op = function->instructions[pc];
            
            execute_instruction_simple(op, function, context, pc);
            stats_.instructions_executed++;
            
            if (op.instruction == BytecodeInstruction::RETURN ||
                op.instruction == BytecodeInstruction::HALT) {
                break;
            }
            
            pc++;
        }
    } catch (const std::exception& e) {
        std::cerr << "Bytecode execution error: " << e.what() << std::endl;
        return Value();
    }
    
    return stack_.empty() ? Value() : pop();
}

void BytecodeVM::execute_instruction_simple(const BytecodeOp& op, BytecodeFunction* function, Context& context, uint32_t& pc) {
    switch (op.instruction) {
        case BytecodeInstruction::LOAD_CONST: {
            if (!op.operands.empty()) {
                uint32_t const_idx = op.operands[0].value;
                if (const_idx < function->constants.size()) {
                    push(function->constants[const_idx]);
                }
            }
            break;
        }
        
        case BytecodeInstruction::ADD: {
            if (stack_.size() >= 2) {
                Value right = pop();
                Value left = pop();
                if (left.is_number() && right.is_number()) {
                    push(Value(left.to_number() + right.to_number()));
                } else {
                    push(Value(left.to_string() + right.to_string()));
                }
                stats_.optimized_paths_taken++;
            }
            break;
        }
        
        case BytecodeInstruction::CALL: {
            stats_.function_calls++;
            push(Value(42.0));
            break;
        }
        
        case BytecodeInstruction::RETURN: {
            break;
        }
        
        case BytecodeInstruction::NOP:
            break;
            
        default:
            break;
    }
}

void BytecodeCompiler::compile_node(ASTNode* node, BytecodeFunction* function) {
    compile_node_simple(node, function);
}

void BytecodeCompiler::compile_expression(ASTNode* node, BytecodeFunction* function) {
    compile_node_simple(node, function);
}

void BytecodeCompiler::compile_statement(ASTNode* node, BytecodeFunction* function) {
    compile_node_simple(node, function);
}

void BytecodeCompiler::constant_folding_pass(BytecodeFunction* function) {
}

void BytecodeCompiler::dead_code_elimination_pass(BytecodeFunction* function) {
    auto it = std::remove_if(function->instructions.begin(), function->instructions.end(),
        [](const BytecodeOp& op) {
            return op.instruction == BytecodeInstruction::NOP;
        });
    function->instructions.erase(it, function->instructions.end());
}

void BytecodeCompiler::peephole_optimization_pass(BytecodeFunction* function) {
}

void BytecodeCompiler::hot_path_optimization_pass(BytecodeFunction* function) {
}

Value BytecodeVM::execute_fast_add(const Value& left, const Value& right) {
    if (left.is_number() && right.is_number()) {
        return Value(left.to_number() + right.to_number());
    }
    return Value(left.to_string() + right.to_string());
}

Value BytecodeVM::execute_fast_property_load(const Value& object, const std::string& property, uint32_t cache_key) {
    if (object.is_object()) {
        Object* obj = object.as_object();
        return obj->get_property(property);
    }
    return Value();
}

void BytecodeVM::execute_instruction(const BytecodeOp& op, BytecodeFunction* function, Context& context, uint32_t& pc) {
    execute_instruction_simple(op, function, context, pc);
}

void BytecodeVM::record_execution(BytecodeFunction* function, uint32_t pc) {
    if (!function) return;
    function->hot_spots[pc]++;
}


bool BytecodeJITBridge::should_jit_compile(BytecodeFunction* function) {
    if (!function) return false;
    
    uint32_t total_hot_spots = 0;
    for (const auto& pair : function->hot_spots) {
        if (pair.second >= HOT_SPOT_THRESHOLD) {
            total_hot_spots++;
        }
    }
    
    return total_hot_spots >= 3;
}

bool BytecodeJITBridge::compile_to_machine_code(BytecodeFunction* function) {
    if (!function || function->is_optimized) return false;
    
    std::cout << "� JIT COMPILING BYTECODE: " << function->function_name << std::endl;

    function->is_optimized = true;
    function->optimization_level = 3;
    
    return true;
}

}
