#pragma once

#include "../../lexer/include/Token.h"
#include "Value.h"
#include <memory>
#include <vector>
#include <string>
#include <cstdint>
#include <array>
#include <unordered_map>

namespace Quanta {

class Context;

// High-performance AST implementation with memory pool allocation
class OptimizedAST {
public:
    // Optimized node types with cache-friendly memory layout
    enum class NodeType : uint8_t {
        NUMBER_LITERAL = 0,
        STRING_LITERAL = 1,
        BOOLEAN_LITERAL = 2,
        IDENTIFIER = 3,
        BINARY_EXPRESSION = 4,
        UNARY_EXPRESSION = 5,
        ASSIGNMENT_EXPRESSION = 6,
        CALL_EXPRESSION = 7,
        MEMBER_EXPRESSION = 8,
        OBJECT_LITERAL = 9,
        ARRAY_LITERAL = 10,
        VARIABLE_DECLARATION = 11,
        FUNCTION_DECLARATION = 12,
        IF_STATEMENT = 13,
        FOR_STATEMENT = 14,
        WHILE_STATEMENT = 15,
        BLOCK_STATEMENT = 16,
        EXPRESSION_STATEMENT = 17,
        RETURN_STATEMENT = 18,
        PROGRAM = 19
    };

    // Cache-line optimized AST node (64 bytes)
    struct alignas(64) OptimizedNode {
        NodeType type;
        uint8_t flags;
        uint16_t child_count;
        uint32_t node_id;
        
        // Inline data storage for small values
        union {
            double number_value;
            uint64_t string_id;
            bool boolean_value;
            uint32_t identifier_id;
            struct {
                uint32_t left_child;
                uint32_t right_child;
                uint8_t operator_type;
            } binary_op;
            struct {
                uint32_t object_child;
                uint32_t property_child;
                bool computed;
            } member_access;
        } data;
        
        // Child node indices (for tree traversal)
        std::array<uint32_t, 4> children;
        
        OptimizedNode() : type(NodeType::NUMBER_LITERAL), flags(0), child_count(0), 
                         node_id(0), children{0, 0, 0, 0} {
            data.number_value = 0.0;
        }
    };

private:
    // Memory pool for nodes
    static constexpr size_t POOL_SIZE = 100000;
    std::vector<OptimizedNode> node_pool_;
    size_t next_node_index_;
    
    // String interning for identifiers and literals
    std::vector<std::string> string_table_;
    std::unordered_map<std::string, uint32_t> string_lookup_;
    
    // Fast evaluation cache with LRU eviction
    std::vector<Value> evaluation_cache_;
    std::vector<bool> cache_valid_;
    std::vector<uint32_t> cache_access_count_;
    std::vector<uint64_t> cache_timestamps_;
    uint64_t current_timestamp_;

public:
    OptimizedAST();
    
    // Fast node creation
    uint32_t create_number_literal(double value);
    uint32_t create_string_literal(const std::string& value);
    uint32_t create_boolean_literal(bool value);
    uint32_t create_identifier(const std::string& name);
    uint32_t create_binary_expression(uint32_t left, uint32_t right, uint8_t op);
    uint32_t create_member_expression(uint32_t object, uint32_t property, bool computed);
    uint32_t create_call_expression(uint32_t callee, const std::vector<uint32_t>& args);
    
    // High-performance evaluation
    Value evaluate_fast(uint32_t node_id, Context& ctx);
    
    // Cache management
    void clear_cache();
    void precompute_constants();
    void evict_cold_cache_entries();
    void prefetch_hot_nodes(const std::vector<uint32_t>& nodes);
    double get_cache_hit_rate() const;
    
    // Memory management
    void reset_pool();
    size_t get_memory_usage() const;
    
    // Node access
    const OptimizedNode& get_node(uint32_t id) const { return node_pool_[id]; }
    OptimizedNode& get_node(uint32_t id) { return node_pool_[id]; }
    
    // String table operations
    uint32_t intern_string(const std::string& str);
    const std::string& get_string(uint32_t id) const { return string_table_[id]; }
    
    // Node count accessor
    size_t get_node_count() const { return next_node_index_; }
};

// AST expression evaluator with SIMD optimizations
class FastASTEvaluator {
private:
    OptimizedAST* ast_;
    
    // Specialized evaluation functions
    Value evaluate_number_literal(const OptimizedAST::OptimizedNode& node);
    Value evaluate_string_literal(const OptimizedAST::OptimizedNode& node);
    Value evaluate_binary_expression(const OptimizedAST::OptimizedNode& node, Context& ctx);
    Value evaluate_member_expression(const OptimizedAST::OptimizedNode& node, Context& ctx);
    
public:
    FastASTEvaluator(OptimizedAST* ast) : ast_(ast) {}
    
    Value evaluate(uint32_t node_id, Context& ctx);
    
    // Batch evaluation for SIMD optimization
    void evaluate_batch(const std::vector<uint32_t>& nodes, 
                       std::vector<Value>& results, Context& ctx);
                       
    // SIMD-optimized evaluation functions
    void evaluate_number_batch_simd(const std::vector<uint32_t>& nodes,
                                   std::vector<Value>& results);
    void evaluate_binary_batch_simd(const std::vector<uint32_t>& nodes,
                                   std::vector<Value>& results, Context& ctx);
    bool has_simd_support() const;
};

// AST compiler that converts traditional AST to optimized format
class ASTOptimizer {
public:
    static std::unique_ptr<OptimizedAST> optimize_ast(const class ASTNode* root);
    
private:
    static uint32_t convert_node(const class ASTNode* node, OptimizedAST& optimized);
};

// Memory-efficient AST builder
class FastASTBuilder {
private:
    OptimizedAST ast_;
    std::vector<uint32_t> node_stack_;
    
public:
    FastASTBuilder();
    
    // Fast construction methods
    void push_number(double value);
    void push_string(const std::string& value);
    void push_identifier(const std::string& name);
    void create_binary_op(uint8_t op);
    void create_member_access(bool computed);
    void create_function_call(size_t arg_count);
    
    // Finalize and get result
    std::unique_ptr<OptimizedAST> build();
    uint32_t get_root() const;
};

} // namespace Quanta