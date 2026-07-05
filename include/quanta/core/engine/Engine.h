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
#include "quanta/core/gc/Heap.h"
#include "quanta/parser/AST.h"
#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <functional>

namespace Quanta {

class ASTNode;

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
        Value exception_value; 
        bool success;
        std::string error_message;
        uint32_t line_number;
        uint32_t column_number;

        Result() : success(false), line_number(0), column_number(0) {}
        Result(const Value& v) : value(v), success(true), line_number(0), column_number(0) {}
        Result(const std::string& error, uint32_t line = 0, uint32_t col = 0)
            : success(false), error_message(error), line_number(line), column_number(col) {}
        Result(const std::string& error, const Value& exc, uint32_t line = 0, uint32_t col = 0)
            : exception_value(exc), success(false), error_message(error), line_number(line), column_number(col) {}
    };

private:
    Config config_;
    // Intentionally immortal for now (like realm engines): static
    // destructors delete cells after the engine dies, so the heap and its
    // metadata must stay valid until process exit. The collector's shutdown
    // protocol will make heaps destructible.
    Heap* heap_;
    std::unique_ptr<Context> global_context_;
    std::unique_ptr<ModuleLoader> module_loader_;

    bool initialized_;
    uint64_t execution_count_;

    // Survivor pool: function contexts kept alive until after microtask drain,
    // so Promise callbacks can use context_ (creation context) for closure lookups
    std::vector<Context*> survivor_contexts_;
    
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
    
    Result evaluate(const std::string& expression, bool strict_mode = false);
    
    Result load_module(const std::string& module_path);
    Result import_module(const std::string& module_name);
    
    void set_global_property(const std::string& name, const Value& value);
    Value get_global_property(const std::string& name);
    bool has_global_property(const std::string& name);
    
    void register_function(const std::string& name, std::function<Value(const std::vector<Value>&)> func);
    void register_object(const std::string& name, Object* object);
    
    Context* get_global_context() const { return global_context_.get(); }
    const std::vector<Context*>& get_survivor_contexts() const { return survivor_contexts_; }
    // Every live engine on this thread, for GC root enumeration (a
    // collection only ever scans the calling thread's own engines/heaps).
    static const std::vector<Engine*>& all_engines();
    Context* get_current_context() const;

    // Survivor pool for function contexts (Promise async support)
    void add_survivor_context(Context* ctx);
    void clear_survivor_contexts();

    // Drains microtasks, then drives any pending timers to exhaustion (real wall-clock wait), repeating until both queues are empty.
    void run_event_loop_to_completion(Context& ctx);
    
    size_t get_heap_usage() const;
    size_t get_heap_size() const;
    void force_gc();
    Heap* get_heap() const { return heap_; }
    
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
