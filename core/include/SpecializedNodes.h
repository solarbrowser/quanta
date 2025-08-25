#ifndef QUANTA_SPECIALIZED_NODES_H
#define QUANTA_SPECIALIZED_NODES_H

#include "OptimizedAST.h"
#include "Value.h"
#include "Context.h"
#include <vector>
#include <array>
#include <cstdint>

namespace Quanta {

// Specialized node types for ultra-fast evaluation of common patterns
enum class SpecializedNodeType : uint8_t {
    FAST_FOR_LOOP,           // for(let i=0; i<n; i++) optimized
    FAST_PROPERTY_CHAIN,     // obj.a.b.c direct memory access
    FAST_MATH_EXPRESSION,    // Mathematical calculations with SIMD
    FAST_ARRAY_ACCESS,       // arr[i] with bounds check elimination
    FAST_FUNCTION_CALL,      // Direct function dispatch
    FAST_STRING_CONCAT,      // String concatenation with buffer pooling
    FAST_CONDITIONAL,        // if/else with branch prediction
    FAST_VARIABLE_ACCESS     // Local variable access with register allocation
};

// Cache-aligned specialized node structure
struct alignas(64) SpecializedNode {
    SpecializedNodeType type;
    uint8_t flags;
    uint16_t optimization_level;
    uint32_t node_id;
    
    union {
        // For loop optimization data
        struct {
            uint32_t init_var_id;
            uint32_t condition_id;
            uint32_t increment_id;
            uint32_t body_id;
            int32_t loop_count_hint;
        } fast_loop;
        
        // Property chain optimization
        struct {
            uint32_t object_id;
            std::array<uint32_t, 8> property_offsets;
            uint8_t chain_length;
            bool all_numeric_indices;
        } property_chain;
        
        // Math expression optimization
        struct {
            std::array<uint32_t, 4> operand_ids;
            uint8_t operation_sequence[16];
            uint8_t operand_count;
            bool simd_compatible;
        } math_expr;
        
        // Array access optimization
        struct {
            uint32_t array_id;
            uint32_t index_id;
            bool bounds_check_eliminated;
            bool index_is_constant;
            int32_t constant_index;
        } array_access;
        
        // Function call optimization
        struct {
            uint32_t function_id;
            std::array<uint32_t, 6> arg_ids;
            uint8_t arg_count;
            bool inline_candidate;
            uint32_t call_site_id;
        } function_call;
        
        // String concatenation optimization
        struct {
            std::array<uint32_t, 8> string_ids;
            uint8_t string_count;
            uint32_t estimated_length;
            bool use_string_builder;
        } string_concat;
    } data;
    
    // Performance metrics
    uint32_t execution_count;
    uint64_t total_execution_time;
    double average_execution_time;
};

class SpecializedNodeProcessor {
private:
    std::vector<SpecializedNode> specialized_nodes_;
    OptimizedAST* ast_context_;
    
    // Performance monitoring
    uint64_t total_specialized_evaluations_;
    uint64_t total_time_saved_;
    
public:
    SpecializedNodeProcessor(OptimizedAST* ast);
    
    // Node creation and optimization
    uint32_t create_fast_for_loop(uint32_t init, uint32_t condition, 
                                 uint32_t increment, uint32_t body);
    uint32_t create_fast_property_chain(uint32_t object, 
                                       const std::vector<std::string>& properties);
    uint32_t create_fast_math_expression(const std::vector<uint32_t>& operands,
                                        const std::vector<uint8_t>& operations);
    uint32_t create_fast_array_access(uint32_t array, uint32_t index);
    uint32_t create_fast_function_call(uint32_t function, 
                                      const std::vector<uint32_t>& args);
    uint32_t create_fast_string_concat(const std::vector<uint32_t>& strings);
    
    // Ultra-fast evaluation methods
    Value evaluate_fast_for_loop(const SpecializedNode& node, Context& ctx);
    Value evaluate_fast_property_chain(const SpecializedNode& node, Context& ctx);
    Value evaluate_fast_math_expression(const SpecializedNode& node, Context& ctx);
    Value evaluate_fast_array_access(const SpecializedNode& node, Context& ctx);
    Value evaluate_fast_function_call(const SpecializedNode& node, Context& ctx);
    Value evaluate_fast_string_concat(const SpecializedNode& node, Context& ctx);
    
    // Main evaluation dispatcher
    Value evaluate_specialized(uint32_t node_id, Context& ctx);
    
    // Optimization analysis
    bool should_specialize(const OptimizedAST::OptimizedNode& node);
    SpecializedNodeType detect_specialization_type(const OptimizedAST::OptimizedNode& node);
    void optimize_hot_nodes();
    
    // Performance tracking
    double get_performance_gain() const;
    uint64_t get_time_saved() const { return total_time_saved_; }
    void print_specialization_stats() const;
    
    // Memory management
    void clear_specialized_nodes();
    size_t get_memory_usage() const;
};

// Pattern-specific optimizers
class FastLoopOptimizer {
public:
    static bool can_unroll_loop(const SpecializedNode& node);
    static Value execute_unrolled_loop(const SpecializedNode& node, Context& ctx);
    static void vectorize_loop_body(const SpecializedNode& node);
};

class PropertyChainOptimizer {
private:
    struct CachedPropertyAccess {
        uint64_t object_shape_id;
        std::array<uint32_t, 8> property_offsets;
        uint8_t chain_length;
        bool valid;
    };
    
    std::vector<CachedPropertyAccess> property_cache_;
    
public:
    Value execute_optimized_chain(const SpecializedNode& node, Context& ctx);
    void cache_property_offsets(uint32_t node_id, Object* obj, 
                               const std::vector<std::string>& properties);
    bool has_cached_access(uint32_t node_id) const;
};

class MathExpressionOptimizer {
public:
    static bool can_use_simd(const SpecializedNode& node);
    static Value execute_simd_math(const SpecializedNode& node, Context& ctx);
    static void optimize_constant_folding(SpecializedNode& node);
};

} // namespace Quanta

#endif // QUANTA_SPECIALIZED_NODES_H