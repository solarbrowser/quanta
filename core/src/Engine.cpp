#include "Engine.h"
#include "JSON.h"
#include "Math.h"
#include "Date.h"
#include "Symbol.h"
#include "WebAPI.h"
#include "NodeJS.h"
#include "Promise.h"
#include "Error.h"
// ES6+ includes disabled due to segfault
// #include "Generator.h"
#include "MapSet.h"
// #include "Iterator.h"
// #include "Async.h"
// #include "ProxyReflect.h"
#include "../../parser/include/AST.h"
#include "../../parser/include/Parser.h"
#include "../../lexer/include/Lexer.h"
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
    // Initialize JIT compiler
    jit_compiler_ = std::make_unique<JITCompiler>();
    
    // Initialize garbage collector
    garbage_collector_ = std::make_unique<GarbageCollector>();
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
        
        // Initialize module loader
        module_loader_ = std::make_unique<ModuleLoader>(this);
        
        // Setup global environment
        setup_global_object();
        setup_built_in_objects();
        setup_built_in_functions();
        setup_error_types();
        setup_browser_globals();
        
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
    
    try {
        // Create lexer and parser for expression evaluation
        Lexer lexer(expression);
        Parser parser(lexer.tokenize());
        
        // Parse the expression into AST
        auto expr_ast = parser.parse_expression();
        if (!expr_ast) {
            return Result("Parse error: Failed to parse expression");
        }
        
        // Evaluate the expression AST
        if (global_context_) {
            Value result = expr_ast->evaluate(*global_context_);
            
            // Check if the context has any thrown exceptions
            if (global_context_->has_exception()) {
                Value exception = global_context_->get_exception();
                global_context_->clear_exception();
                return Result("JavaScript Error: " + exception.to_string());
            }
            
            return Result(result);
        } else {
            return Result("Engine context not initialized");
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
    // Allow registration during initialization
    if (!global_context_) return;
    
    // Create a native function with the actual implementation
    auto native_func = ObjectFactory::create_native_function(name, 
        [func](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused parameter warning
            return func(args);
        });
    
    // Store the function as a global property
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

void Engine::inject_dom(Object* document) {
    if (!initialized_) return;
    
    // Inject DOM object into global scope
    set_global_property("document", Value(document));
    
    // Setup basic DOM globals
    setup_browser_globals();
}

void Engine::setup_nodejs_apis() {
    // Node.js File System API
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
    
    // Sync versions
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
    
    // Node.js Path API
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
    
    // Node.js OS API
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
    
    // Node.js Process API
    auto process_obj = std::make_unique<Object>();
    
    auto process_exit = ObjectFactory::create_native_function("exit", NodeJS::process_exit);
    auto process_cwd = ObjectFactory::create_native_function("cwd", NodeJS::process_cwd);
    auto process_chdir = ObjectFactory::create_native_function("chdir", NodeJS::process_chdir);
    
    process_obj->set_property("exit", Value(process_exit.release()));
    process_obj->set_property("cwd", Value(process_cwd.release()));
    process_obj->set_property("chdir", Value(process_chdir.release()));
    
    set_global_property("process", Value(process_obj.release()));
    
    // Node.js Crypto API
    auto crypto_obj = std::make_unique<Object>();
    
    auto crypto_randomBytes = ObjectFactory::create_native_function("randomBytes", NodeJS::crypto_randomBytes);
    auto crypto_createHash = ObjectFactory::create_native_function("createHash", NodeJS::crypto_createHash);
    
    crypto_obj->set_property("randomBytes", Value(crypto_randomBytes.release()));
    crypto_obj->set_property("createHash", Value(crypto_createHash.release()));
    
    set_global_property("crypto", Value(crypto_obj.release()));
}

void Engine::setup_browser_globals() {
    // Browser-specific globals (placeholder implementations)
    set_global_property("window", Value(global_context_->get_global_object()));
    
    // Create enhanced console object with multiple methods
    auto console_obj = std::make_unique<Object>();
    auto console_log_fn = ObjectFactory::create_native_function("log", WebAPI::console_log);
    auto console_error_fn = ObjectFactory::create_native_function("error", WebAPI::console_error);
    auto console_warn_fn = ObjectFactory::create_native_function("warn", WebAPI::console_warn);
    auto console_info_fn = ObjectFactory::create_native_function("info", WebAPI::console_info);
    auto console_debug_fn = ObjectFactory::create_native_function("debug", WebAPI::console_debug);
    auto console_trace_fn = ObjectFactory::create_native_function("trace", WebAPI::console_trace);
    auto console_time_fn = ObjectFactory::create_native_function("time", WebAPI::console_time);
    auto console_timeEnd_fn = ObjectFactory::create_native_function("timeEnd", WebAPI::console_timeEnd);
    
    console_obj->set_property("log", Value(console_log_fn.release()));
    console_obj->set_property("error", Value(console_error_fn.release()));
    console_obj->set_property("warn", Value(console_warn_fn.release()));
    console_obj->set_property("info", Value(console_info_fn.release()));
    console_obj->set_property("debug", Value(console_debug_fn.release()));
    console_obj->set_property("trace", Value(console_trace_fn.release()));
    console_obj->set_property("time", Value(console_time_fn.release()));
    console_obj->set_property("timeEnd", Value(console_timeEnd_fn.release()));
    
    set_global_property("console", Value(console_obj.release()));
    
    // Create JSON object
    auto json_obj = std::make_unique<Object>();
    
    // Create JSON.parse function
    auto json_parse_fn = ObjectFactory::create_native_function("parse",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_parse(ctx, args);
        });
    
    // Create JSON.stringify function
    auto json_stringify_fn = ObjectFactory::create_native_function("stringify",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_stringify(ctx, args);
        });
    
    json_obj->set_property("parse", Value(json_parse_fn.release()));
    json_obj->set_property("stringify", Value(json_stringify_fn.release()));
    set_global_property("JSON", Value(json_obj.release()));
    
    // Create Math object with native functions
    auto math_obj = std::make_unique<Object>();
    
    // Add Math constants
    math_obj->set_property("E", Value(Math::E));
    math_obj->set_property("LN2", Value(Math::LN2));
    math_obj->set_property("LN10", Value(Math::LN10));
    math_obj->set_property("LOG2E", Value(Math::LOG2E));
    math_obj->set_property("LOG10E", Value(Math::LOG10E));
    math_obj->set_property("PI", Value(Math::PI));
    math_obj->set_property("SQRT1_2", Value(Math::SQRT1_2));
    math_obj->set_property("SQRT2", Value(Math::SQRT2));
    
    // Add Math methods as native functions
    auto math_abs = ObjectFactory::create_native_function("abs", Math::abs);
    auto math_max = ObjectFactory::create_native_function("max", Math::max);
    auto math_min = ObjectFactory::create_native_function("min", Math::min);
    auto math_round = ObjectFactory::create_native_function("round", Math::round);
    auto math_floor = ObjectFactory::create_native_function("floor", Math::floor);
    auto math_ceil = ObjectFactory::create_native_function("ceil", Math::ceil);
    auto math_pow = ObjectFactory::create_native_function("pow", Math::pow);
    auto math_sqrt = ObjectFactory::create_native_function("sqrt", Math::sqrt);
    auto math_sin = ObjectFactory::create_native_function("sin", Math::sin);
    auto math_cos = ObjectFactory::create_native_function("cos", Math::cos);
    auto math_tan = ObjectFactory::create_native_function("tan", Math::tan);
    auto math_random = ObjectFactory::create_native_function("random", Math::random);
    
    math_obj->set_property("abs", Value(math_abs.release()));
    math_obj->set_property("max", Value(math_max.release()));
    math_obj->set_property("min", Value(math_min.release()));
    math_obj->set_property("round", Value(math_round.release()));
    math_obj->set_property("floor", Value(math_floor.release()));
    math_obj->set_property("ceil", Value(math_ceil.release()));
    math_obj->set_property("pow", Value(math_pow.release()));
    math_obj->set_property("sqrt", Value(math_sqrt.release()));
    math_obj->set_property("sin", Value(math_sin.release()));
    math_obj->set_property("cos", Value(math_cos.release()));
    math_obj->set_property("tan", Value(math_tan.release()));
    math_obj->set_property("random", Value(math_random.release()));
    
    // Add ES2026 enhanced Math methods
    auto math_sumPrecise = ObjectFactory::create_native_function("sumPrecise", Math::sumPrecise);
    auto math_f16round = ObjectFactory::create_native_function("f16round", Math::f16round);
    auto math_log10 = ObjectFactory::create_native_function("log10", Math::log10);
    auto math_log2 = ObjectFactory::create_native_function("log2", Math::log2);
    auto math_log1p = ObjectFactory::create_native_function("log1p", Math::log1p);
    auto math_expm1 = ObjectFactory::create_native_function("expm1", Math::expm1);
    auto math_acosh = ObjectFactory::create_native_function("acosh", Math::acosh);
    auto math_asinh = ObjectFactory::create_native_function("asinh", Math::asinh);
    auto math_atanh = ObjectFactory::create_native_function("atanh", Math::atanh);
    auto math_cosh = ObjectFactory::create_native_function("cosh", Math::cosh);
    auto math_sinh = ObjectFactory::create_native_function("sinh", Math::sinh);
    auto math_tanh = ObjectFactory::create_native_function("tanh", Math::tanh);
    
    math_obj->set_property("sumPrecise", Value(math_sumPrecise.release()));
    math_obj->set_property("f16round", Value(math_f16round.release()));
    math_obj->set_property("log10", Value(math_log10.release()));
    math_obj->set_property("log2", Value(math_log2.release()));
    math_obj->set_property("log1p", Value(math_log1p.release()));
    math_obj->set_property("expm1", Value(math_expm1.release()));
    math_obj->set_property("acosh", Value(math_acosh.release()));
    math_obj->set_property("asinh", Value(math_asinh.release()));
    math_obj->set_property("atanh", Value(math_atanh.release()));
    math_obj->set_property("cosh", Value(math_cosh.release()));
    math_obj->set_property("sinh", Value(math_sinh.release()));
    math_obj->set_property("tanh", Value(math_tanh.release()));
    
    set_global_property("Math", Value(math_obj.release()));
    
    // Create Date constructor function
    auto date_constructor_fn = ObjectFactory::create_native_function("Date", Date::date_constructor);
    
    // Add Date static methods
    auto date_now = ObjectFactory::create_native_function("now", Date::now);
    auto date_parse = ObjectFactory::create_native_function("parse", Date::parse);
    auto date_UTC = ObjectFactory::create_native_function("UTC", Date::UTC);
    
    date_constructor_fn->set_property("now", Value(date_now.release()));
    date_constructor_fn->set_property("parse", Value(date_parse.release()));
    date_constructor_fn->set_property("UTC", Value(date_UTC.release()));
    
    // Add Date instance methods (callable as Date.methodName for testing)
    auto date_getTime = ObjectFactory::create_native_function("getTime", Date::getTime);
    auto date_getFullYear = ObjectFactory::create_native_function("getFullYear", Date::getFullYear);
    auto date_getMonth = ObjectFactory::create_native_function("getMonth", Date::getMonth);
    auto date_getDate = ObjectFactory::create_native_function("getDate", Date::getDate);
    auto date_getDay = ObjectFactory::create_native_function("getDay", Date::getDay);
    auto date_getHours = ObjectFactory::create_native_function("getHours", Date::getHours);
    auto date_getMinutes = ObjectFactory::create_native_function("getMinutes", Date::getMinutes);
    auto date_getSeconds = ObjectFactory::create_native_function("getSeconds", Date::getSeconds);
    auto date_getMilliseconds = ObjectFactory::create_native_function("getMilliseconds", Date::getMilliseconds);
    
    // Add Date string methods
    auto date_toString = ObjectFactory::create_native_function("toString", Date::toString);
    auto date_toISOString = ObjectFactory::create_native_function("toISOString", Date::toISOString);
    auto date_toJSON = ObjectFactory::create_native_function("toJSON", Date::toJSON);
    
    // Add setter methods
    auto date_setTime = ObjectFactory::create_native_function("setTime", Date::setTime);
    auto date_setFullYear = ObjectFactory::create_native_function("setFullYear", Date::setFullYear);
    auto date_setMonth = ObjectFactory::create_native_function("setMonth", Date::setMonth);
    auto date_setDate = ObjectFactory::create_native_function("setDate", Date::setDate);
    auto date_setHours = ObjectFactory::create_native_function("setHours", Date::setHours);
    auto date_setMinutes = ObjectFactory::create_native_function("setMinutes", Date::setMinutes);
    auto date_setSeconds = ObjectFactory::create_native_function("setSeconds", Date::setSeconds);
    auto date_setMilliseconds = ObjectFactory::create_native_function("setMilliseconds", Date::setMilliseconds);
    
    // Create Date.prototype for instance methods
    auto date_prototype = ObjectFactory::create_object();
    date_prototype->set_property("getTime", Value(date_getTime.release()));
    date_prototype->set_property("getFullYear", Value(date_getFullYear.release()));
    date_prototype->set_property("getMonth", Value(date_getMonth.release()));
    date_prototype->set_property("getDate", Value(date_getDate.release()));
    date_prototype->set_property("getDay", Value(date_getDay.release()));
    date_prototype->set_property("getHours", Value(date_getHours.release()));
    date_prototype->set_property("getMinutes", Value(date_getMinutes.release()));
    date_prototype->set_property("getSeconds", Value(date_getSeconds.release()));
    date_prototype->set_property("getMilliseconds", Value(date_getMilliseconds.release()));
    
    date_prototype->set_property("toString", Value(date_toString.release()));
    date_prototype->set_property("toISOString", Value(date_toISOString.release()));
    date_prototype->set_property("toJSON", Value(date_toJSON.release()));
    
    date_prototype->set_property("setTime", Value(date_setTime.release()));
    date_prototype->set_property("setFullYear", Value(date_setFullYear.release()));
    date_prototype->set_property("setMonth", Value(date_setMonth.release()));
    date_prototype->set_property("setDate", Value(date_setDate.release()));
    date_prototype->set_property("setHours", Value(date_setHours.release()));
    date_prototype->set_property("setMinutes", Value(date_setMinutes.release()));
    date_prototype->set_property("setSeconds", Value(date_setSeconds.release()));
    date_prototype->set_property("setMilliseconds", Value(date_setMilliseconds.release()));
    
    date_constructor_fn->set_property("prototype", Value(date_prototype.release()));
    
    set_global_property("Date", Value(date_constructor_fn.release()));
    
    // Web APIs - Timer functions
    auto setTimeout_fn = ObjectFactory::create_native_function("setTimeout", WebAPI::setTimeout);
    auto setInterval_fn = ObjectFactory::create_native_function("setInterval", WebAPI::setInterval);
    auto clearTimeout_fn = ObjectFactory::create_native_function("clearTimeout", WebAPI::clearTimeout);
    auto clearInterval_fn = ObjectFactory::create_native_function("clearInterval", WebAPI::clearInterval);
    
    set_global_property("setTimeout", Value(setTimeout_fn.release()));
    set_global_property("setInterval", Value(setInterval_fn.release()));
    set_global_property("clearTimeout", Value(clearTimeout_fn.release()));
    set_global_property("clearInterval", Value(clearInterval_fn.release()));
    
    
    // Fetch API
    auto fetch_fn = ObjectFactory::create_native_function("fetch", WebAPI::fetch);
    set_global_property("fetch", Value(fetch_fn.release()));
    
    // Window API
    auto alert_fn = ObjectFactory::create_native_function("alert", WebAPI::window_alert);
    auto confirm_fn = ObjectFactory::create_native_function("confirm", WebAPI::window_confirm);
    auto prompt_fn = ObjectFactory::create_native_function("prompt", WebAPI::window_prompt);
    
    set_global_property("alert", Value(alert_fn.release()));
    set_global_property("confirm", Value(confirm_fn.release()));
    set_global_property("prompt", Value(prompt_fn.release()));
    
    // Document API (basic)
    auto document_obj = std::make_unique<Object>();
    auto getElementById_fn = ObjectFactory::create_native_function("getElementById", WebAPI::document_getElementById);
    auto createElement_fn = ObjectFactory::create_native_function("createElement", WebAPI::document_createElement);
    auto querySelector_fn = ObjectFactory::create_native_function("querySelector", WebAPI::document_querySelector);
    
    document_obj->set_property("getElementById", Value(getElementById_fn.release()));
    document_obj->set_property("createElement", Value(createElement_fn.release()));
    document_obj->set_property("querySelector", Value(querySelector_fn.release()));
    
    set_global_property("document", Value(document_obj.release()));
    
    // LocalStorage API
    auto localStorage_obj = std::make_unique<Object>();
    auto getItem_fn = ObjectFactory::create_native_function("getItem", WebAPI::localStorage_getItem);
    auto setItem_fn = ObjectFactory::create_native_function("setItem", WebAPI::localStorage_setItem);
    auto removeItem_fn = ObjectFactory::create_native_function("removeItem", WebAPI::localStorage_removeItem);
    auto clear_fn = ObjectFactory::create_native_function("clear", WebAPI::localStorage_clear);
    
    localStorage_obj->set_property("getItem", Value(getItem_fn.release()));
    localStorage_obj->set_property("setItem", Value(setItem_fn.release()));
    localStorage_obj->set_property("removeItem", Value(removeItem_fn.release()));
    localStorage_obj->set_property("clear", Value(clear_fn.release()));
    
    set_global_property("localStorage", Value(localStorage_obj.release()));
    
    // NodeJS API registration - now working on Windows with minimal headers
    setup_nodejs_apis();
    
    // Setup new ES6+ features - DISABLED due to segfault
    // setup_es6_features();
}

void Engine::register_web_apis() {
    // Web API registration (placeholder)
    setup_browser_globals();
}

Engine::Result Engine::execute_internal(const std::string& source, const std::string& filename) {
    try {
        execution_count_++;
        
        // Create lexer and parser for full JavaScript execution
        Lexer lexer(source);
        Parser parser(lexer.tokenize());
        
        // std::cout << "DEBUG: Parser created, about to parse" << std::endl;
        
        // Parse the program into AST
        auto program = parser.parse_program();
        if (!program) {
            return Result("Parse error: Failed to parse JavaScript code");
        }
        
        // std::cout << "DEBUG: Program parsed successfully" << std::endl;
        
        // Execute the AST with full Stage 10 support
        if (global_context_) {
            // std::cout << "DEBUG: About to call program->evaluate()" << std::endl;
            Value result = program->evaluate(*global_context_);
            // std::cout << "DEBUG: program->evaluate() returned" << std::endl;
            
            // Check if the context has any thrown exceptions
            if (global_context_->has_exception()) {
                Value exception = global_context_->get_exception();
                global_context_->clear_exception();
                return Result("JavaScript Error: " + exception.to_string());
            }
            
            return Result(result);
        } else {
            return Result("Engine context not initialized");
        }
        
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
    // Set up Array.prototype methods
    if (!global_context_) return;
    
    // Create Array constructor if it doesn't exist
    Object* global_obj = global_context_->get_global_object();
    if (!global_obj) return;
    
    // Create Array constructor
    auto array_constructor = ObjectFactory::create_native_function("Array", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused parameter warning
            
            // Create array with specified length or elements
            if (args.empty()) {
                return Value(ObjectFactory::create_array(0).release());
            } else if (args.size() == 1 && args[0].is_number()) {
                // new Array(length)
                uint32_t length = static_cast<uint32_t>(args[0].as_number());
                return Value(ObjectFactory::create_array(length).release());
            } else {
                // new Array(element1, element2, ...)
                auto array = ObjectFactory::create_array(args.size());
                for (size_t i = 0; i < args.size(); ++i) {
                    array->set_element(static_cast<uint32_t>(i), args[i]);
                }
                return Value(array.release());
            }
        });
    global_context_->create_binding("Array", Value(array_constructor.get()));
    
    // Create Array.prototype
    auto array_prototype = ObjectFactory::create_object();
    array_constructor->set_property("prototype", Value(array_prototype.get()));
    
    // Add Array.prototype.map
    auto map_fn = ObjectFactory::create_native_function("map", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("TypeError: Array.prototype.map called on non-object"));
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value("TypeError: callback is not a function"));
                return Value();
            }
            
            // Get callback function from the Value
            Function* callback = static_cast<Function*>(args[0].as_object());
            auto result = this_obj->map(callback, ctx);
            return result ? Value(result.release()) : Value();
        });
    
    array_prototype->set_property("map", Value(map_fn.release()));
    
    // Add Array.prototype.filter
    auto filter_fn = ObjectFactory::create_native_function("filter", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("TypeError: Array.prototype.filter called on non-object"));
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value("TypeError: callback is not a function"));
                return Value();
            }
            
            Function* callback = static_cast<Function*>(args[0].as_object());
            auto result = this_obj->filter(callback, ctx);
            return result ? Value(result.release()) : Value();
        });
    
    array_prototype->set_property("filter", Value(filter_fn.release()));
    
    // Add Array.prototype.forEach
    auto forEach_fn = ObjectFactory::create_native_function("forEach", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("TypeError: Array.prototype.forEach called on non-object"));
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value("TypeError: callback is not a function"));
                return Value();
            }
            
            Function* callback = static_cast<Function*>(args[0].as_object());
            this_obj->forEach(callback, ctx);
            return Value(); // undefined
        });
    
    array_prototype->set_property("forEach", Value(forEach_fn.release()));
    
    // Add Array.prototype.reduce
    auto reduce_fn = ObjectFactory::create_native_function("reduce", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("TypeError: Array.prototype.reduce called on non-object"));
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value("TypeError: callback is not a function"));
                return Value();
            }
            
            Function* callback = static_cast<Function*>(args[0].as_object());
            Value initial_value = args.size() > 1 ? args[1] : Value();
            return this_obj->reduce(callback, initial_value, ctx);
        });
    
    array_prototype->set_property("reduce", Value(reduce_fn.release()));
    
    // Note: GroupBy method temporarily disabled due to runtime issues
    // Will be re-enabled after debugging the function calling mechanism
    
    // Create Promise constructor
    auto promise_constructor = ObjectFactory::create_native_function("Promise", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value("TypeError: Promise constructor requires a function argument"));
                return Value();
            }
            
            // Create a new Promise object
            auto promise = new Promise();
            
            // Get the executor function
            Function* executor = static_cast<Function*>(args[0].as_object());
            
            // Create resolve and reject functions
            auto resolve_fn = ObjectFactory::create_native_function("resolve", 
                [=](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; // Suppress unused parameter warning
                    Value resolve_value = args.empty() ? Value() : args[0];
                    promise->fulfill(resolve_value);
                    return Value();
                });
                
            auto reject_fn = ObjectFactory::create_native_function("reject", 
                [=](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; // Suppress unused parameter warning
                    Value reject_reason = args.empty() ? Value() : args[0];
                    promise->reject(reject_reason);
                    return Value();
                });
            
            // Call executor with resolve and reject functions
            std::vector<Value> executor_args = {
                Value(resolve_fn.release()),
                Value(reject_fn.release())
            };
            
            try {
                executor->call(ctx, executor_args);
            } catch (...) {
                // If executor throws, reject the promise
                promise->reject(Value("Promise executor threw an exception"));
            }
            
            return Value(promise);
        });
    
    // Add ES2025 Promise static methods
    auto promise_withResolvers = ObjectFactory::create_native_function("withResolvers", Promise::withResolvers);
    auto promise_try = ObjectFactory::create_native_function("try", Promise::try_method);
    
    promise_constructor->set_property("withResolvers", Value(promise_withResolvers.release()));
    promise_constructor->set_property("try", Value(promise_try.release()));
    
    global_context_->create_binding("Promise", Value(promise_constructor.get()));
    
    // Create Intl object for internationalization
    auto intl_obj = ObjectFactory::create_object();
    
    // Add Intl.NumberFormat
    auto number_format = ObjectFactory::create_native_function("NumberFormat", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused parameter warning
            
            // Create a NumberFormat object
            auto formatter = ObjectFactory::create_object();
            
            // Get locale (default to "en-US")
            std::string locale = "en-US";
            if (!args.empty()) {
                locale = args[0].to_string();
            }
            
            formatter->set_property("locale", Value(locale));
            
            // Add format method
            auto format_fn = ObjectFactory::create_native_function("format", 
                [locale](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; // Suppress unused parameter warning
                    
                    if (args.empty()) {
                        return Value("NaN");
                    }
                    
                    // Simple number formatting
                    if (args[0].is_number()) {
                        double num = args[0].as_number();
                        
                        // Basic formatting based on locale
                        if (locale == "de-DE") {
                            // German format uses comma as decimal separator
                            std::string result = std::to_string(num);
                            // Replace . with ,
                            size_t pos = result.find('.');
                            if (pos != std::string::npos) {
                                result[pos] = ',';
                            }
                            return Value(result);
                        } else {
                            // Default US format
                            return Value(std::to_string(num));
                        }
                    }
                    
                    return Value(args[0].to_string());
                });
            
            formatter->set_property("format", Value(format_fn.release()));
            return Value(formatter.release());
        });
    
    intl_obj->set_property("NumberFormat", Value(number_format.release()));
    
    // Add Intl.Collator (basic implementation)
    auto collator = ObjectFactory::create_native_function("Collator", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused parameter warning
            
            // Create a Collator object
            auto coll = ObjectFactory::create_object();
            
            // Get locale (default to "en-US")
            std::string locale = "en-US";
            if (!args.empty()) {
                locale = args[0].to_string();
            }
            
            coll->set_property("locale", Value(locale));
            
            // Add compare method
            auto compare_fn = ObjectFactory::create_native_function("compare", 
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; // Suppress unused parameter warning
                    
                    if (args.size() < 2) {
                        return Value(0);
                    }
                    
                    std::string str1 = args[0].to_string();
                    std::string str2 = args[1].to_string();
                    
                    if (str1 < str2) return Value(-1);
                    if (str1 > str2) return Value(1);
                    return Value(0);
                });
            
            coll->set_property("compare", Value(compare_fn.release()));
            return Value(coll.release());
        });
    
    intl_obj->set_property("Collator", Value(collator.release()));
    
    // Set Intl in global scope
    global_context_->create_binding("Intl", Value(intl_obj.release()));
    
    // Add a test property to verify global object works
    global_context_->create_binding("TEST_GLOBAL", Value("test_value"));
    
    // Prevent garbage collection of these objects
    array_constructor.release();
    array_prototype.release();
    promise_constructor.release();
}

void Engine::setup_built_in_functions() {
    // Initialize Symbol well-known symbols
    Symbol::initialize_well_known_symbols();
    
    // Symbol constructor
    register_function("Symbol", [](const std::vector<Value>& args) {
        std::string description = "";
        if (!args.empty() && !args[0].is_undefined()) {
            description = args[0].to_string();
        }
        auto symbol = Symbol::create(description);
        return Value(symbol.release());
    });
    
    // Add Symbol.for static method
    // Note: This is simplified - in a full implementation we'd set up the Symbol object with its methods
    
    // Map constructor
    register_function("Map", [](const std::vector<Value>& args) {
        (void)args; // Suppress unused parameter warning
        auto map_obj = std::make_unique<Map>();
        
        // Set up Map prototype methods
        map_obj->set_property("set", Value(ObjectFactory::create_native_function("set", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                return Map::map_set(ctx, args);
            }
        ).release()));
        
        map_obj->set_property("get", Value(ObjectFactory::create_native_function("get", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                return Map::map_get(ctx, args);
            }
        ).release()));
        
        map_obj->set_property("has", Value(ObjectFactory::create_native_function("has", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                return Map::map_has(ctx, args);
            }
        ).release()));
        
        map_obj->set_property("delete", Value(ObjectFactory::create_native_function("delete", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                return Map::map_delete(ctx, args);
            }
        ).release()));
        
        map_obj->set_property("clear", Value(ObjectFactory::create_native_function("clear", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                return Map::map_clear(ctx, args);
            }
        ).release()));
        
        return Value(map_obj.release());
    });
    
    // Set constructor
    register_function("Set", [](const std::vector<Value>& args) {
        (void)args; // Suppress unused parameter warning
        auto set_obj = std::make_unique<Set>();
        
        // Set up Set prototype methods
        set_obj->set_property("add", Value(ObjectFactory::create_native_function("add", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                return Set::set_add(ctx, args);
            }
        ).release()));
        
        set_obj->set_property("has", Value(ObjectFactory::create_native_function("has", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                return Set::set_has(ctx, args);
            }
        ).release()));
        
        set_obj->set_property("delete", Value(ObjectFactory::create_native_function("delete", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                return Set::set_delete(ctx, args);
            }
        ).release()));
        
        set_obj->set_property("clear", Value(ObjectFactory::create_native_function("clear", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                return Set::set_clear(ctx, args);
            }
        ).release()));
        
        return Value(set_obj.release());
    });
    
    // WeakMap constructor
    register_function("WeakMap", [](const std::vector<Value>& args) {
        (void)args; // Suppress unused parameter warning
        auto weakmap_obj = std::make_unique<WeakMap>();
        return Value(weakmap_obj.release());
    });
    
    // WeakSet constructor
    register_function("WeakSet", [](const std::vector<Value>& args) {
        (void)args; // Suppress unused parameter warning
        auto weakset_obj = std::make_unique<WeakSet>();
        return Value(weakset_obj.release());
    });
    
    // Error constructor is now set up in Context.cpp with ES2025 static methods
    
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

// ES6+ features setup removed due to segfault - will be re-added safely later
// void Engine::setup_es6_features() { ... }

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
// JIT and GC Method Implementations
//=============================================================================

void Engine::enable_jit(bool enable) {
    config_.enable_jit = enable;
    if (jit_compiler_) {
        jit_compiler_->enable_jit(enable);
    }
}

bool Engine::is_jit_enabled() const {
    return config_.enable_jit && jit_compiler_ && jit_compiler_->is_jit_enabled();
}

void Engine::set_jit_threshold(uint32_t threshold) {
    if (jit_compiler_) {
        jit_compiler_->set_hotspot_threshold(threshold);
    }
}

std::string Engine::get_jit_stats() const {
    if (!jit_compiler_) {
        return "JIT Compiler not initialized";
    }
    
    std::ostringstream oss;
    oss << "=== JIT Compiler Statistics ===" << std::endl;
    oss << "JIT Enabled: " << (is_jit_enabled() ? "Yes" : "No") << std::endl;
    oss << "Total Compilations: " << jit_compiler_->get_total_compilations() << std::endl;
    oss << "Cache Hits: " << jit_compiler_->get_cache_hits() << std::endl;
    oss << "Cache Misses: " << jit_compiler_->get_cache_misses() << std::endl;
    oss << "Hit Ratio: " << (jit_compiler_->get_cache_hit_ratio() * 100) << "%" << std::endl;
    
    return oss.str();
}

void Engine::enable_gc(bool enable) {
    if (garbage_collector_) {
        garbage_collector_->set_collection_mode(enable ? 
            GarbageCollector::CollectionMode::Automatic : 
            GarbageCollector::CollectionMode::Manual);
    }
}

void Engine::set_gc_mode(GarbageCollector::CollectionMode mode) {
    if (garbage_collector_) {
        garbage_collector_->set_collection_mode(mode);
    }
}

void Engine::force_gc() {
    if (garbage_collector_) {
        garbage_collector_->force_full_collection();
        total_gc_runs_++;
    }
}

std::string Engine::get_gc_stats() const {
    if (!garbage_collector_) {
        return "Garbage Collector not initialized";
    }
    
    std::ostringstream oss;
    const auto& stats = garbage_collector_->get_statistics();
    
    oss << "=== Garbage Collector Statistics ===" << std::endl;
    oss << "Total Allocations: " << stats.total_allocations << std::endl;
    oss << "Total Deallocations: " << stats.total_deallocations << std::endl;
    oss << "Total Collections: " << stats.total_collections << std::endl;
    oss << "Bytes Allocated: " << stats.bytes_allocated << std::endl;
    oss << "Bytes Freed: " << stats.bytes_freed << std::endl;
    oss << "Peak Memory Usage: " << stats.peak_memory_usage << " bytes" << std::endl;
    oss << "Current Heap Size: " << garbage_collector_->get_heap_size() << " bytes" << std::endl;
    oss << "Average GC Time: " << stats.average_gc_time.count() << "ms" << std::endl;
    
    return oss.str();
}

void Engine::set_heap_limit(size_t limit) {
    config_.max_heap_size = limit;
    if (garbage_collector_) {
        garbage_collector_->set_heap_size_limit(limit);
    }
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