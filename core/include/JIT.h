/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_JIT_H
#define QUANTA_JIT_H

#include "Value.h"
#include "Context.h"
#include "../../parser/include/AST.h"
#include <unordered_map>
#include <memory>
#include <chrono>

namespace Quanta {

// Forward declarations
class ASTNode;
class Context;

/**
 * JIT (Just-In-Time) Compiler for Quanta JavaScript Engine
 * Provides runtime optimization for hot code paths
 */
class JITCompiler {
public:
    // Compilation tiers
    enum class OptimizationLevel {
        None,           // No optimization
        Basic,          // Basic optimizations
        Advanced,       // Advanced optimizations
        Maximum         // Maximum optimization
    };
    
    // Hot code detection
    struct HotSpot {
        ASTNode* node;
        uint32_t execution_count;
        std::chrono::high_resolution_clock::time_point last_execution;
        OptimizationLevel optimization_level;
        bool is_compiled;
        
        HotSpot() : node(nullptr), execution_count(0), 
                   optimization_level(OptimizationLevel::None), is_compiled(false) {}
    };
    
    // Compiled code cache
    struct CompiledCode {
        std::function<Value(Context&)> optimized_function;
        OptimizationLevel level;
        std::chrono::high_resolution_clock::time_point compile_time;
        uint32_t execution_count;
        
        CompiledCode() : level(OptimizationLevel::None), execution_count(0) {}
    };

private:
    // Hot spot detection
    std::unordered_map<ASTNode*, HotSpot> hotspots_;
    
    // Compiled code cache
    std::unordered_map<ASTNode*, CompiledCode> compiled_cache_;
    
    // JIT configuration
    uint32_t hotspot_threshold_;
    uint32_t recompile_threshold_;
    bool jit_enabled_;
    
    // PHASE 2: Integration with Phase 1 hot function detection
    std::unordered_map<class Function*, CompiledCode> function_cache_;
    uint32_t function_compile_threshold_;
    
    // Performance metrics
    uint32_t total_compilations_;
    uint32_t cache_hits_;
    uint32_t cache_misses_;
    uint32_t inline_cache_hits_;
    bool type_feedback_enabled_;
    
    // optimization flags
    bool ultra_fast_mode_;
    bool cpu_cache_optimized_;
    
    // Type feedback and profiling
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
    
    // Configuration
    void enable_jit(bool enabled) { jit_enabled_ = enabled; }
    bool is_jit_enabled() const { return jit_enabled_; }
    void set_hotspot_threshold(uint32_t threshold) { hotspot_threshold_ = threshold; }
    
    // Hot spot detection and compilation
    bool should_compile(ASTNode* node);
    bool try_execute_compiled(ASTNode* node, Context& ctx, Value& result);
    void record_execution(ASTNode* node);
    
    // Compilation methods
    bool compile_node(ASTNode* node, OptimizationLevel level);
    Value execute_compiled(ASTNode* node, Context& ctx);
    
    // Optimization levels
    std::function<Value(Context&)> compile_basic_optimization(ASTNode* node);
    std::function<Value(Context&)> compile_advanced_optimization(ASTNode* node);
    std::function<Value(Context&)> compile_maximum_optimization(ASTNode* node);
    
    // PHASE 2: Hot Function JIT Compilation
    bool should_compile_function(class Function* func);
    bool try_execute_compiled_function(class Function* func, Context& ctx, const std::vector<Value>& args, Value& result);
    bool compile_hot_function(class Function* func);
    void record_function_execution(class Function* func);
    
    // Cache management
    void clear_cache();
    void invalidate_cache(ASTNode* node);
    void invalidate_function_cache(class Function* func);
    
    // Performance metrics
    uint32_t get_total_compilations() const { return total_compilations_; }
    uint32_t get_cache_hits() const { return cache_hits_; }
    uint32_t get_cache_misses() const { return cache_misses_; }
    double get_cache_hit_ratio() const;
    
    // Type feedback and profiling
    void record_type_feedback(ASTNode* node, const Value& result);
    void record_function_profile(ASTNode* node);
    void enable_type_feedback(bool enabled) { type_feedback_enabled_ = enabled; }
    
    // Debugging
    void print_hotspots() const;
    void print_cache_stats() const;
};

/**
 * JIT-optimized function types
 */
namespace JITOptimizations {
    // Optimized arithmetic operations
    Value optimized_add(const Value& left, const Value& right);
    Value optimized_subtract(const Value& left, const Value& right);
    Value optimized_multiply(const Value& left, const Value& right);
    Value optimized_divide(const Value& left, const Value& right);
    
    // Optimized string operations
    Value optimized_string_concat(const Value& left, const Value& right);
    Value optimized_string_charAt(const Value& str, const Value& index);
    
    // Optimized array operations
    Value optimized_array_access(const Value& array, const Value& index);
    Value optimized_array_length(const Value& array);
    
    // Optimized loop constructs
    class OptimizedLoop {
    public:
        static Value execute_for_loop(ASTNode* init, ASTNode* test, 
                                     ASTNode* update, ASTNode* body, Context& ctx);
        static Value execute_while_loop(ASTNode* test, ASTNode* body, Context& ctx);
    };
}

} // namespace Quanta

#endif // QUANTA_JIT_H