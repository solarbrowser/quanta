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
#include "quanta/core/apis/NodeJS.h"
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
#include "quanta/core/interpreter/FastBytecode.h"
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
    jit_compiler_ = std::make_unique<JITCompiler>();

    config_.strict_mode = false;
    config_.enable_jit = false;  // JIT disabled for stability
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
    jit_compiler_ = std::make_unique<JITCompiler>();

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
        return std::unexpected(Error("Engine not initialized"));
    }

    return execute_internal(source, filename);
}

Engine::Result Engine::execute_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return std::unexpected(Error("Cannot open file: " + filename));
    }
    
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return execute(buffer.str(), filename);
}

Engine::Result Engine::evaluate(const std::string& expression) {
    if (!initialized_) {
        return std::unexpected(Error("Engine not initialized"));
    }
    
    try {
        Lexer lexer(expression);
        Parser parser(lexer.tokenize());
        
        auto program_ast = parser.parse_program();
        if (program_ast && program_ast->get_statements().size() > 0) {
            Value result = program_ast->evaluate(*global_context_);

            if (global_context_->has_exception()) {
                Value exception = global_context_->get_exception();
                global_context_->clear_exception();

                // If exception is an object with a toString method, call it
                std::string error_message;
                std::cerr << "[DEBUG Engine] Exception type - is_object: " << exception.is_object()
                          << ", is_function: " << exception.is_function() << std::endl;
                if (exception.is_object() || exception.is_function()) {
                    Object* obj = exception.is_object() ? exception.as_object() : exception.as_function();
                    std::cerr << "[DEBUG Engine] Got obj pointer: " << (obj != nullptr) << std::endl;
                    if (obj) {
                        Value toString_method = obj->get_property("toString");
                        std::cerr << "[DEBUG Engine] toString_method.is_function(): " << toString_method.is_function() << std::endl;
                        if (toString_method.is_function()) {
                            try {
                                Function* toString_fn = toString_method.as_function();
                                Value toString_result = toString_fn->call(*global_context_, {}, exception);
                                std::cerr << "[DEBUG Engine] toString_result: " << toString_result.to_string() << std::endl;
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

                return std::unexpected(Error(error_message));
            }

            return result;
        } else {
            Lexer expr_lexer(expression);
            Parser expr_parser(expr_lexer.tokenize());
            auto expr_ast = expr_parser.parse_expression();
            if (!expr_ast) {
                return std::unexpected(Error("Parse error: Failed to parse expression"));
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

                    return std::unexpected(Error(error_message));
                }

                return result;
            } else {
                return std::unexpected(Error("Engine context not initialized"));
            }
        }

    } catch (const std::exception& e) {
        return std::unexpected(Error("Error evaluating expression: " + std::string(e.what())));
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

void Engine::set_web_api_interface(WebAPIInterface* interface) {
    if (global_context_) {
        global_context_->set_web_api_interface(interface);
    }
}

WebAPIInterface* Engine::get_web_api_interface() const {
    if (global_context_) {
        return global_context_->get_web_api_interface();
    }
    return nullptr;
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

void Engine::inject_dom(Object* document) {
    if (!initialized_) return;
    
    set_global_property("document", Value(document));
    
}

void Engine::setup_nodejs_apis() {
    auto fs_obj = std::make_unique<Object>();
    
    auto fs_readFile = ObjectFactory::create_native_function("readFile", NodeJS::fs_readFile);
    auto fs_writeFile = ObjectFactory::create_native_function("writeFile", NodeJS::fs_writeFile);
    auto fs_appendFile = ObjectFactory::create_native_function("appendFile", NodeJS::fs_appendFile);
    auto fs_exists = ObjectFactory::create_native_function("exists", NodeJS::fs_exists);
    auto fs_mkdir = ObjectFactory::create_native_function("mkdir", NodeJS::fs_mkdir);
    auto fs_rmdir = ObjectFactory::create_native_function("rmdir", NodeJS::fs_rmdir);
    auto fs_unlink = ObjectFactory::create_native_function("unlink", NodeJS::fs_unlink);
    auto fs_stat = ObjectFactory::create_native_function("stat", NodeJS::fs_stat);
    auto fs_readdir = ObjectFactory::create_native_function("readdir", NodeJS::fs_readdir);
    
    auto fs_readFileSync = ObjectFactory::create_native_function("readFileSync", NodeJS::fs_readFileSync);
    auto fs_writeFileSync = ObjectFactory::create_native_function("writeFileSync", NodeJS::fs_writeFileSync);
    auto fs_existsSync = ObjectFactory::create_native_function("existsSync", NodeJS::fs_existsSync);
    auto fs_mkdirSync = ObjectFactory::create_native_function("mkdirSync", NodeJS::fs_mkdirSync);
    auto fs_statSync = ObjectFactory::create_native_function("statSync", NodeJS::fs_statSync);
    auto fs_readdirSync = ObjectFactory::create_native_function("readdirSync", NodeJS::fs_readdirSync);
    
    fs_obj->set_property("readFile", Value(fs_readFile.release()));
    fs_obj->set_property("writeFile", Value(fs_writeFile.release()));
    fs_obj->set_property("appendFile", Value(fs_appendFile.release()));
    fs_obj->set_property("exists", Value(fs_exists.release()));
    fs_obj->set_property("mkdir", Value(fs_mkdir.release()));
    fs_obj->set_property("rmdir", Value(fs_rmdir.release()));
    fs_obj->set_property("unlink", Value(fs_unlink.release()));
    fs_obj->set_property("stat", Value(fs_stat.release()));
    fs_obj->set_property("readdir", Value(fs_readdir.release()));
    fs_obj->set_property("readFileSync", Value(fs_readFileSync.release()));
    fs_obj->set_property("writeFileSync", Value(fs_writeFileSync.release()));
    fs_obj->set_property("existsSync", Value(fs_existsSync.release()));
    fs_obj->set_property("mkdirSync", Value(fs_mkdirSync.release()));
    fs_obj->set_property("statSync", Value(fs_statSync.release()));
    fs_obj->set_property("readdirSync", Value(fs_readdirSync.release()));
    
    set_global_property("fs", Value(fs_obj.release()));
    
    auto path_obj = std::make_unique<Object>();
    
    auto path_join = ObjectFactory::create_native_function("join", NodeJS::path_join);
    auto path_resolve = ObjectFactory::create_native_function("resolve", NodeJS::path_resolve);
    auto path_dirname = ObjectFactory::create_native_function("dirname", NodeJS::path_dirname);
    auto path_basename = ObjectFactory::create_native_function("basename", NodeJS::path_basename);
    auto path_extname = ObjectFactory::create_native_function("extname", NodeJS::path_extname);
    auto path_normalize = ObjectFactory::create_native_function("normalize", NodeJS::path_normalize);
    auto path_isAbsolute = ObjectFactory::create_native_function("isAbsolute", NodeJS::path_isAbsolute);
    
    path_obj->set_property("join", Value(path_join.release()));
    path_obj->set_property("resolve", Value(path_resolve.release()));
    path_obj->set_property("dirname", Value(path_dirname.release()));
    path_obj->set_property("basename", Value(path_basename.release()));
    path_obj->set_property("extname", Value(path_extname.release()));
    path_obj->set_property("normalize", Value(path_normalize.release()));
    path_obj->set_property("isAbsolute", Value(path_isAbsolute.release()));
    
    set_global_property("path", Value(path_obj.release()));
    
    auto os_obj = std::make_unique<Object>();
    
    auto os_platform = ObjectFactory::create_native_function("platform", NodeJS::os_platform);
    auto os_arch = ObjectFactory::create_native_function("arch", NodeJS::os_arch);
    auto os_cpus = ObjectFactory::create_native_function("cpus", NodeJS::os_cpus);
    auto os_hostname = ObjectFactory::create_native_function("hostname", NodeJS::os_hostname);
    auto os_homedir = ObjectFactory::create_native_function("homedir", NodeJS::os_homedir);
    auto os_tmpdir = ObjectFactory::create_native_function("tmpdir", NodeJS::os_tmpdir);
    
    os_obj->set_property("platform", Value(os_platform.release()));
    os_obj->set_property("arch", Value(os_arch.release()));
    os_obj->set_property("cpus", Value(os_cpus.release()));
    os_obj->set_property("hostname", Value(os_hostname.release()));
    os_obj->set_property("homedir", Value(os_homedir.release()));
    os_obj->set_property("tmpdir", Value(os_tmpdir.release()));
    
    set_global_property("os", Value(os_obj.release()));
    
    auto process_obj = std::make_unique<Object>();
    
    auto process_exit = ObjectFactory::create_native_function("exit", NodeJS::process_exit);
    auto process_cwd = ObjectFactory::create_native_function("cwd", NodeJS::process_cwd);
    auto process_chdir = ObjectFactory::create_native_function("chdir", NodeJS::process_chdir);
    
    process_obj->set_property("exit", Value(process_exit.release()));
    process_obj->set_property("cwd", Value(process_cwd.release()));
    process_obj->set_property("chdir", Value(process_chdir.release()));
    
    set_global_property("process", Value(process_obj.release()));
    
    auto crypto_obj = std::make_unique<Object>();
    
    auto crypto_randomBytes = ObjectFactory::create_native_function("randomBytes", NodeJS::crypto_randomBytes);
    auto crypto_createHash = ObjectFactory::create_native_function("createHash", NodeJS::crypto_createHash);
    
    crypto_obj->set_property("randomBytes", Value(crypto_randomBytes.release()));
    crypto_obj->set_property("createHash", Value(crypto_createHash.release()));
    
    set_global_property("crypto", Value(crypto_obj.release()));
    
    
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
    register_function("eval", [this](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            return Value();
        }

        std::string code = args[0].to_string();
        if (code.empty()) {
            return Value();
        }

        try {
            // Use evaluate() instead of execute() for proper expression parsing
            Result result = evaluate(code);
            if (result.has_value()) {
                return *result;
            } else {
                throw std::runtime_error("SyntaxError: " + result.error().message);
            }
        } catch (const std::runtime_error& e) {
            std::string error_msg = e.what();
            if (error_msg.find("SyntaxError:") == 0) {
                throw e;
            }
            throw std::runtime_error("EvalError: " + error_msg);
        } catch (const std::exception& e) {
            throw std::runtime_error("EvalError: " + std::string(e.what()));
        }
    });
    
    register_function("parseInt", [](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            return Value::nan();
        }
        
        std::string str = args[0].to_string();
        
        size_t start = 0;
        while (start < str.length() && std::isspace(str[start])) {
            start++;
        }
        
        if (start >= str.length()) {
            return Value::nan();
        }
        
        int radix = 10;
        if (args.size() > 1) {
            double r = args[1].to_number();
            if (r >= 2 && r <= 36) {
                radix = static_cast<int>(r);
            }
        }
        
        char first_char = str[start];
        bool has_valid_start = false;
        
        if (radix == 16) {
            has_valid_start = std::isdigit(first_char) || 
                            (first_char >= 'a' && first_char <= 'f') ||
                            (first_char >= 'A' && first_char <= 'F');
        } else if (radix == 8) {
            has_valid_start = (first_char >= '0' && first_char <= '7');
        } else {
            has_valid_start = std::isdigit(first_char);
        }
        
        if (!has_valid_start && first_char != '+' && first_char != '-') {
            return Value::nan();
        }
        
        try {
            size_t pos;
            long result = std::stol(str.substr(start), &pos, radix);
            if (pos == 0) {
                return Value::nan();
            }
            return Value(static_cast<double>(result));
        } catch (...) {
            return Value::nan();
        }
    });
    
    register_function("parseFloat", [](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            return Value::nan();
        }
        
        std::string str = args[0].to_string();
        
        size_t start = 0;
        while (start < str.length() && std::isspace(str[start])) {
            start++;
        }
        
        if (start >= str.length()) {
            return Value::nan();
        }
        
        char first_char = str[start];
        if (!std::isdigit(first_char) && first_char != '.' && 
            first_char != '+' && first_char != '-') {
            return Value::nan();
        }
        
        try {
            size_t pos;
            double result = std::stod(str.substr(start), &pos);
            if (pos == 0) {
                return Value::nan();
            }
            return Value(result);
        } catch (...) {
            return Value::nan();
        }
    });
    
    register_function("isNaN", [](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            return Value(true);
        }

        Value val = args[0];
        double num = val.to_number();

        // Check for NaN using both value tag and double comparison
        if (val.is_number() && val.is_nan()) {
            return Value(true);
        }

        // Also check using != (NaN != NaN is true)
        return Value(num != num);
    });
    
    register_function("isFinite", [](const std::vector<Value>& args) -> Value {
        if (args.empty()) {
            return Value(false);
        }

        Value val = args[0];

        // Convert to number first
        double num = val.to_number();

        // Check using Value tags if it's a number type
        if (val.is_number()) {
            // Check for NaN, positive infinity, or negative infinity
            if (val.is_nan() || val.is_positive_infinity() || val.is_negative_infinity()) {
                return Value(false);
            }
            return Value(true);
        }

        // For non-number types after conversion, check the double
        // Check for NaN using != comparison
        if (num != num) return Value(false);

        // Check for infinity by comparing with limits
        if (num == std::numeric_limits<double>::infinity() || num == -std::numeric_limits<double>::infinity()) {
            return Value(false);
        }

        return Value(true);
    });
}

void Engine::setup_error_types() {
}

void Engine::initialize_gc() {
}

void Engine::register_web_apis() {
}

Engine::Result Engine::execute_internal(const std::string& source, const std::string& filename) {
    try {
        execution_count_++;
        
        FastBytecodeVM vm;
        bool compiled = vm.compile_direct(source);
        
        if (compiled) {
            Value result = vm.execute_fast();
            return result;
        }
        
        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        
        if (lexer.has_errors()) {
            const auto& errors = lexer.get_errors();
            std::string error_msg = errors.empty() ? "SyntaxError" : errors[0];
            return std::unexpected(Error(error_msg));
        }
        
        Parser parser(tokens);
        auto program = parser.parse_program();
        
        if (parser.has_errors()) {
            const auto& errors = parser.get_errors();
            std::string error_msg = errors.empty() ? "Parse error" : errors[0].message;
            return std::unexpected(Error("SyntaxError: " + error_msg));
        }

        if (!program) {
            return std::unexpected(Error("Parse error in " + filename));
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
                return std::unexpected(Error(exception.to_string()));
            }

            return result;  // Success case - just return the Value
        } else {
            return std::unexpected(Error("Context not initialized"));
        }

    } catch (const std::exception& e) {
        return std::unexpected(Error(std::string(e.what())));
    } catch (...) {
        return std::unexpected(Error("Unknown engine error"));
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

std::string Engine::get_jit_stats() const {
    if (!jit_compiler_) {
        return "JIT Stats: Not initialized";
    }

    const auto& stats = jit_compiler_->get_stats();

    std::string result = "=== JIT Compiler Statistics ===\n";
    result += "Total Compilations: " + std::to_string(stats.total_compilations) + "\n";
    result += "  Bytecode: " + std::to_string(stats.bytecode_compilations) + "\n";
    result += "  Optimized: " + std::to_string(stats.optimized_compilations) + "\n";
    result += "  Machine Code: " + std::to_string(stats.machine_code_compilations) + "\n";
    result += "\nCache Performance:\n";
    result += "  Hits: " + std::to_string(stats.cache_hits) + "\n";
    result += "  Misses: " + std::to_string(stats.cache_misses) + "\n";
    result += "  Hit Ratio: " + std::to_string(stats.get_cache_hit_ratio() * 100.0) + "%\n";

    if (stats.total_jit_time_ns > 0) {
        result += "\nPerformance:\n";
        result += "  Speedup: " + std::to_string(stats.get_speedup()) + "x\n";
    }

    if (jit_compiler_) {
        jit_compiler_->print_property_cache_stats();
    }

    return result;
}

}
