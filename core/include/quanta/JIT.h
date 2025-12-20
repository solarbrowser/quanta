/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_JIT_H
#define QUANTA_JIT_H

#include "quanta/Value.h"
#include "quanta/Context.h"
#include "quanta/AST.h"
#include <unordered_map>
#include <memory>
#include <chrono>

namespace Quanta {

class ASTNode;
class Context;

/**
 * JIT (Just-In-Time) Compiler for Quanta JavaScript Engine
 * Provides runtime optimization for hot code paths
 */
class JITCompiler {
public:
    enum class OptimizationLevel {
        None,
        Basic,
        Advanced,
    };
    
    struct HotSpot {
        ASTNode* node;
        uint32_t execution_count;
        std::chrono::high_resolution_clock::time_point last_execution;
        OptimizationLevel optimization_level;
        bool is_compiled;
        
        HotSpot() : node(nullptr), execution_count(0), 
                   optimization_level(OptimizationLevel::None), is_compiled(false) {}
    };
    
    struct CompiledCode {
        std::function<Value(Context&)> optimized_function;
        OptimizationLevel level;
        std::chrono::high_resolution_clock::time_point compile_time;
        uint32_t execution_count;
        
        CompiledCode() : level(OptimizationLevel::None), execution_count(0) {}
    };

private:
    std::unordered_map<ASTNode*, HotSpot> hotspots_;
    
    std::unordered_map<ASTNode*, CompiledCode> compiled_cache_;
    
    uint32_t hotspot_threshold_;
    uint32_t recompile_threshold_;
    bool jit_enabled_;
    
    std::unordered_map<class Function*, CompiledCode> function_cache_;
    uint32_t function_compile_threshold_;
    
    uint32_t total_compilations_;
    uint32_t cache_hits_;
    uint32_t cache_misses_;
    uint32_t inline_cache_hits_;
    bool type_feedback_enabled_;
    
    bool ultra_fast_mode_;
    bool cpu_cache_optimized_;
    
    struct TypeProfile {
        uint32_t number_count;
        uint32_t string_count;
        uint32_t object_count;
        uint32_t boolean_count;
        uint32_t total_samples;
        
        TypeProfile() : number_count(0), string_count(0), object_count(0), 
                       boolean_count(0), total_samples(0) {}
    };
    
    struct FunctionProfile {
        uint32_t call_count;
        std::chrono::high_resolution_clock::time_point last_call;
        
        FunctionProfile() : call_count(0) {}
    };
    
    std::unordered_map<ASTNode*, TypeProfile> type_profiles_;
    std::unordered_map<ASTNode*, FunctionProfile> function_profiles_;
    
public:
    JITCompiler();
    ~JITCompiler();
    
    void enable_jit(bool enabled) { jit_enabled_ = enabled; }
    bool is_jit_enabled() const { return jit_enabled_; }
    void set_hotspot_threshold(uint32_t threshold) { hotspot_threshold_ = threshold; }
    
    bool should_compile(ASTNode* node);
    bool try_execute_compiled(ASTNode* node, Context& ctx, Value& result);
    void record_execution(ASTNode* node);
    
    bool compile_node(ASTNode* node, OptimizationLevel level);
    Value execute_compiled(ASTNode* node, Context& ctx);
    
    std::function<Value(Context&)> compile_basic_optimization(ASTNode* node);
    std::function<Value(Context&)> compile_advanced_optimization(ASTNode* node);
    std::function<Value(Context&)> compile_maximum_optimization(ASTNode* node);
    
    bool should_compile_function(class Function* func);
    bool try_execute_compiled_function(class Function* func, Context& ctx, const std::vector<Value>& args, Value& result);
    bool compile_hot_function(class Function* func);
    void record_function_execution(class Function* func);
    
    void clear_cache();
    void invalidate_cache(ASTNode* node);
    void invalidate_function_cache(class Function* func);
    
    uint32_t get_total_compilations() const { return total_compilations_; }
    uint32_t get_cache_hits() const { return cache_hits_; }
    uint32_t get_cache_misses() const { return cache_misses_; }
    double get_cache_hit_ratio() const;
    
    void record_type_feedback(ASTNode* node, const Value& result);
    void record_function_profile(ASTNode* node);
    void enable_type_feedback(bool enabled) { type_feedback_enabled_ = enabled; }
    
    void print_hotspots() const;
    void print_cache_stats() const;
};

/**
 * JIT-function types
 */
namespace JITOptimizations {
    Value optimized_add(const Value& left, const Value& right);
    Value optimized_subtract(const Value& left, const Value& right);
    Value optimized_multiply(const Value& left, const Value& right);
    Value optimized_divide(const Value& left, const Value& right);
    
    Value optimized_string_concat(const Value& left, const Value& right);
    Value optimized_string_charAt(const Value& str, const Value& index);
    
    Value optimized_array_access(const Value& array, const Value& index);
    Value optimized_array_length(const Value& array);
    
    class OptimizedLoop {
    public:
        static Value execute_for_loop(ASTNode* init, ASTNode* test, 
                                     ASTNode* update, ASTNode* body, Context& ctx);
        static Value execute_while_loop(ASTNode* test, ASTNode* body, Context& ctx);
    };
}

}

#endif
