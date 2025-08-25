#ifndef QUANTA_LOOP_UNROLL_OPTIMIZER_H
#define QUANTA_LOOP_UNROLL_OPTIMIZER_H

#include "SpecializedNodes.h"
#include "OptimizedAST.h"
#include "Value.h"
#include "Context.h"
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>

namespace Quanta {

// Loop analysis and unrolling strategies
enum class UnrollStrategy : uint8_t {
    NO_UNROLL,           // Loop should not be unrolled
    PARTIAL_UNROLL_2X,   // Unroll 2 iterations at a time
    PARTIAL_UNROLL_4X,   // Unroll 4 iterations at a time  
    PARTIAL_UNROLL_8X,   // Unroll 8 iterations at a time
    FULL_UNROLL,         // Completely unroll the loop
    VECTORIZE_UNROLL     // Unroll and vectorize with SIMD
};

// Loop characteristics analysis
struct LoopAnalysis {
    bool has_constant_bounds;
    bool has_simple_increment;
    bool has_no_side_effects;
    bool has_no_dependencies;
    bool is_countable;
    bool is_vectorizable;
    
    int32_t min_iterations;
    int32_t max_iterations;
    int32_t estimated_iterations;
    
    uint32_t loop_body_complexity;
    uint32_t register_pressure;
    
    UnrollStrategy recommended_strategy;
    double estimated_speedup;
};

// Unrolled loop instruction sequences
struct UnrolledLoopCode {
    std::vector<uint32_t> initialization_nodes;
    std::vector<uint32_t> unrolled_body_nodes;
    std::vector<uint32_t> cleanup_nodes;
    uint32_t unroll_factor;
    bool uses_simd;
};

class LoopUnrollOptimizer {
private:
    OptimizedAST* ast_context_;
    SpecializedNodeProcessor* specialized_processor_;
    
    // Loop analysis cache
    std::unordered_map<uint32_t, LoopAnalysis> analysis_cache_;
    std::unordered_map<uint32_t, UnrolledLoopCode> unrolled_cache_;
    
    // Performance counters
    uint64_t total_loops_analyzed_;
    uint64_t total_loops_unrolled_;
    uint64_t total_time_saved_;
    
public:
    LoopUnrollOptimizer(OptimizedAST* ast, SpecializedNodeProcessor* processor);
    
    // Loop analysis
    LoopAnalysis analyze_loop(uint32_t loop_node_id, Context& ctx);
    bool can_unroll_safely(const LoopAnalysis& analysis);
    UnrollStrategy determine_unroll_strategy(const LoopAnalysis& analysis);
    
    // Loop unrolling
    uint32_t create_unrolled_loop(uint32_t original_loop_id, UnrollStrategy strategy);
    UnrolledLoopCode generate_unrolled_code(uint32_t loop_node_id, uint32_t unroll_factor);
    
    // Execution methods
    Value execute_unrolled_loop(uint32_t unrolled_loop_id, Context& ctx);
    Value execute_vectorized_loop(uint32_t loop_id, Context& ctx);
    Value execute_fully_unrolled_loop(uint32_t loop_id, Context& ctx);
    
    // Optimization detection
    bool should_unroll_loop(uint32_t loop_node_id);
    void optimize_hot_loops();
    void update_loop_statistics(uint32_t loop_id, uint64_t execution_time);
    
    // Performance monitoring
    double get_unroll_effectiveness() const;
    uint64_t get_total_time_saved() const { return total_time_saved_; }
    void print_unroll_statistics() const;
    
    // Memory management
    void clear_unroll_cache();
    size_t get_memory_usage() const;
};

// Specific unroll implementations
class PartialUnrollGenerator {
public:
    static UnrolledLoopCode generate_2x_unroll(uint32_t loop_id, OptimizedAST* ast);
    static UnrolledLoopCode generate_4x_unroll(uint32_t loop_id, OptimizedAST* ast);
    static UnrolledLoopCode generate_8x_unroll(uint32_t loop_id, OptimizedAST* ast);
    
    static Value execute_2x_unrolled(const UnrolledLoopCode& code, Context& ctx);
    static Value execute_4x_unrolled(const UnrolledLoopCode& code, Context& ctx);
    static Value execute_8x_unrolled(const UnrolledLoopCode& code, Context& ctx);
};

class FullUnrollGenerator {
public:
    static UnrolledLoopCode generate_full_unroll(uint32_t loop_id, int32_t iteration_count, 
                                                OptimizedAST* ast);
    static Value execute_fully_unrolled(const UnrolledLoopCode& code, Context& ctx);
    static bool can_fully_unroll(const LoopAnalysis& analysis);
};

class VectorizedUnrollGenerator {
public:
    static UnrolledLoopCode generate_vectorized_unroll(uint32_t loop_id, OptimizedAST* ast);
    static Value execute_vectorized_unrolled(const UnrolledLoopCode& code, Context& ctx);
    static bool can_vectorize_loop(const LoopAnalysis& analysis);
    
    // SIMD-specific operations
    static void vectorize_arithmetic_operations(std::vector<uint32_t>& nodes);
    static void vectorize_array_accesses(std::vector<uint32_t>& nodes);
};

// Loop pattern recognizers
class LoopPatternRecognizer {
public:
    enum class LoopPattern {
        SIMPLE_COUNT,        // for(i=0; i<n; i++)
        ARRAY_ITERATION,     // for(i=0; i<arr.length; i++) arr[i]...
        NESTED_LOOPS,        // for(i...) for(j...) ...
        REDUCTION,           // sum += arr[i]
        MAP_OPERATION,       // result[i] = func(arr[i])
        FILTER_OPERATION,    // if(condition) result.push(arr[i])
        UNKNOWN
    };
    
    static LoopPattern recognize_pattern(uint32_t loop_node_id, OptimizedAST* ast);
    static UnrollStrategy get_optimal_strategy_for_pattern(LoopPattern pattern);
    static bool can_apply_pattern_specific_optimizations(LoopPattern pattern);
};

// Runtime loop optimization
class AdaptiveLoopOptimizer {
private:
    struct LoopProfile {
        uint64_t execution_count;
        uint64_t total_execution_time;
        double average_iteration_count;
        bool is_hot_loop;
        UnrollStrategy current_strategy;
        double current_speedup;
    };
    
    std::unordered_map<uint32_t, LoopProfile> loop_profiles_;
    
public:
    void profile_loop_execution(uint32_t loop_id, uint64_t execution_time, 
                               int32_t iteration_count);
    bool should_reoptimize_loop(uint32_t loop_id);
    UnrollStrategy adapt_unroll_strategy(uint32_t loop_id);
    void optimize_based_on_profile(uint32_t loop_id);
    
    std::vector<uint32_t> get_hot_loops() const;
    void reset_profiles();
};

} // namespace Quanta

#endif // QUANTA_LOOP_UNROLL_OPTIMIZER_H