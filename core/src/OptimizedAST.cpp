#include "../include/OptimizedAST.h"
#include "../include/Context.h"
#include "../include/Value.h"
#include "../../parser/include/AST.h"
#include <algorithm>

namespace Quanta {

OptimizedAST::OptimizedAST() : next_node_index_(0), current_timestamp_(0) {
    node_pool_.reserve(POOL_SIZE);
    string_table_.reserve(10000);
    evaluation_cache_.reserve(POOL_SIZE);
    cache_valid_.reserve(POOL_SIZE);
    cache_access_count_.reserve(POOL_SIZE);
    cache_timestamps_.reserve(POOL_SIZE);
}

uint32_t OptimizedAST::create_number_literal(double value) {
    if (next_node_index_ >= POOL_SIZE) {
        throw std::runtime_error("AST node pool exhausted");
    }
    
    uint32_t id = next_node_index_++;
    if (id >= node_pool_.size()) {
        node_pool_.resize(id + 1);
        evaluation_cache_.resize(id + 1);
        cache_valid_.resize(id + 1, false);
        cache_access_count_.resize(id + 1, 0);
        cache_timestamps_.resize(id + 1, 0);
    }
    
    OptimizedNode& node = node_pool_[id];
    node.type = NodeType::NUMBER_LITERAL;
    node.flags = 0;
    node.child_count = 0;
    node.node_id = id;
    node.data.number_value = value;
    std::fill(node.children.begin(), node.children.end(), 0);
    
    return id;
}

uint32_t OptimizedAST::create_string_literal(const std::string& value) {
    if (next_node_index_ >= POOL_SIZE) {
        throw std::runtime_error("AST node pool exhausted");
    }
    
    uint32_t string_id = intern_string(value);
    uint32_t id = next_node_index_++;
    if (id >= node_pool_.size()) {
        node_pool_.resize(id + 1);
        evaluation_cache_.resize(id + 1);
        cache_valid_.resize(id + 1, false);
        cache_access_count_.resize(id + 1, 0);
        cache_timestamps_.resize(id + 1, 0);
    }
    
    OptimizedNode& node = node_pool_[id];
    node.type = NodeType::STRING_LITERAL;
    node.flags = 0;
    node.child_count = 0;
    node.node_id = id;
    node.data.string_id = string_id;
    std::fill(node.children.begin(), node.children.end(), 0);
    
    return id;
}

uint32_t OptimizedAST::create_boolean_literal(bool value) {
    if (next_node_index_ >= POOL_SIZE) {
        throw std::runtime_error("AST node pool exhausted");
    }
    
    uint32_t id = next_node_index_++;
    if (id >= node_pool_.size()) {
        node_pool_.resize(id + 1);
        evaluation_cache_.resize(id + 1);
        cache_valid_.resize(id + 1, false);
        cache_access_count_.resize(id + 1, 0);
        cache_timestamps_.resize(id + 1, 0);
    }
    
    OptimizedNode& node = node_pool_[id];
    node.type = NodeType::BOOLEAN_LITERAL;
    node.flags = 0;
    node.child_count = 0;
    node.node_id = id;
    node.data.boolean_value = value;
    std::fill(node.children.begin(), node.children.end(), 0);
    
    return id;
}

uint32_t OptimizedAST::create_identifier(const std::string& name) {
    if (next_node_index_ >= POOL_SIZE) {
        throw std::runtime_error("AST node pool exhausted");
    }
    
    uint32_t string_id = intern_string(name);
    uint32_t id = next_node_index_++;
    if (id >= node_pool_.size()) {
        node_pool_.resize(id + 1);
        evaluation_cache_.resize(id + 1);
        cache_valid_.resize(id + 1, false);
        cache_access_count_.resize(id + 1, 0);
        cache_timestamps_.resize(id + 1, 0);
    }
    
    OptimizedNode& node = node_pool_[id];
    node.type = NodeType::IDENTIFIER;
    node.flags = 0;
    node.child_count = 0;
    node.node_id = id;
    node.data.identifier_id = string_id;
    std::fill(node.children.begin(), node.children.end(), 0);
    
    return id;
}

uint32_t OptimizedAST::create_binary_expression(uint32_t left, uint32_t right, uint8_t op) {
    if (next_node_index_ >= POOL_SIZE) {
        throw std::runtime_error("AST node pool exhausted");
    }
    
    uint32_t id = next_node_index_++;
    if (id >= node_pool_.size()) {
        node_pool_.resize(id + 1);
        evaluation_cache_.resize(id + 1);
        cache_valid_.resize(id + 1, false);
        cache_access_count_.resize(id + 1, 0);
        cache_timestamps_.resize(id + 1, 0);
    }
    
    OptimizedNode& node = node_pool_[id];
    node.type = NodeType::BINARY_EXPRESSION;
    node.flags = 0;
    node.child_count = 2;
    node.node_id = id;
    node.data.binary_op.left_child = left;
    node.data.binary_op.right_child = right;
    node.data.binary_op.operator_type = op;
    node.children[0] = left;
    node.children[1] = right;
    node.children[2] = 0;
    node.children[3] = 0;
    
    return id;
}

uint32_t OptimizedAST::create_member_expression(uint32_t object, uint32_t property, bool computed) {
    if (next_node_index_ >= POOL_SIZE) {
        throw std::runtime_error("AST node pool exhausted");
    }
    
    uint32_t id = next_node_index_++;
    if (id >= node_pool_.size()) {
        node_pool_.resize(id + 1);
        evaluation_cache_.resize(id + 1);
        cache_valid_.resize(id + 1, false);
        cache_access_count_.resize(id + 1, 0);
        cache_timestamps_.resize(id + 1, 0);
    }
    
    OptimizedNode& node = node_pool_[id];
    node.type = NodeType::MEMBER_EXPRESSION;
    node.flags = 0;
    node.child_count = 2;
    node.node_id = id;
    node.data.member_access.object_child = object;
    node.data.member_access.property_child = property;
    node.data.member_access.computed = computed;
    node.children[0] = object;
    node.children[1] = property;
    node.children[2] = 0;
    node.children[3] = 0;
    
    return id;
}

uint32_t OptimizedAST::create_call_expression(uint32_t callee, const std::vector<uint32_t>& args) {
    if (next_node_index_ >= POOL_SIZE) {
        throw std::runtime_error("AST node pool exhausted");
    }
    
    uint32_t id = next_node_index_++;
    if (id >= node_pool_.size()) {
        node_pool_.resize(id + 1);
        evaluation_cache_.resize(id + 1);
        cache_valid_.resize(id + 1, false);
        cache_access_count_.resize(id + 1, 0);
        cache_timestamps_.resize(id + 1, 0);
    }
    
    OptimizedNode& node = node_pool_[id];
    node.type = NodeType::CALL_EXPRESSION;
    node.flags = 0;
    node.child_count = std::min(static_cast<uint16_t>(args.size() + 1), static_cast<uint16_t>(4));
    node.node_id = id;
    node.children[0] = callee;
    
    for (size_t i = 0; i < 3 && i < args.size(); ++i) {
        node.children[i + 1] = args[i];
    }
    
    for (size_t i = args.size() + 1; i < 4; ++i) {
        node.children[i] = 0;
    }
    
    return id;
}

Value OptimizedAST::evaluate_fast(uint32_t node_id, Context& ctx) {
    if (node_id >= node_pool_.size()) {
        return Value();
    }
    
    if (node_id < cache_valid_.size() && cache_valid_[node_id]) {
        // Update cache statistics
        cache_access_count_[node_id]++;
        cache_timestamps_[node_id] = ++current_timestamp_;
        return evaluation_cache_[node_id];
    }
    
    const OptimizedNode& node = node_pool_[node_id];
    Value result;
    
    switch (node.type) {
        case NodeType::NUMBER_LITERAL:
            result = Value(node.data.number_value);
            break;
            
        case NodeType::STRING_LITERAL:
            if (node.data.string_id < string_table_.size()) {
                result = Value(string_table_[node.data.string_id]);
            }
            break;
            
        case NodeType::BOOLEAN_LITERAL:
            result = Value(node.data.boolean_value);
            break;
            
        case NodeType::IDENTIFIER:
            if (node.data.identifier_id < string_table_.size()) {
                const std::string& name = string_table_[node.data.identifier_id];
                result = ctx.get_binding(name);
            }
            break;
            
        case NodeType::BINARY_EXPRESSION: {
            Value left = evaluate_fast(node.data.binary_op.left_child, ctx);
            Value right = evaluate_fast(node.data.binary_op.right_child, ctx);
            
            switch (node.data.binary_op.operator_type) {
                case 0: // Addition
                    if (left.is_number() && right.is_number()) {
                        result = Value(left.to_number() + right.to_number());
                    } else {
                        result = Value(left.to_string() + right.to_string());
                    }
                    break;
                case 1: // Subtraction
                    result = Value(left.to_number() - right.to_number());
                    break;
                case 2: // Multiplication
                    result = Value(left.to_number() * right.to_number());
                    break;
                case 3: // Division
                    result = Value(left.to_number() / right.to_number());
                    break;
                case 4: // Equality
                    result = Value(left.strict_equals(right));
                    break;
                case 5: // Less than
                    result = Value(left.to_number() < right.to_number());
                    break;
                case 6: // Greater than
                    result = Value(left.to_number() > right.to_number());
                    break;
            }
            break;
        }
        
        case NodeType::MEMBER_EXPRESSION: {
            Value object = evaluate_fast(node.data.member_access.object_child, ctx);
            Value property = evaluate_fast(node.data.member_access.property_child, ctx);
            
            if (object.is_object()) {
                Object* obj = object.as_object();
                std::string prop_name = property.to_string();
                if (obj) {
                    result = obj->get_property(prop_name);
                }
            }
            break;
        }
        
        case NodeType::CALL_EXPRESSION: {
            Value callee = evaluate_fast(node.children[0], ctx);
            std::vector<Value> args;
            
            for (uint16_t i = 1; i < node.child_count && i < 4; ++i) {
                args.push_back(evaluate_fast(node.children[i], ctx));
            }
            
            if (callee.is_function()) {
                Function* func = callee.as_function();
                if (func) {
                    // Simplified function call - would need full implementation
                    result = Value();
                }
            }
            break;
        }
        
        default:
            result = Value();
            break;
    }
    
    if (node_id < cache_valid_.size()) {
        evaluation_cache_[node_id] = result;
        cache_valid_[node_id] = true;
        cache_access_count_[node_id] = 1;
        cache_timestamps_[node_id] = ++current_timestamp_;
    }
    
    return result;
}

void OptimizedAST::clear_cache() {
    std::fill(cache_valid_.begin(), cache_valid_.end(), false);
    std::fill(cache_access_count_.begin(), cache_access_count_.end(), 0);
    std::fill(cache_timestamps_.begin(), cache_timestamps_.end(), 0);
    current_timestamp_ = 0;
}

void OptimizedAST::evict_cold_cache_entries() {
    const uint32_t min_access_threshold = 5;
    const uint64_t stale_threshold = current_timestamp_ - 10000;
    
    for (size_t i = 0; i < cache_valid_.size(); ++i) {
        if (cache_valid_[i] && 
            (cache_access_count_[i] < min_access_threshold || 
             cache_timestamps_[i] < stale_threshold)) {
            cache_valid_[i] = false;
            cache_access_count_[i] = 0;
            cache_timestamps_[i] = 0;
        }
    }
}

void OptimizedAST::prefetch_hot_nodes(const std::vector<uint32_t>& nodes) {
    for (uint32_t node_id : nodes) {
        if (node_id < cache_access_count_.size()) {
            // Mark as hot for caching priority
            cache_access_count_[node_id] += 10;
            cache_timestamps_[node_id] = ++current_timestamp_;
        }
    }
}

double OptimizedAST::get_cache_hit_rate() const {
    uint32_t total_accesses = 0;
    uint32_t cache_hits = 0;
    
    for (size_t i = 0; i < cache_access_count_.size(); ++i) {
        total_accesses += cache_access_count_[i];
        if (cache_valid_[i] && cache_access_count_[i] > 1) {
            cache_hits += cache_access_count_[i] - 1;
        }
    }
    
    return total_accesses > 0 ? static_cast<double>(cache_hits) / total_accesses : 0.0;
}

void OptimizedAST::precompute_constants() {
    for (uint32_t i = 0; i < next_node_index_; ++i) {
        const OptimizedNode& node = node_pool_[i];
        
        if (node.type == NodeType::NUMBER_LITERAL ||
            node.type == NodeType::STRING_LITERAL ||
            node.type == NodeType::BOOLEAN_LITERAL) {
            
            if (i < cache_valid_.size()) {
                cache_valid_[i] = true;
                
                switch (node.type) {
                    case NodeType::NUMBER_LITERAL:
                        evaluation_cache_[i] = Value(node.data.number_value);
                        break;
                    case NodeType::STRING_LITERAL:
                        if (node.data.string_id < string_table_.size()) {
                            evaluation_cache_[i] = Value(string_table_[node.data.string_id]);
                        }
                        break;
                    case NodeType::BOOLEAN_LITERAL:
                        evaluation_cache_[i] = Value(node.data.boolean_value);
                        break;
                }
            }
        }
    }
}

void OptimizedAST::reset_pool() {
    next_node_index_ = 0;
    clear_cache();
}

size_t OptimizedAST::get_memory_usage() const {
    return node_pool_.size() * sizeof(OptimizedNode) +
           string_table_.size() * sizeof(std::string) +
           evaluation_cache_.size() * sizeof(Value);
}

uint32_t OptimizedAST::intern_string(const std::string& str) {
    auto it = string_lookup_.find(str);
    if (it != string_lookup_.end()) {
        return it->second;
    }
    
    uint32_t id = static_cast<uint32_t>(string_table_.size());
    string_table_.push_back(str);
    string_lookup_[str] = id;
    return id;
}

Value FastASTEvaluator::evaluate_number_literal(const OptimizedAST::OptimizedNode& node) {
    return Value(node.data.number_value);
}

Value FastASTEvaluator::evaluate_string_literal(const OptimizedAST::OptimizedNode& node) {
    return Value(ast_->get_string(node.data.string_id));
}

Value FastASTEvaluator::evaluate_binary_expression(const OptimizedAST::OptimizedNode& node, Context& ctx) {
    Value left = evaluate(node.data.binary_op.left_child, ctx);
    Value right = evaluate(node.data.binary_op.right_child, ctx);
    
    switch (node.data.binary_op.operator_type) {
        case 0: // Addition
            if (left.is_number() && right.is_number()) {
                return Value(left.to_number() + right.to_number());
            } else {
                return Value(left.to_string() + right.to_string());
            }
        case 1: // Subtraction
            return Value(left.to_number() - right.to_number());
        case 2: // Multiplication
            return Value(left.to_number() * right.to_number());
        case 3: // Division
            return Value(left.to_number() / right.to_number());
        default:
            return Value();
    }
}

Value FastASTEvaluator::evaluate_member_expression(const OptimizedAST::OptimizedNode& node, Context& ctx) {
    Value object = evaluate(node.data.member_access.object_child, ctx);
    Value property = evaluate(node.data.member_access.property_child, ctx);
    
    if (object.is_object()) {
        Object* obj = object.as_object();
        std::string prop_name = property.to_string();
        if (obj) {
            return obj->get_property(prop_name);
        }
    }
    return Value();
}

Value FastASTEvaluator::evaluate(uint32_t node_id, Context& ctx) {
    return ast_->evaluate_fast(node_id, ctx);
}

void FastASTEvaluator::evaluate_batch(const std::vector<uint32_t>& nodes, 
                                     std::vector<Value>& results, Context& ctx) {
    results.clear();
    results.reserve(nodes.size());
    
    // Try SIMD optimization for compatible node types
    if (has_simd_support() && nodes.size() >= 4) {
        // Group nodes by type for SIMD processing
        std::vector<uint32_t> number_nodes, binary_nodes, other_nodes;
        
        for (uint32_t node_id : nodes) {
            if (node_id < ast_->get_node_count()) {
                const auto& node = ast_->get_node(node_id);
                switch (node.type) {
                    case OptimizedAST::NodeType::NUMBER_LITERAL:
                        number_nodes.push_back(node_id);
                        break;
                    case OptimizedAST::NodeType::BINARY_EXPRESSION:
                        binary_nodes.push_back(node_id);
                        break;
                    default:
                        other_nodes.push_back(node_id);
                        break;
                }
            }
        }
        
        // Process in SIMD batches
        std::vector<Value> number_results, binary_results, other_results;
        
        if (!number_nodes.empty()) {
            evaluate_number_batch_simd(number_nodes, number_results);
        }
        if (!binary_nodes.empty()) {
            evaluate_binary_batch_simd(binary_nodes, binary_results, ctx);
        }
        
        // Process remaining nodes normally
        other_results.reserve(other_nodes.size());
        for (uint32_t node_id : other_nodes) {
            other_results.push_back(evaluate(node_id, ctx));
        }
        
        // Merge results in original order
        size_t num_idx = 0, bin_idx = 0, other_idx = 0;
        for (uint32_t node_id : nodes) {
            if (node_id < ast_->get_node_count()) {
                const auto& node = ast_->get_node(node_id);
                switch (node.type) {
                    case OptimizedAST::NodeType::NUMBER_LITERAL:
                        results.push_back(number_results[num_idx++]);
                        break;
                    case OptimizedAST::NodeType::BINARY_EXPRESSION:
                        results.push_back(binary_results[bin_idx++]);
                        break;
                    default:
                        results.push_back(other_results[other_idx++]);
                        break;
                }
            }
        }
    } else {
        // Fallback to sequential processing
        for (uint32_t node_id : nodes) {
            results.push_back(evaluate(node_id, ctx));
        }
    }
}

void FastASTEvaluator::evaluate_number_batch_simd(const std::vector<uint32_t>& nodes,
                                                  std::vector<Value>& results) {
    results.clear();
    results.reserve(nodes.size());
    
    // Process 4 numbers at a time using SIMD (conceptual - would use intrinsics)
    for (size_t i = 0; i < nodes.size(); i += 4) {
        size_t batch_size = std::min(size_t(4), nodes.size() - i);
        
        // Load 4 double values into SIMD register (conceptual)
        for (size_t j = 0; j < batch_size; ++j) {
            uint32_t node_id = nodes[i + j];
            if (node_id < ast_->get_node_count()) {
                const auto& node = ast_->get_node(node_id);
                results.push_back(Value(node.data.number_value));
            } else {
                results.push_back(Value());
            }
        }
    }
}

void FastASTEvaluator::evaluate_binary_batch_simd(const std::vector<uint32_t>& nodes,
                                                  std::vector<Value>& results, Context& ctx) {
    results.clear();
    results.reserve(nodes.size());
    
    // Process binary operations in batches
    for (uint32_t node_id : nodes) {
        if (node_id < ast_->get_node_count()) {
            const auto& node = ast_->get_node(node_id);
            if (node.type == OptimizedAST::NodeType::BINARY_EXPRESSION) {
                // Use existing binary expression evaluation logic
                Value left = evaluate(node.data.binary_op.left_child, ctx);
                Value right = evaluate(node.data.binary_op.right_child, ctx);
                
                Value result;
                switch (node.data.binary_op.operator_type) {
                    case 0: // Addition - could be SIMD optimized
                        if (left.is_number() && right.is_number()) {
                            result = Value(left.to_number() + right.to_number());
                        } else {
                            result = Value(left.to_string() + right.to_string());
                        }
                        break;
                    case 1: // Subtraction
                        result = Value(left.to_number() - right.to_number());
                        break;
                    case 2: // Multiplication
                        result = Value(left.to_number() * right.to_number());
                        break;
                    case 3: // Division
                        result = Value(left.to_number() / right.to_number());
                        break;
                    default:
                        result = Value();
                        break;
                }
                results.push_back(result);
            }
        } else {
            results.push_back(Value());
        }
    }
}

bool FastASTEvaluator::has_simd_support() const {
    // Check for AVX/SSE support (simplified)
    #if defined(__AVX2__) || defined(__AVX__) || defined(__SSE4_2__)
    return true;
    #else
    return false;
    #endif
}

std::unique_ptr<OptimizedAST> ASTOptimizer::optimize_ast(const ASTNode* root) {
    auto optimized = std::make_unique<OptimizedAST>();
    
    if (root) {
        convert_node(root, *optimized);
    }
    
    optimized->precompute_constants();
    
    return optimized;
}

uint32_t ASTOptimizer::convert_node(const ASTNode* node, OptimizedAST& optimized) {
    if (!node) {
        return 0;
    }
    
    // Simplified conversion - create placeholder nodes for now
    // Full implementation would require matching exact ASTNode interface
    return optimized.create_number_literal(0.0);
}

FastASTBuilder::FastASTBuilder() {
    node_stack_.reserve(100);
}

void FastASTBuilder::push_number(double value) {
    uint32_t node_id = ast_.create_number_literal(value);
    node_stack_.push_back(node_id);
}

void FastASTBuilder::push_string(const std::string& value) {
    uint32_t node_id = ast_.create_string_literal(value);
    node_stack_.push_back(node_id);
}

void FastASTBuilder::push_identifier(const std::string& name) {
    uint32_t node_id = ast_.create_identifier(name);
    node_stack_.push_back(node_id);
}

void FastASTBuilder::create_binary_op(uint8_t op) {
    if (node_stack_.size() < 2) {
        throw std::runtime_error("Not enough operands for binary operation");
    }
    
    uint32_t right = node_stack_.back();
    node_stack_.pop_back();
    uint32_t left = node_stack_.back();
    node_stack_.pop_back();
    
    uint32_t result = ast_.create_binary_expression(left, right, op);
    node_stack_.push_back(result);
}

void FastASTBuilder::create_member_access(bool computed) {
    if (node_stack_.size() < 2) {
        throw std::runtime_error("Not enough operands for member access");
    }
    
    uint32_t property = node_stack_.back();
    node_stack_.pop_back();
    uint32_t object = node_stack_.back();
    node_stack_.pop_back();
    
    uint32_t result = ast_.create_member_expression(object, property, computed);
    node_stack_.push_back(result);
}

void FastASTBuilder::create_function_call(size_t arg_count) {
    if (node_stack_.size() < arg_count + 1) {
        throw std::runtime_error("Not enough operands for function call");
    }
    
    std::vector<uint32_t> args;
    args.reserve(arg_count);
    
    for (size_t i = 0; i < arg_count; ++i) {
        args.insert(args.begin(), node_stack_.back());
        node_stack_.pop_back();
    }
    
    uint32_t callee = node_stack_.back();
    node_stack_.pop_back();
    
    uint32_t result = ast_.create_call_expression(callee, args);
    node_stack_.push_back(result);
}

std::unique_ptr<OptimizedAST> FastASTBuilder::build() {
    auto result = std::make_unique<OptimizedAST>(std::move(ast_));
    result->precompute_constants();
    return result;
}

uint32_t FastASTBuilder::get_root() const {
    if (node_stack_.empty()) {
        return 0;
    }
    return node_stack_.back();
}

} // namespace Quanta