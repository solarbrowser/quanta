/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef QUANTA_JIT_H
#define QUANTA_JIT_H

#include "quanta/Value.h"
#include "quanta/Bytecode.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <chrono>
#include <cstdint>

namespace Quanta {

class ASTNode;
class Context;
class Function;

enum class JITTier {
    Interpreter,      // No JIT, pure AST interpretation
    Bytecode,         // Compiled to bytecode
    Optimized,        // Type-specialized bytecode
    MachineCode       // Native x86-64 assembly
};


struct TypeFeedback {
    uint32_t number_seen = 0;
    uint32_t string_seen = 0;
    uint32_t object_seen = 0;
    uint32_t boolean_seen = 0;
    uint32_t undefined_seen = 0;
    uint32_t total_samples = 0;

    bool is_monomorphic() const {
        if (total_samples < 10) return false;
        uint32_t max_count = std::max(std::max(number_seen, string_seen), std::max(object_seen, boolean_seen));
        return max_count > total_samples * 0.95;
    }

    Value::Type get_dominant_type() const {
        uint32_t max_count = std::max(std::max(number_seen, string_seen), std::max(object_seen, boolean_seen));
        if (max_count == number_seen) return Value::Type::Number;
        if (max_count == string_seen) return Value::Type::String;
        if (max_count == object_seen) return Value::Type::Object;
        if (max_count == boolean_seen) return Value::Type::Boolean;
        return Value::Type::Undefined;
    }

    void record_type(const Value& value) {
        total_samples++;
        if (value.is_number()) number_seen++;
        else if (value.is_string()) string_seen++;
        else if (value.is_object()) object_seen++;
        else if (value.is_boolean()) boolean_seen++;
        else undefined_seen++;
    }
};


struct HotspotInfo {
    ASTNode* node = nullptr;
    uint32_t execution_count = 0;
    JITTier current_tier = JITTier::Interpreter;

    std::chrono::high_resolution_clock::time_point first_execution;
    std::chrono::high_resolution_clock::time_point last_execution;
    uint64_t total_execution_time_ns = 0;

    std::unordered_map<std::string, TypeFeedback> operation_types;

    bool should_tier_up() const {
        switch (current_tier) {
            case JITTier::Interpreter:
                return execution_count >= 100;  
            case JITTier::Bytecode:
                return execution_count >= 1000; 
            case JITTier::Optimized:
                return execution_count >= 10000; 
            case JITTier::MachineCode:
                return false; 
        }
        return false;
    }
};


struct CompiledBytecode {
    std::vector<BytecodeOp> instructions;
    JITTier tier;
    std::chrono::high_resolution_clock::time_point compile_time;
    uint32_t execution_count = 0;

    uint64_t total_execution_time_ns = 0;

    double get_average_execution_time_ms() const {
        if (execution_count == 0) return 0.0;
        return (total_execution_time_ns / execution_count) / 1000000.0;
    }
};


struct CompiledMachineCode {
    uint8_t* code_ptr = nullptr;
    size_t code_size = 0;
    JITTier tier = JITTier::MachineCode;

    // Function signature: Value (*)(Context&, const Value* args, size_t argc)
    using NativeFunction = Value (*)(Context&, const Value*, size_t);

    NativeFunction get_native_function() const {
        return reinterpret_cast<NativeFunction>(code_ptr);
    }
};


class JITCompiler {
public:
    JITCompiler();
    ~JITCompiler();

    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool is_enabled() const { return enabled_; }

    void set_bytecode_threshold(uint32_t threshold) { bytecode_threshold_ = threshold; }
    void set_optimized_threshold(uint32_t threshold) { optimized_threshold_ = threshold; }
    void set_machine_code_threshold(uint32_t threshold) { machine_code_threshold_ = threshold; }

    void record_execution(ASTNode* node, uint64_t execution_time_ns);
    void record_type_feedback(ASTNode* node, const std::string& operation, const Value& value);

    bool try_execute_jit(ASTNode* node, Context& ctx, Value& result);

    bool compile_to_bytecode(ASTNode* node);
    bool compile_to_optimized(ASTNode* node);
    bool compile_to_machine_code(ASTNode* node);

    bool compile_function(Function* func);
    bool try_execute_jit_function(Function* func, Context& ctx, const std::vector<Value>& args, Value& result);

    void clear_cache();
    void invalidate_node(ASTNode* node);
    void invalidate_function(Function* func);

    struct Stats {
        uint32_t total_compilations = 0;
        uint32_t bytecode_compilations = 0;
        uint32_t optimized_compilations = 0;
        uint32_t machine_code_compilations = 0;

        uint32_t cache_hits = 0;
        uint32_t cache_misses = 0;
        uint32_t deoptimizations = 0;

        uint64_t total_jit_time_ns = 0;
        uint64_t total_interpreter_time_ns = 0;

        double get_speedup() const {
            if (total_interpreter_time_ns == 0) return 1.0;
            return static_cast<double>(total_interpreter_time_ns) / total_jit_time_ns;
        }

        double get_cache_hit_ratio() const {
            uint32_t total = cache_hits + cache_misses;
            if (total == 0) return 0.0;
            return static_cast<double>(cache_hits) / total;
        }
    };

    const Stats& get_stats() const { return stats_; }
    void reset_stats() { stats_ = Stats(); }
    void print_stats() const;

private:
    bool enabled_ = true;

    uint32_t bytecode_threshold_ = 100;
    uint32_t optimized_threshold_ = 1000;
    uint32_t machine_code_threshold_ = 10000;

    std::unordered_map<ASTNode*, HotspotInfo> hotspots_;

    std::unordered_map<ASTNode*, CompiledBytecode> bytecode_cache_;
    std::unordered_map<ASTNode*, CompiledMachineCode> machine_code_cache_;

    std::unordered_map<Function*, CompiledBytecode> function_bytecode_cache_;
    std::unordered_map<Function*, CompiledMachineCode> function_machine_code_cache_;

    Stats stats_;

    Value execute_bytecode(const CompiledBytecode& compiled, Context& ctx);
    Value execute_machine_code(const CompiledMachineCode& compiled, Context& ctx, const Value* args, size_t argc);
    bool generate_bytecode_for_node(ASTNode* node, std::vector<BytecodeOp>& instructions);

    void deoptimize(ASTNode* node);
};


class MachineCodeGenerator {
public:
    MachineCodeGenerator();
    ~MachineCodeGenerator();

    CompiledMachineCode compile(ASTNode* node, const TypeFeedback& feedback);

    CompiledMachineCode compile_function(Function* func, const TypeFeedback& feedback);

    void free_code(CompiledMachineCode& compiled);

private:
    std::vector<uint8_t> code_buffer_;

    uint8_t* allocate_executable_memory(size_t size);
    void free_executable_memory(uint8_t* ptr, size_t size);

    void emit_prologue();
    void emit_epilogue();
    void emit_mov_rax_imm(int64_t value);
    void emit_add_rax_rbx();
    void emit_sub_rax_rbx();
    void emit_mul_rax_rbx();
    void emit_div_rax_rbx();
    void emit_ret();

    void emit_byte(uint8_t byte) { code_buffer_.push_back(byte); }
    void emit_bytes(const uint8_t* bytes, size_t count) {
        code_buffer_.insert(code_buffer_.end(), bytes, bytes + count);
    }
};

} // namespace Quanta

#endif 
