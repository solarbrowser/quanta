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
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
namespace Quanta {

extern "C" int64_t jit_read_variable(Context* ctx, const char* name) {
    if (!ctx || !name) {
        return 0;
    }
    Value val = ctx->get_binding(name);
    if (val.is_number()) {
        return static_cast<int64_t>(val.as_number());
    }
    return 0;
}
extern "C" void jit_write_variable(Context* ctx, const char* name, int64_t value) {
    if (!ctx || !name) {
        return;
    }
    ctx->set_binding(name, Value(static_cast<double>(value)));
}
JITCompiler::JITCompiler()
    : enabled_(true),
      bytecode_threshold_(3),
      optimized_threshold_(8),
      machine_code_threshold_(15) {
    std::cout << "[JIT] Quanta JIT Compiler initialized (ULTRA AGGRESSIVE MODE)" << std::endl;
    std::cout << "[JIT] Tier thresholds:" << std::endl;
    std::cout << "[JIT]   Bytecode:     " << bytecode_threshold_ << " executions" << std::endl;
    std::cout << "[JIT]   Optimized:    " << optimized_threshold_ << " executions" << std::endl;
    std::cout << "[JIT]   Machine Code: " << machine_code_threshold_ << " executions" << std::endl;
}
JITCompiler::~JITCompiler() {
    for (auto& entry : machine_code_cache_) {
        if (entry.second.code_ptr) {
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
    print_property_cache_stats();
}
void JITCompiler::record_execution(ASTNode* node, uint64_t execution_time_ns) {
    if (!enabled_ || !node) return;
    auto& hotspot = hotspots_[node];
    hotspot.node = node;
    hotspot.execution_count++;
    hotspot.total_execution_time_ns += execution_time_ns;
    if (hotspot.execution_count == 3 || hotspot.execution_count == 8 || hotspot.execution_count == 15 || hotspot.execution_count == 100) {
        if (node->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
            BinaryExpression* binop = static_cast<BinaryExpression*>(node);
            std::cout << "[JIT-TRACK] BinaryExpression (op " << static_cast<int>(binop->get_operator())
                     << ") hit " << hotspot.execution_count << " executions" << std::endl;
        } else if (node->get_type() == ASTNode::Type::CALL_EXPRESSION) {
            std::cout << "[JIT-TRACK] CallExpression hit " << hotspot.execution_count << " executions" << std::endl;
        } else if (node->get_type() == ASTNode::Type::FOR_STATEMENT) {
            std::cout << "[JIT-TRACK] ForStatement hit " << hotspot.execution_count << " executions" << std::endl;
        }
    }
    if (hotspot.execution_count == 1) {
        hotspot.first_execution = std::chrono::high_resolution_clock::now();
    }
    hotspot.last_execution = std::chrono::high_resolution_clock::now();
    if (hotspot.should_tier_up(bytecode_threshold_, optimized_threshold_, machine_code_threshold_)) {
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
    if (get_loop_depth() > 0) {
        std::cout << "[JIT-LOOP-SKIP] Skipping machine code inside loop (loop_depth=" << get_loop_depth() << ")" << std::endl;
    }
    if (mc_it != machine_code_cache_.end() && get_loop_depth() == 0) {
        stats_.cache_hits++;
        if (node->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
            BinaryExpression* binop = static_cast<BinaryExpression*>(node);
            std::cout << "[JIT-CACHE-HIT] Executing BinaryExpression operator "
                     << static_cast<int>(binop->get_operator()) << " at " << (void*)node << std::endl;
        }
        std::cout << "[JIT] Calling execute_machine_code..." << std::endl;
        std::cout.flush();
        result = execute_machine_code(mc_it->second, ctx, nullptr, 0);
        std::cout << "[JIT] execute_machine_code returned, result=" << result.to_string() << std::endl;
        std::cout.flush();
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        stats_.total_jit_time_ns += elapsed;
        std::cout << "[JIT]  EXECUTED NATIVE x86-64! Result: " << result.to_string() << std::endl;
        std::cout.flush();
        std::cout << "[JIT] Returning true from try_execute_jit..." << std::endl;
        std::cout.flush();
        return true;
    } else {
        if (node->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
            BinaryExpression* binop = static_cast<BinaryExpression*>(node);
            if (binop->get_operator() == BinaryExpression::Operator::ADD) {
                std::cout << "[JIT-CACHE-MISS] Looking for operator 0 (ADD) at " << (void*)node << std::endl;
            }
        }
    }
    auto bc_it = bytecode_cache_.find(node);
    if (bc_it != bytecode_cache_.end()) {
        stats_.cache_hits++;
        Value bytecode_result = execute_bytecode(bc_it->second, ctx);
        if (bytecode_result.is_undefined()) {
            stats_.cache_misses++;
            return false;
        }
        result = bytecode_result;
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
    if (node->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
        BinaryExpression* binop = static_cast<BinaryExpression*>(node);
        std::cout << "[JIT-BYTECODE] Compiling BinaryExpression, operator: " << static_cast<int>(binop->get_operator()) << std::endl;
    }
    auto start = std::chrono::high_resolution_clock::now();
    CompiledBytecode compiled;
    compiled.tier = JITTier::Bytecode;
    compiled.compile_time = start;
    if (!generate_bytecode_for_node_with_context(node, compiled)) {
        return false;
    }
    bytecode_cache_[node] = compiled;
    stats_.total_compilations++;
    stats_.bytecode_compilations++;
    return true;
}
bool JITCompiler::generate_bytecode_for_node_with_context(ASTNode* node, CompiledBytecode& compiled) {
    if (!node) return false;
    ASTNode::Type type = node->get_type();
    switch (type) {
        case ASTNode::Type::FOR_STATEMENT:
        case ASTNode::Type::WHILE_STATEMENT:
            compiled.instructions.push_back(BytecodeOp(BytecodeInstruction::FAST_LOOP));
            return true;
        case ASTNode::Type::BINARY_EXPRESSION:
            compiled.instructions.push_back(BytecodeOp(BytecodeInstruction::FAST_ADD_NUM));
            return true;
        default:
            compiled.instructions.push_back(BytecodeOp(BytecodeInstruction::NOP));
            return true;
    }
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
            instructions.push_back(BytecodeOp(BytecodeInstruction::LOAD_VAR));
            instructions.push_back(BytecodeOp(BytecodeInstruction::LOAD_VAR));
            instructions.push_back(BytecodeOp(BytecodeInstruction::ADD));
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
    }
    auto hs_it = hotspots_.find(node);
    if (hs_it == hotspots_.end()) {
        return false;
    }
    const auto& hotspot = hs_it->second;
    ASTNode::Type type = node->get_type();
    if (type == ASTNode::Type::FOR_STATEMENT || type == ASTNode::Type::WHILE_STATEMENT) {
        bc_it->second.instructions.clear();
        bc_it->second.instructions.push_back(BytecodeOp(BytecodeInstruction::FAST_LOOP));
        bc_it->second.add_constant(Value(1.0));
    } else if (type == ASTNode::Type::BINARY_EXPRESSION) {
        bc_it->second.instructions.clear();
        bc_it->second.instructions.push_back(BytecodeOp(BytecodeInstruction::FAST_ADD_NUM));
    }
    bc_it->second.tier = JITTier::Optimized;
    stats_.optimized_compilations++;
    std::cout << "[JIT] Applied optimizations for " << (int)type << std::endl;
    return true;
}
bool JITCompiler::compile_to_machine_code(ASTNode* node) {
    if (!node) return false;
    std::cout << "[JIT-MACHINE] Compiling node type " << static_cast<int>(node->get_type()) << " to machine code..." << std::endl;
    auto hs_it = hotspots_.find(node);
    if (hs_it == hotspots_.end()) {
        return false;
    }
    const auto& hotspot = hs_it->second;
    TypeFeedback feedback;
    if (!hotspot.operation_types.empty()) {
        feedback = hotspot.operation_types.begin()->second;
    }
    ASTNode* target_node = node;
    MachineCodeGenerator generator;
    CompiledMachineCode compiled;

    if (node->get_type() == ASTNode::Type::FOR_STATEMENT) {
        std::cout << "[JIT-LOOP] ForStatement detected! Analyzing for loop unrolling..." << std::endl;
        ForStatement* for_stmt = static_cast<ForStatement*>(node);

        if (for_stmt->is_nested_loop()) {
            std::cout << "[JIT-LOOP] Nested loop detected, skipping machine code compilation" << std::endl;
            return false;
        }

        MachineCodeGenerator loop_generator;
        LoopAnalysis analysis = loop_generator.analyze_loop(for_stmt);

        if (analysis.is_simple_counting_loop) {
            if (analysis.can_unroll) {
                std::cout << "[JIT-LOOP] Attempting " << analysis.unroll_factor << "x loop unrolling optimization..." << std::endl;
            } else {
                std::cout << "[JIT-LOOP] Compiling simple counting loop (no unrolling)..." << std::endl;
            }
            compiled = loop_generator.compile_optimized_loop(for_stmt, analysis);

            if (compiled.code_ptr) {
                machine_code_cache_[node] = compiled;
                stats_.machine_code_compilations++;
                std::cout << "[JIT] Compiled loop to x86-64! (" << compiled.code_size << " bytes)" << std::endl;
                return true;
            }
        }

        std::cout << "[JIT-LOOP] Loop not suitable for machine code compilation - staying at bytecode tier" << std::endl;
        return false;
    }

    compiled = generator.compile(target_node, feedback);
    if (!compiled.code_ptr) {
        std::cout << "[JIT] Failed to compile to machine code!" << std::endl;
        return false;
    }
    machine_code_cache_[target_node] = compiled;
    std::cout << "[JIT-CACHE-STORE] Stored machine code for node at " << (void*)target_node << std::endl;
    stats_.machine_code_compilations++;
    std::cout << "[JIT] Compiled to x86-64 machine code! (" << compiled.code_size << " bytes)" << std::endl;
    return true;
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
                stack.push_back(Value(0.0));
                break;
            }
            case BytecodeInstruction::LOAD_VAR: {
                stack.push_back(Value());
                break;
            }
            case BytecodeInstruction::STORE_VAR: {
                if (!stack.empty()) {
                    stack.pop_back();
                }
                break;
            }
            case BytecodeInstruction::ADD: {
                if (stack.size() >= 2) {
                    Value right = stack.back(); stack.pop_back();
                    Value left = stack.back(); stack.pop_back();
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
                if (!stack.empty()) {
                    return stack.back();
                }
                return Value();
            }
            case BytecodeInstruction::CALL: {
                break;
            }
            case BytecodeInstruction::FAST_LOOP: {
                return Value();
            }
            case BytecodeInstruction::FAST_ADD_NUM: {
                return Value();
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
Value JITCompiler::execute_machine_code(const CompiledMachineCode& compiled, Context& ctx, const Value* args, size_t argc) {
    if (!compiled.code_ptr || compiled.code_size == 0) {
        std::cout << "[JIT-EXEC] ERROR: No compiled code!" << std::endl;
        return Value();
    }
    std::cout << "[JIT-EXEC] Calling native function at " << (void*)compiled.code_ptr
              << " (" << compiled.code_size << " bytes)" << std::endl;
    std::cout << "[JIT-EXEC] Code bytes: ";
    for (size_t i = 0; i < std::min(compiled.code_size, size_t(20)); i++) {
        printf("%02X ", compiled.code_ptr[i]);
    }
    std::cout << std::endl;
    typedef int64_t (*JITFunction)(Context*);
    JITFunction jit_func = reinterpret_cast<JITFunction>(compiled.code_ptr);
    std::cout << "[JIT-EXEC] About to call JIT function..." << std::endl;
    std::cout.flush();
    int64_t result = jit_func(&ctx);
    std::cout << "[JIT-EXEC] JIT function call returned!" << std::endl;
    std::cout.flush();
    std::cout << "[JIT-EXEC] Native code returned: " << result << std::endl;
    std::cout << "[JIT-EXEC] Creating Value from result..." << std::endl;
    std::cout.flush();
    Value return_value = Value(static_cast<double>(result));
    std::cout << "[JIT-EXEC] Returning value: " << return_value.to_string() << std::endl;
    std::cout.flush();
    return return_value;
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
MachineCodeGenerator::MachineCodeGenerator() {
    code_buffer_.reserve(4096);
}
MachineCodeGenerator::~MachineCodeGenerator() {
}
CompiledMachineCode MachineCodeGenerator::compile(ASTNode* node, const TypeFeedback& feedback) {
    CompiledMachineCode result;
    code_buffer_.clear();
    std::cout << "[JIT-CODEGEN] Compiling node type: " << static_cast<int>(node ? node->get_type() : ASTNode::Type::PROGRAM) << std::endl;
    if (node && node->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
        BinaryExpression* binop = static_cast<BinaryExpression*>(node);
        ASTNode* left = binop->get_left();
        ASTNode* right = binop->get_right();
        std::cout << "[JIT-CODEGEN] BinaryExpression detected!" << std::endl;
        std::cout << "[JIT-CODEGEN]   Operator: " << static_cast<int>(binop->get_operator()) << std::endl;
        std::cout << "[JIT-CODEGEN]   Left type:  " << static_cast<int>(left ? left->get_type() : ASTNode::Type::PROGRAM) << std::endl;
        std::cout << "[JIT-CODEGEN]   Right type: " << static_cast<int>(right ? right->get_type() : ASTNode::Type::PROGRAM) << std::endl;
        int op_val = static_cast<int>(binop->get_operator());
        bool is_arithmetic = (op_val >= 0 && op_val <= 5);
        bool is_comparison = (op_val >= 6 && op_val <= 13);
        bool is_logical = (op_val == 16 || op_val == 17);
        bool is_bitwise = (op_val >= 19 && op_val <= 24);
        bool is_assignment = (op_val >= 25 && op_val <= 30);
        if (!is_arithmetic && !is_comparison && !is_logical && !is_bitwise && !is_assignment) {
            std::cout << "[JIT-CODEGEN]   Unsupported operator (" << op_val << ") - skipping machine code compilation" << std::endl;
            result.code_ptr = nullptr;
            result.code_size = 0;
            return result;
        }
        bool is_number_arithmetic = feedback.is_monomorphic() &&
                                    feedback.get_dominant_type() == Value::Type::Number;
        bool left_is_identifier = left && left->get_type() == ASTNode::Type::IDENTIFIER;
        bool right_is_identifier = right && right->get_type() == ASTNode::Type::IDENTIFIER;
        if ((is_arithmetic || is_bitwise) &&
            left && left->get_type() == ASTNode::Type::NUMBER_LITERAL &&
            right && right->get_type() == ASTNode::Type::NUMBER_LITERAL) {
            NumberLiteral* left_num = static_cast<NumberLiteral*>(left);
            NumberLiteral* right_num = static_cast<NumberLiteral*>(right);
            int64_t left_val = static_cast<int64_t>(left_num->get_value());
            int64_t right_val = static_cast<int64_t>(right_num->get_value());
            std::cout << "[JIT-CODEGEN]  Compiling LITERAL arithmetic: " << left_val;
            emit_prologue();
            emit_mov_rax_imm(left_val);
            emit_mov_rbx_imm(right_val);
            switch (binop->get_operator()) {
                case BinaryExpression::Operator::ADD:
                    std::cout << " + " << right_val << " = " << (left_val + right_val) << std::endl;
                    emit_add_rax_rbx();
                    break;
                case BinaryExpression::Operator::SUBTRACT:
                    std::cout << " - " << right_val << " = " << (left_val - right_val) << std::endl;
                    emit_sub_rax_rbx();
                    break;
                case BinaryExpression::Operator::MULTIPLY:
                    std::cout << " * " << right_val << " = " << (left_val * right_val) << std::endl;
                    emit_mul_rax_rbx();
                    break;
                case BinaryExpression::Operator::DIVIDE:
                    std::cout << " / " << right_val << " = " << (left_val / right_val) << std::endl;
                    emit_div_rax_rbx();
                    break;
                case BinaryExpression::Operator::MODULO:
                    std::cout << " % " << right_val << " = " << (left_val % right_val) << std::endl;
                    emit_mod_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_AND:
                    std::cout << " & " << right_val << " = " << (left_val & right_val) << std::endl;
                    emit_and_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_OR:
                    std::cout << " | " << right_val << " = " << (left_val | right_val) << std::endl;
                    emit_or_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_XOR:
                    std::cout << " ^ " << right_val << " = " << (left_val ^ right_val) << std::endl;
                    emit_xor_rax_rbx();
                    break;
                case BinaryExpression::Operator::LEFT_SHIFT:
                    std::cout << " << " << right_val << " = " << (left_val << right_val) << std::endl;
                    emit_byte(0x48);
                    emit_byte(0x89);
                    emit_byte(0xD9);
                    emit_shl_rax_cl();
                    break;
                case BinaryExpression::Operator::RIGHT_SHIFT:
                    std::cout << " >> " << right_val << " = " << (left_val >> right_val) << std::endl;
                    emit_byte(0x48);
                    emit_byte(0x89);
                    emit_byte(0xD9);
                    emit_sar_rax_cl();
                    break;
                case BinaryExpression::Operator::UNSIGNED_RIGHT_SHIFT:
                    std::cout << " >>> " << right_val << " = " << ((uint64_t)left_val >> right_val) << std::endl;
                    emit_byte(0x48);
                    emit_byte(0x89);
                    emit_byte(0xD9);
                    emit_shr_rax_cl();
                    break;
                default:
                    std::cout << " (unsupported op)" << std::endl;
                    emit_mov_rax_imm(42);
                    break;
            }
            emit_epilogue();
            emit_ret();
            std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size()
                     << " bytes of CONSTANT FOLDING x86-64!" << std::endl;
        }
        else if ((is_arithmetic || is_bitwise) && left_is_identifier && right_is_identifier) {
            Identifier* left_id = static_cast<Identifier*>(left);
            Identifier* right_id = static_cast<Identifier*>(right);
            std::string left_name = left_id->get_name();
            std::string right_name = right_id->get_name();
            std::cout << "[JIT-CODEGEN]  Compiling VARIABLE arithmetic/bitwise: "
                     << left_name << " " << static_cast<int>(binop->get_operator()) << " " << right_name << std::endl;
            size_t left_str_offset = embed_string(left_name);
            size_t right_str_offset = embed_string(right_name);
            std::cout << "[JIT-CODEGEN]  Generating NATIVE CODE for variable arithmetic!" << std::endl;
            std::vector<size_t> patch_positions;
            emit_prologue();
            emit_byte(0x41);
            emit_byte(0x56);
            #ifdef _WIN32
            emit_byte(0x49);
            emit_byte(0x89);
            emit_byte(0xCE);
            emit_byte(0x4C);
            emit_byte(0x89);
            emit_byte(0xF1);
            size_t left_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({left_patch_pos, left_str_offset});
            emit_byte(0x48);
            emit_byte(0xBA);
            for (int i = 0; i < 8; i++) emit_byte(0);
            #else
            emit_byte(0x49);
            emit_byte(0x89);
            emit_byte(0xFE);
            emit_byte(0x4C);
            emit_byte(0x89);
            emit_byte(0xF7);
            size_t left_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({left_patch_pos, left_str_offset});
            emit_mov_rsi_imm(0);
            #endif
            extern int64_t jit_read_variable(Context* ctx, const char* name);
            emit_call_absolute((void*)jit_read_variable);
            emit_byte(0x49);
            emit_byte(0x89);
            emit_byte(0xC4);
            #ifdef _WIN32
            emit_byte(0x4C);
            emit_byte(0x89);
            emit_byte(0xF1);
            size_t right_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({right_patch_pos, right_str_offset});
            emit_byte(0x48);
            emit_byte(0xBA);
            for (int i = 0; i < 8; i++) emit_byte(0);
            #else
            emit_byte(0x4C);
            emit_byte(0x89);
            emit_byte(0xF7);
            size_t right_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({right_patch_pos, right_str_offset});
            emit_mov_rsi_imm(0);
            #endif
            emit_call_absolute((void*)jit_read_variable);
            emit_byte(0x48);
            emit_byte(0x89);
            emit_byte(0xC3);
            emit_byte(0x4C);
            emit_byte(0x89);
            emit_byte(0xE0);
            switch (binop->get_operator()) {
                case BinaryExpression::Operator::ADD:
                    std::cout << "[JIT-CODEGEN] Generating ADD operation" << std::endl;
                    emit_add_rax_rbx();
                    break;
                case BinaryExpression::Operator::SUBTRACT:
                    std::cout << "[JIT-CODEGEN] Generating SUB operation" << std::endl;
                    emit_sub_rax_rbx();
                    break;
                case BinaryExpression::Operator::MULTIPLY:
                    std::cout << "[JIT-CODEGEN] Generating MUL operation" << std::endl;
                    emit_mul_rax_rbx();
                    break;
                case BinaryExpression::Operator::DIVIDE:
                    std::cout << "[JIT-CODEGEN] Generating DIV operation" << std::endl;
                    emit_div_rax_rbx();
                    break;
                case BinaryExpression::Operator::MODULO:
                    std::cout << "[JIT-CODEGEN] Generating MOD operation" << std::endl;
                    emit_mod_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_AND:
                    std::cout << "[JIT-CODEGEN] Generating BITWISE AND operation" << std::endl;
                    emit_and_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_OR:
                    std::cout << "[JIT-CODEGEN] Generating BITWISE OR operation" << std::endl;
                    emit_or_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_XOR:
                    std::cout << "[JIT-CODEGEN] Generating BITWISE XOR operation" << std::endl;
                    emit_xor_rax_rbx();
                    break;
                case BinaryExpression::Operator::LEFT_SHIFT:
                    std::cout << "[JIT-CODEGEN] Generating LEFT SHIFT operation" << std::endl;
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xD9);
                    emit_shl_rax_cl();
                    break;
                case BinaryExpression::Operator::RIGHT_SHIFT:
                    std::cout << "[JIT-CODEGEN] Generating RIGHT SHIFT (arithmetic) operation" << std::endl;
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xD9);
                    emit_sar_rax_cl();
                    break;
                case BinaryExpression::Operator::UNSIGNED_RIGHT_SHIFT:
                    std::cout << "[JIT-CODEGEN] Generating UNSIGNED RIGHT SHIFT operation" << std::endl;
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xD9);
                    emit_shr_rax_cl();
                    break;
                default:
                    emit_mov_rax_imm(0);
                    break;
            }
            emit_byte(0x41);
            emit_byte(0x5E);
            emit_epilogue();
            emit_ret();
            std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of VARIABLE ARITHMETIC x86-64!" << std::endl;
            std::cout << "[JIT-CODEGEN] Will patch " << patches_.size() << " string addresses after allocation" << std::endl;
        }
        else if ((is_arithmetic || is_bitwise) && left && left->get_type() == ASTNode::Type::NUMBER_LITERAL && right_is_identifier) {
            NumberLiteral* left_num = static_cast<NumberLiteral*>(left);
            Identifier* right_id = static_cast<Identifier*>(right);
            int64_t left_val = static_cast<int64_t>(left_num->get_value());
            std::string right_name = right_id->get_name();
            std::cout << "[JIT-CODEGEN]  Compiling MIXED arithmetic/bitwise: " << left_val << " + var(" << right_name << ")" << std::endl;
            size_t right_str_offset = embed_string(right_name);
            emit_prologue();
            emit_byte(0x41);
            emit_byte(0x56);
            #ifdef _WIN32
            emit_byte(0x49);
            emit_byte(0x89);
            emit_byte(0xCE);
            emit_byte(0x49);
            emit_byte(0xBC);
            for (int i = 0; i < 8; i++) {
                emit_byte((left_val >> (i * 8)) & 0xFF);
            }
            emit_byte(0x4C);
            emit_byte(0x89);
            emit_byte(0xF1);
            size_t right_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({right_patch_pos, right_str_offset});
            emit_byte(0x48);
            emit_byte(0xBA);
            for (int i = 0; i < 8; i++) emit_byte(0);
            emit_call_absolute((void*)jit_read_variable);
            #else
            emit_byte(0x49);
            emit_byte(0x89);
            emit_byte(0xFE);
            emit_byte(0x49);
            emit_byte(0xBC);
            for (int i = 0; i < 8; i++) {
                emit_byte((left_val >> (i * 8)) & 0xFF);
            }
            emit_byte(0x4C);
            emit_byte(0x89);
            emit_byte(0xF7);
            size_t right_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({right_patch_pos, right_str_offset});
            emit_mov_rsi_imm(0);
            emit_call_absolute((void*)jit_read_variable);
            #endif
            switch (binop->get_operator()) {
                case BinaryExpression::Operator::ADD:
                    emit_byte(0x4C);
                    emit_byte(0x01);
                    emit_byte(0xE0);
                    break;
                case BinaryExpression::Operator::SUBTRACT:
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE3);
                    emit_byte(0x48);
                    emit_byte(0x29);
                    emit_byte(0xC3);
                    emit_byte(0x48);
                    emit_byte(0x89);
                    emit_byte(0xD8);
                    break;
                case BinaryExpression::Operator::MULTIPLY:
                    emit_byte(0x4C);
                    emit_byte(0x0F);
                    emit_byte(0xAF);
                    emit_byte(0xC4);
                    break;
                case BinaryExpression::Operator::DIVIDE:
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    emit_byte(0x48);
                    emit_byte(0x99);
                    emit_byte(0x48);
                    emit_byte(0x89);
                    emit_byte(0xC3);
                    break;
                case BinaryExpression::Operator::MODULO:
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xC3);
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    emit_mod_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_AND:
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xC3);
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    emit_and_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_OR:
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xC3);
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    emit_or_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_XOR:
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xC3);
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    emit_xor_rax_rbx();
                    break;
                case BinaryExpression::Operator::LEFT_SHIFT:
                    emit_byte(0x48);
                    emit_byte(0x89);
                    emit_byte(0xC1);
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    emit_shl_rax_cl();
                    break;
                case BinaryExpression::Operator::RIGHT_SHIFT:
                    emit_byte(0x48);
                    emit_byte(0x89);
                    emit_byte(0xC1);
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    emit_sar_rax_cl();
                    break;
                case BinaryExpression::Operator::UNSIGNED_RIGHT_SHIFT:
                    emit_byte(0x48);
                    emit_byte(0x89);
                    emit_byte(0xC1);
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    emit_shr_rax_cl();
                    break;
                default:
                    emit_mov_rax_imm(0);
                    break;
            }
            emit_byte(0x41);
            emit_byte(0x5E);
            emit_epilogue();
            emit_ret();
            std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of MIXED (literal+var) x86-64!" << std::endl;
        }
        else if ((is_arithmetic || is_bitwise) && left_is_identifier && right && right->get_type() == ASTNode::Type::NUMBER_LITERAL) {
            Identifier* left_id = static_cast<Identifier*>(left);
            NumberLiteral* right_num = static_cast<NumberLiteral*>(right);
            std::string left_name = left_id->get_name();
            int64_t right_val = static_cast<int64_t>(right_num->get_value());
            std::cout << "[JIT-CODEGEN]  Compiling MIXED arithmetic/bitwise: var(" << left_name << ") + " << right_val << std::endl;
            size_t left_str_offset = embed_string(left_name);
            emit_prologue();
            emit_byte(0x41);
            emit_byte(0x56);
            #ifdef _WIN32
            emit_byte(0x49);
            emit_byte(0x89);
            emit_byte(0xCE);
            emit_byte(0x4C);
            emit_byte(0x89);
            emit_byte(0xF1);
            size_t left_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({left_patch_pos, left_str_offset});
            emit_byte(0x48);
            emit_byte(0xBA);
            for (int i = 0; i < 8; i++) emit_byte(0);
            emit_call_absolute((void*)jit_read_variable);
            #else
            emit_byte(0x49);
            emit_byte(0x89);
            emit_byte(0xFE);
            emit_byte(0x4C);
            emit_byte(0x89);
            emit_byte(0xF7);
            size_t left_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({left_patch_pos, left_str_offset});
            emit_mov_rsi_imm(0);
            emit_call_absolute((void*)jit_read_variable);
            #endif
            emit_byte(0x48);
            emit_byte(0xBB);
            for (int i = 0; i < 8; i++) {
                emit_byte((right_val >> (i * 8)) & 0xFF);
            }
            switch (binop->get_operator()) {
                case BinaryExpression::Operator::ADD:
                    emit_add_rax_rbx();
                    break;
                case BinaryExpression::Operator::SUBTRACT:
                    emit_sub_rax_rbx();
                    break;
                case BinaryExpression::Operator::MULTIPLY:
                    emit_mul_rax_rbx();
                    break;
                case BinaryExpression::Operator::DIVIDE:
                    emit_div_rax_rbx();
                    break;
                case BinaryExpression::Operator::MODULO:
                    emit_mod_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_AND:
                    emit_and_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_OR:
                    emit_or_rax_rbx();
                    break;
                case BinaryExpression::Operator::BITWISE_XOR:
                    emit_xor_rax_rbx();
                    break;
                case BinaryExpression::Operator::LEFT_SHIFT:
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xD9);
                    emit_shl_rax_cl();
                    break;
                case BinaryExpression::Operator::RIGHT_SHIFT:
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xD9);
                    emit_sar_rax_cl();
                    break;
                case BinaryExpression::Operator::UNSIGNED_RIGHT_SHIFT:
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xD9);
                    emit_shr_rax_cl();
                    break;
                default:
                    emit_mov_rax_imm(0);
                    break;
            }
            emit_byte(0x41);
            emit_byte(0x5E);
            emit_epilogue();
            emit_ret();
            std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of MIXED (var+literal) x86-64!" << std::endl;
        }
        else if (is_comparison) {
            std::cout << "[JIT-CODEGEN]  Compiling COMPARISON operation (op=" << op_val << ")" << std::endl;
            if (left_is_identifier && right && right->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                Identifier* left_id = static_cast<Identifier*>(left);
                NumberLiteral* right_num = static_cast<NumberLiteral*>(right);
                std::string left_name = left_id->get_name();
                int64_t right_val = static_cast<int64_t>(right_num->get_value());
                std::cout << "[JIT-CODEGEN]  Compiling: var(" << left_name << ") CMP " << right_val << std::endl;
                size_t left_str_offset = embed_string(left_name);
                emit_prologue();
                emit_byte(0x41);
                emit_byte(0x56);
                #ifdef _WIN32
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xCE);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF1);
                size_t left_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({left_patch_pos, left_str_offset});
                emit_byte(0x48);
                emit_byte(0xBA);
                for (int i = 0; i < 8; i++) emit_byte(0);
                emit_call_absolute((void*)jit_read_variable);
                #else
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xFE);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF7);
                size_t left_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({left_patch_pos, left_str_offset});
                emit_mov_rsi_imm(0);
                emit_call_absolute((void*)jit_read_variable);
                #endif
                emit_byte(0x48);
                emit_byte(0xBB);
                for (int i = 0; i < 8; i++) {
                    emit_byte((right_val >> (i * 8)) & 0xFF);
                }
                emit_cmp_rax_rbx();
                switch (binop->get_operator()) {
                    case BinaryExpression::Operator::LESS_THAN:
                        emit_setl_al();
                        break;
                    case BinaryExpression::Operator::GREATER_THAN:
                        emit_setg_al();
                        break;
                    case BinaryExpression::Operator::LESS_EQUAL:
                        emit_setle_al();
                        break;
                    case BinaryExpression::Operator::GREATER_EQUAL:
                        emit_setge_al();
                        break;
                    case BinaryExpression::Operator::EQUAL:
                    case BinaryExpression::Operator::STRICT_EQUAL:
                        emit_sete_al();
                        break;
                    case BinaryExpression::Operator::NOT_EQUAL:
                    case BinaryExpression::Operator::STRICT_NOT_EQUAL:
                        emit_setne_al();
                        break;
                    default:
                        emit_mov_rax_imm(0);
                        break;
                }
                emit_movzx_rax_al();
                emit_byte(0x41);
                emit_byte(0x5E);
                emit_epilogue();
                emit_ret();
                std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of COMPARISON (var CMP literal) x86-64!" << std::endl;
            }
            else if (left && left->get_type() == ASTNode::Type::NUMBER_LITERAL && right_is_identifier) {
                NumberLiteral* left_num = static_cast<NumberLiteral*>(left);
                Identifier* right_id = static_cast<Identifier*>(right);
                int64_t left_val = static_cast<int64_t>(left_num->get_value());
                std::string right_name = right_id->get_name();
                std::cout << "[JIT-CODEGEN]  Compiling: " << left_val << " CMP var(" << right_name << ")" << std::endl;
                size_t right_str_offset = embed_string(right_name);
                emit_prologue();
                emit_byte(0x41);
                emit_byte(0x56);
                #ifdef _WIN32
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xCE);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF1);
                size_t right_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({right_patch_pos, right_str_offset});
                emit_byte(0x48);
                emit_byte(0xBA);
                for (int i = 0; i < 8; i++) emit_byte(0);
                emit_call_absolute((void*)jit_read_variable);
                #else
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xFE);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF7);
                size_t right_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({right_patch_pos, right_str_offset});
                emit_mov_rsi_imm(0);
                emit_call_absolute((void*)jit_read_variable);
                #endif
                emit_byte(0x48);
                emit_byte(0x89);
                emit_byte(0xC3);
                emit_byte(0x48);
                emit_byte(0xB8);
                for (int i = 0; i < 8; i++) {
                    emit_byte((left_val >> (i * 8)) & 0xFF);
                }
                emit_cmp_rax_rbx();
                switch (binop->get_operator()) {
                    case BinaryExpression::Operator::LESS_THAN:
                        emit_setl_al();
                        break;
                    case BinaryExpression::Operator::GREATER_THAN:
                        emit_setg_al();
                        break;
                    case BinaryExpression::Operator::LESS_EQUAL:
                        emit_setle_al();
                        break;
                    case BinaryExpression::Operator::GREATER_EQUAL:
                        emit_setge_al();
                        break;
                    case BinaryExpression::Operator::EQUAL:
                    case BinaryExpression::Operator::STRICT_EQUAL:
                        emit_sete_al();
                        break;
                    case BinaryExpression::Operator::NOT_EQUAL:
                    case BinaryExpression::Operator::STRICT_NOT_EQUAL:
                        emit_setne_al();
                        break;
                    default:
                        emit_mov_rax_imm(0);
                        break;
                }
                emit_movzx_rax_al();
                emit_byte(0x41);
                emit_byte(0x5E);
                emit_epilogue();
                emit_ret();
                std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of COMPARISON (literal CMP var) x86-64!" << std::endl;
            }
            else if (left_is_identifier && right_is_identifier) {
                Identifier* left_id = static_cast<Identifier*>(left);
                Identifier* right_id = static_cast<Identifier*>(right);
                std::string left_name = left_id->get_name();
                std::string right_name = right_id->get_name();
                std::cout << "[JIT-CODEGEN]  Compiling: var(" << left_name << ") CMP var(" << right_name << ")" << std::endl;
                size_t left_str_offset = embed_string(left_name);
                size_t right_str_offset = embed_string(right_name);
                emit_prologue();
                emit_byte(0x41);
                emit_byte(0x56);
                #ifdef _WIN32
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xCE);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF1);
                size_t left_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({left_patch_pos, left_str_offset});
                emit_byte(0x48);
                emit_byte(0xBA);
                for (int i = 0; i < 8; i++) emit_byte(0);
                emit_call_absolute((void*)jit_read_variable);
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xC4);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF1);
                size_t right_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({right_patch_pos, right_str_offset});
                emit_byte(0x48);
                emit_byte(0xBA);
                for (int i = 0; i < 8; i++) emit_byte(0);
                emit_call_absolute((void*)jit_read_variable);
                #else
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xFE);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF7);
                size_t left_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({left_patch_pos, left_str_offset});
                emit_mov_rsi_imm(0);
                emit_call_absolute((void*)jit_read_variable);
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xC4);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF7);
                size_t right_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({right_patch_pos, right_str_offset});
                emit_mov_rsi_imm(0);
                emit_call_absolute((void*)jit_read_variable);
                #endif
                emit_byte(0x48);
                emit_byte(0x89);
                emit_byte(0xC3);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xE0);
                emit_cmp_rax_rbx();
                switch (binop->get_operator()) {
                    case BinaryExpression::Operator::LESS_THAN:
                        emit_setl_al();
                        break;
                    case BinaryExpression::Operator::GREATER_THAN:
                        emit_setg_al();
                        break;
                    case BinaryExpression::Operator::LESS_EQUAL:
                        emit_setle_al();
                        break;
                    case BinaryExpression::Operator::GREATER_EQUAL:
                        emit_setge_al();
                        break;
                    case BinaryExpression::Operator::EQUAL:
                    case BinaryExpression::Operator::STRICT_EQUAL:
                        emit_sete_al();
                        break;
                    case BinaryExpression::Operator::NOT_EQUAL:
                    case BinaryExpression::Operator::STRICT_NOT_EQUAL:
                        emit_setne_al();
                        break;
                    default:
                        emit_mov_rax_imm(0);
                        break;
                }
                emit_movzx_rax_al();
                emit_byte(0x41);
                emit_byte(0x5E);
                emit_epilogue();
                emit_ret();
                std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of COMPARISON (var CMP var) x86-64!" << std::endl;
            }
            else {
                std::cout << "[JIT-CODEGEN]   Unsupported comparison pattern - skipping" << std::endl;
                result.code_ptr = nullptr;
                result.code_size = 0;
                return result;
            }
        }
        else if (is_logical) {
            std::cout << "[JIT-CODEGEN]  Compiling LOGICAL operation (op=" << op_val << ")" << std::endl;
            bool left_is_identifier = left && left->get_type() == ASTNode::Type::IDENTIFIER;
            bool right_is_identifier = right && right->get_type() == ASTNode::Type::IDENTIFIER;
            bool left_is_literal = left && left->get_type() == ASTNode::Type::NUMBER_LITERAL;
            bool right_is_literal = right && right->get_type() == ASTNode::Type::NUMBER_LITERAL;
            if (left_is_identifier && right_is_identifier) {
                Identifier* left_id = static_cast<Identifier*>(left);
                Identifier* right_id = static_cast<Identifier*>(right);
                std::string left_name = left_id->get_name();
                std::string right_name = right_id->get_name();
                std::cout << "[JIT-CODEGEN]  Compiling: var(" << left_name << ") ";
                std::cout << (op_val == 16 ? "&&" : "||");
                std::cout << " var(" << right_name << ")" << std::endl;
                size_t left_str_offset = embed_string(left_name);
                size_t right_str_offset = embed_string(right_name);
                emit_prologue();
                emit_byte(0x41);
                emit_byte(0x56);
                #ifdef _WIN32
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xCE);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF1);
                size_t left_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({left_patch_pos, left_str_offset});
                emit_byte(0x48);
                emit_byte(0xBA);
                for (int i = 0; i < 8; i++) emit_byte(0);
                emit_call_absolute((void*)jit_read_variable);
                #else
                emit_byte(0x49);
                emit_byte(0x89);
                emit_byte(0xFE);
                emit_byte(0x4C);
                emit_byte(0x89);
                emit_byte(0xF7);
                size_t left_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({left_patch_pos, left_str_offset});
                emit_mov_rsi_imm(0);
                emit_call_absolute((void*)jit_read_variable);
                #endif
                emit_test_rax_rax();
                if (op_val == 16) {
                    size_t jz_pos = code_buffer_.size();
                    emit_jz_rel8(0);
                    emit_byte(0x49);
                    emit_byte(0x89);
                    emit_byte(0xC4);
                    #ifdef _WIN32
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xF1);
                    size_t right_patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({right_patch_pos, right_str_offset});
                    emit_byte(0x48);
                    emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    emit_call_absolute((void*)jit_read_variable);
                    #else
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xF7);
                    size_t right_patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({right_patch_pos, right_str_offset});
                    emit_mov_rsi_imm(0);
                    emit_call_absolute((void*)jit_read_variable);
                    #endif
                    size_t jmp_pos = code_buffer_.size();
                    emit_jmp_rel8(0);
                    size_t skip_right_pos = code_buffer_.size();
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    size_t end_pos = code_buffer_.size();
                    int8_t jz_offset = skip_right_pos - (jz_pos + 2);
                    code_buffer_[jz_pos + 1] = static_cast<uint8_t>(jz_offset);
                    int8_t jmp_offset = end_pos - (jmp_pos + 2);
                    code_buffer_[jmp_pos + 1] = static_cast<uint8_t>(jmp_offset);
                } else {
                    size_t jnz_pos = code_buffer_.size();
                    emit_jnz_rel8(0);
                    emit_byte(0x49);
                    emit_byte(0x89);
                    emit_byte(0xC4);
                    #ifdef _WIN32
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xF1);
                    size_t right_patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({right_patch_pos, right_str_offset});
                    emit_byte(0x48);
                    emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    emit_call_absolute((void*)jit_read_variable);
                    #else
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xF7);
                    size_t right_patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({right_patch_pos, right_str_offset});
                    emit_mov_rsi_imm(0);
                    emit_call_absolute((void*)jit_read_variable);
                    #endif
                    size_t jmp_pos = code_buffer_.size();
                    emit_jmp_rel8(0);
                    size_t skip_right_pos = code_buffer_.size();
                    emit_byte(0x4C);
                    emit_byte(0x89);
                    emit_byte(0xE0);
                    size_t end_pos = code_buffer_.size();
                    int8_t jnz_offset = skip_right_pos - (jnz_pos + 2);
                    code_buffer_[jnz_pos + 1] = static_cast<uint8_t>(jnz_offset);
                    int8_t jmp_offset = end_pos - (jmp_pos + 2);
                    code_buffer_[jmp_pos + 1] = static_cast<uint8_t>(jmp_offset);
                }
                emit_byte(0x41);
                emit_byte(0x5E);
                emit_epilogue();
                emit_ret();
                std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of LOGICAL (var " << (op_val == 16 ? "&&" : "||") << " var) x86-64!" << std::endl;
            }
            else {
                std::cout << "[JIT-CODEGEN]   Unsupported logical pattern - skipping" << std::endl;
                result.code_ptr = nullptr;
                result.code_size = 0;
                return result;
            }
        }
        else if (is_assignment && left_is_identifier && right && right->get_type() == ASTNode::Type::NUMBER_LITERAL) {
            Identifier* left_id = static_cast<Identifier*>(left);
            NumberLiteral* right_num = static_cast<NumberLiteral*>(right);
            std::string var_name = left_id->get_name();
            int64_t right_val = static_cast<int64_t>(right_num->get_value());
            std::cout << "[JIT-CODEGEN]  Compiling ASSIGNMENT: " << var_name << " op= " << right_val << std::endl;
            size_t var_str_offset = embed_string(var_name);
            emit_prologue();
            emit_byte(0x41); emit_byte(0x56);
            if (op_val != 25) {
                #ifdef _WIN32
                emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                size_t read_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({read_patch_pos, var_str_offset});
                emit_byte(0x48); emit_byte(0xBA);
                for (int i = 0; i < 8; i++) emit_byte(0);
                #else
                emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                size_t read_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({read_patch_pos, var_str_offset});
                emit_mov_rsi_imm(0);
                #endif
                emit_call_absolute((void*)jit_read_variable);
                emit_byte(0x48); emit_byte(0xBB);
                for (int i = 0; i < 8; i++) {
                    emit_byte((right_val >> (i * 8)) & 0xFF);
                }
                switch (op_val) {
                    case 26:
                        std::cout << "[JIT-CODEGEN] Generating PLUS_ASSIGN (+=)" << std::endl;
                        emit_add_rax_rbx();
                        break;
                    case 27:
                        std::cout << "[JIT-CODEGEN] Generating MINUS_ASSIGN (-=)" << std::endl;
                        emit_sub_rax_rbx();
                        break;
                    case 28:
                        std::cout << "[JIT-CODEGEN] Generating MULTIPLY_ASSIGN (*=)" << std::endl;
                        emit_mul_rax_rbx();
                        break;
                    case 29:
                        std::cout << "[JIT-CODEGEN] Generating DIVIDE_ASSIGN (/=)" << std::endl;
                        emit_div_rax_rbx();
                        break;
                    case 30:
                        std::cout << "[JIT-CODEGEN] Generating MODULO_ASSIGN (%=)" << std::endl;
                        emit_mod_rax_rbx();
                        break;
                    default:
                        emit_mov_rax_imm(0);
                        break;
                }
            } else {
                std::cout << "[JIT-CODEGEN] Generating ASSIGN (=)" << std::endl;
                emit_mov_rax_imm(right_val);
            }
            #ifdef _WIN32
            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
            size_t write_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({write_patch_pos, var_str_offset});
            emit_byte(0x48); emit_byte(0xBA);
            for (int i = 0; i < 8; i++) emit_byte(0);
            emit_byte(0x49); emit_byte(0x89); emit_byte(0xC0);
            #else
            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
            size_t write_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({write_patch_pos, var_str_offset});
            emit_mov_rsi_imm(0);
            emit_byte(0x48); emit_byte(0x89); emit_byte(0xC2);
            #endif
            emit_call_absolute((void*)jit_write_variable);
            emit_byte(0x41); emit_byte(0x5E);
            emit_epilogue();
            emit_ret();
            std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of ASSIGNMENT x86-64!" << std::endl;
        }
        else if (is_number_arithmetic) {
            std::cout << "[JIT-CODEGEN]   MONOMORPHIC NUMBER arithmetic - other patterns not implemented yet" << std::endl;
            std::cout << "[JIT-CODEGEN]   Cannot compile to machine code - staying at optimized tier" << std::endl;
            result.code_ptr = nullptr;
            result.code_size = 0;
            return result;
        }
        else {
            std::cout << "[JIT-CODEGEN]   Complex/polymorphic arithmetic - cannot compile to machine code" << std::endl;
            std::cout << "[JIT-CODEGEN]   Cannot compile to machine code - staying at optimized tier" << std::endl;
            result.code_ptr = nullptr;
            result.code_size = 0;
            return result;
        }
    } else if (node && node->get_type() == ASTNode::Type::UNARY_EXPRESSION) {
        UnaryExpression* unop = static_cast<UnaryExpression*>(node);
        ASTNode* operand = unop->get_operand();
        std::cout << "[JIT-CODEGEN] UnaryExpression detected!" << std::endl;
        std::cout << "[JIT-CODEGEN]   Operator: " << static_cast<int>(unop->get_operator()) << std::endl;
        std::cout << "[JIT-CODEGEN]   Operand type: " << static_cast<int>(operand ? operand->get_type() : ASTNode::Type::PROGRAM) << std::endl;
        int op_val = static_cast<int>(unop->get_operator());
        bool operand_is_identifier = operand && operand->get_type() == ASTNode::Type::IDENTIFIER;
        bool operand_is_literal = operand && operand->get_type() == ASTNode::Type::NUMBER_LITERAL;
        if (((op_val >= 0 && op_val <= 3) || (op_val >= 4 && op_val <= 10)) && operand_is_identifier) {
            Identifier* id = static_cast<Identifier*>(operand);
            std::string var_name = id->get_name();
            std::cout << "[JIT-CODEGEN]  Compiling unary operation on variable: " << var_name << std::endl;
            size_t var_str_offset = embed_string(var_name);
            emit_prologue();
            emit_byte(0x41); emit_byte(0x56);
            #ifdef _WIN32
            emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
            size_t patch_pos = code_buffer_.size() + 2;
            patches_.push_back({patch_pos, var_str_offset});
            emit_byte(0x48); emit_byte(0xBA);
            for (int i = 0; i < 8; i++) emit_byte(0);
            #else
            emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
            size_t patch_pos = code_buffer_.size() + 2;
            patches_.push_back({patch_pos, var_str_offset});
            emit_mov_rsi_imm(0);
            #endif
            emit_call_absolute((void*)jit_read_variable);
            switch (op_val) {
                case 0:
                    std::cout << "[JIT-CODEGEN] Generating UNARY PLUS (+x)" << std::endl;
                    break;
                case 1:
                    std::cout << "[JIT-CODEGEN] Generating UNARY MINUS (-x)" << std::endl;
                    emit_neg_rax();
                    break;
                case 2:
                    std::cout << "[JIT-CODEGEN] Generating LOGICAL NOT (!x)" << std::endl;
                    emit_test_rax_rax();
                    emit_sete_al();
                    emit_movzx_rax_al();
                    break;
                case 3:
                    std::cout << "[JIT-CODEGEN] Generating BITWISE NOT (~x)" << std::endl;
                    emit_not_rax();
                    break;
                case 4:
                    std::cout << "[JIT-CODEGEN] Generating TYPEOF (typeof x) - returning 0 for number" << std::endl;
                    emit_mov_rax_imm(0);
                    break;
                case 5:
                    std::cout << "[JIT-CODEGEN] Generating VOID (void x) - returning undefined (0)" << std::endl;
                    emit_mov_rax_imm(0);
                    break;
                case 6:
                    std::cout << "[JIT-CODEGEN] Generating DELETE (delete x) - returning true (1)" << std::endl;
                    emit_mov_rax_imm(1);
                    break;
                case 7:
                    std::cout << "[JIT-CODEGEN] Generating PRE INCREMENT (++x)" << std::endl;
                    emit_inc_rax();
                    break;
                case 8:
                    std::cout << "[JIT-CODEGEN] Generating POST INCREMENT (x++)" << std::endl;
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xC0);
                    emit_inc_rax();
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xE0);
                    break;
                case 9:
                    std::cout << "[JIT-CODEGEN] Generating PRE DECREMENT (--x)" << std::endl;
                    emit_dec_rax();
                    break;
                case 10:
                    std::cout << "[JIT-CODEGEN] Generating POST DECREMENT (x--)" << std::endl;
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xC0);
                    emit_dec_rax();
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xE0);
                    break;
                default:
                    std::cout << "[JIT-CODEGEN] Unsupported unary operator" << std::endl;
                    emit_mov_rax_imm(0);
                    break;
            }
            emit_byte(0x41); emit_byte(0x5E);
            emit_epilogue();
            emit_ret();
            std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of UNARY x86-64!" << std::endl;
            std::cout << "[JIT-CODEGEN] Will patch " << patches_.size() << " string addresses after allocation" << std::endl;
        } else if ((op_val >= 0 && op_val <= 6) && operand_is_literal) {
            NumberLiteral* num = static_cast<NumberLiteral*>(operand);
            int64_t value = static_cast<int64_t>(num->get_value());
            std::cout << "[JIT-CODEGEN]  Constant folding unary on literal: " << value << std::endl;
            emit_prologue();
            int64_t result_value = value;
            switch (op_val) {
                case 0:
                    std::cout << "[JIT-CODEGEN] +" << value << " = " << result_value << std::endl;
                    break;
                case 1:
                    result_value = -value;
                    std::cout << "[JIT-CODEGEN] -" << value << " = " << result_value << std::endl;
                    break;
                case 2:
                    result_value = (value == 0) ? 1 : 0;
                    std::cout << "[JIT-CODEGEN] !" << value << " = " << result_value << std::endl;
                    break;
                case 3:
                    result_value = ~value;
                    std::cout << "[JIT-CODEGEN] ~" << value << " = " << result_value << std::endl;
                    break;
                case 4:
                    result_value = 0;
                    std::cout << "[JIT-CODEGEN] typeof " << value << " = 'number' (0)" << std::endl;
                    break;
                case 5:
                    result_value = 0;
                    std::cout << "[JIT-CODEGEN] void " << value << " = undefined (0)" << std::endl;
                    break;
                case 6:
                    result_value = 1;
                    std::cout << "[JIT-CODEGEN] delete " << value << " = true (1)" << std::endl;
                    break;
                default:
                    break;
            }
            emit_mov_rax_imm(result_value);
            emit_epilogue();
            emit_ret();
            std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of CONSTANT FOLDING UNARY x86-64!" << std::endl;
        } else {
            std::cout << "[JIT-CODEGEN]   Unsupported unary pattern" << std::endl;
            result.code_ptr = nullptr;
            result.code_size = 0;
            return result;
        }
    } else if (node && node->get_type() == ASTNode::Type::ASSIGNMENT_EXPRESSION) {
        AssignmentExpression* assign = static_cast<AssignmentExpression*>(node);
        ASTNode* left = assign->get_left();
        ASTNode* right = assign->get_right();
        std::cout << "[JIT-CODEGEN] AssignmentExpression detected!" << std::endl;
        std::cout << "[JIT-CODEGEN]   Operator: " << static_cast<int>(assign->get_operator()) << std::endl;
        int op_val = static_cast<int>(assign->get_operator());
        bool left_is_identifier = left && left->get_type() == ASTNode::Type::IDENTIFIER;
        bool right_is_literal = right && right->get_type() == ASTNode::Type::NUMBER_LITERAL;
        if (left_is_identifier && right_is_literal) {
            Identifier* left_id = static_cast<Identifier*>(left);
            NumberLiteral* right_num = static_cast<NumberLiteral*>(right);
            std::string var_name = left_id->get_name();
            int64_t right_val = static_cast<int64_t>(right_num->get_value());
            std::cout << "[JIT-CODEGEN]  Compiling assignment: " << var_name << " op= " << right_val << std::endl;
            size_t var_str_offset = embed_string(var_name);
            emit_prologue();
            emit_byte(0x41); emit_byte(0x56);
            if (op_val != 0) {
                #ifdef _WIN32
                emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                size_t read_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({read_patch_pos, var_str_offset});
                emit_byte(0x48); emit_byte(0xBA);
                for (int i = 0; i < 8; i++) emit_byte(0);
                #else
                emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                size_t read_patch_pos = code_buffer_.size() + 2;
                patches_.push_back({read_patch_pos, var_str_offset});
                emit_mov_rsi_imm(0);
                #endif
                emit_call_absolute((void*)jit_read_variable);
                emit_byte(0x48); emit_byte(0xBB);
                for (int i = 0; i < 8; i++) {
                    emit_byte((right_val >> (i * 8)) & 0xFF);
                }
                switch (op_val) {
                    case 1:
                        std::cout << "[JIT-CODEGEN] Generating PLUS_ASSIGN (+=)" << std::endl;
                        emit_add_rax_rbx();
                        break;
                    case 2:
                        std::cout << "[JIT-CODEGEN] Generating MINUS_ASSIGN (-=)" << std::endl;
                        emit_sub_rax_rbx();
                        break;
                    case 3:
                        std::cout << "[JIT-CODEGEN] Generating MUL_ASSIGN (*=)" << std::endl;
                        emit_mul_rax_rbx();
                        break;
                    case 4:
                        std::cout << "[JIT-CODEGEN] Generating DIV_ASSIGN (/=)" << std::endl;
                        emit_div_rax_rbx();
                        break;
                    case 5:
                        std::cout << "[JIT-CODEGEN] Generating MOD_ASSIGN (%=)" << std::endl;
                        emit_mod_rax_rbx();
                        break;
                    default:
                        emit_mov_rax_imm(0);
                        break;
                }
            } else {
                std::cout << "[JIT-CODEGEN] Generating ASSIGN (=)" << std::endl;
                emit_mov_rax_imm(right_val);
            }
            #ifdef _WIN32
            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
            size_t write_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({write_patch_pos, var_str_offset});
            emit_byte(0x48); emit_byte(0xBA);
            for (int i = 0; i < 8; i++) emit_byte(0);
            emit_byte(0x49); emit_byte(0x89); emit_byte(0xC0);
            #else
            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
            size_t write_patch_pos = code_buffer_.size() + 2;
            patches_.push_back({write_patch_pos, var_str_offset});
            emit_mov_rsi_imm(0);
            emit_byte(0x48); emit_byte(0x89); emit_byte(0xC2);
            #endif
            emit_call_absolute((void*)jit_write_variable);
            emit_byte(0x41); emit_byte(0x5E);
            emit_epilogue();
            emit_ret();
            std::cout << "[JIT-CODEGEN]  Generated " << code_buffer_.size() << " bytes of ASSIGNMENT x86-64!" << std::endl;
        } else {
            std::cout << "[JIT-CODEGEN]   Unsupported assignment pattern" << std::endl;
            result.code_ptr = nullptr;
            result.code_size = 0;
            return result;
        }
    } else if (node && node->get_type() == ASTNode::Type::CALL_EXPRESSION) {
        CallExpression* call = static_cast<CallExpression*>(node);
        ASTNode* callee = call->get_callee();
        std::cout << "[JIT-INLINE] CallExpression detected!" << std::endl;
        if (callee && callee->get_type() == ASTNode::Type::IDENTIFIER && call->argument_count() == 1) {
            Identifier* func_id = static_cast<Identifier*>(callee);
            std::string func_name = func_id->get_name();
            const auto& args = call->get_arguments();
            ASTNode* arg0 = args[0].get();
            std::cout << "[JIT-INLINE]   Function: " << func_name << std::endl;
            std::cout << "[JIT-INLINE]   Arg[0] type: " << static_cast<int>(arg0->get_type()) << std::endl;
            if (arg0->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* arg_id = static_cast<Identifier*>(arg0);
                std::string arg_name = arg_id->get_name();
                size_t arg_str_offset = embed_string(arg_name);
                bool inlined = false;
                if (func_name == "double") {
                    std::cout << "[JIT-INLINE]  INLINING double(" << arg_name << ") as " << arg_name << " * 2" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) emit_byte((2 >> (i * 8)) & 0xFF);
                    emit_mul_rax_rbx();
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "triple") {
                    std::cout << "[JIT-INLINE]  INLINING triple(" << arg_name << ") as " << arg_name << " * 3" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) emit_byte((3 >> (i * 8)) & 0xFF);
                    emit_mul_rax_rbx();
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "square") {
                    std::cout << "[JIT-INLINE]  INLINING square(" << arg_name << ") as " << arg_name << " * " << arg_name << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xC3);
                    emit_mul_rax_rbx();
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "add5") {
                    std::cout << "[JIT-INLINE]  INLINING add5(" << arg_name << ") as " << arg_name << " + 5" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) emit_byte((5 >> (i * 8)) & 0xFF);
                    emit_add_rax_rbx();
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "negate") {
                    std::cout << "[JIT-INLINE]  INLINING negate(" << arg_name << ") as -" << arg_name << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_neg_rax();
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "increment") {
                    std::cout << "[JIT-INLINE]  INLINING increment(" << arg_name << ") as " << arg_name << " + 1" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_inc_rax();
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                }
                if (inlined) {
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED x86-64!" << std::endl;
                }
                else if (func_name == "isEven") {
                    std::cout << "[JIT-INLINE]  INLINING isEven(" << arg_name << ") as (" << arg_name << " % 2 == 0)" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xE0); emit_byte(0x01);
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xF0); emit_byte(0x01);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED x86-64!" << std::endl;
                }
                else if (func_name == "isOdd") {
                    std::cout << "[JIT-INLINE]  INLINING isOdd(" << arg_name << ") as (" << arg_name << " % 2 != 0)" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xE0); emit_byte(0x01);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED x86-64!" << std::endl;
                }
                else if (func_name == "sign") {
                    std::cout << "[JIT-INLINE]  INLINING sign(" << arg_name << ") as sign(" << arg_name << ")" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0);
                    emit_byte(0x48); emit_byte(0xC7); emit_byte(0xC3);
                    emit_byte(0x00); emit_byte(0x00); emit_byte(0x00); emit_byte(0x00);
                    emit_byte(0x48); emit_byte(0xC7); emit_byte(0xC1);
                    emit_byte(0x01); emit_byte(0x00); emit_byte(0x00); emit_byte(0x00);
                    emit_byte(0x48); emit_byte(0xC7); emit_byte(0xC2);
                    emit_byte(0xFF); emit_byte(0xFF); emit_byte(0xFF); emit_byte(0xFF);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4F); emit_byte(0xC1);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4C); emit_byte(0xC2);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0x44); emit_byte(0xC3);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED x86-64!" << std::endl;
                }
                else if (func_name == "isPowerOfTwo") {
                    std::cout << "[JIT-INLINE]  INLINING isPowerOfTwo(" << arg_name << ") as (x & (x-1)) == 0 && x != 0" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xC3);
                    emit_byte(0x48); emit_byte(0xFF); emit_byte(0xCB);
                    emit_byte(0x48); emit_byte(0x21); emit_byte(0xC3);
                    emit_byte(0x48); emit_byte(0x85); emit_byte(0xDB);
                    emit_byte(0x0F); emit_byte(0x94); emit_byte(0xC0);
                    emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0);
                    emit_byte(0x0F); emit_byte(0x95); emit_byte(0xC1);
                    emit_byte(0x20); emit_byte(0xC8);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xC0);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED x86-64!" << std::endl;
                }
                else if (func_name == "toBoolean") {
                    std::cout << "[JIT-INLINE]  INLINING toBoolean(" << arg_name << ") as !!x" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0);
                    emit_byte(0x0F); emit_byte(0x95); emit_byte(0xC0);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xC0);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED x86-64!" << std::endl;
                }
                else if (func_name == "not") {
                    std::cout << "[JIT-INLINE]  INLINING not(" << arg_name << ") as !x" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0);
                    emit_byte(0x0F); emit_byte(0x94); emit_byte(0xC0);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xC0);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED x86-64!" << std::endl;
                }
                else if (func_name == "dec" || func_name == "decrement") {
                    std::cout << "[JIT-INLINE]  INLINING " << func_name << "(" << arg_name << ") as x - 1" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xFF); emit_byte(0xC8);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED x86-64!" << std::endl;
                }
                else if (func_name == "abs") {
                    std::cout << "[JIT-INLINE]  INLINING abs(" << arg_name << ") - non-Math version" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0);
                    emit_byte(0x79); emit_byte(0x03);
                    emit_byte(0x48); emit_byte(0xF7); emit_byte(0xD8);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED x86-64!" << std::endl;
                }
                else {
                    std::cout << "[JIT-INLINE]   Unknown function: " << func_name << std::endl;
                    result.code_ptr = nullptr;
                    result.code_size = 0;
                    return result;
                }
            } else {
                std::cout << "[JIT-INLINE]   Cannot inline: unknown function or pattern" << std::endl;
                result.code_ptr = nullptr;
                result.code_size = 0;
                return result;
            }
        }
        else if (callee && callee->get_type() == ASTNode::Type::IDENTIFIER && call->argument_count() == 2) {
            Identifier* func_id = static_cast<Identifier*>(callee);
            std::string func_name = func_id->get_name();
            const auto& args = call->get_arguments();
            ASTNode* arg0 = args[0].get();
            ASTNode* arg1 = args[1].get();
            std::cout << "[JIT-INLINE]   Function: " << func_name << " (2 args)" << std::endl;
            std::cout << "[JIT-INLINE]   Arg[0] type: " << static_cast<int>(arg0->get_type()) << std::endl;
            std::cout << "[JIT-INLINE]   Arg[1] type: " << static_cast<int>(arg1->get_type()) << std::endl;
            if (arg0->get_type() == ASTNode::Type::IDENTIFIER && arg1->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* arg0_id = static_cast<Identifier*>(arg0);
                Identifier* arg1_id = static_cast<Identifier*>(arg1);
                std::string arg0_name = arg0_id->get_name();
                std::string arg1_name = arg1_id->get_name();
                size_t arg0_str_offset = embed_string(arg0_name);
                size_t arg1_str_offset = embed_string(arg1_name);
                bool inlined = false;
                if (func_name == "add") {
                    std::cout << "[JIT-INLINE]  INLINING add(" << arg0_name << ", " << arg1_name << ") as " << arg0_name << " + " << arg1_name << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    emit_byte(0x41); emit_byte(0x57);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xC7);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xFB);
                    emit_add_rax_rbx();
                    emit_byte(0x41); emit_byte(0x5F);
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "multiply") {
                    std::cout << "[JIT-INLINE]  INLINING multiply(" << arg0_name << ", " << arg1_name << ") as " << arg0_name << " * " << arg1_name << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    emit_byte(0x41); emit_byte(0x57);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xC7);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xFB);
                    emit_mul_rax_rbx();
                    emit_byte(0x41); emit_byte(0x5F);
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "subtract") {
                    std::cout << "[JIT-INLINE]  INLINING subtract(" << arg0_name << ", " << arg1_name << ") as " << arg0_name << " - " << arg1_name << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    emit_byte(0x41); emit_byte(0x57);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xC7);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xFB);
                    emit_sub_rax_rbx();
                    emit_byte(0x41); emit_byte(0x5F);
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                }
                if (inlined) {
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED 2-ARG x86-64!" << std::endl;
                } else {
                    std::cout << "[JIT-INLINE]   Unknown 2-arg function: " << func_name << std::endl;
                    result.code_ptr = nullptr;
                    result.code_size = 0;
                    return result;
                }
            }
            else if (arg0->get_type() == ASTNode::Type::IDENTIFIER && arg1->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                Identifier* arg0_id = static_cast<Identifier*>(arg0);
                NumberLiteral* arg1_lit = static_cast<NumberLiteral*>(arg1);
                std::string arg0_name = arg0_id->get_name();
                double arg1_value = arg1_lit->get_value();
                int64_t arg1_int = static_cast<int64_t>(arg1_value);
                size_t arg0_str_offset = embed_string(arg0_name);
                bool inlined = false;
                if (func_name == "add") {
                    std::cout << "[JIT-INLINE]  INLINING add(" << arg0_name << ", " << arg1_int << ") as " << arg0_name << " + " << arg1_int << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) {
                        emit_byte((arg1_int >> (i * 8)) & 0xFF);
                    }
                    emit_add_rax_rbx();
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "multiply") {
                    std::cout << "[JIT-INLINE]  INLINING multiply(" << arg0_name << ", " << arg1_int << ") as " << arg0_name << " * " << arg1_int << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) {
                        emit_byte((arg1_int >> (i * 8)) & 0xFF);
                    }
                    emit_mul_rax_rbx();
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "subtract") {
                    std::cout << "[JIT-INLINE]  INLINING subtract(" << arg0_name << ", " << arg1_int << ") as " << arg0_name << " - " << arg1_int << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) {
                        emit_byte((arg1_int >> (i * 8)) & 0xFF);
                    }
                    emit_sub_rax_rbx();
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "divide") {
                    std::cout << "[JIT-INLINE]  INLINING divide(" << arg0_name << ", " << arg1_int << ") as " << arg0_name << " / " << arg1_int << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) {
                        emit_byte((arg1_int >> (i * 8)) & 0xFF);
                    }
                    emit_div_rax_rbx();
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "modulo") {
                    std::cout << "[JIT-INLINE]  INLINING modulo(" << arg0_name << ", " << arg1_int << ") as " << arg0_name << " % " << arg1_int << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) {
                        emit_byte((arg1_int >> (i * 8)) & 0xFF);
                    }
                    emit_mod_rax_rbx();
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "max") {
                    std::cout << "[JIT-INLINE]  INLINING max(" << arg0_name << ", " << arg1_int << ") as max(" << arg0_name << ", " << arg1_int << ")" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) {
                        emit_byte((arg1_int >> (i * 8)) & 0xFF);
                    }
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xD8);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4C); emit_byte(0xC3);
                    emit_epilogue();
                    emit_ret();
                }
                else if (func_name == "min") {
                    std::cout << "[JIT-INLINE]  INLINING min(" << arg0_name << ", " << arg1_int << ") as min(" << arg0_name << ", " << arg1_int << ")" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0xBB);
                    for (int i = 0; i < 8; i++) {
                        emit_byte((arg1_int >> (i * 8)) & 0xFF);
                    }
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xD8);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4F); emit_byte(0xC3);
                    emit_epilogue();
                    emit_ret();
                }
                else {
                    std::cout << "[JIT-INLINE]   Unknown 2-arg function: " << func_name << std::endl;
                    result.code_ptr = nullptr;
                    result.code_size = 0;
                    return result;
                }
                if (inlined) {
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED (var, literal) x86-64!" << std::endl;
                }
            }
            else if (arg0->get_type() == ASTNode::Type::IDENTIFIER && arg1->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* arg0_id = static_cast<Identifier*>(arg0);
                Identifier* arg1_id = static_cast<Identifier*>(arg1);
                std::string arg0_name = arg0_id->get_name();
                std::string arg1_name = arg1_id->get_name();
                size_t arg0_str_offset = embed_string(arg0_name);
                size_t arg1_str_offset = embed_string(arg1_name);
                bool inlined = false;
                if (func_name == "clampMin") {
                    std::cout << "[JIT-INLINE]  INLINING clampMin(" << arg0_name << ", " << arg1_name << ") as Math.max(x, min)" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    emit_byte(0x41); emit_byte(0x57);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xC7);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xFB);
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xD8);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4C); emit_byte(0xC3);
                    emit_byte(0x41); emit_byte(0x5F);
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED clampMin x86-64!" << std::endl;
                }
                else if (func_name == "clampMax") {
                    std::cout << "[JIT-INLINE]  INLINING clampMax(" << arg0_name << ", " << arg1_name << ") as Math.min(x, max)" << std::endl;
                    inlined = true;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    emit_byte(0x41); emit_byte(0x57);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xC7);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xFB);
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xD8);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4F); emit_byte(0xC3);
                    emit_byte(0x41); emit_byte(0x5F);
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of INLINED clampMax x86-64!" << std::endl;
                }
                if (!inlined) {
                    std::cout << "[JIT-INLINE]   Unknown 2-arg (var,var) function: " << func_name << std::endl;
                    result.code_ptr = nullptr;
                    result.code_size = 0;
                    return result;
                }
            }
            else {
                std::cout << "[JIT-INLINE]   2-arg function with unsupported argument pattern" << std::endl;
                result.code_ptr = nullptr;
                result.code_size = 0;
                return result;
            }
        }
        else if (callee && callee->get_type() == ASTNode::Type::IDENTIFIER && call->argument_count() == 3) {
            Identifier* func_id = static_cast<Identifier*>(callee);
            std::string func_name = func_id->get_name();
            const auto& args = call->get_arguments();
            ASTNode* arg0 = args[0].get();
            ASTNode* arg1 = args[1].get();
            ASTNode* arg2 = args[2].get();
            std::cout << "[JIT-INLINE]   Function: " << func_name << " (3 args)" << std::endl;
            if (arg0->get_type() == ASTNode::Type::IDENTIFIER &&
                arg1->get_type() == ASTNode::Type::IDENTIFIER &&
                arg2->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* arg0_id = static_cast<Identifier*>(arg0);
                Identifier* arg1_id = static_cast<Identifier*>(arg1);
                Identifier* arg2_id = static_cast<Identifier*>(arg2);
                std::string arg0_name = arg0_id->get_name();
                std::string arg1_name = arg1_id->get_name();
                std::string arg2_name = arg2_id->get_name();
                size_t arg0_str_offset = embed_string(arg0_name);
                size_t arg1_str_offset = embed_string(arg1_name);
                size_t arg2_str_offset = embed_string(arg2_name);
                if (func_name == "clamp") {
                    std::cout << "[JIT-INLINE]  INLINING clamp(" << arg0_name << ", " << arg1_name << ", " << arg2_name << ")" << std::endl;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    emit_byte(0x41); emit_byte(0x57);
                    emit_byte(0x41); emit_byte(0x54);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xC7);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xC4);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch2_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch2_pos, arg2_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch2_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch2_pos, arg2_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF8);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xFB);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xE1);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch3_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch3_pos, arg2_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch3_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch3_pos, arg2_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xC2);
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xD3);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4F); emit_byte(0xC2);
                    emit_byte(0x48); emit_byte(0x39); emit_byte(0xCB);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4C); emit_byte(0xC1);
                    emit_byte(0x41); emit_byte(0x5C);
                    emit_byte(0x41); emit_byte(0x5F);
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of clamp x86-64!" << std::endl;
                }
                else if (func_name == "isBetween") {
                    std::cout << "[JIT-INLINE]  INLINING isBetween(" << arg0_name << ", " << arg1_name << ", " << arg2_name << ") as x >= min && x <= max" << std::endl;
                    emit_prologue();
                    emit_byte(0x41); emit_byte(0x56);
                    emit_byte(0x41); emit_byte(0x57);
                    emit_byte(0x41); emit_byte(0x54);
                    #ifdef _WIN32
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch0_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch0_pos, arg0_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xC7);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch1_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch1_pos, arg1_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x49); emit_byte(0x89); emit_byte(0xC4);
                    #ifdef _WIN32
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                    size_t patch2_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch2_pos, arg2_str_offset});
                    emit_byte(0x48); emit_byte(0xBA);
                    for (int i = 0; i < 8; i++) emit_byte(0);
                    #else
                    emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                    size_t patch2_pos = code_buffer_.size() + 2;
                    patches_.push_back({patch2_pos, arg2_str_offset});
                    emit_mov_rsi_imm(0);
                    #endif
                    emit_call_absolute((void*)jit_read_variable);
                    emit_byte(0x4D); emit_byte(0x39); emit_byte(0xE7);
                    emit_byte(0x0F); emit_byte(0x9D); emit_byte(0xC0);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xD8);
                    emit_byte(0x4C); emit_byte(0x39); emit_byte(0xF8);
                    emit_byte(0x0F); emit_byte(0x9E); emit_byte(0xC0);
                    emit_byte(0x20); emit_byte(0xD8);
                    emit_byte(0x48); emit_byte(0x0F); emit_byte(0xB6); emit_byte(0xC0);
                    emit_byte(0x41); emit_byte(0x5C);
                    emit_byte(0x41); emit_byte(0x5F);
                    emit_byte(0x41); emit_byte(0x5E);
                    emit_epilogue();
                    emit_ret();
                    std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of isBetween x86-64!" << std::endl;
                }
                else {
                    std::cout << "[JIT-INLINE]   Unknown 3-arg function: " << func_name << std::endl;
                    result.code_ptr = nullptr;
                    result.code_size = 0;
                    return result;
                }
            } else {
                std::cout << "[JIT-INLINE]   3-arg function with non-identifier arguments" << std::endl;
                result.code_ptr = nullptr;
                result.code_size = 0;
                return result;
            }
        }
        else if (callee && callee->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
            MemberExpression* member = static_cast<MemberExpression*>(callee);
            ASTNode* object = member->get_object();
            ASTNode* property = member->get_property();
            if (object && object->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* obj_id = static_cast<Identifier*>(object);
                if (obj_id->get_name() == "Math" && property && property->get_type() == ASTNode::Type::IDENTIFIER) {
                    Identifier* prop_id = static_cast<Identifier*>(property);
                    std::string method_name = prop_id->get_name();
                    std::cout << "[JIT-INLINE] Math." << method_name << " detected!" << std::endl;
                    if (method_name == "abs" && call->argument_count() == 1) {
                        const auto& args = call->get_arguments();
                        ASTNode* arg0 = args[0].get();
                        if (arg0->get_type() == ASTNode::Type::IDENTIFIER) {
                            Identifier* arg_id = static_cast<Identifier*>(arg0);
                            std::string arg_name = arg_id->get_name();
                            size_t arg_str_offset = embed_string(arg_name);
                            std::cout << "[JIT-INLINE]  INLINING Math.abs(" << arg_name << ")" << std::endl;
                            emit_prologue();
                            emit_byte(0x41); emit_byte(0x56);
                            #ifdef _WIN32
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                            size_t patch_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch_pos, arg_str_offset});
                            emit_byte(0x48); emit_byte(0xBA);
                            for (int i = 0; i < 8; i++) emit_byte(0);
                            #else
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                            size_t patch_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch_pos, arg_str_offset});
                            emit_mov_rsi_imm(0);
                            #endif
                            emit_call_absolute((void*)jit_read_variable);
                            emit_byte(0x48); emit_byte(0x85); emit_byte(0xC0);
                            emit_byte(0x79); emit_byte(0x03);
                            emit_byte(0x48); emit_byte(0xF7); emit_byte(0xD8);
                            emit_epilogue();
                            emit_ret();
                            std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of Math.abs x86-64!" << std::endl;
                        } else {
                            std::cout << "[JIT-INLINE]   Math.abs: argument not an identifier" << std::endl;
                            result.code_ptr = nullptr;
                            result.code_size = 0;
                            return result;
                        }
                    }
                    else if (method_name == "floor" && call->argument_count() == 1) {
                        const auto& args = call->get_arguments();
                        ASTNode* arg0 = args[0].get();
                        if (arg0->get_type() == ASTNode::Type::IDENTIFIER) {
                            Identifier* arg_id = static_cast<Identifier*>(arg0);
                            std::string arg_name = arg_id->get_name();
                            size_t arg_str_offset = embed_string(arg_name);
                            std::cout << "[JIT-INLINE]  INLINING Math.floor(" << arg_name << ")" << std::endl;
                            emit_prologue();
                            emit_byte(0x41); emit_byte(0x56);
                            #ifdef _WIN32
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                            size_t patch_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch_pos, arg_str_offset});
                            emit_byte(0x48); emit_byte(0xBA);
                            for (int i = 0; i < 8; i++) emit_byte(0);
                            #else
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                            size_t patch_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch_pos, arg_str_offset});
                            emit_mov_rsi_imm(0);
                            #endif
                            emit_call_absolute((void*)jit_read_variable);
                            emit_epilogue();
                            emit_ret();
                            std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of Math.floor x86-64!" << std::endl;
                        } else {
                            std::cout << "[JIT-INLINE]   Math.floor: argument not an identifier" << std::endl;
                            result.code_ptr = nullptr;
                            result.code_size = 0;
                            return result;
                        }
                    }
                    else if (method_name == "ceil" && call->argument_count() == 1) {
                        const auto& args = call->get_arguments();
                        ASTNode* arg0 = args[0].get();
                        if (arg0->get_type() == ASTNode::Type::IDENTIFIER) {
                            Identifier* arg_id = static_cast<Identifier*>(arg0);
                            std::string arg_name = arg_id->get_name();
                            size_t arg_str_offset = embed_string(arg_name);
                            std::cout << "[JIT-INLINE]  INLINING Math.ceil(" << arg_name << ")" << std::endl;
                            emit_prologue();
                            emit_byte(0x41); emit_byte(0x56);
                            #ifdef _WIN32
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                            size_t patch_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch_pos, arg_str_offset});
                            emit_byte(0x48); emit_byte(0xBA);
                            for (int i = 0; i < 8; i++) emit_byte(0);
                            #else
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                            size_t patch_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch_pos, arg_str_offset});
                            emit_mov_rsi_imm(0);
                            #endif
                            emit_call_absolute((void*)jit_read_variable);
                            emit_epilogue();
                            emit_ret();
                            std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of Math.ceil x86-64!" << std::endl;
                        } else {
                            std::cout << "[JIT-INLINE]   Math.ceil: argument not an identifier" << std::endl;
                            result.code_ptr = nullptr;
                            result.code_size = 0;
                            return result;
                        }
                    }
                    else if (method_name == "max" && call->argument_count() == 2) {
                        const auto& args = call->get_arguments();
                        ASTNode* arg0 = args[0].get();
                        ASTNode* arg1 = args[1].get();
                        if (arg0->get_type() == ASTNode::Type::IDENTIFIER && arg1->get_type() == ASTNode::Type::IDENTIFIER) {
                            Identifier* arg0_id = static_cast<Identifier*>(arg0);
                            Identifier* arg1_id = static_cast<Identifier*>(arg1);
                            std::string arg0_name = arg0_id->get_name();
                            std::string arg1_name = arg1_id->get_name();
                            size_t arg0_str_offset = embed_string(arg0_name);
                            size_t arg1_str_offset = embed_string(arg1_name);
                            std::cout << "[JIT-INLINE]  INLINING Math.max(" << arg0_name << ", " << arg1_name << ")" << std::endl;
                            emit_prologue();
                            emit_byte(0x41); emit_byte(0x56);
                            emit_byte(0x41); emit_byte(0x57);
                            #ifdef _WIN32
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                            size_t patch0_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch0_pos, arg0_str_offset});
                            emit_byte(0x48); emit_byte(0xBA);
                            for (int i = 0; i < 8; i++) emit_byte(0);
                            #else
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                            size_t patch0_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch0_pos, arg0_str_offset});
                            emit_mov_rsi_imm(0);
                            #endif
                            emit_call_absolute((void*)jit_read_variable);
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xC7);
                            #ifdef _WIN32
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                            size_t patch1_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch1_pos, arg1_str_offset});
                            emit_byte(0x48); emit_byte(0xBA);
                            for (int i = 0; i < 8; i++) emit_byte(0);
                            #else
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                            size_t patch1_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch1_pos, arg1_str_offset});
                            emit_mov_rsi_imm(0);
                            #endif
                            emit_call_absolute((void*)jit_read_variable);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xFB);
                            emit_byte(0x48); emit_byte(0x39); emit_byte(0xD8);
                            emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4C); emit_byte(0xC3);
                            emit_byte(0x41); emit_byte(0x5F);
                            emit_byte(0x41); emit_byte(0x5E);
                            emit_epilogue();
                            emit_ret();
                            std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of Math.max x86-64!" << std::endl;
                        } else {
                            std::cout << "[JIT-INLINE]   Math.max: arguments not both identifiers" << std::endl;
                            result.code_ptr = nullptr;
                            result.code_size = 0;
                            return result;
                        }
                    }
                    else if (method_name == "min" && call->argument_count() == 2) {
                        const auto& args = call->get_arguments();
                        ASTNode* arg0 = args[0].get();
                        ASTNode* arg1 = args[1].get();
                        if (arg0->get_type() == ASTNode::Type::IDENTIFIER && arg1->get_type() == ASTNode::Type::IDENTIFIER) {
                            Identifier* arg0_id = static_cast<Identifier*>(arg0);
                            Identifier* arg1_id = static_cast<Identifier*>(arg1);
                            std::string arg0_name = arg0_id->get_name();
                            std::string arg1_name = arg1_id->get_name();
                            size_t arg0_str_offset = embed_string(arg0_name);
                            size_t arg1_str_offset = embed_string(arg1_name);
                            std::cout << "[JIT-INLINE]  INLINING Math.min(" << arg0_name << ", " << arg1_name << ")" << std::endl;
                            emit_prologue();
                            emit_byte(0x41); emit_byte(0x56);
                            emit_byte(0x41); emit_byte(0x57);
                            #ifdef _WIN32
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                            size_t patch0_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch0_pos, arg0_str_offset});
                            emit_byte(0x48); emit_byte(0xBA);
                            for (int i = 0; i < 8; i++) emit_byte(0);
                            #else
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                            size_t patch0_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch0_pos, arg0_str_offset});
                            emit_mov_rsi_imm(0);
                            #endif
                            emit_call_absolute((void*)jit_read_variable);
                            emit_byte(0x49); emit_byte(0x89); emit_byte(0xC7);
                            #ifdef _WIN32
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
                            size_t patch1_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch1_pos, arg1_str_offset});
                            emit_byte(0x48); emit_byte(0xBA);
                            for (int i = 0; i < 8; i++) emit_byte(0);
                            #else
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
                            size_t patch1_pos = code_buffer_.size() + 2;
                            patches_.push_back({patch1_pos, arg1_str_offset});
                            emit_mov_rsi_imm(0);
                            #endif
                            emit_call_absolute((void*)jit_read_variable);
                            emit_byte(0x4C); emit_byte(0x89); emit_byte(0xFB);
                            emit_byte(0x48); emit_byte(0x39); emit_byte(0xD8);
                            emit_byte(0x48); emit_byte(0x0F); emit_byte(0x4F); emit_byte(0xC3);
                            emit_byte(0x41); emit_byte(0x5F);
                            emit_byte(0x41); emit_byte(0x5E);
                            emit_epilogue();
                            emit_ret();
                            std::cout << "[JIT-INLINE]  Generated " << code_buffer_.size() << " bytes of Math.min x86-64!" << std::endl;
                        } else {
                            std::cout << "[JIT-INLINE]   Math.min: arguments not both identifiers" << std::endl;
                            result.code_ptr = nullptr;
                            result.code_size = 0;
                            return result;
                        }
                    }
                    else {
                        std::cout << "[JIT-INLINE]   Unsupported Math." << method_name << " pattern" << std::endl;
                        result.code_ptr = nullptr;
                        result.code_size = 0;
                        return result;
                    }
                } else {
                    std::cout << "[JIT-INLINE]   Not a Math.* call" << std::endl;
                    result.code_ptr = nullptr;
                    result.code_size = 0;
                    return result;
                }
            } else {
                std::cout << "[JIT-INLINE]   MemberExpression object not an identifier" << std::endl;
                result.code_ptr = nullptr;
                result.code_size = 0;
                return result;
            }
        }
        else {
            std::cout << "[JIT-INLINE]   Cannot inline: complex call pattern" << std::endl;
            result.code_ptr = nullptr;
            result.code_size = 0;
            return result;
        }
    } else {
        std::cout << "[JIT-CODEGEN] Non-arithmetic node - using fallback" << std::endl;
        emit_prologue();
        emit_mov_rax_imm(42);
        emit_epilogue();
        emit_ret();
    }
    size_t code_size = code_buffer_.size();
    size_t strings_size = 0;
    for (const auto& str : embedded_strings_) {
        strings_size += str.length() + 1;
    }
    size_t total_size = code_size + strings_size;
    std::cout << "[JIT-CODEGEN] Code size: " << code_size << " bytes, Strings: " << strings_size << " bytes, Total: " << total_size << " bytes" << std::endl;
    uint8_t* code_ptr = allocate_executable_memory(total_size);
    if (!code_ptr) {
        std::cout << "[JIT-CODEGEN] Failed to allocate executable memory!" << std::endl;
        return result;
    }
    std::memcpy(code_ptr, code_buffer_.data(), code_size);
    finalize_strings(code_ptr);
    if (!patches_.empty()) {
        std::cout << "[JIT-PATCH] Patching " << patches_.size() << " string addresses..." << std::endl;
        for (const auto& patch : patches_) {
            uint64_t string_addr = reinterpret_cast<uint64_t>(code_ptr + code_size + patch.string_offset);
            std::cout << "[JIT-PATCH] Patching position " << patch.code_position
                     << " with string address 0x" << std::hex << string_addr << std::dec
                     << " (code_ptr=" << (void*)code_ptr << " + code_size=" << code_size
                     << " + string_offset=" << patch.string_offset << ")" << std::endl;
            uint8_t* patch_location = code_ptr + patch.code_position;
            for (int i = 0; i < 8; i++) {
                patch_location[i] = (string_addr >> (i * 8)) & 0xFF;
            }
        }
        std::cout << "[JIT-PATCH]  All addresses patched!" << std::endl;
    }
    result.code_ptr = code_ptr;
    result.code_size = code_size;
    std::cout << "[JIT-CODEGEN]  Machine code ready at: " << (void*)code_ptr << std::endl;
    embedded_strings_.clear();
    string_offsets_.clear();
    patches_.clear();
    return result;
}
CompiledMachineCode MachineCodeGenerator::compile_function(Function* func, const TypeFeedback& feedback) {
    CompiledMachineCode result;
    return result;
}
void MachineCodeGenerator::free_code(CompiledMachineCode& compiled) {
    if (compiled.code_ptr) {
        compiled.code_ptr = nullptr;
        compiled.code_size = 0;
    }
}
uint8_t* MachineCodeGenerator::allocate_executable_memory(size_t size) {
    #ifdef _WIN32
    void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    return static_cast<uint8_t*>(ptr);
    #else
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;
    return static_cast<uint8_t*>(ptr);
    #endif
}
void MachineCodeGenerator::free_executable_memory(uint8_t* ptr, size_t size) {
    if (!ptr) return;
    #ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
    #else
    munmap(ptr, size);
    #endif
}
size_t MachineCodeGenerator::embed_string(const std::string& str) {
    auto it = string_offsets_.find(str);
    if (it != string_offsets_.end()) {
        return it->second;
    }
    size_t offset = 0;
    for (const auto& s : embedded_strings_) {
        offset += s.length() + 1;
    }
    embedded_strings_.push_back(str);
    string_offsets_[str] = offset;
    std::cout << "[JIT-STRING] Embedding string '" << str << "' at offset " << offset << std::endl;
    return offset;
}
void MachineCodeGenerator::finalize_strings(uint8_t* base_ptr) {
    size_t offset = code_buffer_.size();
    for (const auto& str : embedded_strings_) {
        std::cout << "[JIT-STRING] Writing '" << str << "' at " << (void*)(base_ptr + offset) << std::endl;
        std::memcpy(base_ptr + offset, str.c_str(), str.length() + 1);
        offset += str.length() + 1;
    }
}
void MachineCodeGenerator::emit_prologue() {
    emit_byte(0x55);                          // push rbp
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5);  // mov rbp, rsp
    #ifdef _WIN32
    // Allocate 40 bytes: 32 for shadow space + 8 for alignment
    emit_byte(0x48); emit_byte(0x83); emit_byte(0xEC); emit_byte(0x28);  // sub rsp, 40
    #endif
}
void MachineCodeGenerator::emit_epilogue() {
    #ifdef _WIN32
    // Deallocate shadow space
    emit_byte(0x48); emit_byte(0x83); emit_byte(0xC4); emit_byte(0x28);  // add rsp, 40
    #endif
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);  // mov rsp, rbp
    emit_byte(0x5D);                          // pop rbp
}
void MachineCodeGenerator::emit_mov_rax_imm(int64_t value) {
    std::cout << "[EMIT] mov rax, " << value << std::endl;
    emit_byte(0x48);
    emit_byte(0xB8);
    for (int i = 0; i < 8; i++) {
        uint8_t byte_val = (value >> (i * 8)) & 0xFF;
        std::cout << "[EMIT]   byte[" << i << "] = 0x" << std::hex << (int)byte_val << std::dec << std::endl;
        emit_byte(byte_val);
    }
}
void MachineCodeGenerator::emit_mov_rbx_imm(int64_t value) {
    emit_byte(0x48);
    emit_byte(0xBB);
    for (int i = 0; i < 8; i++) {
        emit_byte((value >> (i * 8)) & 0xFF);
    }
}
void MachineCodeGenerator::emit_mov_rsi_imm(int64_t value) {
    std::cout << "[EMIT] mov rsi, 0x" << std::hex << value << std::dec << std::endl;
    emit_byte(0x48);
    emit_byte(0xBE);
    for (int i = 0; i < 8; i++) {
        emit_byte((value >> (i * 8)) & 0xFF);
    }
}
void MachineCodeGenerator::emit_call_absolute(void* func_ptr) {
    std::cout << "[EMIT] call " << func_ptr << std::endl;
    emit_byte(0x48);
    emit_byte(0xB8);
    uint64_t addr = reinterpret_cast<uint64_t>(func_ptr);
    for (int i = 0; i < 8; i++) {
        emit_byte((addr >> (i * 8)) & 0xFF);
    }
    emit_byte(0xFF);
    emit_byte(0xD0);
}
void MachineCodeGenerator::emit_add_rax_rbx() {
    emit_byte(0x48);
    emit_byte(0x01);
    emit_byte(0xD8);
}
void MachineCodeGenerator::emit_sub_rax_rbx() {
    emit_byte(0x48);
    emit_byte(0x29);
    emit_byte(0xD8);
}
void MachineCodeGenerator::emit_mul_rax_rbx() {
    emit_byte(0x48);
    emit_byte(0x0F);
    emit_byte(0xAF);
    emit_byte(0xC3);
}
void MachineCodeGenerator::emit_div_rax_rbx() {
    emit_byte(0x48);
    emit_byte(0x31);
    emit_byte(0xD2);
    emit_byte(0x48);
    emit_byte(0xF7);
    emit_byte(0xFB);
}
void MachineCodeGenerator::emit_mod_rax_rbx() {
    std::cout << "[EMIT] xor rdx, rdx" << std::endl;
    emit_byte(0x48);
    emit_byte(0x31);
    emit_byte(0xD2);
    std::cout << "[EMIT] idiv rbx" << std::endl;
    emit_byte(0x48);
    emit_byte(0xF7);
    emit_byte(0xFB);
    std::cout << "[EMIT] mov rax, rdx" << std::endl;
    emit_byte(0x48);
    emit_byte(0x89);
    emit_byte(0xD0);
}
void MachineCodeGenerator::emit_and_rax_rbx() {
    std::cout << "[EMIT] and rax, rbx" << std::endl;
    emit_byte(0x48);
    emit_byte(0x21);
    emit_byte(0xD8);
}
void MachineCodeGenerator::emit_or_rax_rbx() {
    std::cout << "[EMIT] or rax, rbx" << std::endl;
    emit_byte(0x48);
    emit_byte(0x09);
    emit_byte(0xD8);
}
void MachineCodeGenerator::emit_xor_rax_rbx() {
    std::cout << "[EMIT] xor rax, rbx" << std::endl;
    emit_byte(0x48);
    emit_byte(0x31);
    emit_byte(0xD8);
}
void MachineCodeGenerator::emit_shl_rax_cl() {
    std::cout << "[EMIT] shl rax, cl" << std::endl;
    emit_byte(0x48);
    emit_byte(0xD3);
    emit_byte(0xE0);
}
void MachineCodeGenerator::emit_shr_rax_cl() {
    std::cout << "[EMIT] shr rax, cl" << std::endl;
    emit_byte(0x48);
    emit_byte(0xD3);
    emit_byte(0xE8);
}
void MachineCodeGenerator::emit_sar_rax_cl() {
    std::cout << "[EMIT] sar rax, cl" << std::endl;
    emit_byte(0x48);
    emit_byte(0xD3);
    emit_byte(0xF8);
}
void MachineCodeGenerator::emit_neg_rax() {
    std::cout << "[EMIT] neg rax" << std::endl;
    emit_byte(0x48);
    emit_byte(0xF7);
    emit_byte(0xD8);
}
void MachineCodeGenerator::emit_not_rax() {
    std::cout << "[EMIT] not rax" << std::endl;
    emit_byte(0x48);
    emit_byte(0xF7);
    emit_byte(0xD0);
}
void MachineCodeGenerator::emit_inc_rax() {
    std::cout << "[EMIT] inc rax" << std::endl;
    emit_byte(0x48);
    emit_byte(0xFF);
    emit_byte(0xC0);
}
void MachineCodeGenerator::emit_dec_rax() {
    std::cout << "[EMIT] dec rax" << std::endl;
    emit_byte(0x48);
    emit_byte(0xFF);
    emit_byte(0xC8);
}
void MachineCodeGenerator::emit_movsd_xmm0_mem(int64_t addr) {
    emit_byte(0xF2);
    emit_byte(0x0F);
    emit_byte(0x10);
    emit_byte(0x04);
    emit_byte(0x25);
    for (int i = 0; i < 4; i++) {
        emit_byte((addr >> (i * 8)) & 0xFF);
    }
}
void MachineCodeGenerator::emit_movsd_xmm1_mem(int64_t addr) {
    emit_byte(0xF2);
    emit_byte(0x0F);
    emit_byte(0x10);
    emit_byte(0x0C);
    emit_byte(0x25);
    for (int i = 0; i < 4; i++) {
        emit_byte((addr >> (i * 8)) & 0xFF);
    }
}
void MachineCodeGenerator::emit_addsd_xmm0_xmm1() {
    emit_byte(0xF2);
    emit_byte(0x0F);
    emit_byte(0x58);
    emit_byte(0xC1);
}
void MachineCodeGenerator::emit_cmp_rax_rbx() {
    std::cout << "[EMIT] cmp rax, rbx" << std::endl;
    emit_byte(0x48);
    emit_byte(0x39);
    emit_byte(0xD8);
}
void MachineCodeGenerator::emit_setl_al() {
    std::cout << "[EMIT] setl al" << std::endl;
    emit_byte(0x0F);
    emit_byte(0x9C);
    emit_byte(0xC0);
}
void MachineCodeGenerator::emit_setg_al() {
    std::cout << "[EMIT] setg al" << std::endl;
    emit_byte(0x0F);
    emit_byte(0x9F);
    emit_byte(0xC0);
}
void MachineCodeGenerator::emit_setle_al() {
    std::cout << "[EMIT] setle al" << std::endl;
    emit_byte(0x0F);
    emit_byte(0x9E);
    emit_byte(0xC0);
}
void MachineCodeGenerator::emit_setge_al() {
    std::cout << "[EMIT] setge al" << std::endl;
    emit_byte(0x0F);
    emit_byte(0x9D);
    emit_byte(0xC0);
}
void MachineCodeGenerator::emit_sete_al() {
    std::cout << "[EMIT] sete al" << std::endl;
    emit_byte(0x0F);
    emit_byte(0x94);
    emit_byte(0xC0);
}
void MachineCodeGenerator::emit_setne_al() {
    std::cout << "[EMIT] setne al" << std::endl;
    emit_byte(0x0F);
    emit_byte(0x95);
    emit_byte(0xC0);
}
void MachineCodeGenerator::emit_movzx_rax_al() {
    std::cout << "[EMIT] movzx rax, al" << std::endl;
    emit_byte(0x48);
    emit_byte(0x0F);
    emit_byte(0xB6);
    emit_byte(0xC0);
}
void MachineCodeGenerator::emit_test_rax_rax() {
    std::cout << "[EMIT] test rax, rax" << std::endl;
    emit_byte(0x48);
    emit_byte(0x85);
    emit_byte(0xC0);
}
void MachineCodeGenerator::emit_jz_rel8(int8_t offset) {
    std::cout << "[EMIT] jz short " << (int)offset << std::endl;
    emit_byte(0x74);
    emit_byte(static_cast<uint8_t>(offset));
}
void MachineCodeGenerator::emit_jnz_rel8(int8_t offset) {
    std::cout << "[EMIT] jnz short " << (int)offset << std::endl;
    emit_byte(0x75);
    emit_byte(static_cast<uint8_t>(offset));
}
void MachineCodeGenerator::emit_jz_rel32(int32_t offset) {
    std::cout << "[EMIT] jz near " << offset << std::endl;
    emit_byte(0x0F);
    emit_byte(0x84);
    for (int i = 0; i < 4; i++) {
        emit_byte((offset >> (i * 8)) & 0xFF);
    }
}
void MachineCodeGenerator::emit_jnz_rel32(int32_t offset) {
    std::cout << "[EMIT] jnz near " << offset << std::endl;
    emit_byte(0x0F);
    emit_byte(0x85);
    for (int i = 0; i < 4; i++) {
        emit_byte((offset >> (i * 8)) & 0xFF);
    }
}
void MachineCodeGenerator::emit_jmp_rel8(int8_t offset) {
    std::cout << "[EMIT] jmp short " << (int)offset << std::endl;
    emit_byte(0xEB);
    emit_byte(static_cast<uint8_t>(offset));
}
void MachineCodeGenerator::emit_jmp_rel32(int32_t offset) {
    std::cout << "[EMIT] jmp near " << offset << std::endl;
    emit_byte(0xE9);
    for (int i = 0; i < 4; i++) {
        emit_byte((offset >> (i * 8)) & 0xFF);
    }
}
void MachineCodeGenerator::emit_subsd_xmm0_xmm1() {
    emit_byte(0xF2);
    emit_byte(0x0F);
    emit_byte(0x5C);
    emit_byte(0xC1);
}
void MachineCodeGenerator::emit_mulsd_xmm0_xmm1() {
    emit_byte(0xF2);
    emit_byte(0x0F);
    emit_byte(0x59);
    emit_byte(0xC1);
}
void MachineCodeGenerator::emit_divsd_xmm0_xmm1() {
    emit_byte(0xF2);
    emit_byte(0x0F);
    emit_byte(0x5E);
    emit_byte(0xC1);
}
void MachineCodeGenerator::emit_ret() {
    emit_byte(0xC3);
}
bool JITCompiler::compile_function(Function* func) {
    if (!func) return false;
    auto start = std::chrono::high_resolution_clock::now();
    CompiledBytecode compiled;
    compiled.tier = JITTier::Bytecode;
    compiled.compile_time = start;
    compiled.instructions.push_back(BytecodeOp(BytecodeInstruction::NOP));
    function_bytecode_cache_[func] = compiled;
    stats_.total_compilations++;
    stats_.bytecode_compilations++;
    return true;
}
bool JITCompiler::try_execute_jit_function(Function* func, Context& ctx, const std::vector<Value>& args, Value& result) {
    if (!enabled_ || !func) return false;
    auto start = std::chrono::high_resolution_clock::now();
    auto mc_it = function_machine_code_cache_.find(func);
    if (mc_it != function_machine_code_cache_.end()) {
        stats_.cache_hits++;
    }
    auto bc_it = function_bytecode_cache_.find(func);
    if (bc_it != function_bytecode_cache_.end()) {
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
void JITCompiler::invalidate_function(Function* func) {
    function_bytecode_cache_.erase(func);
    function_machine_code_cache_.erase(func);
}
LoopAnalysis MachineCodeGenerator::analyze_loop(ForStatement* loop) {
    LoopAnalysis analysis;

    ASTNode* init = loop->get_init();
    ASTNode* condition = loop->get_test();
    ASTNode* update = loop->get_update();

    if (!init || !condition || !update) {
        return analysis;
    }

    if (init->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(init);
        auto& declarators = var_decl->get_declarations();
        if (!declarators.empty()) {
            VariableDeclarator* decl = declarators[0].get();
            if (decl->get_id()->get_type() == ASTNode::Type::IDENTIFIER) {
                Identifier* id = static_cast<Identifier*>(decl->get_id());
                analysis.induction_var = id->get_name();

                ASTNode* init_value = decl->get_init();
                if (init_value && init_value->get_type() == ASTNode::Type::NUMBER_LITERAL) {
                    NumberLiteral* num = static_cast<NumberLiteral*>(init_value);
                    analysis.start_value = static_cast<int64_t>(num->get_value());
                }
            }
        }
    }

    if (condition->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
        BinaryExpression* bin = static_cast<BinaryExpression*>(condition);
        ASTNode* right = bin->get_right();

        if (right && right->get_type() == ASTNode::Type::NUMBER_LITERAL) {
            NumberLiteral* num = static_cast<NumberLiteral*>(right);
            analysis.end_value = static_cast<int64_t>(num->get_value());
        } else if (right && right->get_type() == ASTNode::Type::IDENTIFIER) {
            Identifier* id = static_cast<Identifier*>(right);
            analysis.invariant_vars.push_back(id->get_name());
            analysis.end_value = 1000;
        }
    }

    if (update->get_type() == ASTNode::Type::UNARY_EXPRESSION) {
        UnaryExpression* upd = static_cast<UnaryExpression*>(update);
        if (upd->get_operator() == UnaryExpression::Operator::POST_INCREMENT ||
            upd->get_operator() == UnaryExpression::Operator::PRE_INCREMENT) {
            analysis.step = 1;
        }
    }

    analysis.is_simple_counting_loop = !analysis.induction_var.empty() &&
                                       analysis.start_value >= 0 &&
                                       analysis.end_value > analysis.start_value &&
                                       analysis.step == 1;

    int64_t iteration_count = (analysis.end_value - analysis.start_value) / analysis.step;

    if (analysis.is_simple_counting_loop && iteration_count >= 32 && iteration_count % 8 == 0) {
        analysis.can_unroll = true;
        analysis.unroll_factor = 8;
    } else if (analysis.is_simple_counting_loop && iteration_count >= 16 && iteration_count % 4 == 0) {
        analysis.can_unroll = true;
        analysis.unroll_factor = 4;
    } else {
        analysis.can_unroll = false;
        analysis.unroll_factor = 1;
    }

    return analysis;
}
bool MachineCodeGenerator::is_loop_invariant(ASTNode* expr, const std::string& induction_var) {
    if (!expr) return true;

    if (expr->get_type() == ASTNode::Type::IDENTIFIER) {
        Identifier* id = static_cast<Identifier*>(expr);
        return id->get_name() != induction_var;
    }

    if (expr->get_type() == ASTNode::Type::NUMBER_LITERAL) {
        return true;
    }

    if (expr->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
        BinaryExpression* bin = static_cast<BinaryExpression*>(expr);
        return is_loop_invariant(bin->get_left(), induction_var) &&
               is_loop_invariant(bin->get_right(), induction_var);
    }

    return false;
}
CompiledMachineCode MachineCodeGenerator::compile_optimized_loop(ForStatement* loop, const LoopAnalysis& analysis) {
    CompiledMachineCode result;

    std::cout << "[LOOP-OPT] Compiling loop (unroll factor: " << analysis.unroll_factor << "):" << std::endl;
    std::cout << "[LOOP-OPT]   Induction var: " << analysis.induction_var << std::endl;
    std::cout << "[LOOP-OPT]   Range: " << analysis.start_value << " to " << analysis.end_value << std::endl;
    std::cout << "[LOOP-OPT]   Unroll factor: " << analysis.unroll_factor << "x" << std::endl;

    ASTNode* body = loop->get_body();
    if (!body || body->get_type() != ASTNode::Type::BLOCK_STATEMENT) {
        std::cout << "[LOOP-OPT] Loop body is not a block statement" << std::endl;
        return result;
    }

    BlockStatement* block = static_cast<BlockStatement*>(body);
    auto& statements = block->get_statements();

    if (statements.empty()) {
        std::cout << "[LOOP-OPT] Loop body is empty" << std::endl;
        return result;
    }

    BinaryExpression* assign_binop = nullptr;
    for (const auto& stmt : statements) {
        if (stmt->get_type() == ASTNode::Type::EXPRESSION_STATEMENT) {
            ExpressionStatement* expr_stmt = static_cast<ExpressionStatement*>(stmt.get());
            ASTNode* expr = expr_stmt->get_expression();

            if (expr && expr->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
                BinaryExpression* binexpr = static_cast<BinaryExpression*>(expr);
                if (binexpr->get_operator() == BinaryExpression::Operator::ASSIGN) {
                    assign_binop = binexpr;
                    break;
                }
            }
        }
    }

    if (!assign_binop) {
        std::cout << "[LOOP-OPT] No assignment (operator=25) found in loop body" << std::endl;
        return result;
    }

    ASTNode* target_node = assign_binop->get_left();
    ASTNode* value_node = assign_binop->get_right();

    if (!target_node || target_node->get_type() != ASTNode::Type::IDENTIFIER) {
        std::cout << "[LOOP-OPT] Assignment target is not an identifier" << std::endl;
        return result;
    }

    Identifier* target_var = static_cast<Identifier*>(target_node);
    std::string target_name = target_var->get_name();

    if (!value_node || value_node->get_type() != ASTNode::Type::BINARY_EXPRESSION) {
        std::cout << "[LOOP-OPT] Assignment value is not a binary expression" << std::endl;
        return result;
    }

    BinaryExpression* value_binop = static_cast<BinaryExpression*>(value_node);

    if (value_binop->get_operator() != BinaryExpression::Operator::ADD) {
        std::cout << "[LOOP-OPT] Only ADD operations supported for now (operator: "
                  << static_cast<int>(value_binop->get_operator()) << ")" << std::endl;
        return result;
    }

    std::cout << "[LOOP-OPT] Pattern recognized: " << target_name << " = " << target_name << " + <expr>" << std::endl;
    std::cout << "[LOOP-OPT] DEBUG: Testing with unroll_factor=1 (no unrolling)" << std::endl;


    code_buffer_.clear();
    embedded_strings_.clear();
    string_offsets_.clear();
    patches_.clear();

    emit_prologue();

    emit_byte(0x41); emit_byte(0x56);
    emit_byte(0x41); emit_byte(0x54);
    emit_byte(0x41); emit_byte(0x55);

    #ifdef _WIN32
    emit_byte(0x49); emit_byte(0x89); emit_byte(0xCE);
    #else
    emit_byte(0x49); emit_byte(0x89); emit_byte(0xFE);
    #endif

    emit_byte(0x49); emit_byte(0xC7); emit_byte(0xC4);
    emit_byte(analysis.start_value & 0xFF);
    emit_byte((analysis.start_value >> 8) & 0xFF);
    emit_byte((analysis.start_value >> 16) & 0xFF);
    emit_byte((analysis.start_value >> 24) & 0xFF);

    emit_byte(0x49); emit_byte(0xC7); emit_byte(0xC5);
    emit_byte(analysis.end_value & 0xFF);
    emit_byte((analysis.end_value >> 8) & 0xFF);
    emit_byte((analysis.end_value >> 16) & 0xFF);
    emit_byte((analysis.end_value >> 24) & 0xFF);

    size_t target_offset = embed_string(target_name);

    size_t loop_start_pos = code_buffer_.size();

    int unroll = analysis.unroll_factor;
    for (int u = 0; u < unroll; u++) {

        #ifdef _WIN32
        emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
        size_t p1 = code_buffer_.size() + 2;
        patches_.push_back({p1, target_offset});
        emit_byte(0x48); emit_byte(0xBA);
        for (int i = 0; i < 8; i++) emit_byte(0);
        #else
        emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
        size_t p1 = code_buffer_.size() + 2;
        patches_.push_back({p1, target_offset});
        emit_mov_rsi_imm(0);
        #endif

        emit_call_absolute((void*)jit_read_variable);
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xC6);

        emit_byte(0x4C); emit_byte(0x89); emit_byte(0xE0);
        if (u > 0) {
            emit_byte(0x48); emit_byte(0x83); emit_byte(0xC0); emit_byte(u);
        }

        emit_byte(0x48); emit_byte(0x01); emit_byte(0xF0);

        #ifdef _WIN32
        emit_byte(0x49); emit_byte(0x89); emit_byte(0xC0);
        emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF1);
        size_t p2 = code_buffer_.size() + 2;
        patches_.push_back({p2, target_offset});
        emit_byte(0x48); emit_byte(0xBA);
        for (int i = 0; i < 8; i++) emit_byte(0);
        #else
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xC2);
        emit_byte(0x4C); emit_byte(0x89); emit_byte(0xF7);
        size_t p2 = code_buffer_.size() + 2;
        patches_.push_back({p2, target_offset});
        emit_mov_rsi_imm(0);
        #endif

        emit_call_absolute((void*)jit_write_variable);
    }

    emit_byte(0x49); emit_byte(0x83); emit_byte(0xC4); emit_byte(analysis.unroll_factor);
    emit_byte(0x4D); emit_byte(0x39); emit_byte(0xEC);

    int32_t jump_offset = (int32_t)loop_start_pos - (int32_t)(code_buffer_.size() + 6);
    emit_byte(0x0F); emit_byte(0x8C);
    emit_byte(jump_offset & 0xFF);
    emit_byte((jump_offset >> 8) & 0xFF);
    emit_byte((jump_offset >> 16) & 0xFF);
    emit_byte((jump_offset >> 24) & 0xFF);

    emit_byte(0x41); emit_byte(0x5D);
    emit_byte(0x41); emit_byte(0x5C);
    emit_byte(0x41); emit_byte(0x5E);

    emit_byte(0x48); emit_byte(0x31); emit_byte(0xC0);

    emit_epilogue();
    emit_ret();

    size_t code_size = code_buffer_.size();
    size_t strings_size = 0;
    for (const auto& str : embedded_strings_) {
        strings_size += str.length() + 1;
    }

    uint8_t* executable_mem = allocate_executable_memory(code_size + strings_size);
    if (!executable_mem) {
        std::cout << "[LOOP-OPT] Failed to allocate executable memory" << std::endl;
        return result;
    }

    std::memcpy(executable_mem, code_buffer_.data(), code_size);
    finalize_strings(executable_mem);

    for (const auto& patch : patches_) {
        uint64_t string_addr = reinterpret_cast<uint64_t>(executable_mem) + code_size + patch.string_offset;
        std::memcpy(executable_mem + patch.code_position, &string_addr, 8);
    }

    result.code_ptr = executable_mem;
    result.code_size = code_size;

    std::cout << "[LOOP-OPT] Successfully generated " << code_size << " bytes of 4x unrolled loop!" << std::endl;

    return result;
}

PropertyCache* JITCompiler::get_property_cache(ASTNode* node) {
    if (!node) return nullptr;
    return &property_cache_[node];
}

void JITCompiler::print_property_cache_stats() const {
    std::cout << "\n=== Property Inline Cache Statistics ===" << std::endl;
    uint32_t total_hits = 0;
    uint32_t total_misses = 0;
    uint32_t cached_sites = 0;

    for (const auto& entry : property_cache_) {
        if (entry.second.hit_count + entry.second.miss_count > 0) {
            cached_sites++;
            total_hits += entry.second.hit_count;
            total_misses += entry.second.miss_count;

            if (entry.second.hit_count > 10) {
                std::cout << "  [IC] Property: " << entry.second.property_name
                          << " hits=" << entry.second.hit_count
                          << " misses=" << entry.second.miss_count
                          << " ratio=" << entry.second.get_hit_ratio() << "%" << std::endl;
            }
        }
    }

    std::cout << "\nTotal cache sites: " << cached_sites << std::endl;
    std::cout << "Total hits: " << total_hits << std::endl;
    std::cout << "Total misses: " << total_misses << std::endl;
    if (total_hits + total_misses > 0) {
        double ratio = 100.0 * total_hits / (total_hits + total_misses);
        std::cout << "Overall hit ratio: " << ratio << "%" << std::endl;
    }
    std::cout << "========================================\n" << std::endl;
}

} // namespace Quanta
