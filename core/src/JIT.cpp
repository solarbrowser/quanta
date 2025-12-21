/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/JIT.h"
#include "quanta/AST.h"
#include "quanta/Context.h"
#include <iostream>
#include <algorithm>

namespace Quanta {

JITCompiler::JITCompiler()
    : enabled_(true),
      bytecode_threshold_(100),
      optimized_threshold_(1000),
      machine_code_threshold_(10000) {

    std::cout << "[JIT] Quanta JIT Compiler initialized" << std::endl;
    std::cout << "[JIT] Tier thresholds:" << std::endl;
    std::cout << "[JIT]   Bytecode:     " << bytecode_threshold_ << " executions" << std::endl;
    std::cout << "[JIT]   Optimized:    " << optimized_threshold_ << " executions" << std::endl;
    std::cout << "[JIT]   Machine Code: " << machine_code_threshold_ << " executions" << std::endl;
}

JITCompiler::~JITCompiler() {
    for (auto& entry : machine_code_cache_) {
        if (entry.second.code_ptr) {
            // TODO: Free executable memory (platform-specific)
            entry.second.code_ptr = nullptr;
        }
    }

    for (auto& entry : function_machine_code_cache_) {
        if (entry.second.code_ptr) {
            entry.second.code_ptr = nullptr;
        }
    }

    std::cout << "[JIT] JIT Compiler shutdown. Final stats:" << std::endl;
    print_stats();
}

void JITCompiler::record_execution(ASTNode* node, uint64_t execution_time_ns) {
    if (!enabled_ || !node) return;

    auto& hotspot = hotspots_[node];
    hotspot.node = node;
    hotspot.execution_count++;
    hotspot.total_execution_time_ns += execution_time_ns;

    if (hotspot.execution_count == 1) {
        hotspot.first_execution = std::chrono::high_resolution_clock::now();
    }
    hotspot.last_execution = std::chrono::high_resolution_clock::now();

    if (hotspot.should_tier_up()) {
        switch (hotspot.current_tier) {
            case JITTier::Interpreter:
                if (compile_to_bytecode(node)) {
                    hotspot.current_tier = JITTier::Bytecode;
                    std::cout << "[JIT] Tiered up to Bytecode (execution count: "
                              << hotspot.execution_count << ")" << std::endl;
                }
                break;

            case JITTier::Bytecode:
                if (compile_to_optimized(node)) {
                    hotspot.current_tier = JITTier::Optimized;
                    std::cout << "[JIT] Tiered up to Optimized (execution count: "
                              << hotspot.execution_count << ")" << std::endl;
                }
                break;

            case JITTier::Optimized:
                if (compile_to_machine_code(node)) {
                    hotspot.current_tier = JITTier::MachineCode;
                    std::cout << "[JIT] Tiered up to Machine Code (execution count: "
                              << hotspot.execution_count << ")" << std::endl;
                }
                break;

            default:
                break;
        }
    }
}

void JITCompiler::record_type_feedback(ASTNode* node, const std::string& operation, const Value& value) {
    if (!enabled_ || !node) return;

    auto& hotspot = hotspots_[node];
    auto& feedback = hotspot.operation_types[operation];
    feedback.record_type(value);
}

bool JITCompiler::try_execute_jit(ASTNode* node, Context& ctx, Value& result) {
    if (!enabled_ || !node) return false;

    auto start = std::chrono::high_resolution_clock::now();

    auto mc_it = machine_code_cache_.find(node);
    if (mc_it != machine_code_cache_.end()) {
        stats_.cache_hits++;
        // TODO: Execute machine code
        // result = execute_machine_code(mc_it->second, ctx, nullptr, 0);
        // For now, fall through to bytecode
    }

    auto bc_it = bytecode_cache_.find(node);
    if (bc_it != bytecode_cache_.end()) {
        stats_.cache_hits++;
        result = execute_bytecode(bc_it->second, ctx);

        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        stats_.total_jit_time_ns += elapsed;

        return true;
    }

    stats_.cache_misses++;
    return false;
}

bool JITCompiler::compile_to_bytecode(ASTNode* node) {
    if (!node) return false;

    auto start = std::chrono::high_resolution_clock::now();

    CompiledBytecode compiled;
    compiled.tier = JITTier::Bytecode;
    compiled.compile_time = start;

    if (!generate_bytecode_for_node(node, compiled.instructions)) {
        return false;
    }

    bytecode_cache_[node] = compiled;
    stats_.total_compilations++;
    stats_.bytecode_compilations++;

    return true;
}

bool JITCompiler::generate_bytecode_for_node(ASTNode* node, std::vector<BytecodeOp>& instructions) {
    if (!node) return false;

    ASTNode::Type type = node->get_type();

    switch (type) {
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
            instructions.push_back(BytecodeOp(BytecodeInstruction::LOAD_CONST));
            return true;

        case ASTNode::Type::IDENTIFIER:
            instructions.push_back(BytecodeOp(BytecodeInstruction::LOAD_VAR));
            return true;

        case ASTNode::Type::BINARY_EXPRESSION:
            // TODO: Get left/right from AST node and recursively compile
            instructions.push_back(BytecodeOp(BytecodeInstruction::LOAD_VAR));  // left (placeholder)
            instructions.push_back(BytecodeOp(BytecodeInstruction::LOAD_VAR));  // right (placeholder)
            instructions.push_back(BytecodeOp(BytecodeInstruction::ADD));       // operation
            return true;

        case ASTNode::Type::ASSIGNMENT_EXPRESSION:
            instructions.push_back(BytecodeOp(BytecodeInstruction::LOAD_VAR));
            instructions.push_back(BytecodeOp(BytecodeInstruction::STORE_VAR));
            return true;

        case ASTNode::Type::CALL_EXPRESSION:
            instructions.push_back(BytecodeOp(BytecodeInstruction::LOAD_VAR));
            instructions.push_back(BytecodeOp(BytecodeInstruction::CALL));
            return true;

        case ASTNode::Type::RETURN_STATEMENT:
            instructions.push_back(BytecodeOp(BytecodeInstruction::RETURN));
            return true;

        default:
            instructions.push_back(BytecodeOp(BytecodeInstruction::NOP));
            return true;
    }
}

bool JITCompiler::compile_to_optimized(ASTNode* node) {
    if (!node) return false;

    auto bc_it = bytecode_cache_.find(node);
    if (bc_it == bytecode_cache_.end()) {
        return false; 

    auto hs_it = hotspots_.find(node);
    if (hs_it == hotspots_.end()) {
        return false;
    }

    const auto& hotspot = hs_it->second;

    // TODO: Apply type-based optimizations to bytecode
    bc_it->second.tier = JITTier::Optimized;
    stats_.optimized_compilations++;

    return true;
}

bool JITCompiler::compile_to_machine_code(ASTNode* node) {
    if (!node) return false;

    // Get type feedback
    auto hs_it = hotspots_.find(node);
    if (hs_it == hotspots_.end()) {
        return false;
    }

    const auto& hotspot = hs_it->second;

    // TODO: Generate x86-64 machine code

    stats_.machine_code_compilations++;
    return false; 
}

Value JITCompiler::execute_bytecode(const CompiledBytecode& compiled, Context& ctx) {
    std::vector<Value> stack;
    size_t ip = 0; 

    while (ip < compiled.instructions.size()) {
        const BytecodeOp& op = compiled.instructions[ip];

        switch (op.instruction) {
            case BytecodeInstruction::NOP:
                break;

            case BytecodeInstruction::LOAD_CONST: {
                // TODO: Load constant from constant pool
                stack.push_back(Value(0.0));
                break;
            }

            case BytecodeInstruction::LOAD_VAR: {
                // TODO: Load variable from context
                stack.push_back(Value());
                break;
            }

            case BytecodeInstruction::STORE_VAR: {
                // TODO: Store top of stack to variable
                if (!stack.empty()) {
                    stack.pop_back();
                }
                break;
            }

            case BytecodeInstruction::ADD: {
                if (stack.size() >= 2) {
                    Value right = stack.back(); stack.pop_back();
                    Value left = stack.back(); stack.pop_back();

                    // TODO: proper type handling
                    if (left.is_number() && right.is_number()) {
                        stack.push_back(Value(left.as_number() + right.as_number()));
                    } else {
                        stack.push_back(Value());
                    }
                }
                break;
            }

            case BytecodeInstruction::SUB: {
                if (stack.size() >= 2) {
                    Value right = stack.back(); stack.pop_back();
                    Value left = stack.back(); stack.pop_back();
                    if (left.is_number() && right.is_number()) {
                        stack.push_back(Value(left.as_number() - right.as_number()));
                    } else {
                        stack.push_back(Value());
                    }
                }
                break;
            }

            case BytecodeInstruction::MUL: {
                if (stack.size() >= 2) {
                    Value right = stack.back(); stack.pop_back();
                    Value left = stack.back(); stack.pop_back();
                    if (left.is_number() && right.is_number()) {
                        stack.push_back(Value(left.as_number() * right.as_number()));
                    } else {
                        stack.push_back(Value());
                    }
                }
                break;
            }

            case BytecodeInstruction::DIV: {
                if (stack.size() >= 2) {
                    Value right = stack.back(); stack.pop_back();
                    Value left = stack.back(); stack.pop_back();
                    if (left.is_number() && right.is_number() && right.as_number() != 0) {
                        stack.push_back(Value(left.as_number() / right.as_number()));
                    } else {
                        stack.push_back(Value());
                    }
                }
                break;
            }

            case BytecodeInstruction::RETURN: {
                // Return top of stack
                if (!stack.empty()) {
                    return stack.back();
                }
                return Value();
            }

            case BytecodeInstruction::CALL: {
                // TODO: Function call
                break;
            }

            default:
                return Value();
        }

        ip++;
    }

    if (!stack.empty()) {
        return stack.back();
    }
    return Value();
}

void JITCompiler::clear_cache() {
    bytecode_cache_.clear();
    machine_code_cache_.clear();
    hotspots_.clear();
    stats_ = Stats();
}

void JITCompiler::invalidate_node(ASTNode* node) {
    bytecode_cache_.erase(node);
    machine_code_cache_.erase(node);
    hotspots_.erase(node);
}

void JITCompiler::print_stats() const {
    std::cout << "\n=== JIT Compiler Statistics ===" << std::endl;
    std::cout << "Total Compilations:    " << stats_.total_compilations << std::endl;
    std::cout << "  Bytecode:            " << stats_.bytecode_compilations << std::endl;
    std::cout << "  Optimized:           " << stats_.optimized_compilations << std::endl;
    std::cout << "  Machine Code:        " << stats_.machine_code_compilations << std::endl;
    std::cout << "\nCache Performance:" << std::endl;
    std::cout << "  Hits:                " << stats_.cache_hits << std::endl;
    std::cout << "  Misses:              " << stats_.cache_misses << std::endl;
    std::cout << "  Hit Ratio:           " << (stats_.get_cache_hit_ratio() * 100.0) << "%" << std::endl;
    std::cout << "  Deoptimizations:     " << stats_.deoptimizations << std::endl;

    if (stats_.total_jit_time_ns > 0 && stats_.total_interpreter_time_ns > 0) {
        std::cout << "\nPerformance:" << std::endl;
        std::cout << "  JIT Time:            " << (stats_.total_jit_time_ns / 1000000.0) << "ms" << std::endl;
        std::cout << "  Interpreter Time:    " << (stats_.total_interpreter_time_ns / 1000000.0) << "ms" << std::endl;
        std::cout << "  Speedup:             " << stats_.get_speedup() << "x" << std::endl;
    }

    std::cout << "================================\n" << std::endl;
}

// machine code jit

MachineCodeGenerator::MachineCodeGenerator() {
    code_buffer_.reserve(4096);
}

MachineCodeGenerator::~MachineCodeGenerator() {
    // f
}

CompiledMachineCode MachineCodeGenerator::compile(ASTNode* node, const TypeFeedback& feedback) {
    CompiledMachineCode result;

    // TODO: Implement x86-64 code generation

    return result;
}

CompiledMachineCode MachineCodeGenerator::compile_function(Function* func, const TypeFeedback& feedback) {
    CompiledMachineCode result;

    // TODO: Implement function compilation

    return result;
}

void MachineCodeGenerator::free_code(CompiledMachineCode& compiled) {
    if (compiled.code_ptr) {
        // TODO: Platform-specific memory deallocation
        compiled.code_ptr = nullptr;
        compiled.code_size = 0;
    }
}

uint8_t* MachineCodeGenerator::allocate_executable_memory(size_t size) {
    // TODO: 
    // Windows: VirtualAlloc with PAGE_EXECUTE_READWRITE
    // Linux: mmap with PROT_EXEC | PROT_READ | PROT_WRITE
    return nullptr;
}

void MachineCodeGenerator::free_executable_memory(uint8_t* ptr, size_t size) {
    // TODO:
    // Windows: VirtualFree
    // Linux: munmap
}

// x86-64 instruction emitters (PHASE 2)
void MachineCodeGenerator::emit_prologue() {
    // push rbp
    // mov rbp, rsp
}

void MachineCodeGenerator::emit_epilogue() {
    // mov rsp, rbp
    // pop rbp
}

void MachineCodeGenerator::emit_mov_rax_imm(int64_t value) {
    // movabs rax, value
}

void MachineCodeGenerator::emit_add_rax_rbx() {
    // add rax, rbx
}

void MachineCodeGenerator::emit_ret() {
    // ret
}

} // namespace Quanta
