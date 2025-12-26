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
    Interpreter,
    Bytecode,
    Optimized,
    MachineCode
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
struct CallSiteFeedback {
    Function* target_function = nullptr;
    uint32_t call_count = 0;
    uint32_t polymorphic_count = 0;
    bool is_monomorphic = true;
    bool should_inline() const {
        return is_monomorphic && call_count >= 10;
    }
    void record_call(Function* func) {
        call_count++;
        if (target_function == nullptr) {
            target_function = func;
        } else if (target_function != func) {
            is_monomorphic = false;
            polymorphic_count++;
        }
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
    CallSiteFeedback call_site_feedback;
    bool should_tier_up(uint32_t bytecode_thresh, uint32_t optimized_thresh, uint32_t machine_code_thresh) const {
        switch (current_tier) {
            case JITTier::Interpreter:
                return execution_count >= bytecode_thresh;
            case JITTier::Bytecode:
                return execution_count >= optimized_thresh;
            case JITTier::Optimized:
                return execution_count >= machine_code_thresh;
            case JITTier::MachineCode:
                return false;
        }
        return false;
    }
};
struct CompiledBytecode {
    std::vector<BytecodeOp> instructions;
    std::vector<Value> constant_pool;
    std::vector<std::string> variable_names;
    JITTier tier;
    std::chrono::high_resolution_clock::time_point compile_time;
    uint32_t execution_count = 0;
    uint64_t total_execution_time_ns = 0;
    double get_average_execution_time_ms() const {
        if (execution_count == 0) return 0.0;
        return (total_execution_time_ns / execution_count) / 1000000.0;
    }
    uint32_t add_constant(const Value& value) {
        constant_pool.push_back(value);
        return constant_pool.size() - 1;
    }
    uint32_t add_variable(const std::string& name) {
        for (size_t i = 0; i < variable_names.size(); i++) {
            if (variable_names[i] == name) {
                return i;
            }
        }
        variable_names.push_back(name);
        return variable_names.size() - 1;
    }
};
struct CompiledMachineCode {
    uint8_t* code_ptr = nullptr;
    size_t code_size = 0;
    JITTier tier = JITTier::MachineCode;
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
    bool generate_bytecode_for_node_with_context(ASTNode* node, CompiledBytecode& compiled);
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
    std::unordered_map<std::string, size_t> string_offsets_;
    std::vector<std::string> embedded_strings_;
    struct PatchInfo {
        size_t code_position;
        size_t string_offset;
    };
    std::vector<PatchInfo> patches_;
    uint8_t* allocate_executable_memory(size_t size);
    void free_executable_memory(uint8_t* ptr, size_t size);
    size_t embed_string(const std::string& str);
    void finalize_strings(uint8_t* base_ptr);
    void emit_prologue();
    void emit_epilogue();
    void emit_mov_rax_imm(int64_t value);
    void emit_mov_rbx_imm(int64_t value);
    void emit_mov_rsi_imm(int64_t value);
    void emit_add_rax_rbx();
    void emit_sub_rax_rbx();
    void emit_mul_rax_rbx();
    void emit_div_rax_rbx();
    void emit_mod_rax_rbx();
    void emit_and_rax_rbx();
    void emit_or_rax_rbx();
    void emit_xor_rax_rbx();
    void emit_shl_rax_cl();
    void emit_shr_rax_cl();
    void emit_sar_rax_cl();
    void emit_neg_rax();
    void emit_not_rax();
    void emit_inc_rax();
    void emit_dec_rax();
    void emit_call_absolute(void* func_ptr);
    void emit_ret();
    void emit_cmp_rax_rbx();
    void emit_setl_al();
    void emit_setg_al();
    void emit_setle_al();
    void emit_setge_al();
    void emit_sete_al();
    void emit_setne_al();
    void emit_movzx_rax_al();
    void emit_test_rax_rax();
    void emit_jz_rel8(int8_t offset);
    void emit_jnz_rel8(int8_t offset);
    void emit_jz_rel32(int32_t offset);
    void emit_jnz_rel32(int32_t offset);
    void emit_jmp_rel8(int8_t offset);
    void emit_jmp_rel32(int32_t offset);
    void emit_movsd_xmm0_mem(int64_t addr);
    void emit_movsd_xmm1_mem(int64_t addr);
    void emit_addsd_xmm0_xmm1();
    void emit_subsd_xmm0_xmm1();
    void emit_mulsd_xmm0_xmm1();
    void emit_divsd_xmm0_xmm1();
    void emit_byte(uint8_t byte) { code_buffer_.push_back(byte); }
    void emit_bytes(const uint8_t* bytes, size_t count) {
        code_buffer_.insert(code_buffer_.end(), bytes, bytes + count);
    }
};
} // namespace Quanta
#endif