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
    
    // Performance metrics
    uint32_t total_compilations_;
    uint32_t cache_hits_;
    uint32_t cache_misses_;
    
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
    
    // Cache management
    void clear_cache();
    void invalidate_cache(ASTNode* node);
    
    // Performance metrics
    uint32_t get_total_compilations() const { return total_compilations_; }
    uint32_t get_cache_hits() const { return cache_hits_; }
    uint32_t get_cache_misses() const { return cache_misses_; }
    double get_cache_hit_ratio() const;
    
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