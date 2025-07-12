#include "Engine.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <iostream>

namespace Quanta {

//=============================================================================
// Engine Implementation
//=============================================================================

Engine::Engine() : initialized_(false), execution_count_(0),
      total_allocations_(0), total_gc_runs_(0) {
    config_.strict_mode = false;
    config_.enable_jit = true;
    config_.enable_optimizations = true;
    config_.max_heap_size = 512 * 1024 * 1024;
    config_.initial_heap_size = 32 * 1024 * 1024;
    config_.max_stack_size = 8 * 1024 * 1024;
    config_.enable_debugger = false;
    config_.enable_profiler = false;
    start_time_ = std::chrono::high_resolution_clock::now();
}

Engine::Engine(const Config& config) 
    : config_(config), initialized_(false), execution_count_(0),
      total_allocations_(0), total_gc_runs_(0) {
    start_time_ = std::chrono::high_resolution_clock::now();
}

Engine::~Engine() {
    shutdown();
}

bool Engine::initialize() {
    if (initialized_) {
        return true;
    }
    
    try {
        // Create global context
        global_context_ = ContextFactory::create_global_context(this);
        
        // Setup global environment
        setup_global_object();
        setup_built_in_objects();
        setup_built_in_functions();
        setup_error_types();
        
        // Initialize garbage collector (placeholder)
        initialize_gc();
        
        initialized_ = true;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Engine initialization failed: " << e.what() << std::endl;
        return false;
    }
}

void Engine::shutdown() {
    if (!initialized_) {
        return;
    }
    
    // Clean up resources
    global_context_.reset();
    
    initialized_ = false;
}

Engine::Result Engine::execute(const std::string& source) {
    return execute(source, "<anonymous>");
}

Engine::Result Engine::execute(const std::string& source, const std::string& filename) {
    if (!initialized_) {
        return Result("Engine not initialized");
    }
    
    return execute_internal(source, filename);
}

Engine::Result Engine::execute_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return Result("Cannot open file: " + filename);
    }
    
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return execute(buffer.str(), filename);
}

Engine::Result Engine::evaluate(const std::string& expression) {
    if (!initialized_) {
        return Result("Engine not initialized");
    }
    
    // For Stage 1, we'll implement a simple expression evaluator
    // This is a placeholder that handles basic literals and operations
    
    try {
        // Remove whitespace
        std::string trimmed = expression;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
        
        if (trimmed.empty()) {
            return Result(Value());
        }
        
        // Handle basic literals
        if (trimmed == "undefined") {
            return Result(Value());
        } else if (trimmed == "null") {
            return Result(Value::null());
        } else if (trimmed == "true") {
            return Result(Value(true));
        } else if (trimmed == "false") {
            return Result(Value(false));
        } else if (trimmed[0] == '"' && trimmed.back() == '"') {
            // String literal
            std::string str = trimmed.substr(1, trimmed.length() - 2);
            return Result(Value(str));
        } else if (trimmed[0] == '\'' && trimmed.back() == '\'') {
            // String literal
            std::string str = trimmed.substr(1, trimmed.length() - 2);
            return Result(Value(str));
        } else {
            // Try to parse as number
            char* end;
            double num = std::strtod(trimmed.c_str(), &end);
            if (end == trimmed.c_str() + trimmed.length()) {
                return Result(Value(num));
            }
        }
        
        // Check global variables
        if (global_context_->has_binding(trimmed)) {
            Value value = global_context_->get_binding(trimmed);
            return Result(value);
        }
        
        return Result("ReferenceError: " + trimmed + " is not defined");
        
    } catch (const std::exception& e) {
        return Result("Error evaluating expression: " + std::string(e.what()));
    }
}

void Engine::set_global_property(const std::string& name, const Value& value) {
    if (initialized_ && global_context_) {
        global_context_->create_binding(name, value);
        
        Object* global_obj = global_context_->get_global_object();
        if (global_obj) {
            global_obj->set_property(name, value);
        }
    }
}

Value Engine::get_global_property(const std::string& name) {
    if (initialized_ && global_context_) {
        return global_context_->get_binding(name);
    }
    return Value();
}

bool Engine::has_global_property(const std::string& name) {
    if (initialized_ && global_context_) {
        return global_context_->has_binding(name);
    }
    return false;
}

void Engine::register_function(const std::string& name, std::function<Value(const std::vector<Value>&)> func) {
    if (!initialized_) return;
    
    // Create a native function wrapper (placeholder)
    auto native_func = ObjectFactory::create_function();
    
    // Store the function somehow - for now, just set as property
    set_global_property(name, Value(native_func.release()));
}

void Engine::register_object(const std::string& name, Object* object) {
    if (!initialized_) return;
    
    set_global_property(name, Value(object));
}

Context* Engine::get_current_context() const {
    return global_context_.get();
}

void Engine::collect_garbage() {
    // Placeholder for GC - will be implemented in later stages
    total_gc_runs_++;
}

size_t Engine::get_heap_usage() const {
    // Placeholder - return estimated usage
    return total_allocations_ * 64; // Rough estimate
}

size_t Engine::get_heap_size() const {
    return config_.max_heap_size;
}

bool Engine::has_pending_exception() const {
    return initialized_ && global_context_ && global_context_->has_exception();
}

Value Engine::get_pending_exception() const {
    if (has_pending_exception()) {
        return global_context_->get_exception();
    }
    return Value();
}

void Engine::clear_pending_exception() {
    if (initialized_ && global_context_) {
        global_context_->clear_exception();
    }
}

std::string Engine::get_performance_stats() const {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_);
    
    std::ostringstream oss;
    oss << "Performance Statistics:\n";
    oss << "  Uptime: " << duration.count() << "ms\n";
    oss << "  Executions: " << execution_count_ << "\n";
    oss << "  Heap Usage: " << get_heap_usage() << " bytes\n";
    oss << "  GC Runs: " << total_gc_runs_ << "\n";
    return oss.str();
}

std::string Engine::get_memory_stats() const {
    std::ostringstream oss;
    oss << "Memory Statistics:\n";
    oss << "  Heap Size: " << get_heap_size() << " bytes\n";
    oss << "  Heap Usage: " << get_heap_usage() << " bytes\n";
    oss << "  Total Allocations: " << total_allocations_ << "\n";
    return oss.str();
}

void Engine::inject_dom(Object* document) {
    if (!initialized_) return;
    
    // Inject DOM object into global scope
    set_global_property("document", Value(document));
    
    // Setup basic DOM globals
    setup_browser_globals();
}

void Engine::setup_browser_globals() {
    // Browser-specific globals (placeholder implementations)
    set_global_property("window", Value(global_context_->get_global_object()));
    set_global_property("console", Value(ObjectFactory::create_object().release()));
    set_global_property("setTimeout", Value(ObjectFactory::create_function().release()));
    set_global_property("setInterval", Value(ObjectFactory::create_function().release()));
}

void Engine::register_web_apis() {
    // Web API registration (placeholder)
    setup_browser_globals();
}

Engine::Result Engine::execute_internal(const std::string& source, const std::string& filename) {
    try {
        execution_count_++;
        
        // For Stage 1, we'll implement a very basic interpreter
        // This will be replaced with proper parsing and execution in later stages
        
        // Handle simple statements
        std::string trimmed = source;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
        
        // Remove trailing semicolon if present
        if (!trimmed.empty() && trimmed.back() == ';') {
            trimmed.pop_back();
        }
        
        if (trimmed.empty()) {
            return Result(Value());
        }
        
        // Check for variable declarations
        if (trimmed.substr(0, 3) == "var") {
            // Simple var declaration: var x = value;
            size_t eq_pos = trimmed.find('=');
            if (eq_pos != std::string::npos) {
                std::string var_part = trimmed.substr(3, eq_pos - 3);
                std::string value_part = trimmed.substr(eq_pos + 1);
                
                // Clean up variable name
                var_part.erase(0, var_part.find_first_not_of(" \t"));
                var_part.erase(var_part.find_last_not_of(" \t") + 1);
                
                // Evaluate the value
                Result value_result = evaluate(value_part);
                if (value_result.success) {
                    set_global_property(var_part, value_result.value);
                    return Result(Value()); // var declarations return undefined
                } else {
                    return value_result;
                }
            }
        }
        
        // Otherwise, treat as expression
        return evaluate(trimmed);
        
    } catch (const std::exception& e) {
        return Result("Runtime error: " + std::string(e.what()));
    }
}

void Engine::setup_global_object() {
    if (!global_context_) return;
    
    Object* global_obj = global_context_->get_global_object();
    if (!global_obj) return;
    
    // Set up global object properties
    global_obj->set_property("globalThis", Value(global_obj));
}

void Engine::setup_built_in_objects() {
    // Built-in objects are already set up in Context initialization
}

void Engine::setup_built_in_functions() {
    // Global functions
    register_function("parseInt", [](const std::vector<Value>& args) {
        if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
        std::string str = args[0].to_string();
        char* end;
        long result = std::strtol(str.c_str(), &end, 10);
        return Value(static_cast<double>(result));
    });
    
    register_function("parseFloat", [](const std::vector<Value>& args) {
        if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
        std::string str = args[0].to_string();
        char* end;
        double result = std::strtod(str.c_str(), &end);
        return Value(result);
    });
    
    register_function("isNaN", [](const std::vector<Value>& args) {
        if (args.empty()) return Value(true);
        double num = args[0].to_number();
        return Value(std::isnan(num));
    });
    
    register_function("isFinite", [](const std::vector<Value>& args) {
        if (args.empty()) return Value(false);
        double num = args[0].to_number();
        return Value(std::isfinite(num));
    });
}

void Engine::setup_error_types() {
    // Error constructors are already set up in Context
}

void Engine::initialize_gc() {
    // Placeholder for garbage collector initialization
}

//=============================================================================
// NativeFunction Implementation
//=============================================================================

NativeFunction::NativeFunction(const std::string& name, FunctionType func, size_t arity)
    : function_(func), name_(name), arity_(arity) {
}

Value NativeFunction::call(Context& ctx, const std::vector<Value>& args) {
    return function_(ctx, args);
}

//=============================================================================
// EngineFactory Implementation
//=============================================================================

namespace EngineFactory {

std::unique_ptr<Engine> create_browser_engine() {
    Engine::Config config;
    config.enable_jit = true;
    config.enable_optimizations = true;
    config.max_heap_size = 256 * 1024 * 1024; // 256MB for browser
    config.enable_debugger = true;
    
    auto engine = std::make_unique<Engine>(config);
    if (engine->initialize()) {
        engine->setup_browser_globals();
        engine->register_web_apis();
        return engine;
    }
    return nullptr;
}

std::unique_ptr<Engine> create_server_engine() {
    Engine::Config config;
    config.enable_jit = true;
    config.enable_optimizations = true;
    config.max_heap_size = 1024 * 1024 * 1024; // 1GB for server
    config.enable_profiler = true;
    
    auto engine = std::make_unique<Engine>(config);
    if (engine->initialize()) {
        return engine;
    }
    return nullptr;
}

std::unique_ptr<Engine> create_embedded_engine() {
    Engine::Config config;
    config.enable_jit = false; // Disable JIT for embedded
    config.enable_optimizations = false;
    config.max_heap_size = 32 * 1024 * 1024; // 32MB for embedded
    config.enable_debugger = false;
    config.enable_profiler = false;
    
    auto engine = std::make_unique<Engine>(config);
    if (engine->initialize()) {
        return engine;
    }
    return nullptr;
}

std::unique_ptr<Engine> create_testing_engine() {
    Engine::Config config;
    config.enable_jit = false;
    config.enable_optimizations = false;
    config.max_heap_size = 64 * 1024 * 1024; // 64MB for testing
    config.enable_debugger = true;
    config.enable_profiler = true;
    
    auto engine = std::make_unique<Engine>(config);
    if (engine->initialize()) {
        return engine;
    }
    return nullptr;
}

} // namespace EngineFactory

} // namespace Quanta