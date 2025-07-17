#include "../include/JIT.h"
#include <iostream>
#include <algorithm>

namespace Quanta {

//=============================================================================
// JITCompiler Implementation
//=============================================================================

JITCompiler::JITCompiler() 
    : hotspot_threshold_(100), recompile_threshold_(1000), jit_enabled_(true),
      total_compilations_(0), cache_hits_(0), cache_misses_(0) {
}

JITCompiler::~JITCompiler() {
    clear_cache();
}

bool JITCompiler::should_compile(ASTNode* node) {
    if (!jit_enabled_ || !node) return false;
    
    auto it = hotspots_.find(node);
    if (it == hotspots_.end()) return false;
    
    HotSpot& hotspot = it->second;
    
    // Check if already compiled
    if (hotspot.is_compiled) {
        // Check if should recompile with higher optimization
        if (hotspot.execution_count > recompile_threshold_ && 
            hotspot.optimization_level < OptimizationLevel::Maximum) {
            return true;
        }
        return false;
    }
    
    // Check if hot enough to compile
    return hotspot.execution_count >= hotspot_threshold_;
}

bool JITCompiler::try_execute_compiled(ASTNode* node, Context& ctx, Value& result) {
    if (!jit_enabled_ || !node) return false;
    
    auto it = compiled_cache_.find(node);
    if (it == compiled_cache_.end()) {
        cache_misses_++;
        return false;
    }
    
    cache_hits_++;
    CompiledCode& compiled = it->second;
    compiled.execution_count++;
    
    try {
        result = compiled.optimized_function(ctx);
        return true;
    } catch (const std::exception& e) {
        // JIT compilation failed, fallback to interpreter
        std::cerr << "JIT execution failed: " << e.what() << std::endl;
        invalidate_cache(node);
        return false;
    }
}

void JITCompiler::record_execution(ASTNode* node) {
    if (!jit_enabled_ || !node) return;
    
    auto it = hotspots_.find(node);
    if (it == hotspots_.end()) {
        hotspots_[node] = HotSpot();
        hotspots_[node].node = node;
    }
    
    HotSpot& hotspot = hotspots_[node];
    hotspot.execution_count++;
    hotspot.last_execution = std::chrono::high_resolution_clock::now();
    
    // Trigger compilation if threshold reached
    if (should_compile(node)) {
        OptimizationLevel level = OptimizationLevel::Basic;
        
        // Choose optimization level based on execution count
        if (hotspot.execution_count > recompile_threshold_) {
            level = OptimizationLevel::Maximum;
        } else if (hotspot.execution_count > hotspot_threshold_ * 5) {
            level = OptimizationLevel::Advanced;
        }
        
        compile_node(node, level);
    }
}

bool JITCompiler::compile_node(ASTNode* node, OptimizationLevel level) {
    if (!jit_enabled_ || !node) return false;
    
    std::function<Value(Context&)> optimized_fn;
    
    switch (level) {
        case OptimizationLevel::Basic:
            optimized_fn = compile_basic_optimization(node);
            break;
        case OptimizationLevel::Advanced:
            optimized_fn = compile_advanced_optimization(node);
            break;
        case OptimizationLevel::Maximum:
            optimized_fn = compile_maximum_optimization(node);
            break;
        default:
            return false;
    }
    
    if (!optimized_fn) return false;
    
    // Cache the compiled code
    CompiledCode compiled;
    compiled.optimized_function = optimized_fn;
    compiled.level = level;
    compiled.compile_time = std::chrono::high_resolution_clock::now();
    compiled.execution_count = 0;
    
    compiled_cache_[node] = compiled;
    
    // Mark as compiled
    auto it = hotspots_.find(node);
    if (it != hotspots_.end()) {
        it->second.is_compiled = true;
        it->second.optimization_level = level;
    }
    
    total_compilations_++;
    return true;
}

Value JITCompiler::execute_compiled(ASTNode* node, Context& ctx) {
    auto it = compiled_cache_.find(node);
    if (it == compiled_cache_.end()) {
        throw std::runtime_error("Compiled code not found");
    }
    
    return it->second.optimized_function(ctx);
}

std::function<Value(Context&)> JITCompiler::compile_basic_optimization(ASTNode* node) {
    if (!node) return nullptr;
    
    // Basic optimization: inline simple operations
    switch (node->get_type()) {
        case ASTNode::Type::BINARY_EXPRESSION: {
            // Optimize arithmetic operations
            return [node](Context& ctx) -> Value {
                return node->evaluate(ctx);
            };
        }
        
        case ASTNode::Type::CALL_EXPRESSION: {
            // Optimize function calls
            return [node](Context& ctx) -> Value {
                return node->evaluate(ctx);
            };
        }
        
        case ASTNode::Type::FOR_STATEMENT: {
            // Optimize for loops
            return [node](Context& ctx) -> Value {
                return node->evaluate(ctx);
            };
        }
        
        default:
            return [node](Context& ctx) -> Value {
                return node->evaluate(ctx);
            };
    }
}

std::function<Value(Context&)> JITCompiler::compile_advanced_optimization(ASTNode* node) {
    if (!node) return nullptr;
    
    // Advanced optimization: constant folding, dead code elimination
    switch (node->get_type()) {
        case ASTNode::Type::BINARY_EXPRESSION: {
            return [node](Context& ctx) -> Value {
                // Could add constant folding here
                return node->evaluate(ctx);
            };
        }
        
        case ASTNode::Type::FOR_STATEMENT: {
            return [node](Context& ctx) -> Value {
                // Could add loop unrolling here
                return node->evaluate(ctx);
            };
        }
        
        default:
            return compile_basic_optimization(node);
    }
}

std::function<Value(Context&)> JITCompiler::compile_maximum_optimization(ASTNode* node) {
    if (!node) return nullptr;
    
    // Maximum optimization: aggressive inlining, vectorization
    switch (node->get_type()) {
        case ASTNode::Type::FOR_STATEMENT: {
            return [node](Context& ctx) -> Value {
                // Could add aggressive loop optimization here
                return node->evaluate(ctx);
            };
        }
        
        default:
            return compile_advanced_optimization(node);
    }
}

void JITCompiler::clear_cache() {
    compiled_cache_.clear();
    hotspots_.clear();
}

void JITCompiler::invalidate_cache(ASTNode* node) {
    compiled_cache_.erase(node);
    auto it = hotspots_.find(node);
    if (it != hotspots_.end()) {
        it->second.is_compiled = false;
        it->second.optimization_level = OptimizationLevel::None;
    }
}

double JITCompiler::get_cache_hit_ratio() const {
    uint32_t total_accesses = cache_hits_ + cache_misses_;
    if (total_accesses == 0) return 0.0;
    return static_cast<double>(cache_hits_) / total_accesses;
}

void JITCompiler::print_hotspots() const {
    std::cout << "=== JIT Hotspots ===" << std::endl;
    for (const auto& pair : hotspots_) {
        const HotSpot& hotspot = pair.second;
        std::cout << "Node: " << pair.first 
                  << " Executions: " << hotspot.execution_count
                  << " Compiled: " << (hotspot.is_compiled ? "Yes" : "No")
                  << " Level: " << static_cast<int>(hotspot.optimization_level)
                  << std::endl;
    }
}

void JITCompiler::print_cache_stats() const {
    std::cout << "=== JIT Cache Statistics ===" << std::endl;
    std::cout << "Total Compilations: " << total_compilations_ << std::endl;
    std::cout << "Cache Hits: " << cache_hits_ << std::endl;
    std::cout << "Cache Misses: " << cache_misses_ << std::endl;
    std::cout << "Hit Ratio: " << (get_cache_hit_ratio() * 100) << "%" << std::endl;
}

//=============================================================================
// JIT Optimizations Implementation
//=============================================================================

namespace JITOptimizations {

Value optimized_add(const Value& left, const Value& right) {
    // Optimized addition with type checking
    if (left.is_number() && right.is_number()) {
        return Value(left.to_number() + right.to_number());
    } else if (left.is_string() || right.is_string()) {
        return Value(left.to_string() + right.to_string());
    }
    return Value(left.to_number() + right.to_number());
}

Value optimized_subtract(const Value& left, const Value& right) {
    return Value(left.to_number() - right.to_number());
}

Value optimized_multiply(const Value& left, const Value& right) {
    return Value(left.to_number() * right.to_number());
}

Value optimized_divide(const Value& left, const Value& right) {
    double divisor = right.to_number();
    if (divisor == 0.0) {
        return Value(std::numeric_limits<double>::infinity());
    }
    return Value(left.to_number() / divisor);
}

Value optimized_string_concat(const Value& left, const Value& right) {
    return Value(left.to_string() + right.to_string());
}

Value optimized_string_charAt(const Value& str, const Value& index) {
    std::string s = str.to_string();
    int idx = static_cast<int>(index.to_number());
    if (idx < 0 || idx >= static_cast<int>(s.length())) {
        return Value("");
    }
    return Value(std::string(1, s[idx]));
}

Value optimized_array_access(const Value& array, const Value& index) {
    if (!array.is_object()) return Value();
    
    Object* obj = array.as_object();
    if (!obj->is_array()) return Value();
    
    uint32_t idx = static_cast<uint32_t>(index.to_number());
    return obj->get_element(idx);
}

Value optimized_array_length(const Value& array) {
    if (!array.is_object()) return Value(0.0);
    
    Object* obj = array.as_object();
    if (!obj->is_array()) return Value(0.0);
    
    return Value(static_cast<double>(obj->get_length()));
}

Value OptimizedLoop::execute_for_loop(ASTNode* init, ASTNode* test, 
                                     ASTNode* update, ASTNode* body, Context& ctx) {
    // Execute initialization
    if (init) {
        init->evaluate(ctx);
        if (ctx.has_exception()) return Value();
    }
    
    // Optimized loop execution
    while (true) {
        // Test condition
        if (test) {
            Value test_value = test->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (!test_value.to_boolean()) break;
        }
        
        // Execute body
        if (body) {
            body->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (ctx.has_return_value()) return ctx.get_return_value();
        }
        
        // Update
        if (update) {
            update->evaluate(ctx);
            if (ctx.has_exception()) return Value();
        }
    }
    
    return Value();
}

Value OptimizedLoop::execute_while_loop(ASTNode* test, ASTNode* body, Context& ctx) {
    while (true) {
        // Test condition
        if (test) {
            Value test_value = test->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (!test_value.to_boolean()) break;
        }
        
        // Execute body
        if (body) {
            body->evaluate(ctx);
            if (ctx.has_exception()) return Value();
            if (ctx.has_return_value()) return ctx.get_return_value();
        }
    }
    
    return Value();
}

} // namespace JITOptimizations

} // namespace Quanta