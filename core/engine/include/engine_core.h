/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include "../../include/Context.h"
#include <memory>
#include <chrono>
#include <string>

namespace Quanta {

class GarbageCollector;
class ModuleLoader;
class Context;

/**
 * Engine Configuration
 */
struct EngineConfig {
    bool strict_mode = false;
    bool enable_jit = true;
    bool enable_optimizations = true;
    size_t max_heap_size = 512 * 1024 * 1024;
    size_t initial_heap_size = 16 * 1024 * 1024;

    // Performance settings
    bool enable_fast_property_access = true;
    bool enable_inline_caching = true;
    bool enable_shape_optimization = true;

    // Debug settings
    bool enable_debug_mode = false;
    bool enable_profiling = false;
    bool enable_nodejs_compatibility = false;

    EngineConfig() = default;
};

/**
 * Core Engine Initialization and Configuration Management
 */
class EngineCore {
public:
    EngineCore();
    explicit EngineCore(const EngineConfig& config);
    ~EngineCore();

    // Initialization and shutdown
    bool initialize();
    void shutdown();

    bool is_initialized() const { return initialized_; }

    // Configuration
    const EngineConfig& get_config() const { return config_; }
    void set_config(const EngineConfig& config) { config_ = config; }

    // Context management
    Context* get_global_context() const { return global_context_.get(); }
    Context* create_new_context();

    // Resource management
    GarbageCollector* get_garbage_collector() const { return garbage_collector_.get(); }
    ModuleLoader* get_module_loader() const { return module_loader_.get(); }

    // Statistics
    uint64_t get_execution_count() const { return execution_count_; }
    uint64_t get_total_allocations() const { return total_allocations_; }
    uint64_t get_total_gc_runs() const { return total_gc_runs_; }

    std::chrono::high_resolution_clock::time_point get_start_time() const {
        return start_time_;
    }

    // Global properties management
    void set_global_property(const std::string& name, const Value& value);
    Value get_global_property(const std::string& name) const;
    bool has_global_property(const std::string& name) const;

    // Built-in objects initialization
    void initialize_builtin_objects();
    void initialize_global_object();
    void initialize_math_object();
    void initialize_date_object();
    void initialize_json_object();
    void initialize_console_object();
    void initialize_nodejs_objects();

protected:
    // Core state
    bool initialized_;
    EngineConfig config_;

    // Core components
    std::unique_ptr<Context> global_context_;
    std::unique_ptr<GarbageCollector> garbage_collector_;
    std::unique_ptr<ModuleLoader> module_loader_;

    // Statistics
    uint64_t execution_count_;
    uint64_t total_allocations_;
    uint64_t total_gc_runs_;

    std::chrono::high_resolution_clock::time_point start_time_;

    // Internal initialization helpers
    bool initialize_memory_system();
    bool initialize_context_system();
    bool initialize_module_system();
    void cleanup_resources();
};

/**
 * Engine Core Factory Functions
 */
namespace EngineCoreFactory {
    std::unique_ptr<EngineCore> create_engine_core();
    std::unique_ptr<EngineCore> create_engine_core(const EngineConfig& config);
    EngineCore* create_engine_core_raw();
    EngineCore* create_engine_core_raw(const EngineConfig& config);
}

} // namespace Quanta