/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/engine_core.h"
#include "../../include/Context.h"
#include "../../include/Object.h"
#include "../../memory/include/garbage_collector.h"
#include <iostream>
#include <memory>

namespace Quanta {

// Forward declarations for placeholder implementations
class ModuleLoader {
public:
    explicit ModuleLoader(EngineCore* engine) : engine_(engine) {}
    ~ModuleLoader() = default;

private:
    EngineCore* engine_;
};

// Use the existing ContextFactory namespace from Context.h
// We'll create a simple wrapper to avoid Engine dependency

//=============================================================================
// EngineCore Implementation
//=============================================================================

EngineCore::EngineCore()
    : initialized_(false), execution_count_(0), total_allocations_(0), total_gc_runs_(0) {

    config_ = EngineConfig(); // Use default configuration
    start_time_ = std::chrono::high_resolution_clock::now();
}

EngineCore::EngineCore(const EngineConfig& config)
    : initialized_(false), config_(config), execution_count_(0),
      total_allocations_(0), total_gc_runs_(0) {

    start_time_ = std::chrono::high_resolution_clock::now();
}

EngineCore::~EngineCore() {
    shutdown();
}

bool EngineCore::initialize() {
    if (initialized_) {
        return true;
    }

    try {
        // Initialize memory system
        if (!initialize_memory_system()) {
            std::cerr << "Failed to initialize memory system" << std::endl;
            return false;
        }

        // Initialize context system
        if (!initialize_context_system()) {
            std::cerr << "Failed to initialize context system" << std::endl;
            return false;
        }

        // Initialize module system
        if (!initialize_module_system()) {
            std::cerr << "Failed to initialize module system" << std::endl;
            return false;
        }

        // Initialize built-in objects
        initialize_builtin_objects();

        initialized_ = true;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Engine initialization failed: " << e.what() << std::endl;
        cleanup_resources();
        return false;
    }
}

void EngineCore::shutdown() {
    if (!initialized_) {
        return;
    }

    try {
        cleanup_resources();
        initialized_ = false;
    } catch (const std::exception& e) {
        std::cerr << "Error during engine shutdown: " << e.what() << std::endl;
    }
}

Context* EngineCore::create_new_context() {
    if (!initialized_) {
        return nullptr;
    }

    // Create a new context - simplified for now
    // In real implementation, this would properly handle Engine dependency
    return nullptr; // Placeholder
}

void EngineCore::set_global_property(const std::string& name, const Value& value) {
    if (global_context_) {
        // In a real implementation, this would set properties on the global object
        // For now, this is a placeholder
    }
}

Value EngineCore::get_global_property(const std::string& name) const {
    if (global_context_) {
        // In a real implementation, this would get properties from the global object
        // For now, return undefined
        return Value();
    }
    return Value();
}

bool EngineCore::has_global_property(const std::string& name) const {
    if (global_context_) {
        // In a real implementation, this would check properties on the global object
        return false;
    }
    return false;
}

void EngineCore::initialize_builtin_objects() {
    try {
        initialize_global_object();
        initialize_math_object();
        initialize_date_object();
        initialize_json_object();
        initialize_console_object();
        initialize_nodejs_objects();
    } catch (const std::exception& e) {
        std::cerr << "Error initializing built-in objects: " << e.what() << std::endl;
    }
}

void EngineCore::initialize_global_object() {
    // Create global object with basic properties
    auto global_obj = std::make_unique<Object>();

    // Add basic global functions (simplified)
    set_global_property("undefined", Value());
    set_global_property("NaN", Value(std::numeric_limits<double>::quiet_NaN()));
    set_global_property("Infinity", Value(std::numeric_limits<double>::infinity()));
}

void EngineCore::initialize_math_object() {
    // Create Math object with mathematical constants and functions
    auto math_obj = std::make_unique<Object>();

    // Math constants
    math_obj->set_property("PI", Value(3.141592653589793));
    math_obj->set_property("E", Value(2.718281828459045));

    set_global_property("Math", Value(math_obj.release()));
}

void EngineCore::initialize_date_object() {
    // Create Date constructor
    // This would be a more complex implementation in practice
    auto date_constructor = std::make_unique<Object>(Object::ObjectType::Function);
    set_global_property("Date", Value(date_constructor.release()));
}

void EngineCore::initialize_json_object() {
    // Create JSON object with parse and stringify methods
    auto json_obj = std::make_unique<Object>();

    // JSON.parse and JSON.stringify would be native functions
    set_global_property("JSON", Value(json_obj.release()));
}

void EngineCore::initialize_console_object() {
    // Create console object for debugging
    auto console_obj = std::make_unique<Object>();

    // console.log, console.error, etc. would be native functions
    set_global_property("console", Value(console_obj.release()));
}

void EngineCore::initialize_nodejs_objects() {
    if (config_.enable_nodejs_compatibility) {
        // Create Node.js-specific objects like process, Buffer, etc.
        auto process_obj = std::make_unique<Object>();
        set_global_property("process", Value(process_obj.release()));
    }
}

// Private initialization methods
bool EngineCore::initialize_memory_system() {
    try {
        // Initialize garbage collector
        garbage_collector_ = std::make_unique<GarbageCollector>();

        // Configure GC based on engine configuration
        if (config_.enable_optimizations) {
            garbage_collector_->enable_ultra_fast_mode(true);
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Memory system initialization failed: " << e.what() << std::endl;
        return false;
    }
}

bool EngineCore::initialize_context_system() {
    try {
        // Create global context - simplified placeholder
        // global_context_ = ContextFactory::create_global_context(this);
        // For now, skip context creation to avoid Engine dependency
        // if (!global_context_) {
        //     return false;
        // }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Context system initialization failed: " << e.what() << std::endl;
        return false;
    }
}

bool EngineCore::initialize_module_system() {
    try {
        // Initialize module loader
        module_loader_ = std::make_unique<ModuleLoader>(this);

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Module system initialization failed: " << e.what() << std::endl;
        return false;
    }
}

void EngineCore::cleanup_resources() {
    try {
        // Clean up in reverse order of initialization
        module_loader_.reset();
        global_context_.reset();
        garbage_collector_.reset();
    } catch (const std::exception& e) {
        std::cerr << "Error cleaning up resources: " << e.what() << std::endl;
    }
}

//=============================================================================
// EngineCoreFactory Implementation
//=============================================================================

namespace EngineCoreFactory {

std::unique_ptr<EngineCore> create_engine_core() {
    return std::make_unique<EngineCore>();
}

std::unique_ptr<EngineCore> create_engine_core(const EngineConfig& config) {
    return std::make_unique<EngineCore>(config);
}

EngineCore* create_engine_core_raw() {
    auto engine = new EngineCore();
    if (engine->initialize()) {
        return engine;
    }
    delete engine;
    return nullptr;
}

EngineCore* create_engine_core_raw(const EngineConfig& config) {
    auto engine = new EngineCore(config);
    if (engine->initialize()) {
        return engine;
    }
    delete engine;
    return nullptr;
}

} // namespace EngineCoreFactory

} // namespace Quanta