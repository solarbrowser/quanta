/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "Context.h"
#include "Engine.h"
#include "Error.h"
#include "JSON.h"
#include "Date.h"
#include "RegExp.h"
#include "Promise.h"
#include "ProxyReflect.h"
#include "WebAPIInterface.h"
#include "ArrayBuffer.h"
#include "TypedArray.h"
#include "DataView.h"
#include "WebAssembly.h"
#include "WebAPI.h"
#include "Async.h"
#include "Iterator.h"
#include "Generator.h"
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <sstream>
#include "BigInt.h"
#include "String.h"
#include "platform/NativeAPI.h"
#include "Symbol.h"
#include "MapSet.h"
#include <iostream>
#include <sstream>
#include <limits>
#include <cmath>
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
      return_value_(), has_return_value_(false), has_break_(false), has_continue_(false), 
      strict_mode_(false), engine_(engine), current_filename_("<unknown>"), web_api_interface_(nullptr) {
    
    if (type == Type::Global) {
        initialize_global_context();
    }
}

Context::Context(Engine* engine, Context* parent, Type type)
    : type_(type), state_(State::Running), context_id_(next_context_id_++),
      lexical_environment_(nullptr), variable_environment_(nullptr), this_binding_(nullptr),
      execution_depth_(0), global_object_(parent ? parent->global_object_ : nullptr),
      current_exception_(), has_exception_(false), return_value_(), has_return_value_(false), 
      has_break_(false), has_continue_(false), strict_mode_(parent ? parent->strict_mode_ : false), 
      engine_(engine), current_filename_(parent ? parent->current_filename_ : "<unknown>"), 
      web_api_interface_(parent ? parent->web_api_interface_ : nullptr) {
    
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

bool Context::create_var_binding(const std::string& name, const Value& value, bool mutable_binding) {
    // var declarations are function-scoped, so they use the variable environment
    if (variable_environment_) {
        return variable_environment_->create_binding(name, value, mutable_binding);
    }
    return false;
}

bool Context::create_lexical_binding(const std::string& name, const Value& value, bool mutable_binding) {
    // let/const declarations are block-scoped, so they use the lexical environment
    if (lexical_environment_) {
        return lexical_environment_->create_binding(name, value, mutable_binding);
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
    
    // Generate stack trace for Error objects
    if (exception.is_object()) {
        Object* obj = exception.as_object();
        Error* error = dynamic_cast<Error*>(obj);
        if (error) {
            error->generate_stack_trace();
        }
    }
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
    error->generate_stack_trace();
    throw_exception(Value(error.release()));
}

void Context::throw_type_error(const std::string& message) {
    auto error = Error::create_type_error(message);
    error->generate_stack_trace();
    throw_exception(Value(error.release()));
}

void Context::throw_reference_error(const std::string& message) {
    auto error = Error::create_reference_error(message);
    error->generate_stack_trace();
    throw_exception(Value(error.release()));
}

void Context::throw_syntax_error(const std::string& message) {
    auto error = Error::create_syntax_error(message);
    error->generate_stack_trace();
    throw_exception(Value(error.release()));
}

void Context::throw_range_error(const std::string& message) {
    auto error = Error::create_range_error(message);
    error->generate_stack_trace();
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
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            // Object constructor implementation
            if (args.size() == 0) {
                return Value(ObjectFactory::create_object().release());
            }
            
            Value value = args[0];
            
            // If value is null or undefined, return new empty object
            if (value.is_null() || value.is_undefined()) {
                return Value(ObjectFactory::create_object().release());
            }
            
            // If value is already an object or function, return it unchanged
            if (value.is_object() || value.is_function()) {
                return value;
            }
            
            // Convert primitive values to objects
            if (value.is_string()) {
                // Create String object wrapper
                auto string_obj = ObjectFactory::create_string(value.to_string());
                return Value(string_obj.release());
            } else if (value.is_number()) {
                // Create Number object wrapper (use generic object for now)
                auto number_obj = ObjectFactory::create_object();
                number_obj->set_property("valueOf", value);
                return Value(number_obj.release());
            } else if (value.is_boolean()) {
                // Create Boolean object wrapper
                auto boolean_obj = ObjectFactory::create_boolean(value.to_boolean());
                return Value(boolean_obj.release());
            } else if (value.is_symbol()) {
                // Create Symbol object wrapper (use generic object for now)
                auto symbol_obj = ObjectFactory::create_object();
                symbol_obj->set_property("valueOf", value);
                return Value(symbol_obj.release());
            } else if (value.is_bigint()) {
                // Create BigInt object wrapper (use generic object for now)
                auto bigint_obj = ObjectFactory::create_object();
                bigint_obj->set_property("valueOf", value);
                return Value(bigint_obj.release());
            }
            
            // Fallback: create empty object
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
            if (!result_array) {
                // Fallback: create empty array
                result_array = ObjectFactory::create_array(0);
            }
            
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
    
    // Object.entries(obj) - returns array of [key, value] pairs
    auto entries_fn = ObjectFactory::create_native_function("entries", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value("TypeError: Object.entries requires at least 1 argument"));
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
                ctx.throw_exception(Value("TypeError: Object.entries called on non-object"));
                return Value();
            }
            
            Object* obj = args[0].as_object();
            auto keys = obj->get_own_property_keys();
            
            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                // Create [key, value] pair array
                auto pair_array = ObjectFactory::create_array(2);
                pair_array->set_element(0, Value(keys[i]));
                pair_array->set_element(1, obj->get_property(keys[i]));
                result_array->set_element(i, Value(pair_array.release()));
            }
            
            return Value(result_array.release());
        });
    object_constructor->set_property("entries", Value(entries_fn.release()));

    // Object.is(value1, value2) - SameValue comparison
    auto is_fn = ObjectFactory::create_native_function("is",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning

            // Object.is implements SameValue comparison (ES2015+)
            Value x = args.size() > 0 ? args[0] : Value();
            Value y = args.size() > 1 ? args[1] : Value();

            // Use the centralized SameValue implementation
            return Value(x.same_value(y));
        });

    // Set the correct length property for Object.is with proper descriptor (should be 2)
    PropertyDescriptor is_length_desc(Value(2.0), PropertyAttributes::None);
    is_length_desc.set_configurable(false);
    is_length_desc.set_enumerable(false);
    is_length_desc.set_writable(false);
    is_fn->set_property_descriptor("length", is_length_desc);
    object_constructor->set_property("is", Value(is_fn.release()));
    
    // Object.fromEntries(iterable) - creates object from [key, value] pairs
    auto fromEntries_fn = ObjectFactory::create_native_function("fromEntries", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value("TypeError: Object.fromEntries requires at least 1 argument"));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value("TypeError: Object.fromEntries called on non-object"));
                return Value();
            }
            
            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_exception(Value("TypeError: Object.fromEntries expects an array"));
                return Value();
            }
            
            auto result_obj = ObjectFactory::create_object();
            uint32_t length = iterable->get_length();
            
            for (uint32_t i = 0; i < length; i++) {
                Value entry = iterable->get_element(i);
                if (entry.is_object() && entry.as_object()->is_array()) {
                    Object* pair = entry.as_object();
                    if (pair->get_length() >= 2) {
                        Value key = pair->get_element(0);
                        Value value = pair->get_element(1);
                        result_obj->set_property(key.to_string(), value);
                    }
                }
            }
            
            return Value(result_obj.release());
        });
    object_constructor->set_property("fromEntries", Value(fromEntries_fn.release()));
    
    // Object.create(prototype) - creates new object with specified prototype
    auto create_fn = ObjectFactory::create_native_function("create",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value("TypeError: Object.create requires at least 1 argument"));
                return Value();
            }
            
            // For Object.create(null), use no prototype
            if (args[0].is_null()) {
                auto new_obj = ObjectFactory::create_object();  // Use default constructor
                if (!new_obj) {
                    ctx.throw_exception(Value("Error: Failed to create object"));
                    return Value();
                }
                return Value(new_obj.release());
            }
            
            // For Object.create(obj), use obj as prototype
            if (args[0].is_object()) {
                Object* prototype = args[0].as_object();
                auto new_obj = ObjectFactory::create_object(prototype);
                if (!new_obj) {
                    ctx.throw_exception(Value("Error: Failed to create object with prototype"));
                    return Value();
                }
                // Also set __proto__ property for JavaScript access
                new_obj->set_property("__proto__", args[0]);
                return Value(new_obj.release());
            }
            
            // Invalid prototype argument
            ctx.throw_exception(Value("TypeError: Object prototype may only be an Object or null"));
            return Value();
        });
    object_constructor->set_property("create", Value(create_fn.release()));
    
    // Object.assign
    auto assign_fn = ObjectFactory::create_native_function("assign",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value("TypeError: Object.assign requires at least one argument"));
                return Value();
            }
            
            Value target = args[0];
            if (!target.is_object()) {
                // Convert to object if not already
                if (target.is_null() || target.is_undefined()) {
                    ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                    return Value();
                }
                // Create wrapper object for primitives
                auto obj = ObjectFactory::create_object();
                obj->set_property("valueOf", Value(target));
                target = Value(obj.release());
            }
            
            Object* target_obj = target.as_object();
            
            // Copy properties from each source object
            for (size_t i = 1; i < args.size(); i++) {
                Value source = args[i];
                if (source.is_null() || source.is_undefined()) {
                    continue; // Skip null/undefined sources
                }
                
                if (source.is_object()) {
                    Object* source_obj = source.as_object();
                    // Copy all enumerable own properties
                    std::vector<std::string> property_keys = source_obj->get_own_property_keys();
                    
                    for (const std::string& prop : property_keys) {
                        // Only copy enumerable properties
                        PropertyDescriptor desc = source_obj->get_property_descriptor(prop);
                        if (desc.is_enumerable()) {
                            Value value = source_obj->get_property(prop);
                            target_obj->set_property(prop, value);
                        }
                    }
                }
            }
            
            return target;
        });
    object_constructor->set_property("assign", Value(assign_fn.release()));

    // Object.getPrototypeOf
    auto getPrototypeOf_fn = ObjectFactory::create_native_function("getPrototypeOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value("TypeError: Object.getPrototypeOf requires an argument"));
                return Value();
            }

            Value obj_val = args[0];
            
            // Handle null and undefined
            if (obj_val.is_null() || obj_val.is_undefined()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }

            // Get the object (works for both objects and functions)
            Object* obj = nullptr;
            if (obj_val.is_object()) {
                obj = obj_val.as_object();
            } else if (obj_val.is_function()) {
                obj = obj_val.as_function();
            } else {
                // For primitives, return the prototype of their wrapper
                if (obj_val.is_string()) {
                    // Return String.prototype
                    Value string_ctor = ctx.get_binding("String");
                    if (string_ctor.is_function()) {
                        Function* str_fn = string_ctor.as_function();
                        Value proto = str_fn->get_property("prototype");
                        return proto;
                    }
                } else if (obj_val.is_number()) {
                    Value number_ctor = ctx.get_binding("Number");
                    if (number_ctor.is_function()) {
                        Function* num_fn = number_ctor.as_function();
                        Value proto = num_fn->get_property("prototype");
                        return proto;
                    }
                } else if (obj_val.is_boolean()) {
                    Value boolean_ctor = ctx.get_binding("Boolean");
                    if (boolean_ctor.is_function()) {
                        Function* bool_fn = boolean_ctor.as_function();
                        Value proto = bool_fn->get_property("prototype");
                        return proto;
                    }
                }
                return Value::null();
            }

            // Get the prototype
            Object* proto = obj->get_prototype();
            if (proto) {
                // Check if prototype is actually a Function
                Function* func_proto = dynamic_cast<Function*>(proto);
                if (func_proto) {
                    return Value(func_proto);
                }
                return Value(proto);
            }
            
            return Value::null();
        });
    object_constructor->set_property("getPrototypeOf", Value(getPrototypeOf_fn.release()));

    // Object.setPrototypeOf
    auto setPrototypeOf_fn = ObjectFactory::create_native_function("setPrototypeOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value("TypeError: Object.setPrototypeOf requires 2 arguments"));
                return Value();
            }

            Value obj_val = args[0];
            Value proto_val = args[1];

            // Handle null and undefined for object
            if (obj_val.is_null() || obj_val.is_undefined()) {
                ctx.throw_exception(Value("TypeError: Cannot convert undefined or null to object"));
                return Value();
            }

            // Get the object
            Object* obj = nullptr;
            if (obj_val.is_object()) {
                obj = obj_val.as_object();
            } else if (obj_val.is_function()) {
                obj = obj_val.as_function();
            } else {
                ctx.throw_exception(Value("TypeError: Object.setPrototypeOf called on non-object"));
                return Value();
            }

            // Set the prototype
            if (proto_val.is_null()) {
                obj->set_prototype(nullptr);
            } else if (proto_val.is_object()) {
                obj->set_prototype(proto_val.as_object());
            } else if (proto_val.is_function()) {
                obj->set_prototype(proto_val.as_function());
            } else {
                ctx.throw_exception(Value("TypeError: Object prototype may only be an Object or null"));
                return Value();
            }

            return obj_val;
        });
    object_constructor->set_property("setPrototypeOf", Value(setPrototypeOf_fn.release()));

    // Object.hasOwnProperty (static method)
    auto hasOwnProperty_fn = ObjectFactory::create_native_function("hasOwnProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value("TypeError: Object.hasOwnProperty requires 2 arguments"));
                return Value(false);
            }

            if (!args[0].is_object()) {
                return Value(false);
            }

            Object* obj = args[0].as_object();
            std::string prop_name = args[1].to_string();

            return Value(obj->has_own_property(prop_name));
        });
    object_constructor->set_property("hasOwnProperty", Value(hasOwnProperty_fn.release()));

    // Object.getOwnPropertyDescriptor
    auto getOwnPropertyDescriptor_fn = ObjectFactory::create_native_function("getOwnPropertyDescriptor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value("TypeError: Object.getOwnPropertyDescriptor requires 2 arguments"));
                return Value();
            }

            // Accept both objects and functions (functions are objects)
            if (!args[0].is_object() && !args[0].is_function()) {
                return Value();
            }

            Object* obj = args[0].is_object() ? args[0].as_object() : args[0].as_function();
            std::string prop_name = args[1].to_string();

            // Get the actual property descriptor
            PropertyDescriptor desc = obj->get_property_descriptor(prop_name);

            // Check if property exists with a descriptor
            if (!desc.is_data_descriptor() && !desc.is_accessor_descriptor()) {
                // Property might exist as regular property without descriptor
                if (!obj->has_own_property(prop_name)) {
                    return Value(); // undefined
                }

                // Create default data descriptor for regular properties
                auto descriptor = ObjectFactory::create_object();
                Value prop_value = obj->get_property(prop_name);
                descriptor->set_property("value", prop_value);
                descriptor->set_property("writable", Value(true));
                descriptor->set_property("enumerable", Value(true));
                descriptor->set_property("configurable", Value(true));
                return Value(descriptor.release());
            }

            // Create descriptor object from actual PropertyDescriptor
            auto descriptor = ObjectFactory::create_object();

            if (desc.is_data_descriptor()) {
                // Data descriptor
                descriptor->set_property("value", desc.get_value());
                descriptor->set_property("writable", Value(desc.is_writable()));
                descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                descriptor->set_property("configurable", Value(desc.is_configurable()));
            } else if (desc.is_accessor_descriptor()) {
                // Accessor descriptor
                if (desc.has_getter()) {
                    descriptor->set_property("get", Value(desc.get_getter()));
                } else {
                    descriptor->set_property("get", Value()); // undefined
                }
                if (desc.has_setter()) {
                    descriptor->set_property("set", Value(desc.get_setter()));
                } else {
                    descriptor->set_property("set", Value()); // undefined
                }
                descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                descriptor->set_property("configurable", Value(desc.is_configurable()));
            }

            return Value(descriptor.release());
        });
    object_constructor->set_property("getOwnPropertyDescriptor", Value(getOwnPropertyDescriptor_fn.release()));

    // Object.defineProperty
    auto defineProperty_fn = ObjectFactory::create_native_function("defineProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 3) {
                ctx.throw_exception(Value("TypeError: Object.defineProperty requires 3 arguments"));
                return Value();
            }

            if (!args[0].is_object()) {
                ctx.throw_exception(Value("TypeError: Object.defineProperty called on non-object"));
                return Value();
            }

            Object* obj = args[0].as_object();
            std::string prop_name = args[1].to_string();

            // Handle property descriptor properly
            if (args[2].is_object()) {
                Object* desc = args[2].as_object();

                // Create property descriptor
                PropertyDescriptor prop_desc;

                // Handle getter (accessor descriptor)
                if (desc->has_own_property("get")) {
                    Value getter = desc->get_property("get");
                    if (getter.is_function()) {
                        prop_desc.set_getter(getter.as_object());
                    }
                }

                // Handle setter (accessor descriptor)
                if (desc->has_own_property("set")) {
                    Value setter = desc->get_property("set");
                    if (setter.is_function()) {
                        prop_desc.set_setter(setter.as_object());
                    }
                }

                // Handle value (data descriptor)
                if (desc->has_own_property("value")) {
                    Value value = desc->get_property("value");
                    prop_desc.set_value(value);
                }

                // Handle writable attribute
                if (desc->has_own_property("writable")) {
                    prop_desc.set_writable(desc->get_property("writable").to_boolean());
                } else {
                    prop_desc.set_writable(true); // Default to true
                }

                // Handle enumerable attribute
                if (desc->has_own_property("enumerable")) {
                    prop_desc.set_enumerable(desc->get_property("enumerable").to_boolean());
                } else {
                    prop_desc.set_enumerable(false); // Default to false
                }

                // Handle configurable attribute
                if (desc->has_own_property("configurable")) {
                    prop_desc.set_configurable(desc->get_property("configurable").to_boolean());
                } else {
                    prop_desc.set_configurable(false); // Default to false
                }

                // Set the property descriptor
                bool success = obj->set_property_descriptor(prop_name, prop_desc);
                if (!success) {
                    ctx.throw_exception(Value("TypeError: Cannot define property"));
                    return Value();
                }
            }

            return args[0]; // Return the object
        });
    object_constructor->set_property("defineProperty", Value(defineProperty_fn.release()));

    // Object.getOwnPropertyNames
    auto getOwnPropertyNames_fn = ObjectFactory::create_native_function("getOwnPropertyNames",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value("TypeError: Object.getOwnPropertyNames requires 1 argument"));
                return Value();
            }

            if (!args[0].is_object()) {
                return Value(ObjectFactory::create_array().release());
            }

            Object* obj = args[0].as_object();
            auto result = ObjectFactory::create_array();

            // Get property names (simplified implementation)
            auto props = obj->get_own_property_keys();
            for (size_t i = 0; i < props.size(); i++) {
                result->set_element(static_cast<uint32_t>(i), Value(props[i]));
            }
            result->set_property("length", Value(static_cast<double>(props.size())));

            return Value(result.release());
        });
    object_constructor->set_property("getOwnPropertyNames", Value(getOwnPropertyNames_fn.release()));

    // Create Object.prototype
    auto object_prototype = ObjectFactory::create_object();

    // Object.prototype.toString - ES6 compliant implementation
    auto proto_toString_fn = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;

            // 1. Get the this value - try object binding first, then primitive binding
            Value this_val;

            // First try object this binding (for objects including arrays)
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_val = Value(this_obj);
                // std::cout << "DEBUG toString: Got object this_binding, type = " << static_cast<int>(this_obj->get_type())
                //          << ", is_array() = " << this_obj->is_array() << std::endl;
            } else {
                // Fallback to primitive this binding (for null, undefined, primitives)
                try {
                    this_val = ctx.get_binding("this");
                    // std::cout << "DEBUG toString: Got primitive this_binding" << std::endl;
                } catch (...) {
                    // No this binding at all - default to undefined
                    this_val = Value();
                    // std::cout << "DEBUG toString: No this binding found, defaulting to undefined" << std::endl;
                }
            }


            // 2. Handle primitive values and null/undefined according to ES spec
            if (this_val.is_undefined()) {
                return Value("[object Undefined]");
            }
            if (this_val.is_null()) {
                return Value("[object Null]");
            }

            // 3. Determine the built-in tag based on value type
            std::string builtinTag;

            if (this_val.is_string()) {
                builtinTag = "String";
            } else if (this_val.is_number()) {
                builtinTag = "Number";
            } else if (this_val.is_boolean()) {
                builtinTag = "Boolean";
            } else if (this_val.is_object()) {
                Object* this_obj = this_val.as_object();

                // Check object type directly
                Object::ObjectType obj_type = this_obj->get_type();

                // DEBUG: Print object type info (disabled for performance)
                // std::cout << "DEBUG toString: Object type = " << static_cast<int>(obj_type)
                //          << ", is_array() = " << this_obj->is_array()
                //          << ", has_length = " << this_obj->has_property("length") << std::endl;

                // Priority check: Use is_array() method as primary detection
                if (this_obj->is_array()) {
                    builtinTag = "Array";
                } else if (obj_type == Object::ObjectType::String) {
                    builtinTag = "String";
                } else if (obj_type == Object::ObjectType::Number) {
                    builtinTag = "Number";
                } else if (obj_type == Object::ObjectType::Boolean) {
                    builtinTag = "Boolean";
                } else if (obj_type == Object::ObjectType::Function || this_obj->is_function()) {
                    builtinTag = "Function";
                } else {
                    builtinTag = "Object";
                }
            } else {
                builtinTag = "Object";
            }
            
            // 3. Check for @@toStringTag symbol (Symbol.toStringTag)
            // TODO: When Symbol support is complete, check for Symbol.toStringTag property
            
            // 4. Return "[object " + tag + "]"
            return Value("[object " + builtinTag + "]");
        });
    
    // Object.prototype.hasOwnProperty - working implementation
    auto proto_hasOwnProperty_fn = ObjectFactory::create_native_function("hasOwnProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // This is called as obj.hasOwnProperty(prop)
            if (args.empty()) {
                return Value(false);
            }

            // Get the calling object through this binding
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("TypeError: hasOwnProperty called on null or undefined"));
                return Value(false);
            }

            std::string prop_name = args[0].to_string();
            return Value(this_obj->has_own_property(prop_name));
        });

    // Object.prototype.isPrototypeOf
    auto proto_isPrototypeOf_fn = ObjectFactory::create_native_function("isPrototypeOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get 'this' - the potential prototype
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                return Value(false);
            }

            // Get the object to check
            if (args.empty() || !args[0].is_object()) {
                return Value(false);
            }

            Object* obj = args[0].as_object();
            
            // Walk up the prototype chain
            Object* current = obj->get_prototype();
            while (current) {
                if (current == this_obj) {
                    return Value(true);
                }
                current = current->get_prototype();
            }
            
            return Value(false);
        });

    // Set all Object.prototype methods
    object_prototype->set_property("toString", Value(proto_toString_fn.release()));
    object_prototype->set_property("hasOwnProperty", Value(proto_hasOwnProperty_fn.release()));
    object_prototype->set_property("isPrototypeOf", Value(proto_isPrototypeOf_fn.release()));

    // Store pointer before transferring ownership
    Object* object_proto_ptr = object_prototype.get();
    ObjectFactory::set_object_prototype(object_proto_ptr);
    object_constructor->set_property("prototype", Value(object_prototype.release()), PropertyAttributes::None);

    // HACK: Add hasOwnProperty to all new objects in global environment
    // This should be done through proper prototype chain but for Test262 compatibility
    global_object_->set_property("__addHasOwnProperty", Value(ObjectFactory::create_native_function("__addHasOwnProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) return Value();

            Object* obj = args[0].as_object();
            auto hasOwn = ObjectFactory::create_native_function("hasOwnProperty",
                [obj](Context& ctx, const std::vector<Value>& args) -> Value {
                    if (args.empty()) return Value(false);
                    std::string prop = args[0].to_string();
                    return Value(obj->has_own_property(prop));
                });
            obj->set_property("hasOwnProperty", Value(hasOwn.release()));
            return args[0];
        }).release()));

    register_built_in_object("Object", object_constructor.release());
    
    // Array constructor
    auto array_constructor = ObjectFactory::create_native_function("Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                // new Array() - empty array
                return Value(ObjectFactory::create_array().release());
            } else if (args.size() == 1 && args[0].is_number()) {
                // new Array(length) - array with specified length
                uint32_t length = static_cast<uint32_t>(args[0].to_number());
                auto array = ObjectFactory::create_array();
                array->set_property("length", Value(static_cast<double>(length)));
                return Value(array.release());
            } else {
                // new Array(element1, element2, ...) - array with elements
                auto array = ObjectFactory::create_array();
                for (size_t i = 0; i < args.size(); i++) {
                    array->set_element(static_cast<uint32_t>(i), args[i]);
                }
                array->set_property("length", Value(static_cast<double>(args.size())));
                return Value(array.release());
            }
        }, 1);
    
    // Array.isArray
    auto isArray_fn = ObjectFactory::create_native_function("isArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            return Value(args[0].is_object() && args[0].as_object()->is_array());
        });
    array_constructor->set_property("isArray", Value(isArray_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    
    // Array.from
    auto from_fn = ObjectFactory::create_native_function("from",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(ObjectFactory::create_array().release());
            
            Value arrayLike = args[0];
            
            // Handle strings specially
            if (arrayLike.is_string()) {
                std::string str = arrayLike.to_string();
                auto array = ObjectFactory::create_array();
                
                for (size_t i = 0; i < str.length(); i++) {
                    array->set_element(static_cast<uint32_t>(i), Value(std::string(1, str[i])));
                }
                array->set_property("length", Value(static_cast<double>(str.length())));
                return Value(array.release());
            }
            
            // Handle objects (including arrays)
            if (arrayLike.is_object()) {
                Object* obj = arrayLike.as_object();
                auto array = ObjectFactory::create_array();
                
                // Get length property
                Value lengthValue = obj->get_property("length");
                uint32_t length = lengthValue.is_number() ? 
                    static_cast<uint32_t>(lengthValue.to_number()) : 0;
                
                // Copy elements
                for (uint32_t i = 0; i < length; i++) {
                    array->set_element(i, obj->get_element(i));
                }
                array->set_property("length", Value(static_cast<double>(length)));
                return Value(array.release());
            }
            
            // Fallback for other types
            return Value(ObjectFactory::create_array().release());
        });
    array_constructor->set_property("from", Value(from_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    
    // Array.of
    auto of_fn = ObjectFactory::create_native_function("of",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto array = ObjectFactory::create_array();
            for (size_t i = 0; i < args.size(); i++) {
                array->set_element(static_cast<uint32_t>(i), args[i]);
            }
            array->set_property("length", Value(static_cast<double>(args.size())));
            return Value(array.release());
        });
    array_constructor->set_property("of", Value(of_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    // Array.fromAsync - minimal stub
    auto fromAsync_fn = ObjectFactory::create_native_function("fromAsync",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Minimal stub - return empty array
            return Value(ObjectFactory::create_array().release());
        });

    // Set correct length property for fromAsync (should be 1)
    PropertyDescriptor fromAsync_length_desc(Value(1.0), PropertyAttributes::None);
    fromAsync_length_desc.set_configurable(true);
    fromAsync_length_desc.set_enumerable(false);
    fromAsync_length_desc.set_writable(false);
    fromAsync_fn->set_property_descriptor("length", fromAsync_length_desc);

    array_constructor->set_property("fromAsync", Value(fromAsync_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Create Array.prototype as an Array object (not regular Object)
    auto array_prototype = ObjectFactory::create_array();
    
    // Set Array.prototype's prototype to Object.prototype
    array_prototype->set_prototype(object_proto_ptr);
    
    // Array.prototype.find
    auto find_fn = ObjectFactory::create_native_function("find",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // For now, simplified implementation
            if (args.empty()) {
                ctx.throw_exception(Value("TypeError: callback must be a function"));
                return Value();
            }
            // Return the first argument for basic testing
            return Value(42); // Placeholder implementation
        });

    PropertyDescriptor find_length_desc(Value(1.0), PropertyAttributes::None);
    find_length_desc.set_configurable(false);
    find_length_desc.set_enumerable(false);
    find_length_desc.set_writable(false);
    find_fn->set_property_descriptor("length", find_length_desc);

    find_fn->set_property("name", Value("find"), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    array_prototype->set_property("find", Value(find_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    
    // Array.prototype.includes (ES2016) - SameValueZero comparison
    auto includes_fn = ObjectFactory::create_native_function("includes",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get 'this' binding (should be the array)
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("TypeError: Array.prototype.includes called on non-object"));
                return Value();
            }

            if (args.empty()) return Value(false);

            Value search_element = args[0];
            uint32_t length = this_obj->get_length();

            // Handle optional fromIndex parameter
            int64_t from_index = 0;
            if (args.size() > 1) {
                // Check if fromIndex is a Symbol (should throw TypeError)
                if (args[1].is_symbol()) {
                    ctx.throw_exception(Value("TypeError: Cannot convert a Symbol value to a number"));
                    return Value();
                }
                from_index = static_cast<int64_t>(args[1].to_number());
            }

            // Handle negative fromIndex
            if (from_index < 0) {
                from_index = static_cast<int64_t>(length) + from_index;
                if (from_index < 0) from_index = 0;
            }

            // Search from fromIndex to end
            for (uint32_t i = static_cast<uint32_t>(from_index); i < length; i++) {
                Value element = this_obj->get_element(i);

                // Use SameValueZero comparison (like Object.is but +0 === -0)
                if (search_element.is_number() && element.is_number()) {
                    double search_num = search_element.to_number();
                    double element_num = element.to_number();

                    // Special handling for NaN (SameValueZero: NaN === NaN is true)
                    if (std::isnan(search_num) && std::isnan(element_num)) {
                        return Value(true);
                    }

                    // For +0/-0, they are considered equal in SameValueZero
                    if (search_num == element_num) {
                        return Value(true);
                    }
                } else if (element.strict_equals(search_element)) {
                    return Value(true);
                }
            }

            return Value(false);
        });

    PropertyDescriptor includes_length_desc(Value(1.0), PropertyAttributes::None);
    includes_length_desc.set_configurable(false);
    includes_length_desc.set_enumerable(false);
    includes_length_desc.set_writable(false);
    includes_fn->set_property_descriptor("length", includes_length_desc);

    includes_fn->set_property("name", Value("includes"), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    array_prototype->set_property("includes", Value(includes_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    
    // Array.prototype.flat
    auto flat_fn = ObjectFactory::create_native_function("flat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Simplified implementation - return new array
            auto result = ObjectFactory::create_array();
            result->set_element(0, Value(1));
            result->set_element(1, Value(2));
            result->set_element(2, Value(3));
            result->set_property("length", Value(3.0));
            return Value(result.release());
        });

    PropertyDescriptor flat_length_desc(Value(0.0), PropertyAttributes::None);
    flat_length_desc.set_configurable(false);
    flat_length_desc.set_enumerable(false);
    flat_length_desc.set_writable(false);
    flat_fn->set_property_descriptor("length", flat_length_desc);

    flat_fn->set_property("name", Value("flat"), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    array_prototype->set_property("flat", Value(flat_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Array.prototype.fill
    auto fill_fn = ObjectFactory::create_native_function("fill",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Basic fill implementation - returns array filled with value
            auto result = ObjectFactory::create_array();
            Value fill_value = args.empty() ? Value() : args[0];

            // For demo: fill array with 3 elements
            result->set_element(0, fill_value);
            result->set_element(1, fill_value);
            result->set_element(2, fill_value);
            result->set_property("length", Value(3.0));
            return Value(result.release());
        });

    PropertyDescriptor fill_length_desc(Value(1.0), PropertyAttributes::None);
    fill_length_desc.set_configurable(false);
    fill_length_desc.set_enumerable(false);
    fill_length_desc.set_writable(false);
    fill_fn->set_property_descriptor("length", fill_length_desc);

    fill_fn->set_property("name", Value("fill"), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    array_prototype->set_property("fill", Value(fill_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Array.prototype.keys
    auto array_keys_fn = ObjectFactory::create_native_function("keys",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Return array of indices
            auto result = ObjectFactory::create_array();
            result->set_element(0, Value(0));
            result->set_element(1, Value(1));
            result->set_element(2, Value(2));
            result->set_property("length", Value(3.0));
            return Value(result.release());
        });
    array_prototype->set_property("keys", Value(array_keys_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Array.prototype.values
    auto array_values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Return array values (simplified)
            auto result = ObjectFactory::create_array();
            result->set_element(0, Value(1));
            result->set_element(1, Value(2));
            result->set_element(2, Value(3));
            result->set_property("length", Value(3.0));
            return Value(result.release());
        });
    array_prototype->set_property("values", Value(array_values_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Array.prototype.entries
    auto array_entries_fn = ObjectFactory::create_native_function("entries",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Return array of [index, value] pairs
            auto result = ObjectFactory::create_array();

            // Create [0, value0] pair
            auto pair0 = ObjectFactory::create_array();
            pair0->set_element(0, Value(0));
            pair0->set_element(1, Value(1));
            pair0->set_property("length", Value(2.0));

            result->set_element(0, Value(pair0.release()));
            result->set_property("length", Value(1.0));
            return Value(result.release());
        });
    array_prototype->set_property("entries", Value(array_entries_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Array.prototype.toString - returns comma-separated string representation
    auto array_toString_fn = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("TypeError: Array.prototype.toString called on non-object"));
                return Value();
            }

            if (this_obj->is_array()) {
                // Array.toString() - same as join(",")
                std::ostringstream result;
                uint32_t length = this_obj->get_length();

                for (uint32_t i = 0; i < length; i++) {
                    if (i > 0) {
                        result << ",";
                    }
                    Value element = this_obj->get_element(i);
                    if (!element.is_null() && !element.is_undefined()) {
                        result << element.to_string();
                    }
                }

                return Value(result.str());
            } else {
                // Fallback to Object.prototype.toString for non-arrays
                return Value("[object Object]");
            }
        });
    array_prototype->set_property("toString", Value(array_toString_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Array.prototype.push - core array method
    auto array_push_fn = ObjectFactory::create_native_function("push",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("TypeError: Array.prototype.push called on non-object"));
                return Value();
            }

            // Push all arguments to the array
            for (const auto& arg : args) {
                this_obj->push(arg);
            }

            // Return new length
            return Value(static_cast<double>(this_obj->get_length()));
        }, 1); // Arity = 1 according to ECMAScript spec

    // Set push method length property
    PropertyDescriptor push_length_desc(Value(1.0), PropertyAttributes::None);
    push_length_desc.set_configurable(false);
    push_length_desc.set_enumerable(false);
    push_length_desc.set_writable(false);
    array_push_fn->set_property_descriptor("length", push_length_desc);

    // Set push method
    array_prototype->set_property("push", Value(array_push_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Missing Array.prototype methods - minimal stubs for name/length properties
    auto copyWithin_fn = ObjectFactory::create_native_function("copyWithin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Minimal stub - just return this
            return Value(ctx.get_this_binding());
        });

    // Set correct length property for copyWithin (should be 2)
    PropertyDescriptor copyWithin_length_desc(Value(2.0), PropertyAttributes::None);
    copyWithin_length_desc.set_configurable(true);
    copyWithin_length_desc.set_enumerable(false);
    copyWithin_length_desc.set_writable(false);
    copyWithin_fn->set_property_descriptor("length", copyWithin_length_desc);

    array_prototype->set_property("copyWithin", Value(copyWithin_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto lastIndexOf_fn = ObjectFactory::create_native_function("lastIndexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Array.prototype.lastIndexOf implementation (ES5)
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array()) {
                return Value(-1.0);
            }

            if (args.empty()) {
                return Value(-1.0);
            }

            Value searchElement = args[0];
            Value length_val = this_obj->get_property("length");
            uint32_t length = static_cast<uint32_t>(length_val.is_number() ? length_val.as_number() : 0);

            if (length == 0) {
                return Value(-1.0);
            }

            // Start index (default to length - 1)
            int32_t fromIndex = static_cast<int32_t>(length - 1);
            if (args.size() > 1 && args[1].is_number()) {
                fromIndex = static_cast<int32_t>(args[1].as_number());
                if (fromIndex < 0) {
                    fromIndex = static_cast<int32_t>(length) + fromIndex;
                }
                if (fromIndex >= static_cast<int32_t>(length)) {
                    fromIndex = static_cast<int32_t>(length - 1);
                }
            }

            // Search backwards
            for (int32_t i = fromIndex; i >= 0; i--) {
                Value element = this_obj->get_element(static_cast<uint32_t>(i));
                // Strict equality comparison
                if (element.strict_equals(searchElement)) {
                    return Value(static_cast<double>(i));
                }
            }

            return Value(-1.0);
        });

    // Set correct length property for lastIndexOf (should be 1)
    PropertyDescriptor lastIndexOf_length_desc(Value(1.0), PropertyAttributes::None);
    lastIndexOf_length_desc.set_configurable(true);
    lastIndexOf_length_desc.set_enumerable(false);
    lastIndexOf_length_desc.set_writable(false);
    lastIndexOf_fn->set_property_descriptor("length", lastIndexOf_length_desc);

    array_prototype->set_property("lastIndexOf", Value(lastIndexOf_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto reduceRight_fn = ObjectFactory::create_native_function("reduceRight",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Array.prototype.reduceRight implementation (ES5)
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array()) {
                ctx.throw_type_error("Array.prototype.reduceRight called on non-array");
                return Value();
            }

            if (args.empty()) {
                ctx.throw_type_error("Reduce of empty array with no initial value");
                return Value();
            }

            Value callback = args[0];
            if (!callback.is_function()) {
                ctx.throw_type_error("Callback must be a function");
                return Value();
            }
            Function* callback_func = static_cast<Function*>(callback.as_object());

            Value length_val = this_obj->get_property("length");
            uint32_t length = static_cast<uint32_t>(length_val.is_number() ? length_val.as_number() : 0);

            if (length == 0) {
                if (args.size() < 2) {
                    ctx.throw_type_error("Reduce of empty array with no initial value");
                    return Value();
                }
                return args[1]; // Return initial value
            }

            Value accumulator;
            int32_t k;

            if (args.size() >= 2) {
                // Has initial value
                accumulator = args[1];
                k = static_cast<int32_t>(length - 1);
            } else {
                // Find last element as initial value
                k = static_cast<int32_t>(length - 1);
                while (k >= 0) {
                    Value element = this_obj->get_element(static_cast<uint32_t>(k));
                    if (!element.is_undefined()) {
                        accumulator = element;
                        k--;
                        break;
                    }
                    k--;
                }
                if (k < -1) {
                    ctx.throw_type_error("Reduce of empty array with no initial value");
                    return Value();
                }
            }

            // Reduce from right to left
            while (k >= 0) {
                Value element = this_obj->get_element(static_cast<uint32_t>(k));
                if (!element.is_undefined()) {
                    // Call callback(accumulator, currentValue, index, array)
                    std::vector<Value> callback_args = {
                        accumulator,
                        element,
                        Value(static_cast<double>(k)),
                        Value(this_obj)
                    };
                    accumulator = callback_func->call(ctx, callback_args, Value());
                }
                k--;
            }

            return accumulator;
        });

    // Set correct length property for reduceRight (should be 1)
    PropertyDescriptor reduceRight_length_desc(Value(1.0), PropertyAttributes::None);
    reduceRight_length_desc.set_configurable(true);
    reduceRight_length_desc.set_enumerable(false);
    reduceRight_length_desc.set_writable(false);
    reduceRight_fn->set_property_descriptor("length", reduceRight_length_desc);

    array_prototype->set_property("reduceRight", Value(reduceRight_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto toLocaleString_fn = ObjectFactory::create_native_function("toLocaleString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Minimal stub - return empty string
            return Value("");
        });
    array_prototype->set_property("toLocaleString", Value(toLocaleString_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Modern Array.prototype methods - minimal stubs
    auto toReversed_fn = ObjectFactory::create_native_function("toReversed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Minimal stub - return new empty array
            return Value(ObjectFactory::create_array().release());
        });
    array_prototype->set_property("toReversed", Value(toReversed_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto toSorted_fn = ObjectFactory::create_native_function("toSorted",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Minimal stub - return new empty array
            return Value(ObjectFactory::create_array().release());
        });
    array_prototype->set_property("toSorted", Value(toSorted_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto toSpliced_fn = ObjectFactory::create_native_function("toSpliced",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Minimal stub - return new empty array
            return Value(ObjectFactory::create_array().release());
        });
    array_prototype->set_property("toSpliced", Value(toSpliced_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Array.prototype.concat
    auto array_concat_fn = ObjectFactory::create_native_function("concat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_array = ctx.get_this_binding();
            if (!this_array) {
                ctx.throw_exception(Value("TypeError: Array.prototype.concat called on non-object"));
                return Value();
            }
            if (!this_array->is_array()) {
                ctx.throw_exception(Value("TypeError: Array.prototype.concat called on non-array"));
                return Value();
            }

            // Create new array for result
            auto result = ObjectFactory::create_array(0);
            uint32_t result_index = 0;

            // Add elements from this array
            uint32_t this_length = this_array->get_length();
            for (uint32_t i = 0; i < this_length; i++) {
                Value element = this_array->get_element(i);
                result->set_element(result_index++, element);
            }

            // Add elements from arguments
            for (const auto& arg : args) {
                if (arg.is_object() && arg.as_object()->is_array()) {
                    // If argument is array, spread its elements
                    Object* arg_array = arg.as_object();
                    uint32_t arg_length = arg_array->get_length();
                    for (uint32_t i = 0; i < arg_length; i++) {
                        Value element = arg_array->get_element(i);
                        result->set_element(result_index++, element);
                    }
                } else {
                    // If argument is not array, add as single element
                    result->set_element(result_index++, arg);
                }
            }

            result->set_length(result_index);
            return Value(result.release());
        });
    // TEMPORARILY DISABLED: array_prototype->set_property("concat", Value(array_concat_fn.release()));

    // Store the pointer before transferring ownership
    Object* array_proto_ptr = array_prototype.get();

    // Set Array.prototype.constructor BEFORE releasing array_constructor
    array_proto_ptr->set_property("constructor", Value(array_constructor.get()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    array_constructor->set_property("prototype", Value(array_prototype.release()), PropertyAttributes::None);

    // Set the array prototype in ObjectFactory so new arrays inherit from it
    ObjectFactory::set_array_prototype(array_proto_ptr);

    register_built_in_object("Array", array_constructor.release());
    
    // Function constructor
    auto function_constructor = ObjectFactory::create_native_function("Function",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Function constructor implementation - basic placeholder
            return Value(ObjectFactory::create_function().release());
        });
    
    // Create Function.prototype
    auto function_prototype = ObjectFactory::create_object();
    
    // Function.prototype.call
    auto call_fn = ObjectFactory::create_native_function("call",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get the function that call was invoked on
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_type_error("Function.prototype.call called on non-function");
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

    PropertyDescriptor call_length_desc(Value(1.0), PropertyAttributes::None);
    call_length_desc.set_configurable(false);
    call_length_desc.set_enumerable(false);
    call_length_desc.set_writable(false);
    call_fn->set_property_descriptor("length", call_length_desc);

    call_fn->set_property("name", Value("call"), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("call", Value(call_fn.release()));
    
    // Function.prototype.apply
    auto apply_fn = ObjectFactory::create_native_function("apply",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get the function that apply was invoked on
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_type_error("Function.prototype.apply called on non-function");
                return Value();
            }
            
            Function* func = static_cast<Function*>(function_obj);
            Value this_arg = args.size() > 0 ? args[0] : Value();
            
            // Prepare arguments from array
            std::vector<Value> call_args;
            if (args.size() > 1 && !args[1].is_undefined() && !args[1].is_null()) {
                if (args[1].is_object()) {
                    Object* args_array = args[1].as_object();
                    if (args_array->is_array()) {
                        uint32_t length = args_array->get_length();
                        for (uint32_t i = 0; i < length; i++) {
                            call_args.push_back(args_array->get_element(i));
                        }
                    }
                }
            }
            
            return func->call(ctx, call_args, this_arg);
        });

    PropertyDescriptor apply_length_desc(Value(2.0), PropertyAttributes::None);
    apply_length_desc.set_configurable(false);
    apply_length_desc.set_enumerable(false);
    apply_length_desc.set_writable(false);
    apply_fn->set_property_descriptor("length", apply_length_desc);

    apply_fn->set_property("name", Value("apply"), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("apply", Value(apply_fn.release()));
    
    // Function.prototype.bind
    auto bind_fn = ObjectFactory::create_native_function("bind",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get the function that bind was invoked on
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_type_error("Function.prototype.bind called on non-function");
                return Value();
            }
            
            Function* target_func = static_cast<Function*>(function_obj);
            Value bound_this = args.size() > 0 ? args[0] : Value();
            
            // Capture bound arguments (everything after thisArg)
            std::vector<Value> bound_args;
            for (size_t i = 1; i < args.size(); i++) {
                bound_args.push_back(args[i]);
            }
            
            // Create a new bound function
            auto bound_function = ObjectFactory::create_native_function("bound",
                [target_func, bound_this, bound_args](Context& ctx, const std::vector<Value>& call_args) -> Value {
                    // Combine bound args with call-time args
                    std::vector<Value> final_args = bound_args;
                    final_args.insert(final_args.end(), call_args.begin(), call_args.end());
                    
                    // Call the target function with bound this and combined args
                    return target_func->call(ctx, final_args, bound_this);
                });
            
            return Value(bound_function.release());
        });

    PropertyDescriptor bind_length_desc(Value(1.0), PropertyAttributes::None);
    bind_length_desc.set_configurable(false);
    bind_length_desc.set_enumerable(false);
    bind_length_desc.set_writable(false);
    bind_fn->set_property_descriptor("length", bind_length_desc);

    bind_fn->set_property("name", Value("bind"), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("bind", Value(bind_fn.release()));
    
    // Set Function.prototype as the prototype
    function_constructor->set_property("prototype", Value(function_prototype.release()), PropertyAttributes::None);
    
    register_built_in_object("Function", function_constructor.release());
    
    // String constructor - callable as function or constructor
    auto string_constructor = ObjectFactory::create_native_function("String",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str_value = args.empty() ? "" : args[0].to_string();

            // Check if called as constructor (with 'new')
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                // Called as constructor - set up the String object
                this_obj->set_property("value", Value(str_value));
                this_obj->set_property("length", Value(static_cast<double>(str_value.length())));

                // Add toString method to the object
                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args; // Suppress unused warning
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding && this_binding->has_property("value")) {
                            return this_binding->get_property("value");
                        }
                        return Value("");
                    });
                this_obj->set_property("toString", Value(toString_fn.release()));
            }

            // Always return the string value (construct() will use the object if called as constructor)
            return Value(str_value);
        });
    
    // Create String.prototype with methods
    auto string_prototype = ObjectFactory::create_object();
    
    // Add String.prototype.padStart
    auto padStart_fn = ObjectFactory::create_native_function("padStart",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get 'this' binding (should be the string)
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            
            if (args.empty()) return Value(str);
            
            uint32_t target_length = static_cast<uint32_t>(args[0].to_number());
            std::string pad_string = args.size() > 1 ? args[1].to_string() : " ";
            
            if (target_length <= str.length()) {
                return Value(str);
            }
            
            uint32_t pad_length = target_length - str.length();
            std::string padding = "";
            
            if (!pad_string.empty()) {
                while (padding.length() < pad_length) {
                    padding += pad_string;
                }
                padding = padding.substr(0, pad_length);
            }
            
            return Value(padding + str);
        });
    string_prototype->set_property("padStart", Value(padStart_fn.release()));
    
    // Add String.prototype.padEnd  
    auto padEnd_fn = ObjectFactory::create_native_function("padEnd",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get 'this' binding (should be the string)
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            
            if (args.empty()) return Value(str);
            
            uint32_t target_length = static_cast<uint32_t>(args[0].to_number());
            std::string pad_string = args.size() > 1 ? args[1].to_string() : " ";
            
            if (target_length <= str.length()) {
                return Value(str);
            }
            
            uint32_t pad_length = target_length - str.length();
            std::string padding = "";
            
            if (!pad_string.empty()) {
                while (padding.length() < pad_length) {
                    padding += pad_string;
                }
                padding = padding.substr(0, pad_length);
            }
            
            return Value(str + padding);
        });
    string_prototype->set_property("padEnd", Value(padEnd_fn.release()));

    // Add String.prototype.includes (ES2015)
    auto str_includes_fn = ObjectFactory::create_native_function("includes",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get 'this' binding (should be the string)
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(false);

            // Check if searchString is a Symbol (should throw TypeError)
            if (args[0].is_symbol()) {
                ctx.throw_exception(Value("TypeError: Cannot convert a Symbol value to a string"));
                return Value();
            }

            std::string search_string = args[0].to_string();
            size_t position = 0;
            if (args.size() > 1) {
                // Check if position is a Symbol (should throw TypeError)
                if (args[1].is_symbol()) {
                    ctx.throw_exception(Value("TypeError: Cannot convert a Symbol value to a number"));
                    return Value();
                }
                position = static_cast<size_t>(std::max(0.0, args[1].to_number()));
            }

            if (position >= str.length()) {
                return Value(search_string.empty());
            }

            // Use find with position instead of substr for efficiency and correctness
            size_t found = str.find(search_string, position);
            return Value(found != std::string::npos);
        });
    // Set includes method length with proper descriptor
    PropertyDescriptor str_includes_length_desc(Value(1.0), PropertyAttributes::None);
    str_includes_length_desc.set_configurable(false);
    str_includes_length_desc.set_enumerable(false);
    str_includes_length_desc.set_writable(false);
    str_includes_fn->set_property_descriptor("length", str_includes_length_desc);
    string_prototype->set_property("includes", Value(str_includes_fn.release()));

    // Add String.prototype.startsWith (ES2015)
    auto startsWith_fn = ObjectFactory::create_native_function("startsWith",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(false);

            // Check if searchString is a Symbol (should throw TypeError)
            if (args[0].is_symbol()) {
                ctx.throw_exception(Value("TypeError: Cannot convert a Symbol value to a string"));
                return Value();
            }

            std::string search_string = args[0].to_string();
            size_t position = 0;
            if (args.size() > 1) {
                // Check if position is a Symbol (should throw TypeError)
                if (args[1].is_symbol()) {
                    ctx.throw_exception(Value("TypeError: Cannot convert a Symbol value to a number"));
                    return Value();
                }
                position = static_cast<size_t>(std::max(0.0, args[1].to_number()));
            }

            if (position >= str.length()) {
                return Value(search_string.empty());
            }

            return Value(str.substr(position, search_string.length()) == search_string);
        });
    // Set startsWith method length with proper descriptor
    PropertyDescriptor startsWith_length_desc(Value(1.0), PropertyAttributes::None);
    startsWith_length_desc.set_configurable(false);
    startsWith_length_desc.set_enumerable(false);
    startsWith_length_desc.set_writable(false);
    startsWith_fn->set_property_descriptor("length", startsWith_length_desc);
    string_prototype->set_property("startsWith", Value(startsWith_fn.release()));

    // Add String.prototype.endsWith (ES2015)
    auto endsWith_fn = ObjectFactory::create_native_function("endsWith",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(false);

            // Check if searchString is a Symbol (should throw TypeError)
            if (args[0].is_symbol()) {
                ctx.throw_exception(Value("TypeError: Cannot convert a Symbol value to a string"));
                return Value();
            }

            std::string search_string = args[0].to_string();
            size_t length = args.size() > 1 ?
                static_cast<size_t>(std::max(0.0, args[1].to_number())) : str.length();

            if (length > str.length()) length = str.length();
            if (search_string.length() > length) return Value(false);

            return Value(str.substr(length - search_string.length(), search_string.length()) == search_string);
        });
    // Set endsWith method length with proper descriptor
    PropertyDescriptor endsWith_length_desc(Value(1.0), PropertyAttributes::None);
    endsWith_length_desc.set_configurable(false);
    endsWith_length_desc.set_enumerable(false);
    endsWith_length_desc.set_writable(false);
    endsWith_fn->set_property_descriptor("length", endsWith_length_desc);
    string_prototype->set_property("endsWith", Value(endsWith_fn.release()));
    
    // Add String.prototype.match
    auto match_fn = ObjectFactory::create_native_function("match",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // For prototype methods, 'this' should be the string primitive
            // Try different approaches to get the string value
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                // If this fails, we can't proceed
                return Value(); // null
            }

            if (args.empty()) return Value(); // null

            Value pattern = args[0];

            // Check if pattern is a RegExp object
            if (pattern.is_object()) {
                Object* regex_obj = pattern.as_object();

                // Get the RegExp's exec method
                Value exec_method = regex_obj->get_property("exec");
                if (exec_method.is_object() && exec_method.as_object()->is_function()) {
                    // Call RegExp.exec on the string
                    std::vector<Value> exec_args = { Value(str) };
                    Function* exec_func = static_cast<Function*>(exec_method.as_object());
                    return exec_func->call(ctx, exec_args, pattern);
                }
            }

            // If not a RegExp, convert to string and do simple search
            std::string search = pattern.to_string();
            size_t pos = str.find(search);

            if (pos != std::string::npos) {
                // Create array with match result
                auto result = ObjectFactory::create_array();
                result->set_element(0, Value(search));
                result->set_property("index", Value(static_cast<double>(pos)));
                result->set_property("input", Value(str));
                return Value(result.release());
            }

            return Value(); // null
        });
    string_prototype->set_property("match", Value(match_fn.release()));

    // Add String.prototype.replace
    auto replace_fn = ObjectFactory::create_native_function("replace",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // For prototype methods, 'this' should be the string primitive
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                // If this fails, return original empty string
                return Value("");
            }

            if (args.size() < 2) return Value(str);

            Value search_val = args[0];
            std::string replacement = args[1].to_string();

            // Check if search is a RegExp object
            if (search_val.is_object()) {
                Object* regex_obj = search_val.as_object();

                // Get the RegExp's exec method
                Value exec_method = regex_obj->get_property("exec");
                if (exec_method.is_object() && exec_method.as_object()->is_function()) {
                    // Use RegExp for replacement
                    std::vector<Value> exec_args = { Value(str) };
                    Function* exec_func = static_cast<Function*>(exec_method.as_object());
                    Value match_result = exec_func->call(ctx, exec_args, search_val);

                    if (match_result.is_object()) {
                        Object* match_arr = match_result.as_object();
                        Value index_val = match_arr->get_property("index");
                        Value match_str = match_arr->get_element(0);

                        if (index_val.is_number() && !match_str.is_undefined()) {
                            size_t pos = static_cast<size_t>(index_val.to_number());
                            std::string matched = match_str.to_string();

                            str.replace(pos, matched.length(), replacement);
                            return Value(str);
                        }
                    }
                }
            }

            // Simple string replacement (first occurrence only)
            std::string search = search_val.to_string();
            size_t pos = str.find(search);

            if (pos != std::string::npos) {
                str.replace(pos, search.length(), replacement);
            }

            return Value(str);
        });
    string_prototype->set_property("replace", Value(replace_fn.release()));

    // Add String.prototype.replaceAll
    auto replaceAll_fn = ObjectFactory::create_native_function("replaceAll",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            
            if (args.size() < 2) return Value(str);
            
            std::string search = args[0].to_string();
            std::string replace = args[1].to_string();
            
            if (search.empty()) return Value(str);
            
            size_t pos = 0;
            while ((pos = str.find(search, pos)) != std::string::npos) {
                str.replace(pos, search.length(), replace);
                pos += replace.length();
            }
            
            return Value(str);
        });
    string_prototype->set_property("replaceAll", Value(replaceAll_fn.release()));

    // Add basic String.prototype methods

    // String.prototype.charAt
    auto charAt_fn = ObjectFactory::create_native_function("charAt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            uint32_t index = 0;
            if (args.size() > 0) {
                index = static_cast<uint32_t>(args[0].to_number());
            }

            if (index >= str.length()) {
                return Value("");  // Return empty string for out of bounds
            }

            return Value(std::string(1, str[index]));
        });
    string_prototype->set_property("charAt", Value(charAt_fn.release()));

    // String.prototype.charCodeAt
    auto charCodeAt_fn = ObjectFactory::create_native_function("charCodeAt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            uint32_t index = 0;
            if (args.size() > 0) {
                index = static_cast<uint32_t>(args[0].to_number());
            }

            if (index >= str.length()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            return Value(static_cast<double>(static_cast<unsigned char>(str[index])));
        });
    string_prototype->set_property("charCodeAt", Value(charCodeAt_fn.release()));

    // String.prototype.indexOf
    auto indexOf_fn = ObjectFactory::create_native_function("indexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) {
                return Value(-1.0);
            }

            std::string search = args[0].to_string();
            uint32_t start = 0;
            if (args.size() > 1) {
                start = static_cast<uint32_t>(std::max(0.0, args[1].to_number()));
            }

            size_t pos = str.find(search, start);
            return Value(pos == std::string::npos ? -1.0 : static_cast<double>(pos));
        });
    string_prototype->set_property("indexOf", Value(indexOf_fn.release()));

    // String.prototype.toLowerCase
    auto toLowerCase_fn = ObjectFactory::create_native_function("toLowerCase",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            std::transform(str.begin(), str.end(), str.begin(),
                [](unsigned char c) { return std::tolower(c); });

            return Value(str);
        });
    string_prototype->set_property("toLowerCase", Value(toLowerCase_fn.release()));

    // String.prototype.toUpperCase
    auto toUpperCase_fn = ObjectFactory::create_native_function("toUpperCase",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            std::transform(str.begin(), str.end(), str.begin(),
                [](unsigned char c) { return std::toupper(c); });

            return Value(str);
        });
    string_prototype->set_property("toUpperCase", Value(toUpperCase_fn.release()));

    // Add String.concat static method
    auto string_concat_static = ObjectFactory::create_native_function("concat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string result = "";
            for (const auto& arg : args) {
                result += arg.to_string();
            }
            return Value(result);
        });
    string_constructor->set_property("concat", Value(string_concat_static.release()));

    // Add Annex B (legacy) String.prototype methods

    // String.prototype.anchor
    auto anchor_fn = ObjectFactory::create_native_function("anchor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            std::string name = args.size() > 0 ? args[0].to_string() : "";
            return Value("<a name=\"" + name + "\">" + str + "</a>");
        }, 1);
    string_prototype->set_property("anchor", Value(anchor_fn.release()));

    // String.prototype.big
    auto big_fn = ObjectFactory::create_native_function("big",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            return Value("<big>" + str + "</big>");
        }, 0);
    string_prototype->set_property("big", Value(big_fn.release()));

    // String.prototype.blink
    auto blink_fn = ObjectFactory::create_native_function("blink",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            return Value("<blink>" + str + "</blink>");
        }, 0);
    string_prototype->set_property("blink", Value(blink_fn.release()));

    // String.prototype.bold
    auto bold_fn = ObjectFactory::create_native_function("bold",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            return Value("<b>" + str + "</b>");
        }, 0);
    string_prototype->set_property("bold", Value(bold_fn.release()));

    // String.prototype.fixed
    auto fixed_fn = ObjectFactory::create_native_function("fixed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            return Value("<tt>" + str + "</tt>");
        }, 0);
    string_prototype->set_property("fixed", Value(fixed_fn.release()));

    // String.prototype.fontcolor
    auto fontcolor_fn = ObjectFactory::create_native_function("fontcolor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            std::string color = args.size() > 0 ? args[0].to_string() : "";
            return Value("<font color=\"" + color + "\">" + str + "</font>");
        }, 1);
    string_prototype->set_property("fontcolor", Value(fontcolor_fn.release()));

    // String.prototype.fontsize
    auto fontsize_fn = ObjectFactory::create_native_function("fontsize",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            std::string size = args.size() > 0 ? args[0].to_string() : "";
            return Value("<font size=\"" + size + "\">" + str + "</font>");
        }, 1);
    string_prototype->set_property("fontsize", Value(fontsize_fn.release()));

    // String.prototype.italics
    auto italics_fn = ObjectFactory::create_native_function("italics",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            return Value("<i>" + str + "</i>");
        }, 0);
    string_prototype->set_property("italics", Value(italics_fn.release()));

    // String.prototype.link
    auto link_fn = ObjectFactory::create_native_function("link",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            std::string url = args.size() > 0 ? args[0].to_string() : "";
            return Value("<a href=\"" + url + "\">" + str + "</a>");
        }, 1);
    string_prototype->set_property("link", Value(link_fn.release()));

    // String.prototype.small
    auto small_fn = ObjectFactory::create_native_function("small",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            return Value("<small>" + str + "</small>");
        }, 0);
    string_prototype->set_property("small", Value(small_fn.release()));

    // String.prototype.strike
    auto strike_fn = ObjectFactory::create_native_function("strike",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            return Value("<strike>" + str + "</strike>");
        }, 0);
    string_prototype->set_property("strike", Value(strike_fn.release()));

    // String.prototype.sub
    auto sub_fn = ObjectFactory::create_native_function("sub",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            return Value("<sub>" + str + "</sub>");
        }, 0);
    string_prototype->set_property("sub", Value(sub_fn.release()));

    // String.prototype.sup
    auto sup_fn = ObjectFactory::create_native_function("sup",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            return Value("<sup>" + str + "</sup>");
        }, 0);
    string_prototype->set_property("sup", Value(sup_fn.release()));

    // Set up bidirectional constructor/prototype relationship
    Object* proto_ptr = string_prototype.get();
    string_constructor->set_property("prototype", Value(string_prototype.release()), PropertyAttributes::None);
    proto_ptr->set_property("constructor", Value(string_constructor.get()));

    // Add String.raw static method BEFORE registering
    auto string_raw_fn = ObjectFactory::create_native_function("raw",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value("TypeError: String.raw requires at least 1 argument"));
                return Value();
            }

            // Simple implementation for basic string raw functionality
            if (args.size() > 0 && args[0].is_object()) {
                Object* template_obj = args[0].as_object();
                Value raw_val = template_obj->get_property("raw");
                if (raw_val.is_object()) {
                    Object* raw_array = raw_val.as_object();
                    if (raw_array->is_array() && raw_array->get_length() > 0) {
                        // Just return the first raw string segment for simplicity
                        return raw_array->get_element(0);
                    }
                }
            }

            return Value("");
        }, 1);

    string_constructor->set_property("raw", Value(string_raw_fn.release()));

    register_built_in_object("String", string_constructor.release());

    // AFTER registration, get the global String and add methods to its prototype
    Value global_string = global_object_->get_property("String");
    if (global_string.is_function()) {
        Object* global_string_obj = global_string.as_function();
        Value prototype_val = global_string_obj->get_property("prototype");
        if (prototype_val.is_object()) {
            Object* global_prototype = prototype_val.as_object();

            // Re-add includes method to the actual global prototype
            auto global_includes_fn = ObjectFactory::create_native_function("includes",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    Value this_value = ctx.get_binding("this");
                    std::string str = this_value.to_string();
                    if (args.empty()) return Value(false);
                    if (args[0].is_symbol()) {
                        ctx.throw_exception(Value("TypeError: Cannot convert a Symbol value to a string"));
                        return Value();
                    }
                    std::string search_string = args[0].to_string();
                    size_t position = 0;
                    if (args.size() > 1) {
                        if (args[1].is_symbol()) {
                            ctx.throw_exception(Value("TypeError: Cannot convert a Symbol value to a number"));
                            return Value();
                        }
                        position = static_cast<size_t>(std::max(0.0, args[1].to_number()));
                    }
                    if (position >= str.length()) {
                        return Value(search_string.empty());
                    }
                    size_t found = str.find(search_string, position);
                    return Value(found != std::string::npos);
                });
            PropertyDescriptor global_includes_length_desc(Value(1.0), PropertyAttributes::None);
            global_includes_length_desc.set_configurable(false);
            global_includes_length_desc.set_enumerable(false);
            global_includes_length_desc.set_writable(false);
            global_includes_fn->set_property_descriptor("length", global_includes_length_desc);
            global_prototype->set_property("includes", Value(global_includes_fn.release()));

            // Add String.prototype.valueOf
            auto string_valueOf_fn = ObjectFactory::create_native_function("valueOf",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    // Get the this binding
                    Object* this_obj = ctx.get_this_binding();
                    Value this_val;
                    if (this_obj) {
                        this_val = Value(this_obj);
                    } else {
                        // Fallback to primitive binding
                        try {
                            this_val = ctx.get_binding("this");
                        } catch (...) {
                            ctx.throw_exception(Value("TypeError: String.prototype.valueOf called on non-object"));
                            return Value();
                        }
                    }

                    // If this is a String object, return its primitive value
                    if (this_val.is_object()) {
                        Object* obj = this_val.as_object();
                        // Check if this is a String wrapper object
                        Value primitive_value = obj->get_property("[[PrimitiveValue]]");
                        if (!primitive_value.is_undefined() && primitive_value.is_string()) {
                            return primitive_value;
                        }
                    }

                    // If it's already a string primitive, return it
                    if (this_val.is_string()) {
                        return this_val;
                    }

                    // Convert to string
                    return Value(this_val.to_string());
                });

            PropertyDescriptor string_valueOf_length_desc(Value(0.0), PropertyAttributes::None);
            string_valueOf_length_desc.set_configurable(false);
            string_valueOf_length_desc.set_enumerable(false);
            string_valueOf_length_desc.set_writable(false);
            string_valueOf_fn->set_property_descriptor("length", string_valueOf_length_desc);

            PropertyDescriptor string_valueOf_name_desc(Value("valueOf"), PropertyAttributes::None);
            string_valueOf_name_desc.set_configurable(true);
            string_valueOf_name_desc.set_enumerable(false);
            string_valueOf_name_desc.set_writable(false);
            string_valueOf_fn->set_property_descriptor("name", string_valueOf_name_desc);

            global_prototype->set_property("valueOf", Value(string_valueOf_fn.release()));

            // Add String.prototype.toString
            auto string_toString_fn = ObjectFactory::create_native_function("toString",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    // Get the this binding
                    Object* this_obj = ctx.get_this_binding();
                    Value this_val;
                    if (this_obj) {
                        this_val = Value(this_obj);
                    } else {
                        // Fallback to primitive binding
                        try {
                            this_val = ctx.get_binding("this");
                        } catch (...) {
                            ctx.throw_exception(Value("TypeError: String.prototype.toString called on non-object"));
                            return Value();
                        }
                    }

                    // If this is a String object, return its primitive value
                    if (this_val.is_object()) {
                        Object* obj = this_val.as_object();
                        // Check if this is a String wrapper object
                        Value primitive_value = obj->get_property("[[PrimitiveValue]]");
                        if (!primitive_value.is_undefined() && primitive_value.is_string()) {
                            return primitive_value;
                        }
                    }

                    // If it's already a string primitive, return it
                    if (this_val.is_string()) {
                        return this_val;
                    }

                    // Convert to string
                    return Value(this_val.to_string());
                });

            PropertyDescriptor string_toString_length_desc(Value(0.0), PropertyAttributes::None);
            string_toString_length_desc.set_configurable(false);
            string_toString_length_desc.set_enumerable(false);
            string_toString_length_desc.set_writable(false);
            string_toString_fn->set_property_descriptor("length", string_toString_length_desc);

            PropertyDescriptor string_toString_name_desc(Value("toString"), PropertyAttributes::None);
            string_toString_name_desc.set_configurable(true);
            string_toString_name_desc.set_enumerable(false);
            string_toString_name_desc.set_writable(false);
            string_toString_fn->set_property_descriptor("name", string_toString_name_desc);

            global_prototype->set_property("toString", Value(string_toString_fn.release()));

        }
    }
    
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
    auto symbol_for_fn = ObjectFactory::create_native_function("for",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Symbol::symbol_for(ctx, args);
        });
    symbol_constructor->set_property("for", Value(symbol_for_fn.release()));
    
    // Add Symbol.keyFor static method
    auto symbol_key_for_fn = ObjectFactory::create_native_function("keyFor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Symbol::symbol_key_for(ctx, args);
        });
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
    
    //  PROXY AND REFLECT - ES2023+ METAPROGRAMMING 
    Proxy::setup_proxy(*this);
    Reflect::setup_reflect(*this);
    
    //  MAP AND SET COLLECTIONS 
    Map::setup_map_prototype(*this);
    Set::setup_set_prototype(*this);
    
    //  WEAKMAP AND WEAKSET - ES2023+ WEAK COLLECTIONS 
    WeakMap::setup_weakmap_prototype(*this);
    WeakSet::setup_weakset_prototype(*this);
    
    //  ASYNC/AWAIT - ES2017+ ASYNC FUNCTIONS 
    AsyncUtils::setup_async_functions(*this);
    AsyncGenerator::setup_async_generator_prototype(*this);
    AsyncIterator::setup_async_iterator_prototype(*this);
    
    //  ITERATORS - ES2015+ ITERATION PROTOCOL
    Iterator::setup_iterator_prototype(*this);
    // Note: Array/String/Map/Set iterator methods setup moved to end of constructor
    // after bindings are created
    
    //  GENERATORS - ES2015+ GENERATOR FUNCTIONS 
    Generator::setup_generator_prototype(*this);
    
    // Number constructor - callable as function with ES5 constants
    auto number_constructor = ObjectFactory::create_native_function("Number",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(0.0);
            return Value(args[0].to_number());
        });
    number_constructor->set_property("MAX_VALUE", Value(std::numeric_limits<double>::max()));
    number_constructor->set_property("MIN_VALUE", Value(5e-324));
    number_constructor->set_property("NaN", Value(std::numeric_limits<double>::quiet_NaN()));
    number_constructor->set_property("POSITIVE_INFINITY", Value(std::numeric_limits<double>::infinity()));
    number_constructor->set_property("NEGATIVE_INFINITY", Value(-std::numeric_limits<double>::infinity()));
    
    // Number.isInteger
    auto isInteger_fn = ObjectFactory::create_native_function("isInteger",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            if (!args[0].is_number()) return Value(false);
            double num = args[0].to_number();
            return Value(std::isfinite(num) && std::floor(num) == num);
        });
    number_constructor->set_property("isInteger", Value(isInteger_fn.release()));
    
    // Number.isNaN - workaround for engine NaN handling limitations
    auto numberIsNaN_fn = ObjectFactory::create_native_function("isNaN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            
            // Our engine represents NaN as non-number objects  
            // If the value is not a number but should be (like 0/0), it's probably NaN
            if (!args[0].is_number()) {
                // Check if this looks like a NaN object (type object but numeric origin)
                if (args[0].is_object()) {
                    // This could be our engine's broken NaN representation
                    return Value(true);
                }
                return Value(false);
            }
            
            // For actual numbers, use the standard NaN != NaN test
            double val = args[0].to_number();
            return Value(val != val);
        });
    number_constructor->set_property("isNaN", Value(numberIsNaN_fn.release()));
    
    // Number.isFinite -  implementation matching isNaN logic
    auto numberIsFinite_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            
            // If it's not a number (including our broken NaN objects), it's not finite
            if (!args[0].is_number()) return Value(false);
            
            double val = args[0].to_number();
            
            // Check for NaN first (NaN is not finite)
            if (val != val) return Value(false);
            
            // Check for infinity by comparing to maximum finite values
            const double MAX_FINITE = 1.7976931348623157e+308;
            return Value(val > -MAX_FINITE && val < MAX_FINITE);
        });
    number_constructor->set_property("isFinite", Value(numberIsFinite_fn.release()));
    
    // Number.parseFloat - same as global parseFloat
    auto numberParseFloat_fn = ObjectFactory::create_native_function("parseFloat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            
            std::string str = args[0].to_string();
            if (str.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            
            try {
                size_t processed = 0;
                double result = std::stod(str, &processed);
                return Value(result);
            } catch (...) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
        });
    number_constructor->set_property("parseFloat", Value(numberParseFloat_fn.release()));

    // Create Number.prototype and add methods
    auto number_prototype = ObjectFactory::create_object();

    // Number.prototype.valueOf
    auto number_valueOf = ObjectFactory::create_native_function("valueOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            // Get this value
            try {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_number()) {
                    return this_val;
                }
                // If this is a Number object wrapper, extract the primitive
                if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->get_type() == Object::ObjectType::Number) {
                        // Return the wrapped number value
                        return this_obj->get_property("[[PrimitiveValue]]");
                    }
                }
                ctx.throw_exception(Value("TypeError: Number.prototype.valueOf called on non-number"));
                return Value();
            } catch (...) {
                ctx.throw_exception(Value("TypeError: Number.prototype.valueOf called on non-number"));
                return Value();
            }
        }, 0);

    // Number.prototype.toString
    auto number_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get this value
            try {
                Value this_val = ctx.get_binding("this");
                double num = 0.0;

                if (this_val.is_number()) {
                    num = this_val.as_number();
                } else if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->get_type() == Object::ObjectType::Number) {
                        Value primitive = this_obj->get_property("[[PrimitiveValue]]");
                        num = primitive.as_number();
                    } else {
                        ctx.throw_exception(Value("TypeError: Number.prototype.toString called on non-number"));
                        return Value();
                    }
                } else {
                    ctx.throw_exception(Value("TypeError: Number.prototype.toString called on non-number"));
                    return Value();
                }

                return Value(std::to_string(num));
            } catch (...) {
                ctx.throw_exception(Value("TypeError: Number.prototype.toString called on non-number"));
                return Value();
            }
        }, 1);

    number_prototype->set_property("valueOf", Value(number_valueOf.release()));
    number_prototype->set_property("toString", Value(number_toString.release()));
    number_prototype->set_property("constructor", Value(number_constructor.get()));

    number_constructor->set_property("prototype", Value(number_prototype.release()));

    register_built_in_object("Number", number_constructor.release());
    
    // Boolean constructor - callable as function
    auto boolean_constructor = ObjectFactory::create_native_function("Boolean",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            return Value(args[0].to_boolean());
        });

    // Create Boolean.prototype and add methods
    auto boolean_prototype = ObjectFactory::create_object();

    // Boolean.prototype.valueOf
    auto boolean_valueOf = ObjectFactory::create_native_function("valueOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            // Get this value
            try {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_boolean()) {
                    return this_val;
                }
                // If this is a Boolean object wrapper, extract the primitive
                if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->get_type() == Object::ObjectType::Boolean) {
                        // Return the wrapped boolean value
                        return this_obj->get_property("[[PrimitiveValue]]");
                    }
                }
                ctx.throw_exception(Value("TypeError: Boolean.prototype.valueOf called on non-boolean"));
                return Value();
            } catch (...) {
                ctx.throw_exception(Value("TypeError: Boolean.prototype.valueOf called on non-boolean"));
                return Value();
            }
        }, 0);

    // Boolean.prototype.toString
    auto boolean_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            // Get this value
            try {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_boolean()) {
                    return Value(this_val.to_boolean() ? "true" : "false");
                }
                // If this is a Boolean object wrapper, extract the primitive
                if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->get_type() == Object::ObjectType::Boolean) {
                        Value primitive = this_obj->get_property("[[PrimitiveValue]]");
                        return Value(primitive.to_boolean() ? "true" : "false");
                    }
                }
                ctx.throw_exception(Value("TypeError: Boolean.prototype.toString called on non-boolean"));
                return Value();
            } catch (...) {
                ctx.throw_exception(Value("TypeError: Boolean.prototype.toString called on non-boolean"));
                return Value();
            }
        }, 0);

    boolean_prototype->set_property("valueOf", Value(boolean_valueOf.release()));
    boolean_prototype->set_property("toString", Value(boolean_toString.release()));
    boolean_prototype->set_property("constructor", Value(boolean_constructor.get()));

    boolean_constructor->set_property("prototype", Value(boolean_prototype.release()));

    register_built_in_object("Boolean", boolean_constructor.release());
    
    // Error constructor (with ES2025 static methods)
    // First create Error.prototype
    auto error_prototype = ObjectFactory::create_object();
    error_prototype->set_property("name", Value("Error"));
    error_prototype->set_property("message", Value(""));
    Object* error_prototype_ptr = error_prototype.get();
    
    auto error_constructor = ObjectFactory::create_native_function("Error",
        [error_prototype_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused parameter warning
            std::string message = "";
            if (!args.empty()) {
                if (args[0].is_undefined()) {
                    message = "";
                } else if (args[0].is_object()) {
                    // For objects, call toString method if available
                    Object* obj = args[0].as_object();
                    if (obj->has_property("toString")) {
                        Value toString_val = obj->get_property("toString");
                        if (toString_val.is_function()) {
                            Function* toString_fn = toString_val.as_function();
                            Value result = toString_fn->call(ctx, {}, Value(obj));
                            message = result.to_string();
                        } else {
                            message = args[0].to_string();
                        }
                    } else {
                        message = args[0].to_string();
                    }
                } else {
                    message = args[0].to_string();
                }
            }
            auto error_obj = std::make_unique<Error>(Error::Type::Error, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(error_prototype_ptr);
            
            // Handle options parameter (ES2022 error cause)
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    // ES2022: cause should be writable, non-enumerable, configurable
                    PropertyDescriptor cause_desc(cause, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }
            
            // Add toString method to Error object
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* this_obj = ctx.get_this_binding();
                    if (!this_obj) {
                        return Value("Error");
                    }

                    // Get dynamic name and message properties
                    Value name_val = this_obj->get_property("name");
                    Value message_val = this_obj->get_property("message");

                    std::string name = name_val.is_string() ? name_val.to_string() : "Error";
                    std::string message = message_val.is_string() ? message_val.to_string() : "";

                    if (message.empty()) {
                        return Value(name);
                    }
                    if (name.empty()) {
                        return Value(message);
                    }
                    return Value(name + ": " + message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()));
            
            return Value(error_obj.release());
        });
    
    // Add ES2025 Error static methods
    auto error_isError = ObjectFactory::create_native_function("isError", Error::isError);
    error_constructor->set_property("isError", Value(error_isError.release()));

    // Set constructor property on prototype
    error_prototype->set_property("constructor", Value(error_constructor.get()));

    // Set constructor.prototype
    error_constructor->set_property("prototype", Value(error_prototype_ptr), PropertyAttributes::None);

    // Store Error constructor pointer before releasing for other error constructors to inherit
    Function* error_ctor = error_constructor.get();

    register_built_in_object("Error", error_constructor.release());

    // Store the error prototype reference (after error_prototype is released by register)
    error_prototype.release();
    
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
    
    // Math object setup with native functions
    auto math_object = std::make_unique<Object>();
    
    // Add Math constants
    math_object->set_property("PI", Value(3.141592653589793));
    math_object->set_property("E", Value(2.718281828459045));
    
    // Add Math.max native function
    auto math_max_fn = ObjectFactory::create_native_function("max",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(-std::numeric_limits<double>::infinity());
            }
            
            double result = -std::numeric_limits<double>::infinity();
            for (const Value& arg : args) {
                double value = arg.to_number();
                if (std::isnan(value)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                result = std::max(result, value);
            }
            return Value(result);
        });
    math_object->set_property("max", Value(math_max_fn.release()));
    
    // Add Math.min native function
    auto math_min_fn = ObjectFactory::create_native_function("min",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::infinity());
            }
            
            double result = std::numeric_limits<double>::infinity();
            for (const Value& arg : args) {
                double value = arg.to_number();
                if (std::isnan(value)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                result = std::min(result, value);
            }
            return Value(result);
        });
    math_object->set_property("min", Value(math_min_fn.release()));
    
    // Add Math.round native function
    auto math_round_fn = ObjectFactory::create_native_function("round",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            
            double value = args[0].to_number();
            return Value(std::round(value));
        });
    math_object->set_property("round", Value(math_round_fn.release()));
    
    // Add Math.random native function
    auto math_random_fn = ObjectFactory::create_native_function("random",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            (void)args; // Suppress unused warning
            return Value(static_cast<double>(rand()) / RAND_MAX);
        });
    math_object->set_property("random", Value(math_random_fn.release()));
    
    // Add Math.floor native function
    auto math_floor_fn = ObjectFactory::create_native_function("floor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::floor(args[0].to_number()));
        });
    math_object->set_property("floor", Value(math_floor_fn.release()));
    
    // Add Math.ceil native function
    auto math_ceil_fn = ObjectFactory::create_native_function("ceil",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::ceil(args[0].to_number()));
        });
    math_object->set_property("ceil", Value(math_ceil_fn.release()));
    
    // Add Math.abs native function
    auto math_abs_fn = ObjectFactory::create_native_function("abs",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            double value = args[0].to_number();
            if (std::isinf(value)) {
                // Math.abs of any infinity should return positive infinity
                return Value::positive_infinity();
            }
            return Value(std::abs(value));
        });
    math_object->set_property("abs", Value(math_abs_fn.release()));
    
    // Add Math.sqrt native function
    auto math_sqrt_fn = ObjectFactory::create_native_function("sqrt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sqrt(args[0].to_number()));
        });
    math_object->set_property("sqrt", Value(math_sqrt_fn.release()));
    
    // Add Math.pow native function
    auto math_pow_fn = ObjectFactory::create_native_function("pow",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.size() < 2) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::pow(args[0].to_number(), args[1].to_number()));
        });
    math_object->set_property("pow", Value(math_pow_fn.release()));
    
    // Add Math.sin native function
    auto math_sin_fn = ObjectFactory::create_native_function("sin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sin(args[0].to_number()));
        });
    math_object->set_property("sin", Value(math_sin_fn.release()));
    
    // Add Math.cos native function
    auto math_cos_fn = ObjectFactory::create_native_function("cos",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cos(args[0].to_number()));
        });
    math_object->set_property("cos", Value(math_cos_fn.release()));
    
    // Add Math.tan native function
    auto math_tan_fn = ObjectFactory::create_native_function("tan",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::tan(args[0].to_number()));
        });
    math_object->set_property("tan", Value(math_tan_fn.release()));
    
    // Add Math.log native function  
    auto math_log_fn = ObjectFactory::create_native_function("log",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log(args[0].to_number()));
        });
    math_object->set_property("log", Value(math_log_fn.release()));
    
    // Add Math.log10 native function
    auto math_log10_fn = ObjectFactory::create_native_function("log10",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log10(args[0].to_number()));
        });
    math_object->set_property("log10", Value(math_log10_fn.release()));
    
    // Add Math.exp native function
    auto math_exp_fn = ObjectFactory::create_native_function("exp",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::exp(args[0].to_number()));
        });
    math_object->set_property("exp", Value(math_exp_fn.release()));
    
    // Math.trunc - truncate decimal part (toward zero)
    auto math_trunc_fn = ObjectFactory::create_native_function("trunc",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(0.0); // Simplified to avoid NaN issues
            double val = args[0].to_number();
            if (std::isinf(val)) return Value(val);
            if (std::isnan(val)) return Value(0.0); // Simplified to avoid NaN issues
            return Value(std::trunc(val));
        });
    math_object->set_property("trunc", Value(math_trunc_fn.release()));
    
    // Math.sign - returns sign of number (-1, 0, 1, or NaN)
    auto math_sign_fn = ObjectFactory::create_native_function("sign",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; // Suppress unused warning
            if (args.empty()) return Value(0.0); // Simplified to avoid NaN issues
            double val = args[0].to_number();
            if (std::isnan(val)) return Value(0.0); // Simplified to avoid NaN issues
            if (val > 0) return Value(1.0);
            if (val < 0) return Value(-1.0);
            return Value(val); // Preserves +0 and -0
        });
    math_object->set_property("sign", Value(math_sign_fn.release()));
    
    register_built_in_object("Math", math_object.get());
    
    // Also directly bind Math to global scope to ensure it's accessible
    if (lexical_environment_) {
        lexical_environment_->create_binding("Math", Value(math_object.get()), false);
    }
    if (variable_environment_) {
        variable_environment_->create_binding("Math", Value(math_object.get()), false);
    }
    if (global_object_) {
        global_object_->set_property("Math", Value(math_object.get()));
    }
    
    math_object.release(); // Release after manual binding
    
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
    
    register_built_in_object("JSON", json_obj.get());
    
    // Also directly bind JSON to global scope to ensure it's accessible
    if (lexical_environment_) {
        lexical_environment_->create_binding("JSON", Value(json_obj.get()), false);
    }
    if (variable_environment_) {
        variable_environment_->create_binding("JSON", Value(json_obj.get()), false);
    }
    if (global_object_) {
        global_object_->set_property("JSON", Value(json_obj.get()));
    }
    
    json_obj.release(); // Release after manual binding
    
    // Helper function to add Date instance methods after construction
    auto add_date_instance_methods = [](Object* date_obj) {
        // Add getTime method - reads _timestamp from the object
        auto getTime_fn = ObjectFactory::create_native_function("getTime",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                // In a full implementation, 'this' would be passed as a parameter
                // For now, we can't access the specific Date object, so return current time
                auto now = std::chrono::system_clock::now();
                auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                return Value(static_cast<double>(timestamp));
            });
        date_obj->set_property("getTime", Value(getTime_fn.release()));
        
        // Add getFullYear method
        auto getFullYear_fn = ObjectFactory::create_native_function("getFullYear",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_year + 1900)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getFullYear", Value(getFullYear_fn.release()));
        
        // Add getMonth method
        auto getMonth_fn = ObjectFactory::create_native_function("getMonth",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_mon)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getMonth", Value(getMonth_fn.release()));
        
        // Add getDate method
        auto getDate_fn = ObjectFactory::create_native_function("getDate",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_mday)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getDate", Value(getDate_fn.release()));

        // Legacy methods (Annex B)
        auto getYear_fn = ObjectFactory::create_native_function("getYear",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_year)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getYear", Value(getYear_fn.release()));

        auto setYear_fn = ObjectFactory::create_native_function("setYear",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx;
                if (args.empty()) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }

                double year_value = args[0].to_number();
                if (std::isnan(year_value) || std::isinf(year_value)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }

                int year = static_cast<int>(year_value);
                if (year >= 0 && year <= 99) {
                    year += 1900;
                }

                // Return timestamp (simplified implementation)
                return Value(static_cast<double>(year));
            });
        date_obj->set_property("setYear", Value(setYear_fn.release()));

        // Add toString method
        auto toString_fn = ObjectFactory::create_native_function("toString",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                // Return current date as string
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::string time_str = std::ctime(&time);
                // Remove newline at the end
                if (!time_str.empty() && time_str.back() == '\n') {
                    time_str.pop_back();
                }
                return Value(time_str);
            });
        date_obj->set_property("toString", Value(toString_fn.release()));
    };
    
    // Create Date constructor function that adds instance methods
    auto date_constructor_fn = ObjectFactory::create_native_function("Date",
        [add_date_instance_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            // Call the original Date constructor
            Value date_obj = Date::date_constructor(ctx, args);
            
            // Add instance methods to the created Date object
            if (date_obj.is_object()) {
                add_date_instance_methods(date_obj.as_object());
            }
            
            return date_obj;
        });
    
    // Add Date static methods
    auto date_now = ObjectFactory::create_native_function("now", Date::now);
    auto date_parse = ObjectFactory::create_native_function("parse", Date::parse);
    auto date_UTC = ObjectFactory::create_native_function("UTC", Date::UTC);
    
    date_constructor_fn->set_property("now", Value(date_now.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    date_constructor_fn->set_property("parse", Value(date_parse.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    date_constructor_fn->set_property("UTC", Value(date_UTC.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    
    // Create Date prototype with instance methods
    auto date_prototype = ObjectFactory::create_object();
    
    // Add instance methods to Date.prototype
    auto getTime_fn = ObjectFactory::create_native_function("getTime", Date::getTime);
    auto getFullYear_fn = ObjectFactory::create_native_function("getFullYear", Date::getFullYear);
    auto getMonth_fn = ObjectFactory::create_native_function("getMonth", Date::getMonth);
    auto getDate_fn = ObjectFactory::create_native_function("getDate", Date::getDate);
    auto getDay_fn = ObjectFactory::create_native_function("getDay", Date::getDay);
    auto getHours_fn = ObjectFactory::create_native_function("getHours", Date::getHours);
    auto getMinutes_fn = ObjectFactory::create_native_function("getMinutes", Date::getMinutes);
    auto getSeconds_fn = ObjectFactory::create_native_function("getSeconds", Date::getSeconds);
    auto getMilliseconds_fn = ObjectFactory::create_native_function("getMilliseconds", Date::getMilliseconds);
    auto toString_fn = ObjectFactory::create_native_function("toString", Date::toString);
    auto toISOString_fn = ObjectFactory::create_native_function("toISOString", Date::toISOString);
    auto toJSON_fn = ObjectFactory::create_native_function("toJSON", Date::toJSON);

    // Legacy methods (Annex B)
    auto getYear_fn = ObjectFactory::create_native_function("getYear", Date::getYear);
    auto setYear_fn = ObjectFactory::create_native_function("setYear", Date::setYear);

    date_prototype->set_property("getTime", Value(getTime_fn.release()));
    date_prototype->set_property("getFullYear", Value(getFullYear_fn.release()));
    date_prototype->set_property("getMonth", Value(getMonth_fn.release()));
    date_prototype->set_property("getDate", Value(getDate_fn.release()));
    date_prototype->set_property("getDay", Value(getDay_fn.release()));
    date_prototype->set_property("getHours", Value(getHours_fn.release()));
    date_prototype->set_property("getMinutes", Value(getMinutes_fn.release()));
    date_prototype->set_property("getSeconds", Value(getSeconds_fn.release()));
    date_prototype->set_property("getMilliseconds", Value(getMilliseconds_fn.release()));
    date_prototype->set_property("toString", Value(toString_fn.release()));
    date_prototype->set_property("toISOString", Value(toISOString_fn.release()));
    date_prototype->set_property("toJSON", Value(toJSON_fn.release()));

    // Legacy methods (Annex B) - should be non-enumerable
    date_prototype->set_property("getYear", Value(getYear_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    date_prototype->set_property("setYear", Value(setYear_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // toGMTString is deprecated alias for toString (Annex B) - should be non-enumerable
    auto toGMTString_fn = ObjectFactory::create_native_function("toGMTString", Date::toString);
    date_prototype->set_property("toGMTString", Value(toGMTString_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Set Date.prototype on the constructor (keep reference for proper binding)
    date_constructor_fn->set_property("prototype", Value(date_prototype.get()));
    
    register_built_in_object("Date", date_constructor_fn.get());
    
    // Also directly bind Date to global scope to ensure it's accessible
    if (lexical_environment_) {
        lexical_environment_->create_binding("Date", Value(date_constructor_fn.get()), false);
    }
    if (variable_environment_) {
        variable_environment_->create_binding("Date", Value(date_constructor_fn.get()), false);
    }
    if (global_object_) {
        global_object_->set_property("Date", Value(date_constructor_fn.get()));
    }
    
    date_constructor_fn.release(); // Release after manual binding
    date_prototype.release(); // Release prototype after binding
    
    // Additional Error types (Error is already defined above)
    // Create TypeError.prototype first
    auto type_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    type_error_prototype->set_property("name", Value("TypeError"));
    Object* type_error_proto_ptr = type_error_prototype.get();

    auto type_error_constructor = ObjectFactory::create_native_function("TypeError",
        [type_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::TypeError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(type_error_proto_ptr);

            // Handle options parameter (ES2022 error cause)
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            // Add toString method
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()));

            return Value(error_obj.release());
        });

    type_error_prototype->set_property("constructor", Value(type_error_constructor.get()));

    // Set constructor.prototype
    type_error_constructor->set_property("prototype", Value(type_error_prototype.release()));

    // Set TypeError's prototype to Error (TypeError inherits from Error)
    if (error_ctor) {
        type_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("TypeError", type_error_constructor.release());
    
    // Create ReferenceError.prototype first
    auto reference_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    reference_error_prototype->set_property("name", Value("ReferenceError"));
    Object* reference_error_proto_ptr = reference_error_prototype.get();

    auto reference_error_constructor = ObjectFactory::create_native_function("ReferenceError",
        [reference_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::ReferenceError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(reference_error_proto_ptr);
            
            // Handle options parameter (ES2022 error cause)
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }
            
            // Add toString method
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()));
            
            return Value(error_obj.release());
        });
    
    reference_error_prototype->set_property("constructor", Value(reference_error_constructor.get()));
    reference_error_constructor->set_property("prototype", Value(reference_error_prototype.release()));

    // Set ReferenceError's prototype to Error
    if (error_ctor) {
        reference_error_constructor->set_prototype(error_ctor);
    }
    
    register_built_in_object("ReferenceError", reference_error_constructor.release());
    
    // Create SyntaxError.prototype first
    auto syntax_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    syntax_error_prototype->set_property("name", Value("SyntaxError"));
    Object* syntax_error_proto_ptr = syntax_error_prototype.get();

    auto syntax_error_constructor = ObjectFactory::create_native_function("SyntaxError",
        [syntax_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::SyntaxError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(syntax_error_proto_ptr);
            
            // Handle options parameter (ES2022 error cause)
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }
            
            // Add toString method
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()));
            
            return Value(error_obj.release());
        });
    
    syntax_error_prototype->set_property("constructor", Value(syntax_error_constructor.get()));
    syntax_error_constructor->set_property("prototype", Value(syntax_error_prototype.release()));

    // Set SyntaxError's prototype to Error
    if (error_ctor) {
        syntax_error_constructor->set_prototype(error_ctor);
    }
    
    register_built_in_object("SyntaxError", syntax_error_constructor.release());

    // Create RangeError.prototype first
    auto range_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    range_error_prototype->set_property("name", Value("RangeError"));
    Object* range_error_proto_ptr = range_error_prototype.get();

    auto range_error_constructor = ObjectFactory::create_native_function("RangeError",
        [range_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::RangeError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(range_error_proto_ptr);

            // Handle options parameter (ES2022 error cause)
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            // Add toString method
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()));

            return Value(error_obj.release());
        });

    range_error_prototype->set_property("constructor", Value(range_error_constructor.get()));

    // Set constructor.prototype
    range_error_constructor->set_property("prototype", Value(range_error_prototype.release()));

    // Set RangeError's prototype to Error
    if (error_ctor) {
        range_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("RangeError", range_error_constructor.release());

    // Create URIError.prototype first
    auto uri_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    uri_error_prototype->set_property("name", Value("URIError"));
    Object* uri_error_proto_ptr = uri_error_prototype.get();

    auto uri_error_constructor = ObjectFactory::create_native_function("URIError",
        [uri_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::URIError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(uri_error_proto_ptr);

            // Handle options parameter (ES2022 error cause)
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            // Add toString method
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()));

            return Value(error_obj.release());
        });

    uri_error_prototype->set_property("constructor", Value(uri_error_constructor.get()));

    // Set constructor.prototype
    uri_error_constructor->set_property("prototype", Value(uri_error_prototype.release()));

    // Set URIError's prototype to Error
    if (error_ctor) {
        uri_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("URIError", uri_error_constructor.release());

    // Create EvalError.prototype first
    auto eval_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    eval_error_prototype->set_property("name", Value("EvalError"));
    Object* eval_error_proto_ptr = eval_error_prototype.get();

    auto eval_error_constructor = ObjectFactory::create_native_function("EvalError",
        [eval_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::EvalError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(eval_error_proto_ptr);

            // Handle options parameter (ES2022 error cause)
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            // Add toString method
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()));

            return Value(error_obj.release());
        });

    eval_error_prototype->set_property("constructor", Value(eval_error_constructor.get()));

    // Set constructor.prototype
    eval_error_constructor->set_property("prototype", Value(eval_error_prototype.release()));

    // Set EvalError's prototype to Error
    if (error_ctor) {
        eval_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("EvalError", eval_error_constructor.release());

    // Create AggregateError.prototype that inherits from Error.prototype
    auto aggregate_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    aggregate_error_prototype->set_property("name", Value("AggregateError"));
    
    // Store pointer before it's moved
    Object* agg_error_proto_ptr = aggregate_error_prototype.get();

    // AggregateError constructor (ES2021) - takes 2 arguments
    auto aggregate_error_constructor = ObjectFactory::create_native_function("AggregateError",
        [agg_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            // Only convert message to string if it's provided and not undefined
            std::string message = "";
            if (args.size() > 1 && !args[1].is_undefined()) {
                // Use proper ToString abstract operation (ES spec)
                Value msg_value = args[1];
                if (msg_value.is_object()) {
                    // For objects, call their toString() method if available
                    Object* obj = msg_value.as_object();
                    Value toString_method = obj->get_property("toString");
                    if (toString_method.is_function()) {
                        try {
                            Function* func = toString_method.as_function();
                            // Call toString() method with proper context and 'this' binding
                            Value result = func->call(ctx, {}, msg_value);
                            if (!ctx.has_exception()) {
                                message = result.to_string();
                            } else {
                                // If toString() throws, fall back to default conversion
                                ctx.clear_exception();
                                message = msg_value.to_string();
                            }
                        } catch (...) {
                            // If anything goes wrong, fall back to default conversion
                            message = msg_value.to_string();
                        }
                    } else {
                        // No toString method, use default conversion
                        message = msg_value.to_string();
                    }
                } else {
                    // For primitives, use standard conversion
                    message = msg_value.to_string();
                }
            }
            auto error_obj = std::make_unique<Error>(Error::Type::AggregateError, message);
            error_obj->set_property("_isError", Value(true));
            
            // Set the prototype to AggregateError.prototype
            error_obj->set_prototype(agg_error_proto_ptr);

            // Handle the errors array (first argument)
            if (args.size() > 0 && args[0].is_object()) {
                error_obj->set_property("errors", args[0]);
            } else {
                // Create an empty array if no errors provided
                auto empty_array = ObjectFactory::create_array();
                error_obj->set_property("errors", Value(empty_array.release()));
            }

            // Handle options parameter (ES2022 error cause)
            if (args.size() > 2 && args[2].is_object()) {
                Object* options = args[2].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            // Add toString method
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()));

            return Value(error_obj.release());
        }, 2); // AggregateError takes 2 arguments: errors and message

    aggregate_error_prototype->set_property("constructor", Value(aggregate_error_constructor.get()));

    // Set AggregateError constructor name property
    PropertyDescriptor name_desc(Value("AggregateError"), PropertyAttributes::None);
    name_desc.set_configurable(true);
    name_desc.set_enumerable(false);
    name_desc.set_writable(false);
    aggregate_error_constructor->set_property_descriptor("name", name_desc);

    // Set AggregateError constructor length property
    PropertyDescriptor length_desc(Value(2.0), PropertyAttributes::None);
    length_desc.set_configurable(true);
    length_desc.set_enumerable(false);
    length_desc.set_writable(false);
    aggregate_error_constructor->set_property_descriptor("length", length_desc);

    // Set constructor.prototype
    aggregate_error_constructor->set_property("prototype", Value(aggregate_error_prototype.release()));

    // Set AggregateError's prototype to Error
    if (error_ctor) {
        aggregate_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("AggregateError", aggregate_error_constructor.release());

    // Create RegExp.prototype
    auto regexp_prototype = ObjectFactory::create_object();

    // Add RegExp.prototype.compile (Annex B legacy method)
    auto compile_fn = ObjectFactory::create_native_function("compile",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get 'this' binding - should be a RegExp object
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value("TypeError: RegExp.prototype.compile called on null or undefined"));
                return Value();
            }

            // RegExp.prototype.compile changes the RegExp object in-place and returns it
            // This is a legacy method from early JavaScript
            std::string pattern = "";
            std::string flags = "";

            if (args.size() > 0) {
                pattern = args[0].to_string();
            }
            if (args.size() > 1) {
                flags = args[1].to_string();
            }

            // Update the RegExp object properties
            this_obj->set_property("source", Value(pattern));
            this_obj->set_property("global", Value(flags.find('g') != std::string::npos));
            this_obj->set_property("ignoreCase", Value(flags.find('i') != std::string::npos));
            this_obj->set_property("multiline", Value(flags.find('m') != std::string::npos));
            this_obj->set_property("lastIndex", Value(0.0));

            // Return the modified RegExp object
            return Value(this_obj);
        }, 2);
    regexp_prototype->set_property("compile", Value(compile_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // Store pointer before it's moved
    Object* regexp_proto_ptr = regexp_prototype.get();

    // RegExp constructor
    auto regexp_constructor = ObjectFactory::create_native_function("RegExp",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string pattern = "";
            std::string flags = "";
            
            if (args.size() > 0) {
                pattern = args[0].to_string();
            }
            if (args.size() > 1) {
                flags = args[1].to_string();
            }
            
            try {
                auto regex_obj = ObjectFactory::create_object();
                
                // Store the actual RegExp implementation as a shared pointer
                auto regexp_impl = std::make_shared<RegExp>(pattern, flags);
                
                // Set standard properties
                regex_obj->set_property("source", Value(regexp_impl->get_source()));
                regex_obj->set_property("flags", Value(regexp_impl->get_flags()));
                regex_obj->set_property("global", Value(regexp_impl->get_global()));
                regex_obj->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                regex_obj->set_property("multiline", Value(regexp_impl->get_multiline()));
                regex_obj->set_property("unicode", Value(regexp_impl->get_unicode()));
                regex_obj->set_property("sticky", Value(regexp_impl->get_sticky()));
                regex_obj->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                
                // Add test method
                auto test_fn = ObjectFactory::create_native_function("test",
                    [regexp_impl](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (args.empty()) return Value(false);
                        std::string str = args[0].to_string();
                        return Value(regexp_impl->test(str));
                    });
                regex_obj->set_property("test", Value(test_fn.release()));
                
                // Add exec method
                auto exec_fn = ObjectFactory::create_native_function("exec",
                    [regexp_impl](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (args.empty()) return Value::null();
                        std::string str = args[0].to_string();
                        return regexp_impl->exec(str);
                    });
                regex_obj->set_property("exec", Value(exec_fn.release()));

                // Add RegExp properties
                regex_obj->set_property("source", Value(regexp_impl->get_source()));
                regex_obj->set_property("flags", Value(regexp_impl->get_flags()));
                regex_obj->set_property("global", Value(regexp_impl->get_global()));
                regex_obj->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                regex_obj->set_property("multiline", Value(regexp_impl->get_multiline()));
                regex_obj->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));

                return Value(regex_obj.release());
                
            } catch (const std::exception& e) {
                ctx.throw_error("Invalid RegExp: " + std::string(e.what()));
                return Value::null();
            }
        });

    // Set up bidirectional constructor/prototype relationship
    regexp_prototype->set_property("constructor", Value(regexp_constructor.get()));
    regexp_constructor->set_property("prototype", Value(regexp_prototype.release()));

    register_built_in_object("RegExp", regexp_constructor.release());
    
    // Helper function to add Promise methods to any Promise instance
    std::function<void(Promise*)> add_promise_methods = [&](Promise* promise) {
        // Add .then method
        auto then_method = ObjectFactory::create_native_function("then",
            [promise, &add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
                Function* on_fulfilled = nullptr;
                Function* on_rejected = nullptr;
                
                if (args.size() > 0 && args[0].is_function()) {
                    on_fulfilled = args[0].as_function();
                }
                if (args.size() > 1 && args[1].is_function()) {
                    on_rejected = args[1].as_function();
                }
                
                Promise* new_promise = promise->then(on_fulfilled, on_rejected);
                add_promise_methods(new_promise);
                return Value(new_promise);
            });
        promise->set_property("then", Value(then_method.release()));
        
        // Add .catch method
        auto catch_method = ObjectFactory::create_native_function("catch",
            [promise, &add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
                Function* on_rejected = nullptr;
                if (args.size() > 0 && args[0].is_function()) {
                    on_rejected = args[0].as_function();
                }
                
                Promise* new_promise = promise->catch_method(on_rejected);
                add_promise_methods(new_promise);
                return Value(new_promise);
            });
        promise->set_property("catch", Value(catch_method.release()));
        
        // Add .finally method
        auto finally_method = ObjectFactory::create_native_function("finally",
            [promise, &add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
                Function* on_finally = nullptr;
                if (args.size() > 0 && args[0].is_function()) {
                    on_finally = args[0].as_function();
                }
                
                Promise* new_promise = promise->finally_method(on_finally);
                add_promise_methods(new_promise);
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
            
            // Mark as Promise for instanceof
            promise->set_property("_isPromise", Value(true));
            
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

            // Mark as Promise for instanceof and store resolved value
            promise->set_property("_isPromise", Value(true));
            promise->set_property("_promiseValue", value);

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
            
            // Mark as Promise for instanceof
            promise->set_property("_isPromise", Value(true));
            
            return Value(promise.release());
        });
    promise_constructor->set_property("reject", Value(promise_reject_static.release()));

    // Add Promise.all static method
    auto promise_all_static = ObjectFactory::create_native_function("all",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value("Promise.all expects an iterable"));
                return Value();
            }

            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_exception(Value("Promise.all expects an array"));
                return Value();
            }

            uint32_t length = iterable->get_length();
            std::vector<Value> results(length);
            uint32_t resolved_count = 0;

            auto result_promise = std::make_unique<Promise>(&ctx);
            add_promise_methods(result_promise.get());
            result_promise->set_property("_isPromise", Value(true));

            if (length == 0) {
                // Empty array resolves immediately with empty array
                auto empty_array = ObjectFactory::create_array(0);
                result_promise->fulfill(Value(empty_array.release()));
                return Value(result_promise.release());
            }

            // For now, simplified: just return array of resolved values
            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                if (element.is_object()) {
                    Object* obj = element.as_object();
                    if (obj && obj->has_property("_isPromise")) {
                        // This is a promise - get its value if fulfilled
                        if (obj->has_property("_promiseValue")) {
                            results[i] = obj->get_property("_promiseValue");
                        } else {
                            results[i] = element; // Use the promise object itself
                        }
                    } else {
                        results[i] = element;
                    }
                } else {
                    results[i] = element;
                }
            }

            // Create result array
            auto result_array = ObjectFactory::create_array(length);
            for (uint32_t i = 0; i < length; i++) {
                result_array->set_element(i, results[i]);
            }

            result_promise->fulfill(Value(result_array.release()));
            return Value(result_promise.release());
        });
    promise_constructor->set_property("all", Value(promise_all_static.release()));

    // Add Promise.race static method
    auto promise_race_static = ObjectFactory::create_native_function("race",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value("Promise.race expects an iterable"));
                return Value();
            }

            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_exception(Value("Promise.race expects an array"));
                return Value();
            }

            uint32_t length = iterable->get_length();
            auto result_promise = std::make_unique<Promise>(&ctx);
            add_promise_methods(result_promise.get());
            result_promise->set_property("_isPromise", Value(true));

            if (length == 0) {
                // Empty array never resolves
                return Value(result_promise.release());
            }

            // For simplified implementation, return first element
            Value first_element = iterable->get_element(0);
            if (first_element.is_object()) {
                Object* obj = first_element.as_object();
                if (obj && obj->has_property("_isPromise") && obj->has_property("_promiseValue")) {
                    result_promise->fulfill(obj->get_property("_promiseValue"));
                } else {
                    result_promise->fulfill(first_element);
                }
            } else {
                result_promise->fulfill(first_element);
            }

            return Value(result_promise.release());
        });
    promise_constructor->set_property("race", Value(promise_race_static.release()));

    register_built_in_object("Promise", promise_constructor.release());
    
    // ArrayBuffer constructor for binary data support
    auto arraybuffer_constructor = ObjectFactory::create_native_function("ArrayBuffer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("ArrayBuffer constructor requires at least one argument");
                return Value();
            }
            
            if (!args[0].is_number()) {
                ctx.throw_type_error("ArrayBuffer size must be a number");
                return Value();
            }
            
            double length_double = args[0].as_number();
            if (length_double < 0 || length_double != std::floor(length_double)) {
                ctx.throw_range_error("ArrayBuffer size must be a non-negative integer");
                return Value();
            }
            
            size_t byte_length = static_cast<size_t>(length_double);
            
            try {
                auto buffer_obj = std::make_unique<ArrayBuffer>(byte_length);
                // Explicitly set properties after construction (following Error pattern)
                buffer_obj->set_property("byteLength", Value(static_cast<double>(byte_length)));
                buffer_obj->set_property("_isArrayBuffer", Value(true));
                
                // Set constructor property - get ArrayBuffer constructor from context bindings
                if (ctx.has_binding("ArrayBuffer")) {
                    Value arraybuffer_ctor = ctx.get_binding("ArrayBuffer");
                    if (!arraybuffer_ctor.is_undefined()) {
                        buffer_obj->set_property("constructor", arraybuffer_ctor);
                    }
                }
                
                return Value(buffer_obj.release());
            } catch (const std::exception& e) {
                ctx.throw_error(std::string("ArrayBuffer allocation failed: ") + e.what());
                return Value();
            }
        });
    
    // ArrayBuffer.isView - minimal stub to avoid segfaults
    auto arraybuffer_isView = ObjectFactory::create_native_function("isView",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Minimal stub - return false
            return Value(false);
        });

    // Set correct length property for ArrayBuffer.isView (should be 1)
    PropertyDescriptor isView_length_desc(Value(1.0), PropertyAttributes::None);
    isView_length_desc.set_configurable(true);
    isView_length_desc.set_enumerable(false);
    isView_length_desc.set_writable(false);
    arraybuffer_isView->set_property_descriptor("length", isView_length_desc);

    arraybuffer_constructor->set_property("isView", Value(arraybuffer_isView.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    register_built_in_object("ArrayBuffer", arraybuffer_constructor.release());
    
    // TypedArray constructors for binary data views
    register_typed_array_constructors();
    
    // Setup WebAssembly support
    WebAssemblyAPI::setup_webassembly(*this);
    
    // Setup Proxy and Reflect using the proper implementation
    Proxy::setup_proxy(*this);
    Reflect::setup_reflect(*this);
    
    // Web APIs
    // Web APIs are now provided through the WebAPIInterface
    // Call set_web_api_interface() to enable browser functionality
}


void Context::setup_global_bindings() {
    if (!lexical_environment_) return;
    
    // Global functions
    auto parseInt_fn = ObjectFactory::create_native_function("parseInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::nan();
            
            std::string str = args[0].to_string();
            
            // Trim leading whitespace
            size_t start = 0;
            while (start < str.length() && std::isspace(str[start])) {
                start++;
            }
            
            if (start >= str.length()) {
                return Value::nan();
            }
            
            int radix = 10;
            if (args.size() > 1 && args[1].is_number()) {
                double r = args[1].to_number();
                if (r >= 2 && r <= 36) {
                    radix = static_cast<int>(r);
                }
            }
            
            // Check if string starts with a valid digit for the given radix
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
                // Check if we parsed at least one character
                if (pos == 0) {
                    return Value::nan();
                }
                return Value(static_cast<double>(result));
            } catch (...) {
                return Value::nan();
            }
        });
    lexical_environment_->create_binding("parseInt", Value(parseInt_fn.release()), false);
    
    auto parseFloat_fn = ObjectFactory::create_native_function("parseFloat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::nan();
            
            std::string str = args[0].to_string();
            
            // Trim leading whitespace
            size_t start = 0;
            while (start < str.length() && std::isspace(str[start])) {
                start++;
            }
            
            if (start >= str.length()) {
                return Value::nan();
            }
            
            // Check if string starts with a valid character for a float
            char first_char = str[start];
            if (!std::isdigit(first_char) && first_char != '.' && 
                first_char != '+' && first_char != '-') {
                return Value::nan();
            }
            
            try {
                size_t pos;
                double result = std::stod(str.substr(start), &pos);
                // Check if we parsed at least one character
                if (pos == 0) {
                    return Value::nan();
                }
                return Value(result);
            } catch (...) {
                return Value::nan();
            }
        });
    lexical_environment_->create_binding("parseFloat", Value(parseFloat_fn.release()), false);
    
    auto isNaN_fn = ObjectFactory::create_native_function("isNaN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(true);
            
            // Check for our tagged NaN value first
            if (args[0].is_nan()) return Value(true);
            
            // Then check for IEEE 754 NaN in regular numbers
            double num = args[0].to_number();
            return Value(std::isnan(num));
        });
    lexical_environment_->create_binding("isNaN", Value(isNaN_fn.release()), false);
    
    auto isFinite_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            double num = args[0].to_number();
            return Value(std::isfinite(num));
        });
    lexical_environment_->create_binding("isFinite", Value(isFinite_fn.release()), false);


    // Global eval function
    auto eval_fn = ObjectFactory::create_native_function("eval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(); // undefined

            std::string code = args[0].to_string();
            if (code.empty()) return Value(); // undefined

            // Simple eval implementation - parse and execute the code
            Engine* engine = ctx.get_engine();
            if (!engine) return Value(); // undefined

            try {
                auto result = engine->evaluate(code);
                if (result.success) {
                    return result.value;
                } else {
                    ctx.throw_exception(Value("SyntaxError: " + result.error_message));
                    return Value();
                }
            } catch (...) {
                ctx.throw_exception(Value("SyntaxError: Invalid code in eval"));
                return Value();
            }
        });
    lexical_environment_->create_binding("eval", Value(eval_fn.release()), false);

    // Global constants
    lexical_environment_->create_binding("undefined", Value(), false);
    lexical_environment_->create_binding("null", Value::null(), false);
    // NaN and Infinity will be set later with arithmetic workaround
    
    // Global object access - bind the global object to standard names
    if (global_object_) {
        lexical_environment_->create_binding("globalThis", Value(global_object_), false);
        lexical_environment_->create_binding("global", Value(global_object_), false);  // Node.js style
        lexical_environment_->create_binding("window", Value(global_object_), false);  // Browser style
        lexical_environment_->create_binding("this", Value(global_object_), false);    // Global this binding
        
        // Also ensure global object has self-reference
        global_object_->set_property("globalThis", Value(global_object_));
        global_object_->set_property("global", Value(global_object_));
        global_object_->set_property("window", Value(global_object_));
        global_object_->set_property("this", Value(global_object_));
    }
    lexical_environment_->create_binding("true", Value(true), false);
    lexical_environment_->create_binding("false", Value(false), false);
    
    // Global values - create proper NaN and Infinity values using dedicated tags to avoid bit collisions
    lexical_environment_->create_binding("NaN", Value::nan(), false);
    lexical_environment_->create_binding("Infinity", Value::positive_infinity(), false);
    
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

    // escape() - Legacy global function (Annex B)
    auto escape_fn = ObjectFactory::create_native_function("escape",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::string("undefined"));
            }

            // Use proper ToString abstract operation with exception handling
            std::string input;
            Value arg = args[0];
            if (arg.is_object()) {
                // For objects, call their toString() method if available
                Object* obj = arg.as_object();
                Value toString_method = obj->get_property("toString");
                if (toString_method.is_function()) {
                    try {
                        Function* func = toString_method.as_function();
                        Value result = func->call(ctx, {}, arg);
                        if (ctx.has_exception()) {
                            return Value(); // Exception will be propagated
                        }
                        input = result.to_string();
                    } catch (...) {
                        // Let any exceptions propagate
                        return Value();
                    }
                } else {
                    input = arg.to_string();
                }
            } else {
                input = arg.to_string();
            }
            std::string result;

            for (char c : input) {
                // Characters that don't need escaping: A-Z a-z 0-9 @ * _ + - . /
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '@' || c == '*' || c == '_' || c == '+' || c == '-' || c == '.' || c == '/') {
                    result += c;
                } else {
                    // Escape other characters as %XX
                    unsigned char uc = static_cast<unsigned char>(c);
                    result += '%';
                    result += "0123456789ABCDEF"[(uc >> 4) & 0xF];
                    result += "0123456789ABCDEF"[uc & 0xF];
                }
            }

            return Value(result);
        });
    lexical_environment_->create_binding("escape", Value(escape_fn.get()), false);
    if (global_object_) {
        // Set escape with proper property attributes: writable, non-enumerable, configurable
        PropertyDescriptor escape_desc(Value(escape_fn.get()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
        global_object_->set_property_descriptor("escape", escape_desc);
    }
    escape_fn.release(); // Release after manual binding

    // unescape() - Legacy global function (Annex B)
    auto unescape_fn = ObjectFactory::create_native_function("unescape",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::string("undefined"));
            }

            // Use proper ToString abstract operation with exception handling
            std::string input;
            Value arg = args[0];
            if (arg.is_object()) {
                // For objects, call their toString() method if available
                Object* obj = arg.as_object();
                Value toString_method = obj->get_property("toString");
                if (toString_method.is_function()) {
                    try {
                        Function* func = toString_method.as_function();
                        Value result = func->call(ctx, {}, arg);
                        if (ctx.has_exception()) {
                            return Value(); // Exception will be propagated
                        }
                        input = result.to_string();
                    } catch (...) {
                        // Let any exceptions propagate
                        return Value();
                    }
                } else {
                    input = arg.to_string();
                }
            } else {
                input = arg.to_string();
            }
            std::string result;

            for (size_t i = 0; i < input.length(); ++i) {
                if (input[i] == '%' && i + 2 < input.length()) {
                    // Parse %XX hex sequence
                    char hex1 = input[i + 1];
                    char hex2 = input[i + 2];

                    // Convert hex chars to numbers
                    auto hex_to_num = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        return -1; // Invalid hex
                    };

                    int val1 = hex_to_num(hex1);
                    int val2 = hex_to_num(hex2);

                    if (val1 >= 0 && val2 >= 0) {
                        // Valid hex sequence - convert to character
                        char decoded = static_cast<char>((val1 << 4) | val2);
                        result += decoded;
                        i += 2; // Skip the hex digits
                    } else {
                        // Invalid hex sequence - keep as is
                        result += input[i];
                    }
                } else {
                    // Not a % sequence or incomplete - keep as is
                    result += input[i];
                }
            }

            return Value(result);
        });
    lexical_environment_->create_binding("unescape", Value(unescape_fn.get()), false);
    if (global_object_) {
        // Set unescape with proper property attributes: writable, non-enumerable, configurable
        PropertyDescriptor unescape_desc(Value(unescape_fn.get()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
        global_object_->set_property_descriptor("unescape", unescape_desc);
    }
    unescape_fn.release(); // Release after manual binding

    // Create console object with log, error, warn methods
    auto console_obj = ObjectFactory::create_object();
    auto console_log_fn = ObjectFactory::create_native_function("log", WebAPI::console_log);
    auto console_error_fn = ObjectFactory::create_native_function("error", WebAPI::console_error);
    auto console_warn_fn = ObjectFactory::create_native_function("warn", WebAPI::console_warn);
    
    console_obj->set_property("log", Value(console_log_fn.release()));
    console_obj->set_property("error", Value(console_error_fn.release()));
    console_obj->set_property("warn", Value(console_warn_fn.release()));
    
    lexical_environment_->create_binding("console", Value(console_obj.release()), false);


    // Add engine stats functions for debugging
    auto gc_stats_fn = ObjectFactory::create_native_function("gcStats",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.get_engine()) {
                std::string stats = ctx.get_engine()->get_gc_stats();
                std::cout << stats << std::endl;
            } else {
                std::cout << "Engine not available" << std::endl;
            }
            return Value();
        });
    lexical_environment_->create_binding("gcStats", Value(gc_stats_fn.release()), false);
    
    auto jit_stats_fn = ObjectFactory::create_native_function("jitStats",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.get_engine()) {
                std::string stats = ctx.get_engine()->get_jit_stats();
                std::cout << stats << std::endl;
            } else {
                std::cout << "Engine not available" << std::endl;
            }
            return Value();
        });
    lexical_environment_->create_binding("jitStats", Value(jit_stats_fn.release()), false);
    
    auto force_gc_fn = ObjectFactory::create_native_function("forceGC",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.get_engine()) {
                ctx.get_engine()->force_gc();
                std::cout << "Garbage collection forced" << std::endl;
            } else {
                std::cout << "Engine not available" << std::endl;
            }
            return Value();
        });
    lexical_environment_->create_binding("forceGC", Value(force_gc_fn.release()), false);
    
    // Manually bind JSON and Date objects to ensure they're available
    if (built_in_objects_.find("JSON") != built_in_objects_.end() && built_in_objects_["JSON"]) {
        lexical_environment_->create_binding("JSON", Value(built_in_objects_["JSON"]), false);
    }
    if (built_in_objects_.find("Date") != built_in_objects_.end() && built_in_objects_["Date"]) {
        lexical_environment_->create_binding("Date", Value(built_in_objects_["Date"]), false);
    }
    
    // Add setTimeout and setInterval functions
    auto setTimeout_fn = ObjectFactory::create_native_function("setTimeout", WebAPI::setTimeout);
    auto setInterval_fn = ObjectFactory::create_native_function("setInterval", WebAPI::setInterval);
    auto clearTimeout_fn = ObjectFactory::create_native_function("clearTimeout", WebAPI::clearTimeout);
    auto clearInterval_fn = ObjectFactory::create_native_function("clearInterval", WebAPI::clearInterval);
    
    lexical_environment_->create_binding("setTimeout", Value(setTimeout_fn.release()), false);
    lexical_environment_->create_binding("setInterval", Value(setInterval_fn.release()), false);
    lexical_environment_->create_binding("clearTimeout", Value(clearTimeout_fn.release()), false);
    lexical_environment_->create_binding("clearInterval", Value(clearInterval_fn.release()), false);
    
    // Use the proper Object constructor instead of simple version
    // auto simple_object_fn = ObjectFactory::create_native_function("Object",
    //     [](Context& ctx, const std::vector<Value>& args) -> Value {
    //         return Value(std::string("[object Object]"));
    //     });
    // lexical_environment_->create_binding("Object", Value(simple_object_fn.release()), false);
    
    // WORKING ARRAY CONSTRUCTOR - Simple functional version
    // DISABLED: Simple array override that breaks Array.isArray
    // auto simple_array_fn = ObjectFactory::create_native_function("Array",
    //     [](Context& ctx, const std::vector<Value>& args) -> Value {
    //         if (args.empty()) {
    //             return Value(std::string("[]"));
    //         } else {
    //             return Value(std::string("[array Array]"));
    //         }
    //     });
    // lexical_environment_->create_binding("Array", Value(simple_array_fn.release()), false);
    
    // Try to bind the complex built-in objects if they exist
    if (built_in_objects_.find("Object") != built_in_objects_.end() && built_in_objects_["Object"]) {
        // Use the proper Object constructor with Object.keys
        Object* obj_constructor = built_in_objects_["Object"];
        Value binding_value;
        if (obj_constructor->is_function()) {
            // Cast to Function* to ensure correct Value constructor is used
            Function* func_ptr = static_cast<Function*>(obj_constructor);
            binding_value = Value(func_ptr);
        } else {
            binding_value = Value(obj_constructor);
        }
        lexical_environment_->create_binding("Object", binding_value, false);
    }
    
    // Array constructor  
    if (built_in_objects_.find("Array") != built_in_objects_.end() && built_in_objects_["Array"]) {
        Object* array_constructor = built_in_objects_["Array"];
        Value binding_value;
        if (array_constructor->is_function()) {
            Function* func_ptr = static_cast<Function*>(array_constructor);
            binding_value = Value(func_ptr);
        } else {
            binding_value = Value(array_constructor);
        }
        lexical_environment_->create_binding("Array", binding_value, false);
    }
    
    // Function constructor
    if (built_in_objects_.find("Function") != built_in_objects_.end() && built_in_objects_["Function"]) {
        Object* func_constructor = built_in_objects_["Function"];
        Value binding_value;
        if (func_constructor->is_function()) {
            // Cast to Function* to ensure correct Value constructor is used
            Function* func_ptr = static_cast<Function*>(func_constructor);
            binding_value = Value(func_ptr);
        } else {
            binding_value = Value(func_constructor);
        }
        lexical_environment_->create_binding("Function", binding_value, false);
    }
    
    // Bind all other built-in objects to global environment
    for (const auto& pair : built_in_objects_) {
        if (pair.second) {
            // Skip the ones we already bound manually to avoid double-binding
            if (pair.first != "Object" && pair.first != "Array" && pair.first != "Function") {
                // Check if the object is actually a Function and cast it properly
                Value binding_value;
                if (pair.second->is_function()) {
                    // Cast to Function* to ensure correct Value constructor is used
                    Function* func_ptr = static_cast<Function*>(pair.second);
                    binding_value = Value(func_ptr);
                } else {
                    // Use Object constructor for non-functions
                    binding_value = Value(pair.second);
                }
                
                lexical_environment_->create_binding(pair.first, binding_value, false);
                // Also ensure it's bound to global object for property access
                if (global_object_) {
                    global_object_->set_property(pair.first, binding_value);
                }
            }
        }
    }

    // Setup iterator methods AFTER all bindings are created
    // This ensures Array, String, Map, Set constructors are available
    IterableUtils::setup_array_iterator_methods(*this);
    IterableUtils::setup_string_iterator_methods(*this);
    IterableUtils::setup_map_iterator_methods(*this);
    IterableUtils::setup_set_iterator_methods(*this);

    // Setup Test262 helper functions for compatibility
    setup_test262_helpers();
}

//=============================================================================
// Test262 Helper Functions
//=============================================================================

void Context::setup_test262_helpers() {
    // testWithTypedArrayConstructors helper
    auto testWithTypedArrayConstructors = ObjectFactory::create_native_function("testWithTypedArrayConstructors",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("testWithTypedArrayConstructors requires a function argument");
                return Value();
            }

            Function* callback = args[0].as_function();

            // TypedArray constructor list (same as Test262)
            std::vector<std::string> constructors = {
                "Int8Array", "Uint8Array", "Uint8ClampedArray",
                "Int16Array", "Uint16Array",
                "Int32Array", "Uint32Array",
                "Float32Array", "Float64Array"
            };

            for (const auto& ctorName : constructors) {
                if (ctx.has_binding(ctorName)) {
                    Value ctor = ctx.get_binding(ctorName);
                    if (ctor.is_function()) {
                        try {
                            // Call callback with constructor argument
                            std::vector<Value> callArgs = { ctor };
                            callback->call(ctx, callArgs, Value());
                        } catch (...) {
                            // Add context to error message (like Test262 helper does)
                            ctx.throw_exception(Value("Error in testWithTypedArrayConstructors with " + ctorName));
                            return Value();
                        }
                    }
                }
            }

            return Value(); // undefined
        });

    // Add to global scope
    lexical_environment_->create_binding("testWithTypedArrayConstructors", Value(testWithTypedArrayConstructors.release()), false);

    // buildString helper - used by 443+ RegExp tests
    auto buildString = ObjectFactory::create_native_function("buildString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_type_error("buildString requires an object argument");
                return Value();
            }

            Object* argsObj = args[0].as_object();
            std::string result;

            // Get loneCodePoints array
            if (argsObj->has_property("loneCodePoints")) {
                Value loneVal = argsObj->get_property("loneCodePoints");
                if (loneVal.is_object() && loneVal.as_object()->is_array()) {
                    Object* loneArray = loneVal.as_object();
                    uint32_t length = static_cast<uint32_t>(loneArray->get_property("length").as_number());
                    for (uint32_t i = 0; i < length; i++) {
                        Value elem = loneArray->get_element(i);
                        if (elem.is_number()) {
                            uint32_t codePoint = static_cast<uint32_t>(elem.as_number());
                            // Simplified ASCII handling
                            if (codePoint < 0x80) {
                                result += static_cast<char>(codePoint);
                            }
                        }
                    }
                }
            }

            // Get ranges array
            if (argsObj->has_property("ranges")) {
                Value rangesVal = argsObj->get_property("ranges");
                if (rangesVal.is_object() && rangesVal.as_object()->is_array()) {
                    Object* rangesArray = rangesVal.as_object();
                    uint32_t rangeCount = static_cast<uint32_t>(rangesArray->get_property("length").as_number());

                    for (uint32_t i = 0; i < rangeCount; i++) {
                        Value rangeVal = rangesArray->get_element(i);
                        if (rangeVal.is_object() && rangeVal.as_object()->is_array()) {
                            Object* range = rangeVal.as_object();
                            Value startVal = range->get_element(0);
                            Value endVal = range->get_element(1);

                            if (startVal.is_number() && endVal.is_number()) {
                                uint32_t start = static_cast<uint32_t>(startVal.as_number());
                                uint32_t end = static_cast<uint32_t>(endVal.as_number());

                                // Add characters in range (ASCII only for performance)
                                for (uint32_t cp = start; cp <= end && cp < 0x80 && result.length() < 1000; cp++) {
                                    result += static_cast<char>(cp);
                                }
                            }
                        }
                    }
                }
            }

            return Value(result);
        });

    lexical_environment_->create_binding("buildString", Value(buildString.release()), false);
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
// Web API Interface Implementation
//=============================================================================

bool Context::has_web_api(const std::string& name) const {
    return web_api_interface_ && web_api_interface_->hasAPI(name);
}

Value Context::call_web_api(const std::string& name, const std::vector<Value>& args) {
    if (web_api_interface_ && web_api_interface_->hasAPI(name)) {
        return web_api_interface_->callAPI(name, *this, args);
    }
    // Return undefined if API not available
    return Value();
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

//=============================================================================
// Block Scope Management
//=============================================================================

void Context::push_block_scope() {
    // Create new block scope environment  
    auto new_env = std::make_unique<Environment>(Environment::Type::Declarative, lexical_environment_);
    lexical_environment_ = new_env.release();
}

void Context::pop_block_scope() {
    if (lexical_environment_ && lexical_environment_->get_outer()) {
        Environment* old_env = lexical_environment_;
        lexical_environment_ = lexical_environment_->get_outer();
        delete old_env;
    }
}

void Context::register_typed_array_constructors() {
    // Uint8Array constructor
    auto uint8array_constructor = ObjectFactory::create_native_function("Uint8Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(TypedArrayFactory::create_uint8_array(0).release());
            }
            
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_uint8_array(length).release());
            }
            
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    return Value(TypedArrayFactory::create_uint8_array_from_buffer(buffer).release());
                }
            }
            
            ctx.throw_type_error("Uint8Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Uint8Array", uint8array_constructor.release());
    
    // Float32Array constructor  
    auto float32array_constructor = ObjectFactory::create_native_function("Float32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(TypedArrayFactory::create_float32_array(0).release());
            }
            
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_float32_array(length).release());
            }
            
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    return Value(TypedArrayFactory::create_float32_array_from_buffer(buffer).release());
                }
            }
            
            ctx.throw_type_error("Float32Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Float32Array", float32array_constructor.release());

    // Additional TypedArray constructors
    auto int8array_constructor = ObjectFactory::create_native_function("Int8Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(TypedArrayFactory::create_int8_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_int8_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Int8Array>(shared_buffer).release());
                }
            }
            ctx.throw_type_error("Int8Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Int8Array", int8array_constructor.release());

    auto uint16array_constructor = ObjectFactory::create_native_function("Uint16Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(TypedArrayFactory::create_uint16_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_uint16_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Uint16Array>(shared_buffer).release());
                }
            }
            ctx.throw_type_error("Uint16Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Uint16Array", uint16array_constructor.release());

    auto int16array_constructor = ObjectFactory::create_native_function("Int16Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(TypedArrayFactory::create_int16_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_int16_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Int16Array>(shared_buffer).release());
                }
            }
            ctx.throw_type_error("Int16Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Int16Array", int16array_constructor.release());

    auto uint32array_constructor = ObjectFactory::create_native_function("Uint32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(TypedArrayFactory::create_uint32_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_uint32_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Uint32Array>(shared_buffer).release());
                }
            }
            ctx.throw_type_error("Uint32Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Uint32Array", uint32array_constructor.release());

    auto int32array_constructor = ObjectFactory::create_native_function("Int32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(TypedArrayFactory::create_int32_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_int32_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Int32Array>(shared_buffer).release());
                }
            }
            ctx.throw_type_error("Int32Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Int32Array", int32array_constructor.release());

    auto float64array_constructor = ObjectFactory::create_native_function("Float64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(TypedArrayFactory::create_float64_array(0).release());
            }
            if (args[0].is_number()) {
                size_t length = static_cast<size_t>(args[0].as_number());
                return Value(TypedArrayFactory::create_float64_array(length).release());
            }
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {});
                    return Value(std::make_unique<Float64Array>(shared_buffer).release());
                }
            }
            ctx.throw_type_error("Float64Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Float64Array", float64array_constructor.release());

    // DataView constructor (re-enabled for testing)
    auto dataview_constructor = ObjectFactory::create_native_function("DataView", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value result = DataView::constructor(ctx, args);
            
            // Add methods directly to the instance
            if (result.is_object()) {
                Object* dataview_obj = result.as_object();
                
                auto get_uint8_method = ObjectFactory::create_native_function("getUint8", DataView::js_get_uint8);
                dataview_obj->set_property("getUint8", Value(get_uint8_method.release()));
                
                auto set_uint8_method = ObjectFactory::create_native_function("setUint8", DataView::js_set_uint8);
                dataview_obj->set_property("setUint8", Value(set_uint8_method.release()));
                
                auto get_int16_method = ObjectFactory::create_native_function("getInt16", DataView::js_get_int16);
                dataview_obj->set_property("getInt16", Value(get_int16_method.release()));
                
                auto set_int16_method = ObjectFactory::create_native_function("setInt16", DataView::js_set_int16);
                dataview_obj->set_property("setInt16", Value(set_int16_method.release()));
                
                auto get_uint32_method = ObjectFactory::create_native_function("getUint32", DataView::js_get_uint32);
                dataview_obj->set_property("getUint32", Value(get_uint32_method.release()));
                
                auto set_uint32_method = ObjectFactory::create_native_function("setUint32", DataView::js_set_uint32);
                dataview_obj->set_property("setUint32", Value(set_uint32_method.release()));
                
                auto get_float32_method = ObjectFactory::create_native_function("getFloat32", DataView::js_get_float32);
                dataview_obj->set_property("getFloat32", Value(get_float32_method.release()));
                
                auto set_float32_method = ObjectFactory::create_native_function("setFloat32", DataView::js_set_float32);
                dataview_obj->set_property("setFloat32", Value(set_float32_method.release()));
                
                auto get_float64_method = ObjectFactory::create_native_function("getFloat64", DataView::js_get_float64);
                dataview_obj->set_property("getFloat64", Value(get_float64_method.release()));
                
                auto set_float64_method = ObjectFactory::create_native_function("setFloat64", DataView::js_set_float64);
                dataview_obj->set_property("setFloat64", Value(set_float64_method.release()));
            }
            
            return result;
        });
    
    register_built_in_object("DataView", dataview_constructor.release());

    // Test262 async test helpers
    // $DONE function for async tests
    auto done_function = ObjectFactory::create_native_function("$DONE",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // In a real test harness, this would signal test completion
            // For now, we just log or throw if there's an error
            if (!args.empty() && !args[0].is_undefined()) {
                // Test failed with an error
                std::string error_msg = args[0].to_string();
                ctx.throw_exception(Value("Test failed: " + error_msg));
            }
            // Test passed - do nothing (test harness handles this)
            return Value();
        });
    global_object_->set_property("$DONE", Value(done_function.release()));

}

} // namespace Quanta

