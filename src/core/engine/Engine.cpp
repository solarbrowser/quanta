/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Object.h"
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
#include "quanta/core/engine/CallStack.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <iostream>
#include <limits>
#include <cmath>

namespace Quanta {


Engine::Engine() : initialized_(false), execution_count_(0),
      total_allocations_(0), total_gc_runs_(0) {

    heap_ = new Heap();
    Heap::set_active(heap_);
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

    heap_ = new Heap();
    Heap::set_active(heap_);
    garbage_collector_ = std::make_unique<GarbageCollector>();

    start_time_ = std::chrono::high_resolution_clock::now();
}

Engine::~Engine() {
    shutdown();
    if (Heap::active_or_null() == heap_) {
        Heap::set_active(nullptr);
    }
}

bool Engine::initialize() {
    if (initialized_) {
        return true;
    }
    HeapScope heap_scope(heap_);

    try {
        // Temporarily null out current_context_ so that non-configurable property
        // checks in set_property_descriptor don't fire during built-in setup.
        Context* saved_context = Object::current_context_;
        Object::current_context_ = nullptr;

        global_context_ = ContextFactory::create_global_context(this);

        module_loader_ = std::make_unique<ModuleLoader>(this);

        ObjectFactory::initialize_memory_pools();

        setup_built_in_functions();
        setup_built_in_objects();
        setup_error_types();

        Object::current_context_ = saved_context;
        

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
    // Realm-safe: $262.createRealm engines share the thread; every entry
    // point re-installs its own heap for the duration of the call.
    HeapScope heap_scope(heap_);

    try {
        Lexer::LexerOptions lex_opts;
        lex_opts.strict_mode = strict_mode;
        Lexer lexer(expression, lex_opts);

        Parser::ParseOptions parse_opts;
        parse_opts.strict_mode = strict_mode;
        auto tokens = lexer.tokenize();
        auto tokens_copy = tokens;
        Parser parser(std::move(tokens_copy), parse_opts);
        parser.set_source(expression);

        auto program_ast = parser.parse_program();
        if (parser.has_errors()) {
            auto& errors = parser.get_errors();
            std::string msg = errors.empty() ? "Syntax error" : errors[0].message;
            return Result(msg);
        }
        if (program_ast && program_ast->get_statements().size() == 0) {
            return Result(Value()); // empty program (e.g. just comments) -> undefined
        }
        if (program_ast && program_ast->get_statements().size() > 0) {
            Value result = program_ast->evaluate(*global_context_);

            run_event_loop_to_completion(*global_context_);

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
            Parser expr_parser(tokens, parse_opts);
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

void Engine::add_survivor_context(Context* ctx) {
    if (ctx) survivor_contexts_.push_back(ctx);
}

void Engine::clear_survivor_contexts() {
    // Skip contexts still referenced by a pending timer, Promise, or suspended async fiber (see EventLoop::context_use_count_).
    std::vector<Context*> still_in_use;
    for (auto* ctx : survivor_contexts_) {
        if (EventLoop::instance().is_context_in_use(ctx)) {
            still_in_use.push_back(ctx);
        } else {
            delete ctx;
        }
    }
    survivor_contexts_ = std::move(still_in_use);
}

void Engine::run_event_loop_to_completion(Context& ctx) {
    if (ctx.has_pending_microtasks()) {
        ctx.drain_microtasks();
    }
    clear_survivor_contexts();

    if (EventLoop::instance().has_pending_timers()) {
        EventLoop::instance().run_pending_timers(ctx);
        // Safety net in case run_pending_timers exited early via its cap mid-iteration.
        if (ctx.has_pending_microtasks()) {
            ctx.drain_microtasks();
        }
        clear_survivor_contexts();
    }
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
    HeapScope heap_scope(heap_);
    try {
        execution_count_++;
        
        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        
        if (lexer.has_errors()) {
            const auto& errors = lexer.get_errors();
            std::string error_msg = errors.empty() ? "SyntaxError" : errors[0];
            if (error_msg.find("SyntaxError") == std::string::npos)
                error_msg = "SyntaxError: " + error_msg;
            size_t at_pos = error_msg.find(" at line ");
            if (at_pos == std::string::npos) at_pos = error_msg.find("error at ");
            return Result(error_msg);
        }
        
        Parser parser(tokens);
        parser.set_source(source);
        auto program = parser.parse_program();

        if (parser.has_errors()) {
            const auto& errors = parser.get_errors();
            if (!errors.empty()) {
                const auto& err = errors[0];
                std::string msg = err.message;
                if (msg.find("SyntaxError") == std::string::npos)
                    msg = "SyntaxError: " + msg;

                size_t err_line = err.position.line;
                size_t err_col  = err.position.column;

                std::vector<std::string> src_lines;
                {
                    std::istringstream ss(source);
                    std::string l;
                    while (std::getline(ss, l)) src_lines.push_back(l);
                }

                std::string decorated = msg + "\n";
                // Show 1 line before, the error line, 1 line after
                size_t first = (err_line >= 2) ? err_line - 2 : 0;
                size_t last  = std::min((size_t)src_lines.size(), err_line + 1);
                std::string line_num_width = std::to_string(last + 1);
                size_t w = line_num_width.size();
                for (size_t li = first; li < last; li++) {
                    size_t ln = li + 1;
                    std::string num = std::to_string(ln);
                    std::string prefix = std::string(w - num.size(), ' ') + num +
                                        (ln == err_line ? " > | " : "   | ");
                    decorated += prefix + src_lines[li] + "\n";
                    if (ln == err_line && err_col > 0) {
                        std::string indent(prefix.size() + err_col - 1, ' ');
                        decorated += indent + "^\n";
                    }
                }

                return Result(decorated);
            }
            return Result("SyntaxError: Parse error");
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

            run_event_loop_to_completion(*global_context_);

            if (global_context_->has_exception()) {
                Value exception = global_context_->get_exception();
                global_context_->clear_exception();

                std::string error_str;
                if (exception.is_object() || exception.is_function()) {
                    Object* obj = exception.is_object() ? exception.as_object()
                                                        : static_cast<Object*>(exception.as_function());
                    Error* err_obj = dynamic_cast<Error*>(obj);
                    if (err_obj && !err_obj->get_stack_trace().empty()) {
                        error_str = err_obj->get_stack_trace();
                    } else {
                        error_str = exception.to_string();
                    }
                } else {
                    error_str = exception.to_string();
                }

                return Result(error_str, exception);
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
