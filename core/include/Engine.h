/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ENGINE_H
#define QUANTA_ENGINE_H

#include "Value.h"
#include "Object.h"
#include "Context.h"
#include "ModuleLoader.h"
#include "JIT.h"
#include "GC.h"
#include "InlineCache.h"
#include "ZeroLeakOptimizer.h"
#include "ArrayOptimizer.h"
#include "OptimizedAST.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <functional>

namespace Quanta {

// Forward declarations
class WebAPIInterface;
class ASTNode;

/**
 * Main JavaScript engine interface
 * High-performance, production-ready JavaScript execution engine
 */
class Engine {
public:
    // Engine configuration
    struct Config {
        bool strict_mode = false;
        bool enable_jit = true;
        bool enable_optimizations = true;
        size_t max_heap_size = 512 * 1024 * 1024;  // 512MB
        size_t initial_heap_size = 32 * 1024 * 1024; // 32MB
        size_t max_stack_size = 8 * 1024 * 1024;    // 8MB
        bool enable_debugger = false;
        bool enable_profiler = false;
    };

    // Execution result
    struct Result {
        Value value;
        bool success;
        std::string error_message;
        uint32_t line_number;
        uint32_t column_number;
        
        Result() : success(false), line_number(0), column_number(0) {}
        Result(const Value& v) : value(v), success(true), line_number(0), column_number(0) {}
        Result(const std::string& error, uint32_t line = 0, uint32_t col = 0) 
            : success(false), error_message(error), line_number(line), column_number(col) {}
    };

private:
    Config config_;
    std::unique_ptr<Context> global_context_;
    std::unique_ptr<ModuleLoader> module_loader_;
    
    // Advanced performance systems
    std::unique_ptr<JITCompiler> jit_compiler_;
    std::unique_ptr<GarbageCollector> garbage_collector_;
    std::unique_ptr<PerformanceCache> performance_cache_;
    std::unique_ptr<ZeroLeakOptimizer> zero_leak_optimizer_;
    std::unique_ptr<ArrayOptimizer> array_optimizer_;
    std::unique_ptr<OptimizedAST> optimized_ast_;
    std::unique_ptr<FastASTEvaluator> ast_evaluator_;
    
    // Engine state
    bool initialized_;
    uint64_t execution_count_;
    
    // ES6 default export registry for direct file execution
    std::unordered_map<std::string, Value> default_exports_registry_;
    
    // Performance tracking
    std::chrono::high_resolution_clock::time_point start_time_;
    size_t total_allocations_;
    size_t total_gc_runs_;

public:
    // Constructor and destructor
    Engine();
    explicit Engine(const Config& config);
    ~Engine();

    // Engine lifecycle
    bool initialize();
    void shutdown();
    bool is_initialized() const { return initialized_; }

    // Script execution
    Result execute(const std::string& source);
    Result execute(const std::string& source, const std::string& filename);
    Result execute_file(const std::string& filename);
    
    // Expression evaluation
    Result evaluate(const std::string& expression);
    
    // Module loading
    Result load_module(const std::string& module_path);
    Result import_module(const std::string& module_name);
    
    // Global object manipulation
    void set_global_property(const std::string& name, const Value& value);
    Value get_global_property(const std::string& name);
    bool has_global_property(const std::string& name);
    
    // Function registration
    void register_function(const std::string& name, std::function<Value(const std::vector<Value>&)> func);
    void register_object(const std::string& name, Object* object);
    
    // Context management
    Context* get_global_context() const { return global_context_.get(); }
    Context* get_current_context() const;
    
    // Web API interface management
    void set_web_api_interface(WebAPIInterface* interface);
    WebAPIInterface* get_web_api_interface() const;
    
    // Memory management
    void collect_garbage();
    size_t get_heap_usage() const;
    size_t get_heap_size() const;
    void set_heap_limit(size_t limit);
    
    // JIT Compilation
    void enable_jit(bool enable);
    bool is_jit_enabled() const;
    void set_jit_threshold(uint32_t threshold);
    std::string get_jit_stats() const;
    
    // JIT Compiler access
    class JITCompiler* get_jit_compiler() const { return jit_compiler_.get(); }
    
    // Garbage Collection
    void enable_gc(bool enable);
    void set_gc_mode(GarbageCollector::CollectionMode mode);
    void force_gc();
    std::string get_gc_stats() const;
    
    // Performance and debugging
    void enable_profiler(bool enable);
    void enable_debugger(bool enable);
    std::string get_performance_stats() const;
    std::string get_memory_stats() const;
    
    // Performance caching system
    PerformanceCache* get_performance_cache() const { return performance_cache_.get(); }
    void enable_performance_optimization(bool enable);
    void enable_maximum_performance_mode();
    
    // Zero-leak optimization system
    ZeroLeakOptimizer* get_zero_leak_optimizer() const { return zero_leak_optimizer_.get(); }
    void prepare_for_heavy_operations(ZeroLeakOptimizer::OperationType type, size_t scale);
    void enable_optimized_performance_mode();
    
    // Optimized array optimization system
    ArrayOptimizer* get_array_optimizer() const { return array_optimizer_.get(); }
    void enable_optimized_arrays();
    
    // Error handling
    void set_error_handler(std::function<void(const std::string&)> handler);
    bool has_pending_exception() const;
    Value get_pending_exception() const;
    void clear_pending_exception();
    
    // Configuration
    const Config& get_config() const { return config_; }
    void update_config(const Config& config);

    // Module system
    ModuleLoader* get_module_loader() { return module_loader_.get(); }
    
    // ES6 default export registry
    void register_default_export(const std::string& filename, const Value& value);
    Value get_default_export(const std::string& filename);
    bool has_default_export(const std::string& filename);

    // Browser integration helpers
    void inject_dom(Object* document);
    void setup_nodejs_apis();
    void setup_browser_globals();
    void register_web_apis();

private:
    // Internal initialization
    void setup_global_object();
    void setup_built_in_objects();
    void setup_built_in_functions();
    void setup_error_types();
    void setup_minimal_globals();  // Minimal global setup
    void setup_stellar_globals();  // High-performance global setup
    // void setup_es6_features(); // Removed due to segfault
    
    // Execution helpers
    Result execute_internal(const std::string& source, const std::string& filename);
    
    // HIGH PERFORMANCE optimization methods
    bool is_simple_mathematical_loop(ASTNode* ast);
    Result execute_optimized_mathematical_loop(ASTNode* ast);
    void handle_exception(const Value& exception);
    
    // Memory management helpers
    void initialize_gc();
    void schedule_gc_if_needed();
};

/**
 * Simplified function wrapper for native functions
 */
class NativeFunction {
public:
    using FunctionType = std::function<Value(Context&, const std::vector<Value>&)>;
    
private:
    FunctionType function_;
    std::string name_;
    size_t arity_;

public:
    NativeFunction(const std::string& name, FunctionType func, size_t arity = 0);
    
    Value call(Context& ctx, const std::vector<Value>& args);
    const std::string& get_name() const { return name_; }
    size_t get_arity() const { return arity_; }
};

/**
 * Engine factory for different configurations
 */
namespace EngineFactory {
    std::unique_ptr<Engine> create_browser_engine();
    std::unique_ptr<Engine> create_server_engine();
    std::unique_ptr<Engine> create_embedded_engine();
    std::unique_ptr<Engine> create_testing_engine();
}

} // namespace Quanta

#endif // QUANTA_ENGINE_H