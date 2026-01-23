/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ENGINE_H
#define QUANTA_ENGINE_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/modules/ModuleLoader.h"
#include "quanta/core/gc/GC.h"
#include "quanta/parser/AST.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <functional>

namespace Quanta {

class ASTNode;

/**
 * Main JavaScript engine interface
 * High-performance, production-ready JavaScript execution engine
 */
class Engine {
public:
    struct Config {
        bool strict_mode = false;
        bool enable_optimizations = true;
        size_t max_heap_size = 512 * 1024 * 1024;
        size_t initial_heap_size = 32 * 1024 * 1024;
        size_t max_stack_size = 8 * 1024 * 1024;
        bool enable_debugger = false;
        bool enable_profiler = false;
    };

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

    std::unique_ptr<GarbageCollector> garbage_collector_;

    bool initialized_;
    uint64_t execution_count_;
    
    std::unordered_map<std::string, Value> default_exports_registry_;
    
    std::chrono::high_resolution_clock::time_point start_time_;
    size_t total_allocations_;
    size_t total_gc_runs_;

public:
    Engine();
    explicit Engine(const Config& config);
    ~Engine();

    bool initialize();
    void shutdown();
    bool is_initialized() const { return initialized_; }

    Result execute(const std::string& source);
    Result execute(const std::string& source, const std::string& filename);
    Result execute_file(const std::string& filename);
    
    Result evaluate(const std::string& expression);
    
    Result load_module(const std::string& module_path);
    Result import_module(const std::string& module_name);
    
    void set_global_property(const std::string& name, const Value& value);
    Value get_global_property(const std::string& name);
    bool has_global_property(const std::string& name);
    
    void register_function(const std::string& name, std::function<Value(const std::vector<Value>&)> func);
    void register_object(const std::string& name, Object* object);
    
    Context* get_global_context() const { return global_context_.get(); }
    Context* get_current_context() const;
    
    void collect_garbage();
    size_t get_heap_usage() const;
    size_t get_heap_size() const;
    void set_heap_limit(size_t limit);
    
    void enable_gc(bool enable);
    void set_gc_mode(GarbageCollector::CollectionMode mode);
    void force_gc();
    std::string get_gc_stats() const;
    
    class GarbageCollector* get_garbage_collector() const { return garbage_collector_.get(); }
    
    void enable_profiler(bool enable);
    void enable_debugger(bool enable);
    std::string get_performance_stats() const;
    std::string get_memory_stats() const;
    
    
    void set_error_handler(std::function<void(const std::string&)> handler);
    bool has_pending_exception() const;
    Value get_pending_exception() const;
    void clear_pending_exception();
    
    const Config& get_config() const { return config_; }
    void update_config(const Config& config);

    ModuleLoader* get_module_loader() { return module_loader_.get(); }
    
    void register_default_export(const std::string& filename, const Value& value);
    Value get_default_export(const std::string& filename);
    bool has_default_export(const std::string& filename);

private:
    void setup_global_object();
    void setup_built_in_objects();
    void setup_built_in_functions();
    void setup_error_types();
    void setup_minimal_globals();
    
    Result execute_internal(const std::string& source, const std::string& filename);
    
    void handle_exception(const Value& exception);
    
    bool is_simple_mathematical_loop(ASTNode* ast);
    Result execute_optimized_mathematical_loop(ASTNode* ast);
    
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

}

#endif
