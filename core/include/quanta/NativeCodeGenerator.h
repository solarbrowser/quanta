/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_NATIVE_CODE_GENERATOR_H
#define QUANTA_NATIVE_CODE_GENERATOR_H

#include "quanta/SpecializedNodes.h"
#include "quanta/Value.h"
#include "quanta/Context.h"
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <functional>

namespace Quanta {

enum class NativeInstruction : uint8_t {
    LOAD_IMMEDIATE,
    LOAD_VARIABLE,
    STORE_VARIABLE,
    ADD_NUMBERS,
    SUB_NUMBERS,
    MUL_NUMBERS,
    DIV_NUMBERS,
    COMPARE_EQUAL,
    COMPARE_LESS,
    JUMP_CONDITIONAL,
    JUMP_UNCONDITIONAL,
    CALL_FUNCTION,
    RETURN_VALUE,
    SIMD_ADD_4X,
    SIMD_MUL_4X,
    PREFETCH_MEMORY
};

struct alignas(16) NativeCodeInstruction {
    NativeInstruction opcode;
    uint8_t flags;
    uint16_t operand_count;
    uint32_t target_register;
    
    union {
        struct {
            double immediate_value;
        } load_imm;
        
        struct {
            uint32_t variable_id;
            uint32_t memory_offset;
        } load_var;
        
        struct {
            uint32_t source_reg;
            uint32_t dest_reg;
        } binary_op;
        
        struct {
            uint32_t condition_reg;
            uint32_t jump_target;
        } conditional_jump;
        
        struct {
            uint32_t function_id;
            std::array<uint32_t, 6> arg_registers;
            uint8_t arg_count;
        } function_call;
        
        struct {
            std::array<uint32_t, 4> source_regs;
            uint32_t dest_reg;
        } simd_op;
    } operands;
};

struct NativeCompiledFunction {
    std::vector<uint8_t> machine_code;
    std::vector<NativeCodeInstruction> instructions;
    std::function<Value(Context&)> native_function;
    
    uint32_t function_id;
    uint32_t original_ast_node;
    size_t code_size;
    bool uses_simd;
    bool is_hot_function;
    
    uint64_t execution_count;
    uint64_t total_execution_time;
    double average_speedup;
};

class NativeCodeGenerator {
private:
    OptimizedAST* ast_context_;
    SpecializedNodeProcessor* specialized_processor_;
    
    std::unordered_map<uint32_t, std::unique_ptr<NativeCompiledFunction>> compiled_functions_;
    std::vector<uint8_t> code_buffer_;
    
    std::array<bool, 16> register_usage_;
    uint32_t next_available_register_;
    
    uint64_t total_functions_compiled_;
    uint64_t total_native_executions_;
    uint64_t total_compilation_time_;
    
public:
    NativeCodeGenerator(OptimizedAST* ast, SpecializedNodeProcessor* processor);
    ~NativeCodeGenerator();
    
    uint32_t compile_to_native(uint32_t ast_node_id);
    uint32_t compile_specialized_node(uint32_t specialized_node_id);
    std::unique_ptr<NativeCompiledFunction> compile_function(uint32_t node_id);
    
    void generate_arithmetic_code(NativeCompiledFunction& func, 
                                 const OptimizedAST::OptimizedNode& node);
    void generate_loop_code(NativeCompiledFunction& func,
                           const SpecializedNode& node);
    void generate_property_access_code(NativeCompiledFunction& func,
                                      const SpecializedNode& node);
    void generate_simd_code(NativeCompiledFunction& func,
                           const std::vector<uint32_t>& operands);
    
    Value execute_native_function(uint32_t function_id, Context& ctx);
    
    bool should_compile_to_native(uint32_t node_id);
    void identify_hot_functions();
    void recompile_with_better_optimization(uint32_t function_id);
    
    uint32_t allocate_register();
    void free_register(uint32_t reg_id);
    void reset_register_allocation();
    
    void emit_x86_instruction(NativeCompiledFunction& func, 
                             const NativeCodeInstruction& instruction);
    void emit_function_prologue(NativeCompiledFunction& func);
    void emit_function_epilogue(NativeCompiledFunction& func);
    
    double get_native_code_speedup() const;
    size_t get_total_code_size() const;
    void print_compilation_stats() const;
    
    void clear_compiled_code();
    void garbage_collect_unused_functions();
    size_t get_memory_usage() const;
};

class X86_64CodeGenerator {
public:
    static void generate_add_instruction(std::vector<uint8_t>& code, uint32_t src, uint32_t dest);
    static void generate_mul_instruction(std::vector<uint8_t>& code, uint32_t src, uint32_t dest);
    static void generate_load_immediate(std::vector<uint8_t>& code, double value, uint32_t dest);
    static void generate_function_call(std::vector<uint8_t>& code, uint32_t function_addr);
    static void generate_conditional_jump(std::vector<uint8_t>& code, uint32_t condition, uint32_t target);
    
    static void generate_simd_add_4x(std::vector<uint8_t>& code, uint32_t src, uint32_t dest);
    static void generate_simd_mul_4x(std::vector<uint8_t>& code, uint32_t src, uint32_t dest);
    
    static void generate_memory_load(std::vector<uint8_t>& code, uint32_t addr, uint32_t dest);
    static void generate_memory_store(std::vector<uint8_t>& code, uint32_t src, uint32_t addr);
    static void generate_prefetch(std::vector<uint8_t>& code, uint32_t addr);
};

class JITCompilationPipeline {
private:
    struct CompilationJob {
        uint32_t node_id;
        uint32_t priority;
        uint64_t creation_time;
        bool requires_simd;
    };
    
    std::vector<CompilationJob> compilation_queue_;
    NativeCodeGenerator* code_generator_;
    
public:
    JITCompilationPipeline(NativeCodeGenerator* generator);
    
    void queue_for_compilation(uint32_t node_id, uint32_t priority = 1);
    void process_compilation_queue();
    uint32_t get_next_compilation_job();
    
    void update_compilation_priorities();
    void trigger_recompilation_if_beneficial(uint32_t function_id);
    
    void start_background_compilation();
    void stop_background_compilation();
    bool is_compiling_in_background() const;
};

class RuntimeOptimizationFeedback {
private:
    struct FunctionProfile {
        uint64_t call_count;
        uint64_t total_execution_time;
        std::vector<uint32_t> hot_paths;
        std::vector<double> typical_argument_values;
        bool benefits_from_simd;
        double current_speedup;
    };
    
    std::unordered_map<uint32_t, FunctionProfile> function_profiles_;
    
public:
    void record_function_execution(uint32_t function_id, uint64_t execution_time,
                                  const std::vector<Value>& arguments);
    void identify_optimization_opportunities();
    std::vector<uint32_t> get_functions_needing_recompilation() const;
    void suggest_simd_opportunities(uint32_t function_id) const;
    
    bool should_enable_simd(uint32_t function_id) const;
    bool should_unroll_loops(uint32_t function_id) const;
    bool should_inline_functions(uint32_t function_id) const;
};

class NativeExecutionEnvironment {
private:
    std::vector<uint8_t> executable_memory_;
    size_t memory_size_;
    void* execution_context_;
    
public:
    NativeExecutionEnvironment(size_t memory_size = 1024 * 1024);
    ~NativeExecutionEnvironment();
    
    void* allocate_executable_memory(size_t size);
    void make_memory_executable(void* memory, size_t size);
    void free_executable_memory(void* memory, size_t size);
    
    Value execute_native_code(void* code_ptr, Context& ctx);
    void setup_execution_context(Context& ctx);
    void cleanup_execution_context();
    
    bool verify_code_integrity(void* code_ptr, size_t size);
    void enable_execution_profiling(bool enable);
    void dump_execution_statistics() const;
};

}

#endif
