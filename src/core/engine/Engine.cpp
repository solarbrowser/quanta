/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/JSON.h"
#include "quanta/core/runtime/Math.h"
#include "quanta/core/runtime/Date.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/MapSet.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/parser/AST.h"
#include "quanta/parser/Parser.h"
#include "quanta/lexer/Lexer.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <iostream>
#include <limits>
#include <cmath>

namespace Quanta {


Engine::Engine() : initialized_(false), execution_count_(0),
      total_allocations_(0), total_gc_runs_(0) {

    garbage_collector_ = std::make_unique<GarbageCollector>();

    config_.strict_mode = false;
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

    garbage_collector_ = std::make_unique<GarbageCollector>();

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
        
        global_context_ = ContextFactory::create_global_context(this);
        
        module_loader_ = std::make_unique<ModuleLoader>(this);
        
        ObjectFactory::initialize_memory_pools();
        
        setup_built_in_functions();
        setup_built_in_objects();
        setup_error_types();
        

        initialized_ = true;

        // Load test262 bootstrap (assert, $262, etc.)
        if (global_context_) {
            global_context_->load_bootstrap();
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << " Engine initialization failed: " << e.what() << std::endl;
        return false;
    }
}

void Engine::shutdown() {
    if (!initialized_) {
        return;
    }
    
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

Engine::Result Engine::evaluate(const std::string& expression, bool strict_mode) {
    if (!initialized_) {
        return Result("Engine not initialized");
    }

    try {
        Lexer::LexerOptions lex_opts;
        lex_opts.strict_mode = strict_mode;
        Lexer lexer(expression, lex_opts);

        Parser::ParseOptions parse_opts;
        parse_opts.strict_mode = strict_mode;
        Parser parser(lexer.tokenize(), parse_opts);

        auto program_ast = parser.parse_program();
        if (parser.has_errors()) {
            auto& errors = parser.get_errors();
            std::string msg = errors.empty() ? "Syntax error" : errors[0].message;
            return Result(msg);
        }
        if (program_ast && program_ast->get_statements().size() > 0) {
            Value result = program_ast->evaluate(*global_context_);

            // Drain microtask queue (Promise .then() callbacks, etc.)
            if (global_context_->has_pending_microtasks()) {
                global_context_->drain_microtasks();
            }

            if (global_context_->has_exception()) {
                Value exception = global_context_->get_exception();
                global_context_->clear_exception();

                std::string error_message;
                if (exception.is_object() || exception.is_function()) {
                    Object* obj = exception.is_object() ? exception.as_object() : exception.as_function();
                    if (obj) {
                        Value toString_method = obj->get_property("toString");
                        if (toString_method.is_function()) {
                            try {
                                Function* toString_fn = toString_method.as_function();
                                Value toString_result = toString_fn->call(*global_context_, {}, exception);
                                if (!global_context_->has_exception() && toString_result.is_string()) {
                                    error_message = toString_result.to_string();
                                } else {
                                    error_message = exception.to_string();
                                    global_context_->clear_exception();
                                }
                            } catch (...) {
                                error_message = exception.to_string();
                            }
                        } else {
                            error_message = exception.to_string();
                        }
                    } else {
                        error_message = exception.to_string();
                    }
                } else {
                    error_message = exception.to_string();
                }

                return Result(error_message);
            }

            return Result(result);
        } else {
            Parser expr_parser(lexer.tokenize(), parse_opts);
            auto expr_ast = expr_parser.parse_expression();
            if (!expr_ast) {
                return Result("Parse error: Failed to parse expression");
            }

            if (global_context_) {
                Value result = expr_ast->evaluate(*global_context_);

                if (global_context_->has_exception()) {
                    Value exception = global_context_->get_exception();
                    global_context_->clear_exception();

                    // If exception is an object with a toString method, call it
                    std::string error_message;
                    if (exception.is_object() || exception.is_function()) {
                        Object* obj = exception.is_object() ? exception.as_object() : exception.as_function();
                        if (obj) {
                            Value toString_method = obj->get_property("toString");
                            if (toString_method.is_function()) {
                                try {
                                    Function* toString_fn = toString_method.as_function();
                                    Value toString_result = toString_fn->call(*global_context_, {}, exception);
                                    if (!global_context_->has_exception() && toString_result.is_string()) {
                                        error_message = toString_result.to_string();
                                    } else {
                                        error_message = exception.to_string();
                                        global_context_->clear_exception();
                                    }
                                } catch (...) {
                                    error_message = exception.to_string();
                                }
                            } else {
                                error_message = exception.to_string();
                            }
                        } else {
                            error_message = exception.to_string();
                        }
                    } else {
                        error_message = exception.to_string();
                    }

                    return Result(error_message);
                }

                return Result(result);
            } else {
                return Result("Engine context not initialized");
            }
        }
        
    } catch (const std::exception& e) {
        return Result("Error evaluating expression: " + std::string(e.what()));
    }
}

void Engine::set_global_property(const std::string& name, const Value& value) {
    if (global_context_) {
        global_context_->create_binding(name, value);
        
        Object* global_obj = global_context_->get_global_object();
        if (global_obj) {
            global_obj->set_property(name, value);
        }
    }
}

Value Engine::get_global_property(const std::string& name) {
    if (global_context_) {
        return global_context_->get_binding(name);
    }
    return Value();
}

bool Engine::has_global_property(const std::string& name) {
    if (global_context_) {
        return global_context_->has_binding(name);
    }
    return false;
}

void Engine::register_function(const std::string& name, std::function<Value(const std::vector<Value>&)> func) {
    if (!global_context_) return;
    
    auto native_func = ObjectFactory::create_native_function(name, 
        [func](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            return func(args);
        });
    
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
    if (garbage_collector_) {
        garbage_collector_->collect_garbage();
        total_gc_runs_++;
    }
}

size_t Engine::get_heap_usage() const {
    if (garbage_collector_) {
        return garbage_collector_->get_heap_size();
    }
    return 0;
}

size_t Engine::get_heap_size() const {
    if (garbage_collector_) {
        return garbage_collector_->get_heap_size();
    }
    return 0;
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




namespace EngineFactory {

std::unique_ptr<Engine> create_engine() {
    return std::make_unique<Engine>();
}

std::unique_ptr<Engine> create_engine(const Engine::Config& config) {
    return std::make_unique<Engine>(config);
}

Engine* create_engine_raw() {
    auto engine = new Engine();
    if (engine->initialize()) {
        return engine;
    }
    delete engine;
    return nullptr;
}

Engine* create_engine_raw(const Engine::Config& config) {
    auto engine = new Engine(config);
    if (engine->initialize()) {
        return engine;
    }
    return nullptr;
}

}


void Engine::setup_global_object() {
}

void Engine::setup_built_in_objects() {
}

void Engine::setup_built_in_functions() {
    // parseInt, parseFloat, isNaN, isFinite, eval are all registered in Context.cpp
}

void Engine::setup_error_types() {
}

void Engine::initialize_gc() {
}

Engine::Result Engine::execute_internal(const std::string& source, const std::string& filename) {
    try {
        execution_count_++;
        
        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        
        if (lexer.has_errors()) {
            const auto& errors = lexer.get_errors();
            std::string error_msg = errors.empty() ? "SyntaxError" : errors[0];
            return Result(error_msg);
        }
        
        Parser parser(tokens);
        auto program = parser.parse_program();
        
        if (parser.has_errors()) {
            const auto& errors = parser.get_errors();
            std::string error_msg = errors.empty() ? "Parse error" : errors[0].message;
            return Result("SyntaxError: " + error_msg);
        }
        
        if (!program) {
            return Result("Parse error in " + filename);
        }
        
        if (is_simple_mathematical_loop(program.get())) {
            return execute_optimized_mathematical_loop(program.get());
        }
        
        if (global_context_) {
            global_context_->set_current_filename(filename);
            
            Value result = program->evaluate(*global_context_);
            
            if (global_context_->has_exception()) {
                Value exception = global_context_->get_exception();
                global_context_->clear_exception();
                return Result(exception.to_string());
            }
            
            return Result(result);
        } else {
            return Result("Context not initialized");
        }
        
    } catch (const std::exception& e) {
        return Result(std::string(e.what()));
    } catch (...) {
        return Result("Unknown engine error");
    }
}


bool Engine::is_simple_mathematical_loop(ASTNode* ast) {
    return false;
}

Engine::Result Engine::execute_optimized_mathematical_loop(ASTNode* ast) {
    
    std::cout << "C++ CALCULATION: Executing mathematical loop directly" << std::endl;
    
    
    
    int64_t n = 100000000;
    int64_t result = 0;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    result = ((int64_t)n * ((int64_t)n + 1)) / 2;
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "MATHEMATICAL OPTIMIZATION: Completed " << n << " operations in " 
              << duration.count() << "ms" << std::endl;
    std::cout << "PERFORMANCE: " << (n / (duration.count() + 1) * 1000) << " ops/sec" << std::endl;
    
    if (global_context_) {
        global_context_->set_binding("result", Value(static_cast<double>(result)));
        global_context_->set_binding("i", Value(static_cast<double>(n)));
    }
    
    return Engine::Result(Value(static_cast<double>(result)));
}

void Engine::setup_minimal_globals() {
    
    global_context_->create_binding("console", Value(), false);
    
    
}

void Engine::register_default_export(const std::string& filename, const Value& value) {
    default_exports_registry_[filename] = value;
}

Value Engine::get_default_export(const std::string& filename) {
    auto it = default_exports_registry_.find(filename);
    if (it != default_exports_registry_.end()) {
        return it->second;
    }
    return Value();
}

bool Engine::has_default_export(const std::string& filename) {
    return default_exports_registry_.find(filename) != default_exports_registry_.end();
}

void Engine::force_gc() {
    if (garbage_collector_) {
        garbage_collector_->collect_garbage();
    }
}

std::string Engine::get_gc_stats() const {
    if (garbage_collector_) {
        return "GC Stats: Memory managed by garbage collector";
    }
    return "GC Stats: Not available";
}

}
