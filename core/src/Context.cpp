#include "Context.h"
#include "Engine.h"
#include "Error.h"
#include "JSON.h"
#include "Promise.h"
#include "ProxyReflect.h"
#include "WebAPI.h"
#include "Async.h"
#include "BigInt.h"
#include "String.h"
#include "Symbol.h"
#include "MapSet.h"
#include <iostream>
#include <sstream>
#include <limits>
#include <cstdlib>

namespace Quanta {

// Static member initialization
uint32_t Context::next_context_id_ = 1;

//=============================================================================
// Context Implementation
//=============================================================================

Context::Context(Engine* engine, Type type) 
    : type_(type), state_(State::Running), context_id_(next_context_id_++),
      lexical_environment_(nullptr), variable_environment_(nullptr), this_binding_(nullptr),
      execution_depth_(0), global_object_(nullptr), current_exception_(), has_exception_(false), 
      return_value_(), has_return_value_(false), has_break_(false), has_continue_(false), engine_(engine) {
    
    if (type == Type::Global) {
        initialize_global_context();
    }
}

Context::Context(Engine* engine, Context* parent, Type type)
    : type_(type), state_(State::Running), context_id_(next_context_id_++),
      lexical_environment_(nullptr), variable_environment_(nullptr), this_binding_(nullptr),
      execution_depth_(0), global_object_(parent ? parent->global_object_ : nullptr),
      current_exception_(), has_exception_(false), return_value_(), has_return_value_(false), has_break_(false), has_continue_(false), engine_(engine) {
    
    // Inherit built-ins from parent
    if (parent) {
        built_in_objects_ = parent->built_in_objects_;
        built_in_functions_ = parent->built_in_functions_;
    }
}

Context::~Context() {
    // Clear call stack
    call_stack_.clear();
}

void Context::set_global_object(Object* global) {
    global_object_ = global;
}

bool Context::has_binding(const std::string& name) const {
    if (lexical_environment_) {
        return lexical_environment_->has_binding(name);
    }
    return false;
}

Value Context::get_binding(const std::string& name) const {
    if (!check_execution_depth()) {
        // Prevent infinite recursion
        const_cast<Context*>(this)->throw_exception(Value("Maximum execution depth exceeded"));
        return Value();
    }
    
    increment_execution_depth();
    Value result;
    
    if (lexical_environment_) {
        result = lexical_environment_->get_binding(name);
    } else {
        result = Value(); // undefined
    }
    
    decrement_execution_depth();
    return result;
}

bool Context::set_binding(const std::string& name, const Value& value) {
    if (lexical_environment_) {
        return lexical_environment_->set_binding(name, value);
    }
    return false;
}

bool Context::create_binding(const std::string& name, const Value& value, bool mutable_binding) {
    if (variable_environment_) {
        return variable_environment_->create_binding(name, value, mutable_binding);
    }
    return false;
}

bool Context::delete_binding(const std::string& name) {
    if (lexical_environment_) {
        return lexical_environment_->delete_binding(name);
    }
    return false;
}

void Context::push_frame(std::unique_ptr<StackFrame> frame) {
    if (is_stack_overflow()) {
        throw_exception(Value("RangeError: Maximum call stack size exceeded"));
        return;
    }
    call_stack_.push_back(std::move(frame));
}

std::unique_ptr<StackFrame> Context::pop_frame() {
    if (call_stack_.empty()) {
        return nullptr;
    }
    
    auto frame = std::move(call_stack_.back());
    call_stack_.pop_back();
    return frame;
}

StackFrame* Context::current_frame() const {
    if (call_stack_.empty()) {
        return nullptr;
    }
    return call_stack_.back().get();
}

void Context::throw_exception(const Value& exception) {
    current_exception_ = exception;
    has_exception_ = true;
    state_ = State::Thrown;
}

void Context::clear_exception() {
    current_exception_ = Value();
    has_exception_ = false;
    if (state_ == State::Thrown) {
        state_ = State::Running;
    }
}

void Context::throw_error(const std::string& message) {
    auto error = Error::create_error(message);
    throw_exception(Value(error.release()));
}

void Context::throw_type_error(const std::string& message) {
    auto error = Error::create_type_error(message);
    throw_exception(Value(error.release()));
}

void Context::throw_reference_error(const std::string& message) {
    auto error = Error::create_reference_error(message);
    throw_exception(Value(error.release()));
}

void Context::throw_syntax_error(const std::string& message) {
    auto error = Error::create_syntax_error(message);
    throw_exception(Value(error.release()));
}

void Context::throw_range_error(const std::string& message) {
    auto error = Error::create_range_error(message);
    throw_exception(Value(error.release()));
}

void Context::register_built_in_object(const std::string& name, Object* object) {
    built_in_objects_[name] = object;
    
    // Also bind to global object if available
    if (global_object_) {
        global_object_->set_property(name, Value(object));
    }
}

void Context::register_built_in_function(const std::string& name, Function* function) {
    built_in_functions_[name] = function;
    
    // Also bind to global object if available
    if (global_object_) {
        global_object_->set_property(name, Value(function));
    }
}

Object* Context::get_built_in_object(const std::string& name) const {
    auto it = built_in_objects_.find(name);
    return (it != built_in_objects_.end()) ? it->second : nullptr;
}

Function* Context::get_built_in_function(const std::string& name) const {
    auto it = built_in_functions_.find(name);
    return (it != built_in_functions_.end()) ? it->second : nullptr;
}

std::string Context::get_stack_trace() const {
    std::ostringstream oss;
    oss << "Stack trace:\n";
    
    // Print frames in reverse order (most recent first)
    for (int i = static_cast<int>(call_stack_.size()) - 1; i >= 0; --i) {
        oss << "  at " << call_stack_[i]->to_string() << "\n";
    }
    
    return oss.str();
}

std::vector<std::string> Context::get_variable_names() const {
    std::vector<std::string> names;
    
    if (lexical_environment_) {
        auto env_names = lexical_environment_->get_binding_names();
        names.insert(names.end(), env_names.begin(), env_names.end());
    }
    
    return names;
}

std::string Context::debug_string() const {
    std::ostringstream oss;
    oss << "Context(id=" << context_id_ 
        << ", type=" << static_cast<int>(type_)
        << ", state=" << static_cast<int>(state_)
        << ", stack_depth=" << stack_depth()
        << ", has_exception=" << has_exception_ << ")";
    return oss.str();
}

bool Context::check_execution_depth() const {
    return execution_depth_ < max_execution_depth_;
}

void Context::initialize_global_context() {
    // Create global object
    global_object_ = ObjectFactory::create_object().release();
    this_binding_ = global_object_;
    
    // Create global environment
    auto global_env = std::make_unique<Environment>(Environment::Type::Global);
    lexical_environment_ = global_env.release();
    variable_environment_ = lexical_environment_;
    
    // Initialize built-ins
    initialize_built_ins();
    setup_global_bindings();
}

void Context::initialize_built_ins() {
    // Create built-in objects (placeholder implementations)
    
    // Object constructor - create as a proper native function
    auto object_constructor = ObjectFactory::create_native_function("Object",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Object constructor implementation - for now just create empty object
            if (args.size() == 0) {
                return Value(ObjectFactory::create_object().release());
            }
            // TODO: Handle Object(value) constructor calls
            return Value(ObjectFactory::create_object().release());
        });
    
    // Add Object static methods
    // Object.keys(obj) - returns array of own enumerable property names
    auto keys_fn = ObjectFactory::create_native_function("keys", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value("TypeError: Object.keys requires at least 1 argument"));
                return Value();
            }
            
            // Check for null and undefined specifically
            if (args[0].is_null()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value("TypeError: Object.keys called on non-object"));
                return Value();
            }
            
            Object* obj = args[0].as_object();
            auto keys = obj->get_own_property_keys();  // Use direct method instead of enumerable
            
            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                result_array->set_element(i, Value(keys[i]));
            }
            
            return Value(result_array.release());
        });
    object_constructor->set_property("keys", Value(keys_fn.release()));
    
    // Object.values(obj) - returns array of own enumerable property values
    auto values_fn = ObjectFactory::create_native_function("values", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value("TypeError: Object.values requires at least 1 argument"));
                return Value();
            }
            
            // Check for null and undefined specifically
            if (args[0].is_null()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value("TypeError: Object.values called on non-object"));
                return Value();
            }
            
            Object* obj = args[0].as_object();
            auto keys = obj->get_own_property_keys();  // Use direct method instead of enumerable
            
            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                Value value = obj->get_property(keys[i]);
                result_array->set_element(i, value);
            }
            
            return Value(result_array.release());
        });
    object_constructor->set_property("values", Value(values_fn.release()));
    
    // Object.create(prototype) - creates new object with specified prototype
    auto create_fn = ObjectFactory::create_native_function("create",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* prototype = nullptr;
            if (args.size() > 0 && args[0].is_object()) {
                prototype = args[0].as_object();
            }
            
            auto new_obj = ObjectFactory::create_object(prototype);
            return Value(new_obj.release());
        });
    object_constructor->set_property("create", Value(create_fn.release()));
    
    register_built_in_object("Object", object_constructor.release());
    
    // Array constructor is now set up in Engine.cpp with proper constructor logic
    
    // Function constructor
    auto function_constructor = ObjectFactory::create_native_function("Function",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Function constructor implementation - basic placeholder
            return Value(ObjectFactory::create_function().release());
        });
    
    // Add Function.prototype methods that will be inherited by all functions
    // These will be added to the prototype, but for now add them as static methods
    
    // Function.prototype.call
    auto call_fn = ObjectFactory::create_native_function("call",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get the function that call was invoked on
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_exception(Value("Function.call called on non-function"));
                return Value();
            }
            
            Function* func = static_cast<Function*>(function_obj);
            Value this_arg = args.size() > 0 ? args[0] : Value();
            
            // Prepare arguments (skip the first 'this' argument)
            std::vector<Value> call_args;
            for (size_t i = 1; i < args.size(); i++) {
                call_args.push_back(args[i]);
            }
            
            return func->call(ctx, call_args, this_arg);
        });
    function_constructor->set_property("call", Value(call_fn.release()));
    
    // Function.prototype.apply
    auto apply_fn = ObjectFactory::create_native_function("apply",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get the function that apply was invoked on
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_exception(Value("Function.apply called on non-function"));
                return Value();
            }
            
            Function* func = static_cast<Function*>(function_obj);
            Value this_arg = args.size() > 0 ? args[0] : Value();
            
            // Prepare arguments from array
            std::vector<Value> call_args;
            if (args.size() > 1 && args[1].is_object()) {
                Object* args_array = args[1].as_object();
                if (args_array->is_array()) {
                    uint32_t length = args_array->get_length();
                    for (uint32_t i = 0; i < length; i++) {
                        call_args.push_back(args_array->get_element(i));
                    }
                }
            }
            
            return func->call(ctx, call_args, this_arg);
        });
    function_constructor->set_property("apply", Value(apply_fn.release()));
    
    register_built_in_object("Function", function_constructor.release());
    
    // String constructor - callable as function
    auto string_constructor = ObjectFactory::create_native_function("String",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            return Value(args[0].to_string());
        });
    register_built_in_object("String", string_constructor.release());
    
    // BigInt constructor - callable as function
    auto bigint_constructor = ObjectFactory::create_native_function("BigInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value("BigInt constructor requires an argument"));
                return Value();
            }
            
            try {
                if (args[0].is_number()) {
                    double num = args[0].as_number();
                    if (std::floor(num) != num) {
                        ctx.throw_exception(Value("Cannot convert non-integer Number to BigInt"));
                        return Value();
                    }
                    auto bigint = std::make_unique<BigInt>(static_cast<int64_t>(num));
                    return Value(bigint.release());
                } else if (args[0].is_string()) {
                    auto bigint = std::make_unique<BigInt>(args[0].to_string());
                    return Value(bigint.release());
                } else {
                    ctx.throw_exception(Value("Cannot convert value to BigInt"));
                    return Value();
                }
            } catch (const std::exception& e) {
                ctx.throw_exception(Value("Invalid BigInt: " + std::string(e.what())));
                return Value();
            }
        });
    register_built_in_object("BigInt", bigint_constructor.release());
    
    // Symbol constructor - NOT callable with 'new', only as function
    auto symbol_constructor = ObjectFactory::create_native_function("Symbol",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Symbol() can only be called as a function, not with 'new'
            std::string description = "";
            if (!args.empty() && !args[0].is_undefined()) {
                description = args[0].to_string();
            }
            
            auto symbol = Symbol::create(description);
            return Value(symbol.release());
        });
    
    // Add Symbol.for static method
    auto symbol_for_fn = ObjectFactory::create_native_function("for", Symbol::symbol_for);
    symbol_constructor->set_property("for", Value(symbol_for_fn.release()));
    
    // Add Symbol.keyFor static method
    auto symbol_key_for_fn = ObjectFactory::create_native_function("keyFor", Symbol::symbol_key_for);
    symbol_constructor->set_property("keyFor", Value(symbol_key_for_fn.release()));
    
    // Initialize well-known symbols and add them as static properties
    Symbol::initialize_well_known_symbols();
    
    // Add well-known symbols with null checks
    Symbol* iterator_sym = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_sym) {
        symbol_constructor->set_property("iterator", Value(iterator_sym));
    }
    
    Symbol* async_iterator_sym = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
    if (async_iterator_sym) {
        symbol_constructor->set_property("asyncIterator", Value(async_iterator_sym));
    }
    
    Symbol* match_sym = Symbol::get_well_known(Symbol::MATCH);
    if (match_sym) {
        symbol_constructor->set_property("match", Value(match_sym));
    }
    
    Symbol* replace_sym = Symbol::get_well_known(Symbol::REPLACE);
    if (replace_sym) {
        symbol_constructor->set_property("replace", Value(replace_sym));
    }
    
    Symbol* search_sym = Symbol::get_well_known(Symbol::SEARCH);
    if (search_sym) {
        symbol_constructor->set_property("search", Value(search_sym));
    }
    
    Symbol* split_sym = Symbol::get_well_known(Symbol::SPLIT);
    if (split_sym) {
        symbol_constructor->set_property("split", Value(split_sym));
    }
    
    Symbol* has_instance_sym = Symbol::get_well_known(Symbol::HAS_INSTANCE);
    if (has_instance_sym) {
        symbol_constructor->set_property("hasInstance", Value(has_instance_sym));
    }
    
    Symbol* is_concat_spreadable_sym = Symbol::get_well_known(Symbol::IS_CONCAT_SPREADABLE);
    if (is_concat_spreadable_sym) {
        symbol_constructor->set_property("isConcatSpreadable", Value(is_concat_spreadable_sym));
    }
    
    Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
    if (species_sym) {
        symbol_constructor->set_property("species", Value(species_sym));
    }
    
    Symbol* to_primitive_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    if (to_primitive_sym) {
        symbol_constructor->set_property("toPrimitive", Value(to_primitive_sym));
    }
    
    Symbol* to_string_tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
    if (to_string_tag_sym) {
        symbol_constructor->set_property("toStringTag", Value(to_string_tag_sym));
    }
    
    Symbol* unscopables_sym = Symbol::get_well_known(Symbol::UNSCOPABLES);
    if (unscopables_sym) {
        symbol_constructor->set_property("unscopables", Value(unscopables_sym));
    }
    
    register_built_in_object("Symbol", symbol_constructor.release());
    
    // 🚀 PROXY AND REFLECT - ES2023+ METAPROGRAMMING 🚀
    Proxy::setup_proxy(*this);
    Reflect::setup_reflect(*this);
    
    // 🚀 MAP AND SET COLLECTIONS 🚀
    Map::setup_map_prototype(*this);
    Set::setup_set_prototype(*this);
    
    // 🚀 WEAKMAP AND WEAKSET - ES2023+ WEAK COLLECTIONS 🚀
    WeakMap::setup_weakmap_prototype(*this);
    WeakSet::setup_weakset_prototype(*this);
    
    // Number constructor - callable as function with ES5 constants
    auto number_constructor = ObjectFactory::create_native_function("Number",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(0.0);
            return Value(args[0].to_number());
        });
    number_constructor->set_property("MAX_VALUE", Value(1.7976931348623157e+308));
    number_constructor->set_property("MIN_VALUE", Value(5e-324));
    number_constructor->set_property("NaN", Value(std::numeric_limits<double>::quiet_NaN()));
    number_constructor->set_property("POSITIVE_INFINITY", Value(std::numeric_limits<double>::infinity()));
    number_constructor->set_property("NEGATIVE_INFINITY", Value(-std::numeric_limits<double>::infinity()));
    register_built_in_object("Number", number_constructor.release());
    
    // Boolean constructor - callable as function
    auto boolean_constructor = ObjectFactory::create_native_function("Boolean",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            return Value(args[0].to_boolean());
        });
    register_built_in_object("Boolean", boolean_constructor.release());
    
    // Error constructor (with ES2025 static methods)
    auto error_constructor = ObjectFactory::create_native_function("Error",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused parameter warning
            auto error_obj = ObjectFactory::create_error(
                args.empty() ? "Error" : args[0].to_string()
            );
            return Value(error_obj.release());
        });
    
    // Add ES2025 Error static methods
    auto error_isError = ObjectFactory::create_native_function("isError", Error::isError);
    error_constructor->set_property("isError", Value(error_isError.release()));
    
    register_built_in_object("Error", error_constructor.release());
    
    // JSON object
    auto json_object = ObjectFactory::create_object();
    
    // JSON.parse
    auto json_parse = ObjectFactory::create_native_function("parse", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_parse(ctx, args);
        });
    json_object->set_property("parse", Value(json_parse.release()));
    
    // JSON.stringify  
    auto json_stringify = ObjectFactory::create_native_function("stringify",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_stringify(ctx, args);
        });
    json_object->set_property("stringify", Value(json_stringify.release()));
    
    register_built_in_object("JSON", json_object.release());
    
    // Math object is now created in Engine.cpp with complete function set
    
    // Helper function to add Promise methods to any Promise instance
    auto add_promise_methods = [](Promise* promise) {
        // Add .then method
        auto then_method = ObjectFactory::create_native_function("then",
            [promise](Context& ctx, const std::vector<Value>& args) -> Value {
                Function* on_fulfilled = nullptr;
                Function* on_rejected = nullptr;
                
                if (args.size() > 0 && args[0].is_function()) {
                    on_fulfilled = args[0].as_function();
                }
                if (args.size() > 1 && args[1].is_function()) {
                    on_rejected = args[1].as_function();
                }
                
                Promise* new_promise = promise->then(on_fulfilled, on_rejected);
                // TODO: Add methods to new_promise recursively
                return Value(new_promise);
            });
        promise->set_property("then", Value(then_method.release()));
        
        // Add .catch method
        auto catch_method = ObjectFactory::create_native_function("catch",
            [promise](Context& ctx, const std::vector<Value>& args) -> Value {
                Function* on_rejected = nullptr;
                if (args.size() > 0 && args[0].is_function()) {
                    on_rejected = args[0].as_function();
                }
                
                Promise* new_promise = promise->catch_method(on_rejected);
                // TODO: Add methods to new_promise recursively
                return Value(new_promise);
            });
        promise->set_property("catch", Value(catch_method.release()));
        
        // Add .finally method
        auto finally_method = ObjectFactory::create_native_function("finally",
            [promise](Context& ctx, const std::vector<Value>& args) -> Value {
                Function* on_finally = nullptr;
                if (args.size() > 0 && args[0].is_function()) {
                    on_finally = args[0].as_function();
                }
                
                Promise* new_promise = promise->finally_method(on_finally);
                // TODO: Add methods to new_promise recursively
                return Value(new_promise);
            });
        promise->set_property("finally", Value(finally_method.release()));
    };
    
    // Promise constructor
    auto promise_constructor = ObjectFactory::create_native_function("Promise",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value("Promise executor must be a function"));
                return Value();
            }
            
            auto promise = std::make_unique<Promise>(&ctx);
            
            // Execute the executor function with resolve and reject
            Function* executor = args[0].as_function();
            
            // Create resolve function
            auto resolve_fn = ObjectFactory::create_native_function("resolve",
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value value = args.empty() ? Value() : args[0];
                    promise_ptr->fulfill(value);
                    return Value();
                });
            
            // Create reject function
            auto reject_fn = ObjectFactory::create_native_function("reject",
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value reason = args.empty() ? Value() : args[0];
                    promise_ptr->reject(reason);
                    return Value();
                });
            
            // Call executor with resolve and reject
            std::vector<Value> executor_args = {
                Value(resolve_fn.release()),
                Value(reject_fn.release())
            };
            
            try {
                executor->call(ctx, executor_args);
            } catch (...) {
                promise->reject(Value("Promise executor threw"));
            }
            
            // Add Promise methods to this instance
            add_promise_methods(promise.get());
            
            return Value(promise.release());
        });
    
    // Promise.try - ES2025 static method
    auto promise_try = ObjectFactory::create_native_function("try",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value("Promise.try requires a function"));
                return Value();
            }
            
            Function* fn = args[0].as_function();
            auto promise = std::make_unique<Promise>(&ctx);
            
            try {
                Value result = fn->call(ctx, {});
                promise->fulfill(result);
            } catch (...) {
                promise->reject(Value("Function threw in Promise.try"));
            }
            
            return Value(promise.release());
        });
    promise_constructor->set_property("try", Value(promise_try.release()));
    
    // Promise.withResolvers - ES2025 static method
    auto promise_withResolvers = ObjectFactory::create_native_function("withResolvers",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; // Suppress unused parameter warning
            auto promise = std::make_unique<Promise>(&ctx);
            
            // Create resolve function
            auto resolve_fn = ObjectFactory::create_native_function("resolve",
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value value = args.empty() ? Value() : args[0];
                    promise_ptr->fulfill(value);
                    return Value();
                });
            
            // Create reject function
            auto reject_fn = ObjectFactory::create_native_function("reject",
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value reason = args.empty() ? Value() : args[0];
                    promise_ptr->reject(reason);
                    return Value();
                });
            
            // Create result object
            auto result_obj = ObjectFactory::create_object();
            result_obj->set_property("promise", Value(promise.release()));
            result_obj->set_property("resolve", Value(resolve_fn.release()));
            result_obj->set_property("reject", Value(reject_fn.release()));
            
            return Value(result_obj.release());
        });
    promise_constructor->set_property("withResolvers", Value(promise_withResolvers.release()));
    
    // Create Promise.prototype object
    auto promise_prototype = ObjectFactory::create_object();
    
    // Promise.prototype.then
    auto promise_then = ObjectFactory::create_native_function("then",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("Promise.prototype.then called on non-object"));
                return Value();
            }
            
            // Cast to Promise - need to verify this is actually a Promise
            Promise* promise = dynamic_cast<Promise*>(this_obj);
            if (!promise) {
                ctx.throw_exception(Value("Promise.prototype.then called on non-Promise"));
                return Value();
            }
            
            Function* on_fulfilled = nullptr;
            Function* on_rejected = nullptr;
            
            if (args.size() > 0 && args[0].is_function()) {
                on_fulfilled = args[0].as_function();
            }
            if (args.size() > 1 && args[1].is_function()) {
                on_rejected = args[1].as_function();
            }
            
            Promise* new_promise = promise->then(on_fulfilled, on_rejected);
            return Value(new_promise);
        });
    promise_prototype->set_property("then", Value(promise_then.release()));
    
    // Promise.prototype.catch
    auto promise_catch = ObjectFactory::create_native_function("catch",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("Promise.prototype.catch called on non-object"));
                return Value();
            }
            
            Promise* promise = dynamic_cast<Promise*>(this_obj);
            if (!promise) {
                ctx.throw_exception(Value("Promise.prototype.catch called on non-Promise"));
                return Value();
            }
            
            Function* on_rejected = nullptr;
            if (args.size() > 0 && args[0].is_function()) {
                on_rejected = args[0].as_function();
            }
            
            Promise* new_promise = promise->catch_method(on_rejected);
            return Value(new_promise);
        });
    promise_prototype->set_property("catch", Value(promise_catch.release()));
    
    // Promise.prototype.finally
    auto promise_finally = ObjectFactory::create_native_function("finally",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("Promise.prototype.finally called on non-object"));
                return Value();
            }
            
            Promise* promise = dynamic_cast<Promise*>(this_obj);
            if (!promise) {
                ctx.throw_exception(Value("Promise.prototype.finally called on non-Promise"));
                return Value();
            }
            
            Function* on_finally = nullptr;
            if (args.size() > 0 && args[0].is_function()) {
                on_finally = args[0].as_function();
            }
            
            Promise* new_promise = promise->finally_method(on_finally);
            return Value(new_promise);
        });
    promise_prototype->set_property("finally", Value(promise_finally.release()));
    
    // Set Promise.prototype on the constructor
    promise_constructor->set_property("prototype", Value(promise_prototype.release()));
    
    // Add Promise.resolve static method
    auto promise_resolve_static = ObjectFactory::create_native_function("resolve",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            Value value = args.empty() ? Value() : args[0];
            auto promise = std::make_unique<Promise>(&ctx);
            promise->fulfill(value);
            
            // Add instance methods to this promise
            add_promise_methods(promise.get());
            
            return Value(promise.release());
        });
    promise_constructor->set_property("resolve", Value(promise_resolve_static.release()));
    
    // Add Promise.reject static method
    auto promise_reject_static = ObjectFactory::create_native_function("reject",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            Value reason = args.empty() ? Value() : args[0];
            auto promise = std::make_unique<Promise>(&ctx);
            promise->reject(reason);
            
            // Add instance methods to this promise
            add_promise_methods(promise.get());
            
            return Value(promise.release());
        });
    promise_constructor->set_property("reject", Value(promise_reject_static.release()));
    
    register_built_in_object("Promise", promise_constructor.release());
    
    // Setup Proxy and Reflect using the proper implementation
    Proxy::setup_proxy(*this);
    Reflect::setup_reflect(*this);
    
    // Web APIs
    setup_web_apis();
}

void Context::setup_web_apis() {
    // Timer APIs
    auto setTimeout_fn = ObjectFactory::create_native_function("setTimeout", WebAPI::setTimeout);
    lexical_environment_->create_binding("setTimeout", Value(setTimeout_fn.release()), false);
    
    auto setInterval_fn = ObjectFactory::create_native_function("setInterval", WebAPI::setInterval);
    lexical_environment_->create_binding("setInterval", Value(setInterval_fn.release()), false);
    
    auto clearTimeout_fn = ObjectFactory::create_native_function("clearTimeout", WebAPI::clearTimeout);
    lexical_environment_->create_binding("clearTimeout", Value(clearTimeout_fn.release()), false);
    
    auto clearInterval_fn = ObjectFactory::create_native_function("clearInterval", WebAPI::clearInterval);
    lexical_environment_->create_binding("clearInterval", Value(clearInterval_fn.release()), false);
    
    // Enhanced Console API
    auto console_obj = ObjectFactory::create_object();
    console_obj->set_property("error", Value(ObjectFactory::create_native_function("error", WebAPI::console_error).release()));
    console_obj->set_property("warn", Value(ObjectFactory::create_native_function("warn", WebAPI::console_warn).release()));
    console_obj->set_property("info", Value(ObjectFactory::create_native_function("info", WebAPI::console_info).release()));
    console_obj->set_property("debug", Value(ObjectFactory::create_native_function("debug", WebAPI::console_debug).release()));
    console_obj->set_property("trace", Value(ObjectFactory::create_native_function("trace", WebAPI::console_trace).release()));
    console_obj->set_property("time", Value(ObjectFactory::create_native_function("time", WebAPI::console_time).release()));
    console_obj->set_property("timeEnd", Value(ObjectFactory::create_native_function("timeEnd", WebAPI::console_timeEnd).release()));
    
    // Update existing console object if it exists
    if (has_binding("console")) {
        Value existing_console = get_binding("console");
        if (existing_console.is_object()) {
            Object* console_existing = existing_console.as_object();
            console_existing->set_property("error", Value(ObjectFactory::create_native_function("error", WebAPI::console_error).release()));
            console_existing->set_property("warn", Value(ObjectFactory::create_native_function("warn", WebAPI::console_warn).release()));
            console_existing->set_property("info", Value(ObjectFactory::create_native_function("info", WebAPI::console_info).release()));
            console_existing->set_property("debug", Value(ObjectFactory::create_native_function("debug", WebAPI::console_debug).release()));
            console_existing->set_property("trace", Value(ObjectFactory::create_native_function("trace", WebAPI::console_trace).release()));
            console_existing->set_property("time", Value(ObjectFactory::create_native_function("time", WebAPI::console_time).release()));
            console_existing->set_property("timeEnd", Value(ObjectFactory::create_native_function("timeEnd", WebAPI::console_timeEnd).release()));
        }
    }
    
    // Complete Fetch API
    auto fetch_fn = ObjectFactory::create_native_function("fetch", WebAPI::fetch);
    lexical_environment_->create_binding("fetch", Value(fetch_fn.release()), false);
    
    // Headers constructor
    auto Headers_fn = ObjectFactory::create_native_function("Headers", WebAPI::Headers_constructor);
    lexical_environment_->create_binding("Headers", Value(Headers_fn.release()), false);
    
    // Request constructor  
    auto Request_fn = ObjectFactory::create_native_function("Request", WebAPI::Request_constructor);
    lexical_environment_->create_binding("Request", Value(Request_fn.release()), false);
    
    // Response constructor
    auto Response_fn = ObjectFactory::create_native_function("Response", WebAPI::Response_constructor);
    lexical_environment_->create_binding("Response", Value(Response_fn.release()), false);
    
    // DOM API - Document object
    auto document_obj = ObjectFactory::create_object();
    document_obj->set_property("getElementById", Value(ObjectFactory::create_native_function("getElementById", WebAPI::document_getElementById).release()));
    document_obj->set_property("createElement", Value(ObjectFactory::create_native_function("createElement", WebAPI::document_createElement).release()));
    document_obj->set_property("querySelector", Value(ObjectFactory::create_native_function("querySelector", WebAPI::document_querySelector).release()));
    document_obj->set_property("querySelectorAll", Value(ObjectFactory::create_native_function("querySelectorAll", WebAPI::document_querySelectorAll).release()));
    
    // Cookie API - Set up getter/setter property
    auto cookie_getter_fn = ObjectFactory::create_native_function("get cookie", WebAPI::document_getCookie);
    auto cookie_setter_fn = ObjectFactory::create_native_function("set cookie", WebAPI::document_setCookie);
    PropertyDescriptor cookie_desc(cookie_getter_fn.get(), cookie_setter_fn.get());
    std::cout << "DEBUG: Creating cookie property descriptor, is_accessor: " << cookie_desc.is_accessor_descriptor() << std::endl;
    document_obj->set_property_descriptor("cookie", cookie_desc);
    cookie_getter_fn.release(); // Transfer ownership to descriptor
    cookie_setter_fn.release(); // Transfer ownership to descriptor
    lexical_environment_->create_binding("document", Value(document_obj.release()), false);
    
    // Window API
    auto alert_fn = ObjectFactory::create_native_function("alert", WebAPI::window_alert);
    lexical_environment_->create_binding("alert", Value(alert_fn.release()), false);
    
    auto confirm_fn = ObjectFactory::create_native_function("confirm", WebAPI::window_confirm);
    lexical_environment_->create_binding("confirm", Value(confirm_fn.release()), false);
    
    auto prompt_fn = ObjectFactory::create_native_function("prompt", WebAPI::window_prompt);
    lexical_environment_->create_binding("prompt", Value(prompt_fn.release()), false);
    
    // Enhanced Storage API - localStorage with full interface
    auto localStorage_obj = ObjectFactory::create_object();
    localStorage_obj->set_property("getItem", Value(ObjectFactory::create_native_function("getItem", WebAPI::localStorage_getItem).release()));
    localStorage_obj->set_property("setItem", Value(ObjectFactory::create_native_function("setItem", WebAPI::localStorage_setItem).release()));
    localStorage_obj->set_property("removeItem", Value(ObjectFactory::create_native_function("removeItem", WebAPI::localStorage_removeItem).release()));
    localStorage_obj->set_property("clear", Value(ObjectFactory::create_native_function("clear", WebAPI::localStorage_clear).release()));
    localStorage_obj->set_property("key", Value(ObjectFactory::create_native_function("key", WebAPI::localStorage_key).release()));
    localStorage_obj->set_property("length", Value(ObjectFactory::create_native_function("length", WebAPI::localStorage_length).release()));
    localStorage_obj->set_property("addEventListener", Value(ObjectFactory::create_native_function("addEventListener", WebAPI::storage_addEventListener).release()));
    lexical_environment_->create_binding("localStorage", Value(localStorage_obj.release()), false);
    
    // Enhanced Storage API - sessionStorage with separate implementation
    auto sessionStorage_obj = ObjectFactory::create_object();
    sessionStorage_obj->set_property("getItem", Value(ObjectFactory::create_native_function("getItem", WebAPI::sessionStorage_getItem).release()));
    sessionStorage_obj->set_property("setItem", Value(ObjectFactory::create_native_function("setItem", WebAPI::sessionStorage_setItem).release()));
    sessionStorage_obj->set_property("removeItem", Value(ObjectFactory::create_native_function("removeItem", WebAPI::sessionStorage_removeItem).release()));
    sessionStorage_obj->set_property("clear", Value(ObjectFactory::create_native_function("clear", WebAPI::sessionStorage_clear).release()));
    sessionStorage_obj->set_property("key", Value(ObjectFactory::create_native_function("key", WebAPI::sessionStorage_key).release()));
    sessionStorage_obj->set_property("length", Value(ObjectFactory::create_native_function("length", WebAPI::sessionStorage_length).release()));
    sessionStorage_obj->set_property("addEventListener", Value(ObjectFactory::create_native_function("addEventListener", WebAPI::storage_addEventListener).release()));
    lexical_environment_->create_binding("sessionStorage", Value(sessionStorage_obj.release()), false);
    
    // Navigator Storage API - Modern storage management
    auto navigator_obj = ObjectFactory::create_object();
    
    // Basic navigator properties
    navigator_obj->set_property("userAgent", Value("Quanta/1.0 (JavaScript Engine)"));
    navigator_obj->set_property("platform", Value("Quanta"));
    navigator_obj->set_property("appName", Value("Quanta"));
    navigator_obj->set_property("appVersion", Value("1.0"));
    navigator_obj->set_property("language", Value("en-US"));
    navigator_obj->set_property("languages", Value("en-US,en"));
    navigator_obj->set_property("onLine", Value(true));
    navigator_obj->set_property("cookieEnabled", Value(true));
    
    // Navigator Storage API - Modern storage management
    auto storage_obj = ObjectFactory::create_object();
    storage_obj->set_property("estimate", Value(ObjectFactory::create_native_function("estimate", WebAPI::navigator_storage_estimate).release()));
    storage_obj->set_property("persist", Value(ObjectFactory::create_native_function("persist", WebAPI::navigator_storage_persist).release()));
    storage_obj->set_property("persisted", Value(ObjectFactory::create_native_function("persisted", WebAPI::navigator_storage_persisted).release()));
    navigator_obj->set_property("storage", Value(storage_obj.release()));
    
    // Navigator MediaDevices API - Modern media access
    auto mediaDevices_obj = ObjectFactory::create_object();
    auto getUserMedia_fn = ObjectFactory::create_native_function("getUserMedia", WebAPI::navigator_mediaDevices_getUserMedia);
    mediaDevices_obj->set_property("getUserMedia", Value(getUserMedia_fn.release()));
    
    auto enumerateDevices_fn = ObjectFactory::create_native_function("enumerateDevices", WebAPI::navigator_mediaDevices_enumerateDevices);
    mediaDevices_obj->set_property("enumerateDevices", Value(enumerateDevices_fn.release()));
    
    navigator_obj->set_property("mediaDevices", Value(mediaDevices_obj.release()));
    
    // Navigator Geolocation API - Location services
    auto geolocation_obj = ObjectFactory::create_object();
    auto getCurrentPosition_fn = ObjectFactory::create_native_function("getCurrentPosition", WebAPI::navigator_geolocation_getCurrentPosition);
    geolocation_obj->set_property("getCurrentPosition", Value(getCurrentPosition_fn.release()));
    
    auto watchPosition_fn = ObjectFactory::create_native_function("watchPosition", WebAPI::navigator_geolocation_watchPosition);
    geolocation_obj->set_property("watchPosition", Value(watchPosition_fn.release()));
    
    auto clearWatch_fn = ObjectFactory::create_native_function("clearWatch", WebAPI::navigator_geolocation_clearWatch);
    geolocation_obj->set_property("clearWatch", Value(clearWatch_fn.release()));
    
    navigator_obj->set_property("geolocation", Value(geolocation_obj.release()));
    
    // Navigator Clipboard API - Modern clipboard access
    auto clipboard_obj = ObjectFactory::create_object();
    auto readText_fn = ObjectFactory::create_native_function("readText", WebAPI::navigator_clipboard_readText);
    clipboard_obj->set_property("readText", Value(readText_fn.release()));
    
    auto writeText_fn = ObjectFactory::create_native_function("writeText", WebAPI::navigator_clipboard_writeText);
    clipboard_obj->set_property("writeText", Value(writeText_fn.release()));
    
    auto clipboardRead_fn = ObjectFactory::create_native_function("read", WebAPI::navigator_clipboard_read);
    clipboard_obj->set_property("read", Value(clipboardRead_fn.release()));
    
    auto clipboardWrite_fn = ObjectFactory::create_native_function("write", WebAPI::navigator_clipboard_write);
    clipboard_obj->set_property("write", Value(clipboardWrite_fn.release()));
    
    navigator_obj->set_property("clipboard", Value(clipboard_obj.release()));
    
    // Navigator Battery API - Device battery status
    auto getBattery_fn = ObjectFactory::create_native_function("getBattery", WebAPI::navigator_getBattery);
    navigator_obj->set_property("getBattery", Value(getBattery_fn.release()));
    
    // Navigator Vibration API - Haptic feedback
    auto vibrate_fn = ObjectFactory::create_native_function("vibrate", WebAPI::navigator_vibrate);
    navigator_obj->set_property("vibrate", Value(vibrate_fn.release()));
    
    lexical_environment_->create_binding("navigator", Value(navigator_obj.release()), false);
    
    // URL API
    auto URL_constructor_fn = ObjectFactory::create_native_function("URL", WebAPI::URL_constructor);
    lexical_environment_->create_binding("URL", Value(URL_constructor_fn.release()), false);
    
    // URLSearchParams API
    auto URLSearchParams_constructor_fn = ObjectFactory::create_native_function("URLSearchParams", WebAPI::URLSearchParams_constructor);
    lexical_environment_->create_binding("URLSearchParams", Value(URLSearchParams_constructor_fn.release()), false);
    
    // Event system - Global event functions
    auto addEventListener_fn = ObjectFactory::create_native_function("addEventListener", WebAPI::addEventListener);
    lexical_environment_->create_binding("addEventListener", Value(addEventListener_fn.release()), false);
    
    auto removeEventListener_fn = ObjectFactory::create_native_function("removeEventListener", WebAPI::removeEventListener);
    lexical_environment_->create_binding("removeEventListener", Value(removeEventListener_fn.release()), false);
    
    auto dispatchEvent_fn = ObjectFactory::create_native_function("dispatchEvent", WebAPI::dispatchEvent);
    lexical_environment_->create_binding("dispatchEvent", Value(dispatchEvent_fn.release()), false);
    
    // Audio API - HTML5 Audio element
    auto Audio_constructor_fn = ObjectFactory::create_native_function("Audio", WebAPI::Audio_constructor);
    lexical_environment_->create_binding("Audio", Value(Audio_constructor_fn.release()), false);
    
    // Complete Crypto API - Modern web security
    auto crypto_obj = ObjectFactory::create_object();
    
    // Basic crypto functions
    auto crypto_randomUUID_fn = ObjectFactory::create_native_function("randomUUID", WebAPI::crypto_randomUUID);
    crypto_obj->set_property("randomUUID", Value(crypto_randomUUID_fn.release()));
    
    auto crypto_getRandomValues_fn = ObjectFactory::create_native_function("getRandomValues", WebAPI::crypto_getRandomValues);
    crypto_obj->set_property("getRandomValues", Value(crypto_getRandomValues_fn.release()));
    
    // SubtleCrypto API - Advanced cryptographic operations
    auto subtle_obj = ObjectFactory::create_object();
    
    auto digest_fn = ObjectFactory::create_native_function("digest", WebAPI::crypto_subtle_digest);
    subtle_obj->set_property("digest", Value(digest_fn.release()));
    
    auto encrypt_fn = ObjectFactory::create_native_function("encrypt", WebAPI::crypto_subtle_encrypt);
    subtle_obj->set_property("encrypt", Value(encrypt_fn.release()));
    
    auto decrypt_fn = ObjectFactory::create_native_function("decrypt", WebAPI::crypto_subtle_decrypt);
    subtle_obj->set_property("decrypt", Value(decrypt_fn.release()));
    
    auto generateKey_fn = ObjectFactory::create_native_function("generateKey", WebAPI::crypto_subtle_generateKey);
    subtle_obj->set_property("generateKey", Value(generateKey_fn.release()));
    
    auto importKey_fn = ObjectFactory::create_native_function("importKey", WebAPI::crypto_subtle_importKey);
    subtle_obj->set_property("importKey", Value(importKey_fn.release()));
    
    auto exportKey_fn = ObjectFactory::create_native_function("exportKey", WebAPI::crypto_subtle_exportKey);
    subtle_obj->set_property("exportKey", Value(exportKey_fn.release()));
    
    auto sign_fn = ObjectFactory::create_native_function("sign", WebAPI::crypto_subtle_sign);
    subtle_obj->set_property("sign", Value(sign_fn.release()));
    
    auto verify_fn = ObjectFactory::create_native_function("verify", WebAPI::crypto_subtle_verify);
    subtle_obj->set_property("verify", Value(verify_fn.release()));
    
    crypto_obj->set_property("subtle", Value(subtle_obj.release()));
    lexical_environment_->create_binding("crypto", Value(crypto_obj.release()), false);
    
    // Complete File and Blob APIs - Modern file handling
    auto File_constructor_fn = ObjectFactory::create_native_function("File", WebAPI::File_constructor);
    lexical_environment_->create_binding("File", Value(File_constructor_fn.release()), false);
    
    auto Blob_constructor_fn = ObjectFactory::create_native_function("Blob", WebAPI::Blob_constructor);
    lexical_environment_->create_binding("Blob", Value(Blob_constructor_fn.release()), false);
    
    auto FileReader_constructor_fn = ObjectFactory::create_native_function("FileReader", WebAPI::FileReader_constructor);
    lexical_environment_->create_binding("FileReader", Value(FileReader_constructor_fn.release()), false);
    
    auto FormData_constructor_fn = ObjectFactory::create_native_function("FormData", WebAPI::FormData_constructor);
    lexical_environment_->create_binding("FormData", Value(FormData_constructor_fn.release()), false);
    
    // Complete Notification API - Desktop notifications
    auto Notification_constructor_fn = ObjectFactory::create_native_function("Notification", WebAPI::Notification_constructor);
    
    // Add Notification.requestPermission as static method
    auto requestPermission_fn = ObjectFactory::create_native_function("requestPermission", WebAPI::Notification_requestPermission);
    Notification_constructor_fn->set_property("requestPermission", Value(requestPermission_fn.release()));
    
    lexical_environment_->create_binding("Notification", Value(Notification_constructor_fn.release()), false);
    
    // Complete Media APIs - Modern multimedia
    auto MediaStream_constructor_fn = ObjectFactory::create_native_function("MediaStream", WebAPI::MediaStream_constructor);
    lexical_environment_->create_binding("MediaStream", Value(MediaStream_constructor_fn.release()), false);
    
    auto RTCPeerConnection_constructor_fn = ObjectFactory::create_native_function("RTCPeerConnection", WebAPI::RTCPeerConnection_constructor);
    lexical_environment_->create_binding("RTCPeerConnection", Value(RTCPeerConnection_constructor_fn.release()), false);
    
    // Complete History API - SPA navigation power
    auto history_obj = ObjectFactory::create_object();
    
    // History methods
    auto pushState_fn = ObjectFactory::create_native_function("pushState", WebAPI::history_pushState);
    history_obj->set_property("pushState", Value(pushState_fn.release()));
    
    auto replaceState_fn = ObjectFactory::create_native_function("replaceState", WebAPI::history_replaceState);
    history_obj->set_property("replaceState", Value(replaceState_fn.release()));
    
    auto back_fn = ObjectFactory::create_native_function("back", WebAPI::history_back);
    history_obj->set_property("back", Value(back_fn.release()));
    
    auto forward_fn = ObjectFactory::create_native_function("forward", WebAPI::history_forward);
    history_obj->set_property("forward", Value(forward_fn.release()));
    
    auto go_fn = ObjectFactory::create_native_function("go", WebAPI::history_go);
    history_obj->set_property("go", Value(go_fn.release()));
    
    // History properties (using accessor properties)
    auto length_fn = ObjectFactory::create_native_function("length", WebAPI::history_length);
    history_obj->set_property("length", Value(length_fn.release()));
    
    auto state_fn = ObjectFactory::create_native_function("state", WebAPI::history_state);
    history_obj->set_property("state", Value(state_fn.release()));
    
    auto scrollRestoration_fn = ObjectFactory::create_native_function("scrollRestoration", WebAPI::history_scrollRestoration);
    history_obj->set_property("scrollRestoration", Value(scrollRestoration_fn.release()));
    
    lexical_environment_->create_binding("history", Value(history_obj.release()), false);
    
    // Complete Location API - URL navigation
    auto location_obj = ObjectFactory::create_object();
    
    auto href_fn = ObjectFactory::create_native_function("href", WebAPI::location_href);
    location_obj->set_property("href", Value(href_fn.release()));
    
    auto protocol_fn = ObjectFactory::create_native_function("protocol", WebAPI::location_protocol);
    location_obj->set_property("protocol", Value(protocol_fn.release()));
    
    auto host_fn = ObjectFactory::create_native_function("host", WebAPI::location_host);
    location_obj->set_property("host", Value(host_fn.release()));
    
    auto hostname_fn = ObjectFactory::create_native_function("hostname", WebAPI::location_hostname);
    location_obj->set_property("hostname", Value(hostname_fn.release()));
    
    auto port_fn = ObjectFactory::create_native_function("port", WebAPI::location_port);
    location_obj->set_property("port", Value(port_fn.release()));
    
    auto pathname_fn = ObjectFactory::create_native_function("pathname", WebAPI::location_pathname);
    location_obj->set_property("pathname", Value(pathname_fn.release()));
    
    auto search_fn = ObjectFactory::create_native_function("search", WebAPI::location_search);
    location_obj->set_property("search", Value(search_fn.release()));
    
    auto hash_fn = ObjectFactory::create_native_function("hash", WebAPI::location_hash);
    location_obj->set_property("hash", Value(hash_fn.release()));
    
    auto origin_fn = ObjectFactory::create_native_function("origin", WebAPI::location_origin);
    location_obj->set_property("origin", Value(origin_fn.release()));
    
    auto assign_fn = ObjectFactory::create_native_function("assign", WebAPI::location_assign);
    location_obj->set_property("assign", Value(assign_fn.release()));
    
    auto replace_fn = ObjectFactory::create_native_function("replace", WebAPI::location_replace);
    location_obj->set_property("replace", Value(replace_fn.release()));
    
    auto reload_fn = ObjectFactory::create_native_function("reload", WebAPI::location_reload);
    location_obj->set_property("reload", Value(reload_fn.release()));
    
    auto locationToString_fn = ObjectFactory::create_native_function("toString", WebAPI::location_toString);
    location_obj->set_property("toString", Value(locationToString_fn.release()));
    
    lexical_environment_->create_binding("location", Value(location_obj.release()), false);
    
    // Complete Performance API - Web performance monitoring
    auto performance_obj = ObjectFactory::create_object();
    
    auto now_fn = ObjectFactory::create_native_function("now", WebAPI::performance_now);
    performance_obj->set_property("now", Value(now_fn.release()));
    
    auto mark_fn = ObjectFactory::create_native_function("mark", WebAPI::performance_mark);
    performance_obj->set_property("mark", Value(mark_fn.release()));
    
    auto measure_fn = ObjectFactory::create_native_function("measure", WebAPI::performance_measure);
    performance_obj->set_property("measure", Value(measure_fn.release()));
    
    auto clearMarks_fn = ObjectFactory::create_native_function("clearMarks", WebAPI::performance_clearMarks);
    performance_obj->set_property("clearMarks", Value(clearMarks_fn.release()));
    
    auto clearMeasures_fn = ObjectFactory::create_native_function("clearMeasures", WebAPI::performance_clearMeasures);
    performance_obj->set_property("clearMeasures", Value(clearMeasures_fn.release()));
    
    auto getEntries_fn = ObjectFactory::create_native_function("getEntries", WebAPI::performance_getEntries);
    performance_obj->set_property("getEntries", Value(getEntries_fn.release()));
    
    auto getEntriesByName_fn = ObjectFactory::create_native_function("getEntriesByName", WebAPI::performance_getEntriesByName);
    performance_obj->set_property("getEntriesByName", Value(getEntriesByName_fn.release()));
    
    auto getEntriesByType_fn = ObjectFactory::create_native_function("getEntriesByType", WebAPI::performance_getEntriesByType);
    performance_obj->set_property("getEntriesByType", Value(getEntriesByType_fn.release()));
    
    lexical_environment_->create_binding("performance", Value(performance_obj.release()), false);
    
    // Complete Screen API - Display information
    auto screen_obj = ObjectFactory::create_object();
    
    auto screenWidth_fn = ObjectFactory::create_native_function("width", WebAPI::screen_width);
    screen_obj->set_property("width", Value(screenWidth_fn.release()));
    
    auto screenHeight_fn = ObjectFactory::create_native_function("height", WebAPI::screen_height);
    screen_obj->set_property("height", Value(screenHeight_fn.release()));
    
    auto availWidth_fn = ObjectFactory::create_native_function("availWidth", WebAPI::screen_availWidth);
    screen_obj->set_property("availWidth", Value(availWidth_fn.release()));
    
    auto availHeight_fn = ObjectFactory::create_native_function("availHeight", WebAPI::screen_availHeight);
    screen_obj->set_property("availHeight", Value(availHeight_fn.release()));
    
    auto colorDepth_fn = ObjectFactory::create_native_function("colorDepth", WebAPI::screen_colorDepth);
    screen_obj->set_property("colorDepth", Value(colorDepth_fn.release()));
    
    auto pixelDepth_fn = ObjectFactory::create_native_function("pixelDepth", WebAPI::screen_pixelDepth);
    screen_obj->set_property("pixelDepth", Value(pixelDepth_fn.release()));
    
    // Screen orientation
    auto orientation_obj = ObjectFactory::create_object();
    auto angle_fn = ObjectFactory::create_native_function("angle", WebAPI::screen_orientation_angle);
    orientation_obj->set_property("angle", Value(angle_fn.release()));
    
    auto type_fn = ObjectFactory::create_native_function("type", WebAPI::screen_orientation_type);
    orientation_obj->set_property("type", Value(type_fn.release()));
    
    screen_obj->set_property("orientation", Value(orientation_obj.release()));
    
    lexical_environment_->create_binding("screen", Value(screen_obj.release()), false);
    
    // Observer APIs - IntersectionObserver and ResizeObserver
    auto IntersectionObserver_constructor_fn = ObjectFactory::create_native_function("IntersectionObserver", WebAPI::IntersectionObserver_constructor);
    lexical_environment_->create_binding("IntersectionObserver", Value(IntersectionObserver_constructor_fn.release()), false);
    
    auto ResizeObserver_constructor_fn = ObjectFactory::create_native_function("ResizeObserver", WebAPI::ResizeObserver_constructor);
    lexical_environment_->create_binding("ResizeObserver", Value(ResizeObserver_constructor_fn.release()), false);
    
    // RequestAnimationFrame API
    auto requestAnimationFrame_fn = ObjectFactory::create_native_function("requestAnimationFrame",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() >= 1) {
                static int animation_frame_id = 1;
                std::cout << "requestAnimationFrame: Scheduled callback for next frame (simulated)" << std::endl;
                return Value(static_cast<double>(animation_frame_id++));
            }
            return Value();
        });
    lexical_environment_->create_binding("requestAnimationFrame", Value(requestAnimationFrame_fn.release()), false);
    
    auto cancelAnimationFrame_fn = ObjectFactory::create_native_function("cancelAnimationFrame",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() >= 1) {
                double id = args[0].to_number();
                std::cout << "cancelAnimationFrame: Cancelled animation frame " << id << " (simulated)" << std::endl;
            }
            return Value();
        });
    lexical_environment_->create_binding("cancelAnimationFrame", Value(cancelAnimationFrame_fn.release()), false);
}

void Context::setup_global_bindings() {
    if (!lexical_environment_) return;
    
    // Global constants
    lexical_environment_->create_binding("undefined", Value(), false);
    lexical_environment_->create_binding("null", Value::null(), false);
    lexical_environment_->create_binding("true", Value(true), false);
    lexical_environment_->create_binding("false", Value(false), false);
    
    // Global values - create proper NaN value
    lexical_environment_->create_binding("NaN", Value(std::numeric_limits<double>::quiet_NaN()), false);
    lexical_environment_->create_binding("Infinity", Value(std::numeric_limits<double>::infinity()), false);
    
    // Missing global functions
    auto encode_uri_fn = ObjectFactory::create_native_function("encodeURI",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            std::string input = args[0].to_string();
            // Basic implementation - just return the input for now
            return Value(input);
        });
    lexical_environment_->create_binding("encodeURI", Value(encode_uri_fn.release()), false);
    
    auto decode_uri_fn = ObjectFactory::create_native_function("decodeURI",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            std::string input = args[0].to_string();
            // Basic implementation - just return the input for now
            return Value(input);
        });
    lexical_environment_->create_binding("decodeURI", Value(decode_uri_fn.release()), false);
    
    auto encode_uri_component_fn = ObjectFactory::create_native_function("encodeURIComponent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            std::string input = args[0].to_string();
            // Basic implementation - just return the input for now
            return Value(input);
        });
    lexical_environment_->create_binding("encodeURIComponent", Value(encode_uri_component_fn.release()), false);
    
    auto decode_uri_component_fn = ObjectFactory::create_native_function("decodeURIComponent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            std::string input = args[0].to_string();
            // Basic implementation - just return the input for now
            return Value(input);
        });
    lexical_environment_->create_binding("decodeURIComponent", Value(decode_uri_component_fn.release()), false);
    
    // BigInt constructor
    auto bigint_fn = ObjectFactory::create_native_function("BigInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("BigInt constructor requires an argument");
                return Value();
            }
            
            Value arg = args[0];
            if (arg.is_bigint()) {
                return arg; // Already a BigInt
            }
            
            if (arg.is_number()) {
                double num = arg.as_number();
                if (std::isnan(num) || std::isinf(num) || std::fmod(num, 1.0) != 0.0) {
                    ctx.throw_range_error("Cannot convert Number to BigInt");
                    return Value();
                }
                auto bigint = std::make_unique<Quanta::BigInt>(static_cast<int64_t>(num));
                return Value(bigint.release());
            }
            
            if (arg.is_string()) {
                try {
                    std::string str = arg.as_string()->str();
                    auto bigint = std::make_unique<Quanta::BigInt>(str);
                    return Value(bigint.release());
                } catch (const std::exception& e) {
                    ctx.throw_syntax_error("Cannot convert string to BigInt");
                    return Value();
                }
            }
            
            ctx.throw_type_error("Cannot convert value to BigInt");
            return Value();
        });
    lexical_environment_->create_binding("BigInt", Value(bigint_fn.release()), false);
    
    // Bind built-in objects to global environment
    for (const auto& pair : built_in_objects_) {
        bool bound = lexical_environment_->create_binding(pair.first, Value(pair.second), false);
        // Also ensure it's bound to global object for property access
        if (global_object_ && pair.second) {
            global_object_->set_property(pair.first, Value(pair.second));
        }
    }
}

//=============================================================================
// Return Value Handling
//=============================================================================

void Context::set_return_value(const Value& value) {
    return_value_ = value;
    has_return_value_ = true;
}

void Context::clear_return_value() {
    return_value_ = Value();
    has_return_value_ = false;
}

//=============================================================================
// Break/Continue Handling
//=============================================================================

void Context::set_break() {
    has_break_ = true;
}

void Context::set_continue() {
    has_continue_ = true;
}

void Context::clear_break_continue() {
    has_break_ = false;
    has_continue_ = false;
}

//=============================================================================
// StackFrame Implementation
//=============================================================================

StackFrame::StackFrame(Type type, Function* function, Object* this_binding)
    : type_(type), function_(function), this_binding_(this_binding),
      environment_(nullptr), program_counter_(0), line_number_(0), column_number_(0) {
}

Value StackFrame::get_argument(size_t index) const {
    if (index < arguments_.size()) {
        return arguments_[index];
    }
    return Value(); // undefined
}

bool StackFrame::has_local(const std::string& name) const {
    return local_variables_.find(name) != local_variables_.end();
}

Value StackFrame::get_local(const std::string& name) const {
    auto it = local_variables_.find(name);
    if (it != local_variables_.end()) {
        return it->second;
    }
    return Value(); // undefined
}

void StackFrame::set_local(const std::string& name, const Value& value) {
    local_variables_[name] = value;
}

void StackFrame::set_source_location(const std::string& location, uint32_t line, uint32_t column) {
    source_location_ = location;
    line_number_ = line;
    column_number_ = column;
}

std::string StackFrame::to_string() const {
    std::ostringstream oss;
    
    if (function_) {
        oss << "function";
    } else {
        oss << "anonymous";
    }
    
    if (!source_location_.empty()) {
        oss << " (" << source_location_;
        if (line_number_ > 0) {
            oss << ":" << line_number_;
            if (column_number_ > 0) {
                oss << ":" << column_number_;
            }
        }
        oss << ")";
    }
    
    return oss.str();
}

//=============================================================================
// Environment Implementation
//=============================================================================

Environment::Environment(Type type, Environment* outer)
    : type_(type), outer_environment_(outer), binding_object_(nullptr) {
}

Environment::Environment(Object* binding_object, Environment* outer)
    : type_(Type::Object), outer_environment_(outer), binding_object_(binding_object) {
}

bool Environment::has_binding(const std::string& name) const {
    if (has_own_binding(name)) {
        return true;
    }
    
    if (outer_environment_) {
        return outer_environment_->has_binding(name);
    }
    
    return false;
}

Value Environment::get_binding(const std::string& name) const {
    return get_binding_with_depth(name, 0);
}

Value Environment::get_binding_with_depth(const std::string& name, int depth) const {
    // Prevent infinite recursion
    if (depth > 100) {
        return Value(); // undefined
    }
    
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->get_property(name);
        } else {
            auto it = bindings_.find(name);
            if (it != bindings_.end()) {
                return it->second;
            }
        }
    }
    
    if (outer_environment_) {
        return outer_environment_->get_binding_with_depth(name, depth + 1);
    }
    
    return Value(); // undefined
}

bool Environment::set_binding(const std::string& name, const Value& value) {
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->set_property(name, value);
        } else {
            // Check if mutable
            if (is_mutable_binding(name)) {
                bindings_[name] = value;
                return true;
            }
            return false; // Immutable binding
        }
    }
    
    if (outer_environment_) {
        return outer_environment_->set_binding(name, value);
    }
    
    return false;
}

bool Environment::create_binding(const std::string& name, const Value& value, bool mutable_binding) {
    if (has_own_binding(name)) {
        return false; // Binding already exists
    }
    
    if (type_ == Type::Object && binding_object_) {
        return binding_object_->set_property(name, value);
    } else {
        bindings_[name] = value;
        mutable_flags_[name] = mutable_binding;
        initialized_flags_[name] = true;
        return true;
    }
}

bool Environment::delete_binding(const std::string& name) {
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->delete_property(name);
        } else {
            bindings_.erase(name);
            mutable_flags_.erase(name);
            initialized_flags_.erase(name);
            return true;
        }
    }
    
    return false;
}

bool Environment::is_mutable_binding(const std::string& name) const {
    auto it = mutable_flags_.find(name);
    return (it != mutable_flags_.end()) ? it->second : true; // Default to mutable
}

bool Environment::is_initialized_binding(const std::string& name) const {
    auto it = initialized_flags_.find(name);
    return (it != initialized_flags_.end()) ? it->second : false;
}

void Environment::initialize_binding(const std::string& name, const Value& value) {
    bindings_[name] = value;
    initialized_flags_[name] = true;
}

std::vector<std::string> Environment::get_binding_names() const {
    std::vector<std::string> names;
    
    if (type_ == Type::Object && binding_object_) {
        auto keys = binding_object_->get_own_property_keys();
        names.insert(names.end(), keys.begin(), keys.end());
    } else {
        for (const auto& pair : bindings_) {
            names.push_back(pair.first);
        }
    }
    
    return names;
}

std::string Environment::debug_string() const {
    std::ostringstream oss;
    oss << "Environment(type=" << static_cast<int>(type_)
        << ", bindings=" << bindings_.size() << ")";
    return oss.str();
}

bool Environment::has_own_binding(const std::string& name) const {
    if (type_ == Type::Object && binding_object_) {
        return binding_object_->has_own_property(name);
    } else {
        return bindings_.find(name) != bindings_.end();
    }
}

//=============================================================================
// ContextFactory Implementation
//=============================================================================

namespace ContextFactory {

std::unique_ptr<Context> create_global_context(Engine* engine) {
    return std::make_unique<Context>(engine, Context::Type::Global);
}

std::unique_ptr<Context> create_function_context(Engine* engine, Context* parent, Function* function) {
    auto context = std::make_unique<Context>(engine, parent, Context::Type::Function);
    
    // Create function environment
    auto func_env = std::make_unique<Environment>(Environment::Type::Function, parent->get_lexical_environment());
    context->set_lexical_environment(func_env.release());
    context->set_variable_environment(context->get_lexical_environment());
    
    return context;
}

std::unique_ptr<Context> create_eval_context(Engine* engine, Context* parent) {
    auto context = std::make_unique<Context>(engine, parent, Context::Type::Eval);
    
    // Eval context shares the parent's environment
    context->set_lexical_environment(parent->get_lexical_environment());
    context->set_variable_environment(parent->get_variable_environment());
    
    return context;
}

std::unique_ptr<Context> create_module_context(Engine* engine) {
    auto context = std::make_unique<Context>(engine, Context::Type::Module);
    
    // Create module environment
    auto module_env = std::make_unique<Environment>(Environment::Type::Module);
    context->set_lexical_environment(module_env.release());
    context->set_variable_environment(context->get_lexical_environment());
    
    return context;
}

} // namespace ContextFactory

} // namespace Quanta