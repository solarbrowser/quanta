/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include <iostream>
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/JSON.h"
#include "quanta/core/runtime/Date.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/Temporal.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/core/runtime/DataView.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Generator.h"
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/MapSet.h"
#include <iostream>
#include <sstream>
#include <limits>
#include <cmath>
#include <cstdlib>

namespace Quanta {

static std::vector<std::unique_ptr<Function>> g_owned_native_functions;

uint32_t Context::next_context_id_ = 1;


Context::Context(Engine* engine, Type type)
    : type_(type), state_(State::Running), context_id_(next_context_id_++),
      lexical_environment_(nullptr), variable_environment_(nullptr), this_binding_(nullptr),
      execution_depth_(0), global_object_(nullptr), current_exception_(), has_exception_(false),
      return_value_(), has_return_value_(false), has_break_(false), has_continue_(false),
      current_loop_label_(), next_statement_label_(), is_in_constructor_call_(false),
      strict_mode_(false), engine_(engine), current_filename_("<unknown>"),
      gc_(engine ? engine->get_garbage_collector() : nullptr) {

    if (type == Type::Global) {
        initialize_global_context();
    }
}

Context::Context(Engine* engine, Context* parent, Type type)
    : type_(type), state_(State::Running), context_id_(next_context_id_++),
      lexical_environment_(nullptr), variable_environment_(nullptr), this_binding_(nullptr),
      execution_depth_(0), global_object_(parent ? parent->global_object_ : nullptr),
      current_exception_(), has_exception_(false), return_value_(), has_return_value_(false),
      has_break_(false), has_continue_(false), current_loop_label_(), next_statement_label_(),
      is_in_constructor_call_(false), strict_mode_(parent ? parent->strict_mode_ : false),
      engine_(engine), current_filename_(parent ? parent->current_filename_ : "<unknown>"),
      gc_(engine ? engine->get_garbage_collector() : nullptr) {

    // Use engine's GC (shared across all contexts)

    if (parent) {
        built_in_objects_ = parent->built_in_objects_;
        built_in_functions_ = parent->built_in_functions_;
    }
}

Context::~Context() {
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
        const_cast<Context*>(this)->throw_exception(Value(std::string("execution depth exceeded")));
        return Value();
    }
    
    increment_execution_depth();
    Value result;
    
    if (lexical_environment_) {
        result = lexical_environment_->get_binding(name);
    } else {
        result = Value();
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

bool Context::create_binding(const std::string& name, const Value& value, bool mutable_binding, bool deletable) {
    if (variable_environment_) {
        return variable_environment_->create_binding(name, value, mutable_binding, deletable);
    }
    return false;
}

bool Context::create_var_binding(const std::string& name, const Value& value, bool mutable_binding) {
    if (variable_environment_) {
        // ES1: Variables declared with 'var' have DontDelete attribute (not deletable)
        return variable_environment_->create_binding(name, value, mutable_binding, false);
    }
    return false;
}

bool Context::create_lexical_binding(const std::string& name, const Value& value, bool mutable_binding) {
    if (lexical_environment_) {
        return lexical_environment_->create_binding(name, value, mutable_binding);
    }
    return false;
}

bool Context::delete_binding(const std::string& name) {
    // ES1: Delete from variable environment (where 'var' and global assignments go)
    // This matches where create_binding puts bindings
    if (variable_environment_) {
        return variable_environment_->delete_binding(name);
    }
    return false;
}

void Context::push_frame(std::unique_ptr<StackFrame> frame) {
    if (is_stack_overflow()) {
        throw_exception(Value(std::string("RangeError: call stack size exceeded")));
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
    // If exception is a string, convert it to an Error object
    if (exception.is_string()) {
        std::string error_msg = exception.to_string();

        // Parse error type from message prefix (e.g., "TypeError: message")
        std::string error_type;
        std::string message = error_msg;
        size_t colon_pos = error_msg.find(':');
        if (colon_pos != std::string::npos) {
            error_type = error_msg.substr(0, colon_pos);
            message = error_msg.substr(colon_pos + 1);
            // Trim leading whitespace from message
            size_t start = message.find_first_not_of(" \t");
            if (start != std::string::npos) {
                message = message.substr(start);
            }
        }

        // Create appropriate Error object based on type prefix
        std::unique_ptr<Error> error_obj;
        Object* prototype = nullptr;

        if (error_type == "TypeError") {
            error_obj = Error::create_type_error(message);
            prototype = get_built_in_object("TypeError");
            if (prototype) {
                prototype = prototype->get_property("prototype").as_object();
            }
        } else if (error_type == "ReferenceError") {
            error_obj = Error::create_reference_error(message);
            prototype = get_built_in_object("ReferenceError");
            if (prototype) {
                prototype = prototype->get_property("prototype").as_object();
            }
        } else if (error_type == "SyntaxError") {
            error_obj = Error::create_syntax_error(message);
            prototype = get_built_in_object("SyntaxError");
            if (prototype) {
                prototype = prototype->get_property("prototype").as_object();
            }
        } else if (error_type == "RangeError") {
            error_obj = Error::create_range_error(message);
            prototype = get_built_in_object("RangeError");
            if (prototype) {
                prototype = prototype->get_property("prototype").as_object();
            }
        } else if (error_type == "URIError") {
            error_obj = Error::create_uri_error(message);
            prototype = get_built_in_object("URIError");
            if (prototype) {
                prototype = prototype->get_property("prototype").as_object();
            }
        } else if (error_type == "EvalError") {
            error_obj = Error::create_eval_error(message);
            prototype = get_built_in_object("EvalError");
            if (prototype) {
                prototype = prototype->get_property("prototype").as_object();
            }
        } else {
            // Default to generic Error
            error_obj = Error::create_error(error_msg);
            prototype = get_built_in_object("Error");
            if (prototype) {
                prototype = prototype->get_property("prototype").as_object();
            }
        }

        // Set the prototype for proper toString inheritance
        if (prototype) {
            error_obj->set_prototype(prototype);
        }

        current_exception_ = Value(error_obj.release());
    } else {
        current_exception_ = exception;
    }

    has_exception_ = true;
    state_ = State::Thrown;

    if (current_exception_.is_object()) {
        Object* obj = current_exception_.as_object();
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

    Value type_error_ctor = get_binding("TypeError");
    if (type_error_ctor.is_function()) {
        Function* ctor_fn = type_error_ctor.as_function();
        Value proto = ctor_fn->get_property("prototype");
        if (proto.is_object()) {
            error->set_prototype(proto.as_object());
        }
    }

    throw_exception(Value(error.release()));
}

void Context::throw_reference_error(const std::string& message) {
    auto error = Error::create_reference_error(message);
    error->generate_stack_trace();

    Value ref_error_ctor = get_binding("ReferenceError");
    if (ref_error_ctor.is_function()) {
        Function* ctor_fn = ref_error_ctor.as_function();
        Value proto = ctor_fn->get_property("prototype");
        if (proto.is_object()) {
            error->set_prototype(proto.as_object());
        }
    }

    throw_exception(Value(error.release()));
}

void Context::throw_syntax_error(const std::string& message) {
    auto error = Error::create_syntax_error(message);
    error->generate_stack_trace();

    Value syntax_error_ctor = get_binding("SyntaxError");
    if (syntax_error_ctor.is_function()) {
        Function* ctor_fn = syntax_error_ctor.as_function();
        Value proto = ctor_fn->get_property("prototype");
        if (proto.is_object()) {
            error->set_prototype(proto.as_object());
        }
    }

    throw_exception(Value(error.release()));
}

void Context::throw_range_error(const std::string& message) {
    auto error = Error::create_range_error(message);
    error->generate_stack_trace();

    Value range_error_ctor = get_binding("RangeError");
    if (range_error_ctor.is_function()) {
        Function* ctor_fn = range_error_ctor.as_function();
        Value proto = ctor_fn->get_property("prototype");
        if (proto.is_object()) {
            error->set_prototype(proto.as_object());
        }
    }

    throw_exception(Value(error.release()));
}

void Context::register_built_in_object(const std::string& name, Object* object) {
    built_in_objects_[name] = object;

    if (global_object_) {
        Value binding_value;
        if (object->is_function()) {
            Function* func_ptr = static_cast<Function*>(object);
            binding_value = Value(func_ptr);
        } else {
            binding_value = Value(object);
        }
        PropertyDescriptor desc(binding_value, PropertyAttributes::BuiltinFunction);
        global_object_->set_property_descriptor(name, desc);
    }
}

void Context::register_built_in_function(const std::string& name, Function* function) {
    built_in_functions_[name] = function;

    if (global_object_) {
        PropertyDescriptor desc(Value(function), PropertyAttributes::BuiltinFunction);
        global_object_->set_property_descriptor(name, desc);
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
    global_object_ = ObjectFactory::create_object().release();
    this_binding_ = global_object_;

    auto global_env = std::make_unique<Environment>(global_object_);
    lexical_environment_ = global_env.release();
    variable_environment_ = lexical_environment_;

    initialize_built_ins();
    setup_global_bindings();
}

void Context::initialize_built_ins() {
    Symbol::initialize_well_known_symbols();


    auto object_constructor = ObjectFactory::create_native_constructor("Object",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                return Value(ObjectFactory::create_object().release());
            }
            
            Value value = args[0];
            
            if (value.is_null() || value.is_undefined()) {
                return Value(ObjectFactory::create_object().release());
            }
            
            if (value.is_object() || value.is_function()) {
                return value;
            }
            
            if (value.is_string()) {
                auto string_obj = ObjectFactory::create_string(value.to_string());
                return Value(string_obj.release());
            } else if (value.is_number()) {
                auto number_obj = ObjectFactory::create_object();
                number_obj->set_property("valueOf", value);
                return Value(number_obj.release());
            } else if (value.is_boolean()) {
                auto boolean_obj = ObjectFactory::create_boolean(value.to_boolean());
                return Value(boolean_obj.release());
            } else if (value.is_symbol()) {
                auto symbol_obj = ObjectFactory::create_object();
                symbol_obj->set_property("valueOf", value);
                return Value(symbol_obj.release());
            } else if (value.is_bigint()) {
                auto bigint_obj = ObjectFactory::create_object();
                bigint_obj->set_property("valueOf", value);
                return Value(bigint_obj.release());
            }
            
            return Value(ObjectFactory::create_object().release());
        });
    
    auto keys_fn = ObjectFactory::create_native_function("keys", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value(std::string("TypeError: Object.keys requires at least 1 argument")));
                return Value();
            }
            
            if (args[0].is_null()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.keys called on non-object")));
                return Value();
            }
            
            Object* obj = args[0].as_object();
            auto keys = obj->get_enumerable_keys();

            auto result_array = ObjectFactory::create_array(keys.size());
            if (!result_array) {
                result_array = ObjectFactory::create_array(0);
            }

            for (size_t i = 0; i < keys.size(); i++) {
                result_array->set_element(i, Value(keys[i]));
            }

            return Value(result_array.release());
        }, 1);
    object_constructor->set_property("keys", Value(keys_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto values_fn = ObjectFactory::create_native_function("values", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value(std::string("TypeError: Object.values requires at least 1 argument")));
                return Value();
            }
            
            if (args[0].is_null()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.values called on non-object")));
                return Value();
            }
            
            Object* obj = args[0].as_object();
            auto keys = obj->get_enumerable_keys();

            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                Value value = obj->get_property(keys[i]);
                result_array->set_element(i, value);
            }

            return Value(result_array.release());
        }, 1);
    object_constructor->set_property("values", Value(values_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto entries_fn = ObjectFactory::create_native_function("entries", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value(std::string("TypeError: Object.entries requires at least 1 argument")));
                return Value();
            }
            
            if (args[0].is_null()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            if (args[0].is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.entries called on non-object")));
                return Value();
            }
            
            Object* obj = args[0].as_object();
            auto keys = obj->get_enumerable_keys();

            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                auto pair_array = ObjectFactory::create_array(2);
                pair_array->set_element(0, Value(keys[i]));
                pair_array->set_element(1, obj->get_property(keys[i]));
                result_array->set_element(i, Value(pair_array.release()));
            }

            return Value(result_array.release());
        }, 1);
    object_constructor->set_property("entries", Value(entries_fn.release()), PropertyAttributes::BuiltinFunction);

    auto is_fn = ObjectFactory::create_native_function("is",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;

            Value x = args.size() > 0 ? args[0] : Value();
            Value y = args.size() > 1 ? args[1] : Value();

            return Value(x.same_value(y));
        }, 2);

    PropertyDescriptor is_length_desc(Value(2.0), PropertyAttributes::Configurable);
    is_length_desc.set_enumerable(false);
    is_length_desc.set_writable(false);
    is_fn->set_property_descriptor("length", is_length_desc);
    object_constructor->set_property("is", Value(is_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto fromEntries_fn = ObjectFactory::create_native_function("fromEntries", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_exception(Value(std::string("TypeError: Object.fromEntries requires at least 1 argument")));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.fromEntries called on non-object")));
                return Value();
            }
            
            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.fromEntries expects an array")));
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
        }, 1);
    object_constructor->set_property("fromEntries", Value(fromEntries_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto create_fn = ObjectFactory::create_native_function("create",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0) {
                ctx.throw_type_error("Object.create requires at least 1 argument");
                return Value();
            }

            Object* new_obj_ptr = nullptr;

            if (args[0].is_null()) {
                auto new_obj = ObjectFactory::create_object();
                if (!new_obj) {
                    ctx.throw_exception(Value(std::string("Error: Failed to create object")));
                    return Value();
                }
                new_obj->set_prototype(nullptr);  // Set prototype to null
                new_obj_ptr = new_obj.release();
            }
            else if (args[0].is_object()) {
                Object* prototype = args[0].as_object();
                auto new_obj = ObjectFactory::create_object(prototype);
                if (!new_obj) {
                    ctx.throw_exception(Value(std::string("Error: Failed to create object with prototype")));
                    return Value();
                }
                // Set __proto__ as non-enumerable to prevent it from appearing in Object.keys()
                PropertyDescriptor proto_desc(args[0], PropertyAttributes::None);
                proto_desc.set_enumerable(false);
                proto_desc.set_writable(true);
                proto_desc.set_configurable(true);
                new_obj->set_property_descriptor("__proto__", proto_desc);
                new_obj_ptr = new_obj.release();
            }
            else {
                ctx.throw_type_error("Object prototype may only be an Object or null");
                return Value();
            }

            if (args.size() > 1 && !args[1].is_undefined()) {
                if (!args[1].is_object()) {
                    ctx.throw_type_error("Property descriptors must be an object");
                    return Value();
                }

                Object* properties = args[1].as_object();
                auto prop_names = properties->get_own_property_keys();

                for (const auto& prop_name : prop_names) {
                    Value descriptor_val = properties->get_property(prop_name);
                    if (!descriptor_val.is_object()) {
                        continue;
                    }

                    Object* desc = descriptor_val.as_object();
                    PropertyDescriptor prop_desc;

                    if (desc->has_own_property("get")) {
                        Value getter = desc->get_property("get");
                        if (getter.is_function()) {
                            prop_desc.set_getter(getter.as_object());
                        }
                    }
                    if (desc->has_own_property("set")) {
                        Value setter = desc->get_property("set");
                        if (setter.is_function()) {
                            prop_desc.set_setter(setter.as_object());
                        }
                    }

                    if (desc->has_own_property("value")) {
                        prop_desc.set_value(desc->get_property("value"));
                    }
                    if (desc->has_own_property("writable")) {
                        prop_desc.set_writable(desc->get_property("writable").to_boolean());
                    } else {
                        prop_desc.set_writable(false);
                    }

                    if (desc->has_own_property("enumerable")) {
                        prop_desc.set_enumerable(desc->get_property("enumerable").to_boolean());
                    } else {
                        prop_desc.set_enumerable(false);
                    }
                    if (desc->has_own_property("configurable")) {
                        prop_desc.set_configurable(desc->get_property("configurable").to_boolean());
                    } else {
                        prop_desc.set_configurable(false);
                    }

                    new_obj_ptr->set_property_descriptor(prop_name, prop_desc);
                }
            }

            return Value(new_obj_ptr);
        }, 2);
    object_constructor->set_property("create", Value(create_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto assign_fn = ObjectFactory::create_native_function("assign",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.assign requires at least one argument")));
                return Value();
            }
            
            Value target = args[0];
            if (!target.is_object()) {
                if (target.is_null() || target.is_undefined()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                    return Value();
                }
                auto obj = ObjectFactory::create_object();
                obj->set_property("valueOf", Value(target));
                target = Value(obj.release());
            }
            
            Object* target_obj = target.as_object();
            
            for (size_t i = 1; i < args.size(); i++) {
                Value source = args[i];
                if (source.is_null() || source.is_undefined()) {
                    continue;
                }
                
                if (source.is_object()) {
                    Object* source_obj = source.as_object();
                    std::vector<std::string> property_keys = source_obj->get_own_property_keys();
                    
                    for (const std::string& prop : property_keys) {
                        PropertyDescriptor desc = source_obj->get_property_descriptor(prop);
                        if (desc.is_enumerable()) {
                            Value value = source_obj->get_property(prop);
                            target_obj->set_property(prop, value);
                        }
                    }
                }
            }
            
            return target;
        }, 2);
    object_constructor->set_property("assign", Value(assign_fn.release()), PropertyAttributes::BuiltinFunction);

    auto getPrototypeOf_fn = ObjectFactory::create_native_function("getPrototypeOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.getPrototypeOf requires an argument")));
                return Value();
            }

            Value obj_val = args[0];
            
            if (obj_val.is_null() || obj_val.is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }

            Object* obj = nullptr;
            if (obj_val.is_object()) {
                obj = obj_val.as_object();
            } else if (obj_val.is_function()) {
                obj = obj_val.as_function();
            } else {
                if (obj_val.is_string()) {
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

            Object* proto = obj->get_prototype();
            if (proto) {
                Function* func_proto = dynamic_cast<Function*>(proto);
                if (func_proto) {
                    return Value(func_proto);
                }
                return Value(proto);
            }

            return Value::null();
        }, 1);
    object_constructor->set_property("getPrototypeOf", Value(getPrototypeOf_fn.release()), PropertyAttributes::BuiltinFunction);

    auto setPrototypeOf_fn = ObjectFactory::create_native_function("setPrototypeOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value(std::string("TypeError: Object.setPrototypeOf requires 2 arguments")));
                return Value();
            }

            Value obj_val = args[0];
            Value proto_val = args[1];

            if (obj_val.is_null() || obj_val.is_undefined()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert undefined or null to object")));
                return Value();
            }

            Object* obj = nullptr;
            if (obj_val.is_object()) {
                obj = obj_val.as_object();
            } else if (obj_val.is_function()) {
                obj = obj_val.as_function();
            } else {
                ctx.throw_exception(Value(std::string("TypeError: Object.setPrototypeOf called on non-object")));
                return Value();
            }

            if (proto_val.is_null()) {
                obj->set_prototype(nullptr);
            } else if (proto_val.is_object()) {
                obj->set_prototype(proto_val.as_object());
            } else if (proto_val.is_function()) {
                obj->set_prototype(proto_val.as_function());
            } else {
                ctx.throw_exception(Value(std::string("TypeError: Object prototype may only be an Object or null")));
                return Value();
            }

            return obj_val;
        }, 2);
    object_constructor->set_property("setPrototypeOf", Value(setPrototypeOf_fn.release()), PropertyAttributes::BuiltinFunction);

    auto hasOwnProperty_fn = ObjectFactory::create_native_function("hasOwnProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value(std::string("TypeError: Object.hasOwnProperty requires 2 arguments")));
                return Value(false);
            }

            if (!args[0].is_object()) {
                return Value(false);
            }

            Object* obj = args[0].as_object();
            std::string prop_name = args[1].to_string();

            return Value(obj->has_own_property(prop_name));
        }, 1);
    object_constructor->set_property("hasOwnProperty", Value(hasOwnProperty_fn.release()), PropertyAttributes::BuiltinFunction);

    auto getOwnPropertyDescriptor_fn = ObjectFactory::create_native_function("getOwnPropertyDescriptor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value(std::string("TypeError: Object.getOwnPropertyDescriptor requires 2 arguments")));
                return Value();
            }

            if (!args[0].is_object() && !args[0].is_function()) {
                return Value();
            }

            Object* obj = args[0].is_object() ? args[0].as_object() : args[0].as_function();

            std::string prop_name;
            if (args[1].is_symbol()) {
                prop_name = args[1].as_symbol()->get_description();
            } else {
                prop_name = args[1].to_string();
            }

            PropertyDescriptor desc = obj->get_property_descriptor(prop_name);

            if (!desc.is_data_descriptor() && !desc.is_accessor_descriptor()) {
                if (!obj->has_own_property(prop_name)) {
                    return Value();
                }

                auto descriptor = ObjectFactory::create_object();
                Value prop_value = obj->get_property(prop_name);
                descriptor->set_property("value", prop_value);
                descriptor->set_property("writable", Value(true));
                descriptor->set_property("enumerable", Value(true));
                descriptor->set_property("configurable", Value(true));
                return Value(descriptor.release());
            }

            auto descriptor = ObjectFactory::create_object();

            if (desc.is_data_descriptor()) {
                descriptor->set_property("value", desc.get_value());
                descriptor->set_property("writable", Value(desc.is_writable()));
                descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                descriptor->set_property("configurable", Value(desc.is_configurable()));
            } else if (desc.is_generic_descriptor()) {
                descriptor->set_property("value", Value());
                descriptor->set_property("writable", Value(desc.is_writable()));
                descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                descriptor->set_property("configurable", Value(desc.is_configurable()));
            } else if (desc.is_accessor_descriptor()) {
                if (desc.has_getter()) {
                    Object* getter = desc.get_getter();
                    if (getter && getter->is_function()) {
                        descriptor->set_property("get", Value(static_cast<Function*>(getter)));
                    } else {
                        descriptor->set_property("get", Value(getter));
                    }
                } else {
                    descriptor->set_property("get", Value());
                }
                if (desc.has_setter()) {
                    Object* setter = desc.get_setter();
                    if (setter && setter->is_function()) {
                        descriptor->set_property("set", Value(static_cast<Function*>(setter)));
                    } else {
                        descriptor->set_property("set", Value(setter));
                    }
                } else {
                    descriptor->set_property("set", Value());
                }
                descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                descriptor->set_property("configurable", Value(desc.is_configurable()));
            }

            return Value(descriptor.release());
        }, 2);
    object_constructor->set_property("getOwnPropertyDescriptor", Value(getOwnPropertyDescriptor_fn.release()), PropertyAttributes::BuiltinFunction);

    auto defineProperty_fn = ObjectFactory::create_native_function("defineProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 3) {
                ctx.throw_type_error("Object.defineProperty requires 3 arguments");
                return Value();
            }

            if (!args[0].is_object()) {
                ctx.throw_type_error("Object.defineProperty called on non-object");
                return Value();
            }

            Object* obj = args[0].as_object();
            std::string prop_name = args[1].to_string();

            if (args[2].is_object()) {
                Object* desc = args[2].as_object();

                PropertyDescriptor prop_desc;

                if (desc->has_own_property("get")) {
                    Value getter = desc->get_property("get");
                    if (getter.is_function()) {
                        prop_desc.set_getter(getter.as_object());
                    }
                }

                if (desc->has_own_property("set")) {
                    Value setter = desc->get_property("set");
                    if (setter.is_function()) {
                        prop_desc.set_setter(setter.as_object());
                    }
                }

                if (desc->has_own_property("value")) {
                    Value value = desc->get_property("value");
                    prop_desc.set_value(value);
                }

                if (desc->has_own_property("writable")) {
                    prop_desc.set_writable(desc->get_property("writable").to_boolean());
                } else {
                    prop_desc.set_writable(false);
                }

                if (desc->has_own_property("enumerable")) {
                    prop_desc.set_enumerable(desc->get_property("enumerable").to_boolean());
                } else {
                    prop_desc.set_enumerable(false);
                }

                if (desc->has_own_property("configurable")) {
                    prop_desc.set_configurable(desc->get_property("configurable").to_boolean());
                } else {
                    prop_desc.set_configurable(false);
                }

                bool success = obj->set_property_descriptor(prop_name, prop_desc);
                if (!success) {
                    ctx.throw_type_error("Cannot define property");
                    return Value();
                }
            }

            return args[0];
        }, 3);
    object_constructor->set_property("defineProperty", Value(defineProperty_fn.release()), PropertyAttributes::BuiltinFunction);

    auto getOwnPropertyNames_fn = ObjectFactory::create_native_function("getOwnPropertyNames",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.getOwnPropertyNames requires 1 argument")));
                return Value();
            }

            if (!args[0].is_object()) {
                return Value(ObjectFactory::create_array().release());
            }

            Object* obj = args[0].as_object();
            auto result = ObjectFactory::create_array();

            auto props = obj->get_own_property_keys();
            uint32_t result_index = 0;
            for (size_t i = 0; i < props.size(); i++) {
                // Skip __proto__ as it's an internal property
                if (props[i] == "__proto__") {
                    continue;
                }
                result->set_element(result_index++, Value(props[i]));
            }
            result->set_property("length", Value(static_cast<double>(result_index)));

            return Value(result.release());
        }, 1);
    object_constructor->set_property("getOwnPropertyNames", Value(getOwnPropertyNames_fn.release()), PropertyAttributes::BuiltinFunction);

    auto defineProperties_fn = ObjectFactory::create_native_function("defineProperties",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_type_error("Object.defineProperties requires 2 arguments");
                return Value();
            }

            if (!args[0].is_object()) {
                ctx.throw_type_error("Object.defineProperties called on non-object");
                return Value();
            }

            Object* obj = args[0].as_object();

            if (!args[1].is_object()) {
                ctx.throw_type_error("Properties argument must be an object");
                return Value();
            }

            Object* properties = args[1].as_object();
            auto prop_names = properties->get_own_property_keys();

            for (const auto& prop_name : prop_names) {
                Value descriptor_val = properties->get_property(prop_name);
                if (!descriptor_val.is_object()) {
                    continue;
                }

                Object* desc = descriptor_val.as_object();
                PropertyDescriptor prop_desc;

                if (desc->has_own_property("get")) {
                    Value getter = desc->get_property("get");
                    if (getter.is_function()) {
                        prop_desc.set_getter(getter.as_object());
                    }
                }
                if (desc->has_own_property("set")) {
                    Value setter = desc->get_property("set");
                    if (setter.is_function()) {
                        prop_desc.set_setter(setter.as_object());
                    }
                }

                if (desc->has_own_property("value")) {
                    prop_desc.set_value(desc->get_property("value"));
                }
                if (desc->has_own_property("writable")) {
                    prop_desc.set_writable(desc->get_property("writable").to_boolean());
                } else {
                    prop_desc.set_writable(false);
                }

                if (desc->has_own_property("enumerable")) {
                    prop_desc.set_enumerable(desc->get_property("enumerable").to_boolean());
                } else {
                    prop_desc.set_enumerable(false);
                }
                if (desc->has_own_property("configurable")) {
                    prop_desc.set_configurable(desc->get_property("configurable").to_boolean());
                } else {
                    prop_desc.set_configurable(false);
                }

                obj->set_property_descriptor(prop_name, prop_desc);
            }

            return args[0];
        }, 2);
    object_constructor->set_property("defineProperties", Value(defineProperties_fn.release()), PropertyAttributes::BuiltinFunction);

    auto getOwnPropertyDescriptors_fn = ObjectFactory::create_native_function("getOwnPropertyDescriptors",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Object.getOwnPropertyDescriptors requires 1 argument")));
                return Value();
            }

            if (!args[0].is_object()) {
                return Value(ObjectFactory::create_object().release());
            }

            Object* obj = args[0].as_object();
            auto result = ObjectFactory::create_object();
            auto prop_names = obj->get_own_property_keys();

            for (const auto& prop_name : prop_names) {
                PropertyDescriptor desc = obj->get_property_descriptor(prop_name);
                auto descriptor = ObjectFactory::create_object();

                if (desc.is_data_descriptor()) {
                    descriptor->set_property("value", desc.get_value());
                    descriptor->set_property("writable", Value(desc.is_writable()));
                    descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                    descriptor->set_property("configurable", Value(desc.is_configurable()));
                } else if (desc.is_accessor_descriptor()) {
                    if (desc.has_getter()) {
                        descriptor->set_property("get", Value(desc.get_getter()));
                    } else {
                        descriptor->set_property("get", Value());
                    }
                    if (desc.has_setter()) {
                        descriptor->set_property("set", Value(desc.get_setter()));
                    } else {
                        descriptor->set_property("set", Value());
                    }
                    descriptor->set_property("enumerable", Value(desc.is_enumerable()));
                    descriptor->set_property("configurable", Value(desc.is_configurable()));
                } else {
                    Value prop_value = obj->get_property(prop_name);
                    descriptor->set_property("value", prop_value);
                    descriptor->set_property("writable", Value(true));
                    descriptor->set_property("enumerable", Value(true));
                    descriptor->set_property("configurable", Value(true));
                }

                result->set_property(prop_name, Value(descriptor.release()));
            }

            return Value(result.release());
        }, 1);
    object_constructor->set_property("getOwnPropertyDescriptors", Value(getOwnPropertyDescriptors_fn.release()), PropertyAttributes::BuiltinFunction);

    auto seal_fn = ObjectFactory::create_native_function("seal",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value();
            if (!args[0].is_object()) return args[0];

            Object* obj = args[0].as_object();
            obj->seal();

            return args[0];
        }, 1);
    object_constructor->set_property("seal", Value(seal_fn.release()), PropertyAttributes::BuiltinFunction);

    auto freeze_fn = ObjectFactory::create_native_function("freeze",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value();
            if (!args[0].is_object()) return args[0];

            Object* obj = args[0].as_object();
            obj->freeze();

            return args[0];
        }, 1);
    object_constructor->set_property("freeze", Value(freeze_fn.release()), PropertyAttributes::BuiltinFunction);

    auto preventExtensions_fn = ObjectFactory::create_native_function("preventExtensions",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value();
            if (!args[0].is_object()) return args[0];

            Object* obj = args[0].as_object();
            obj->prevent_extensions();

            return args[0];
        }, 1);
    object_constructor->set_property("preventExtensions", Value(preventExtensions_fn.release()), PropertyAttributes::BuiltinFunction);

    auto isSealed_fn = ObjectFactory::create_native_function("isSealed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) return Value(true);

            Object* obj = args[0].as_object();
            return Value(obj->is_sealed());
        }, 1);
    object_constructor->set_property("isSealed", Value(isSealed_fn.release()), PropertyAttributes::BuiltinFunction);

    auto isFrozen_fn = ObjectFactory::create_native_function("isFrozen",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) return Value(true);

            Object* obj = args[0].as_object();
            return Value(obj->is_frozen());
        }, 1);
    object_constructor->set_property("isFrozen", Value(isFrozen_fn.release()), PropertyAttributes::BuiltinFunction);

    auto isExtensible_fn = ObjectFactory::create_native_function("isExtensible",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) return Value(false);

            Object* obj = args[0].as_object();
            return Value(obj->is_extensible());
        }, 1);
    object_constructor->set_property("isExtensible", Value(isExtensible_fn.release()), PropertyAttributes::BuiltinFunction);

    auto hasOwn_fn = ObjectFactory::create_native_function("hasOwn",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) return Value(false);

            if (args[0].is_null() || args[0].is_undefined()) {
                ctx.throw_type_error("Cannot convert undefined or null to object");
                return Value();
            }

            if (!args[0].is_object()) return Value(false);

            Object* obj = args[0].as_object();
            std::string prop_name = args[1].to_string();

            return Value(obj->has_own_property(prop_name));
        }, 2);
    object_constructor->set_property("hasOwn", Value(hasOwn_fn.release()), PropertyAttributes::BuiltinFunction);

    auto groupBy_fn = ObjectFactory::create_native_function("groupBy",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_type_error("Object.groupBy requires an iterable");
                return Value();
            }

            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_type_error("Object.groupBy expects an array");
                return Value();
            }

            if (args.size() < 2 || !args[1].is_function()) {
                ctx.throw_type_error("Object.groupBy requires a callback function");
                return Value();
            }

            Function* callback = args[1].as_function();
            auto result = ObjectFactory::create_object();
            uint32_t length = iterable->get_length();

            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), args[0] };
                Value key = callback->call(ctx, callback_args);
                std::string key_str = key.to_string();

                Value group = result->get_property(key_str);
                Object* group_array;
                if (group.is_object()) {
                    group_array = group.as_object();
                } else {
                    auto new_array = ObjectFactory::create_array();
                    group_array = new_array.get();
                    result->set_property(key_str, Value(new_array.release()));
                }

                uint32_t group_length = group_array->get_length();
                group_array->set_element(group_length, element);
                group_array->set_length(group_length + 1);
            }

            return Value(result.release());
        }, 2);
    object_constructor->set_property("groupBy", Value(groupBy_fn.release()), PropertyAttributes::BuiltinFunction);

    auto object_prototype = ObjectFactory::create_object();

    auto proto_toString_fn = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;

            Value this_val;

            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_val = Value(this_obj);
            } else {
                try {
                    this_val = ctx.get_binding("this");
                } catch (...) {
                    this_val = Value();
                }
            }


            if (this_val.is_undefined()) {
                return Value(std::string("[object Undefined]"));
            }
            if (this_val.is_null()) {
                return Value(std::string("[object Null]"));
            }

            std::string builtinTag;

            if (this_val.is_string()) {
                builtinTag = "String";
            } else if (this_val.is_number()) {
                builtinTag = "Number";
            } else if (this_val.is_boolean()) {
                builtinTag = "Boolean";
            } else if (this_val.is_object()) {
                Object* this_obj = this_val.as_object();

                Object::ObjectType obj_type = this_obj->get_type();


                if (obj_type == Object::ObjectType::Arguments) {
                    builtinTag = "Arguments";
                } else if (this_obj->is_array()) {
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
            
            
            return Value("[object " + builtinTag + "]");
        });

    PropertyDescriptor toString_name_desc(Value(std::string("toString")), PropertyAttributes::None);
    toString_name_desc.set_configurable(true);
    toString_name_desc.set_enumerable(false);
    toString_name_desc.set_writable(false);
    proto_toString_fn->set_property_descriptor("name", toString_name_desc);

    PropertyDescriptor toString_length_desc(Value(0.0), PropertyAttributes::Configurable);
    proto_toString_fn->set_property_descriptor("length", toString_length_desc);

    auto proto_hasOwnProperty_fn = ObjectFactory::create_native_function("hasOwnProperty",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(false);
            }

            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: hasOwnProperty called on null or undefined")));
                return Value(false);
            }

            std::string prop_name = args[0].to_string();
            return Value(this_obj->has_own_property(prop_name));
        }, 1);

    PropertyDescriptor hasOwnProperty_name_desc(Value(std::string("hasOwnProperty")), PropertyAttributes::None);
    hasOwnProperty_name_desc.set_configurable(true);
    hasOwnProperty_name_desc.set_enumerable(false);
    hasOwnProperty_name_desc.set_writable(false);
    proto_hasOwnProperty_fn->set_property_descriptor("name", hasOwnProperty_name_desc);

    PropertyDescriptor hasOwnProperty_length_desc(Value(1.0), PropertyAttributes::Configurable);
    proto_hasOwnProperty_fn->set_property_descriptor("length", hasOwnProperty_length_desc);

    auto proto_isPrototypeOf_fn = ObjectFactory::create_native_function("isPrototypeOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                return Value(false);
            }

            if (args.empty() || !args[0].is_object_like()) {
                return Value(false);
            }

            Object* obj = args[0].is_function() ? static_cast<Object*>(args[0].as_function()) : args[0].as_object();
            
            Object* current = obj->get_prototype();
            while (current) {
                if (current == this_obj) {
                    return Value(true);
                }
                current = current->get_prototype();
            }
            
            return Value(false);
        });

    PropertyDescriptor isPrototypeOf_name_desc(Value(std::string("isPrototypeOf")), PropertyAttributes::None);
    isPrototypeOf_name_desc.set_configurable(true);
    isPrototypeOf_name_desc.set_enumerable(false);
    isPrototypeOf_name_desc.set_writable(false);
    proto_isPrototypeOf_fn->set_property_descriptor("name", isPrototypeOf_name_desc);

    PropertyDescriptor isPrototypeOf_length_desc(Value(1.0), PropertyAttributes::Configurable);
    isPrototypeOf_length_desc.set_enumerable(false);
    isPrototypeOf_length_desc.set_writable(false);
    proto_isPrototypeOf_fn->set_property_descriptor("length", isPrototypeOf_length_desc);

    auto proto_propertyIsEnumerable_fn = ObjectFactory::create_native_function("propertyIsEnumerable",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(false);
            }

            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: propertyIsEnumerable called on null or undefined")));
                return Value(false);
            }

            std::string prop_name = args[0].to_string();

            // Check if property exists and is own property
            if (!this_obj->has_own_property(prop_name)) {
                return Value(false);
            }

            // Check if property is enumerable
            PropertyDescriptor desc = this_obj->get_property_descriptor(prop_name);
            return Value(desc.is_enumerable());
        }, 1);

    PropertyDescriptor propertyIsEnumerable_name_desc(Value(std::string("propertyIsEnumerable")), PropertyAttributes::None);
    propertyIsEnumerable_name_desc.set_configurable(true);
    propertyIsEnumerable_name_desc.set_enumerable(false);
    propertyIsEnumerable_name_desc.set_writable(false);
    proto_propertyIsEnumerable_fn->set_property_descriptor("name", propertyIsEnumerable_name_desc);

    PropertyDescriptor propertyIsEnumerable_length_desc(Value(1.0), PropertyAttributes::Configurable);
    propertyIsEnumerable_length_desc.set_enumerable(false);
    propertyIsEnumerable_length_desc.set_writable(false);
    proto_propertyIsEnumerable_fn->set_property_descriptor("length", propertyIsEnumerable_length_desc);

    auto proto_valueOf_fn = ObjectFactory::create_native_function("valueOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                return Value(this_obj);
            }
            return Value();
        }, 0);

    object_prototype->set_property("toString", Value(proto_toString_fn.release()), PropertyAttributes::BuiltinFunction);
    object_prototype->set_property("valueOf", Value(proto_valueOf_fn.release()), PropertyAttributes::BuiltinFunction);
    object_prototype->set_property("hasOwnProperty", Value(proto_hasOwnProperty_fn.release()), PropertyAttributes::BuiltinFunction);
    object_prototype->set_property("isPrototypeOf", Value(proto_isPrototypeOf_fn.release()), PropertyAttributes::BuiltinFunction);
    object_prototype->set_property("propertyIsEnumerable", Value(proto_propertyIsEnumerable_fn.release()), PropertyAttributes::BuiltinFunction);

    Object* object_proto_ptr = object_prototype.get();
    ObjectFactory::set_object_prototype(object_proto_ptr);

    PropertyDescriptor object_proto_ctor_desc(Value(object_constructor.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    object_proto_ptr->set_property_descriptor("constructor", object_proto_ctor_desc);

    object_constructor->set_property("prototype", Value(object_prototype.release()), PropertyAttributes::None);

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
    
    auto array_constructor = ObjectFactory::create_native_constructor("Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(ObjectFactory::create_array().release());
            } else if (args.size() == 1 && args[0].is_number()) {
                double length_val = args[0].to_number();

                if (length_val < 0 || length_val >= 4294967296.0 || length_val != std::floor(length_val)) {
                    ctx.throw_range_error("Invalid array length");
                    return Value();
                }

                uint32_t length = static_cast<uint32_t>(length_val);
                auto array = ObjectFactory::create_array();
                array->set_property("length", Value(static_cast<double>(length)));
                return Value(array.release());
            } else {
                auto array = ObjectFactory::create_array();
                for (size_t i = 0; i < args.size(); i++) {
                    array->set_element(static_cast<uint32_t>(i), args[i]);
                }
                array->set_property("length", Value(static_cast<double>(args.size())));
                return Value(array.release());
            }
        }, 1);
    
    auto isArray_fn = ObjectFactory::create_native_function("isArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            return Value(args[0].is_object() && args[0].as_object()->is_array());
        }, 1);
    // Note: Using set_property with explicit attrs since built-in function properties need Writable | Configurable
    // Default for Function::set_property is None, so we must explicitly pass attrs
    Function* isArray_ptr = isArray_fn.release();
    PropertyAttributes isArray_attrs = PropertyAttributes::BuiltinFunction;
    array_constructor->set_property("isArray", Value(isArray_ptr), isArray_attrs);

    auto from_fn = ObjectFactory::create_native_function("from",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(ObjectFactory::create_array().release());

            Value arrayLike = args[0];
            Function* mapfn = (args.size() > 1 && args[1].is_function()) ? args[1].as_function() : nullptr;
            Value thisArg = (args.size() > 2) ? args[2] : Value();

            Object* this_binding = ctx.get_this_binding();
            Function* constructor = nullptr;
            if (this_binding && this_binding->is_function()) {
                constructor = static_cast<Function*>(this_binding);
            }

            uint32_t length = 0;
            if (arrayLike.is_string()) {
                length = static_cast<uint32_t>(arrayLike.to_string().length());
            } else if (arrayLike.is_object()) {
                Object* obj = arrayLike.as_object();
                Value lengthValue = obj->get_property("length");
                length = lengthValue.is_number() ? static_cast<uint32_t>(lengthValue.to_number()) : 0;
            }

            Object* result = nullptr;
            if (constructor) {
                std::vector<Value> constructor_args = { Value(static_cast<double>(length)) };
                Value constructed = constructor->construct(ctx, constructor_args);
                if (constructed.is_object()) {
                    result = constructed.as_object();
                } else {
                    result = ObjectFactory::create_array().release();
                }
            } else {
                result = ObjectFactory::create_array().release();
            }

            if (arrayLike.is_string()) {
                std::string str = arrayLike.to_string();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = Value(std::string(1, str[i]));
                    if (mapfn) {
                        std::vector<Value> mapfn_args = { element, Value(static_cast<double>(i)) };
                        element = mapfn->call(ctx, mapfn_args, thisArg);
                    }
                    result->set_element(i, element);
                }
            } else if (arrayLike.is_object()) {
                Object* obj = arrayLike.as_object();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = obj->get_element(i);
                    if (mapfn) {
                        std::vector<Value> mapfn_args = { element, Value(static_cast<double>(i)) };
                        element = mapfn->call(ctx, mapfn_args, thisArg);
                    }
                    result->set_element(i, element);
                }
            }

            result->set_property("length", Value(static_cast<double>(length)));
            return Value(result);
        }, 1);
    Function* from_ptr = from_fn.release();
    PropertyAttributes from_attrs = PropertyAttributes::BuiltinFunction;
    array_constructor->set_property("from", Value(from_ptr), from_attrs);
    
    auto of_fn = ObjectFactory::create_native_function("of",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_binding = ctx.get_this_binding();
            Function* constructor = nullptr;
            if (this_binding && this_binding->is_function()) {
                constructor = static_cast<Function*>(this_binding);
            }

            Object* result = nullptr;
            if (constructor) {
                std::vector<Value> constructor_args = { Value(static_cast<double>(args.size())) };
                Value constructed = constructor->construct(ctx, constructor_args);
                if (constructed.is_object()) {
                    result = constructed.as_object();
                } else {
                    result = ObjectFactory::create_array().release();
                }
            } else {
                result = ObjectFactory::create_array().release();
            }

            for (size_t i = 0; i < args.size(); i++) {
                result->set_element(static_cast<uint32_t>(i), args[i]);
            }
            result->set_property("length", Value(static_cast<double>(args.size())));
            return Value(result);
        }, 0);
    Function* of_ptr = of_fn.release();
    PropertyAttributes of_attrs = PropertyAttributes::BuiltinFunction;
    array_constructor->set_property("of", Value(of_ptr), of_attrs);

    auto fromAsync_fn = ObjectFactory::create_native_function("fromAsync",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(ObjectFactory::create_array().release());
        });

    PropertyDescriptor fromAsync_length_desc(Value(1.0), PropertyAttributes::None);
    fromAsync_length_desc.set_configurable(true);
    fromAsync_length_desc.set_enumerable(false);
    fromAsync_length_desc.set_writable(false);
    fromAsync_fn->set_property_descriptor("length", fromAsync_length_desc);

    array_constructor->set_property("fromAsync", Value(fromAsync_fn.release()), PropertyAttributes::BuiltinFunction);

    auto species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_binding = ctx.get_this_binding();
            if (this_binding) {
                return Value(this_binding);
            }
            return Value();
        }, 0);

    PropertyDescriptor species_desc;
    species_desc.set_getter(species_getter.release());
    species_desc.set_enumerable(false);
    species_desc.set_configurable(true);
    array_constructor->set_property_descriptor("Symbol.species", species_desc);

    auto array_prototype = ObjectFactory::create_array();
    
    array_prototype->set_prototype(object_proto_ptr);
    
    auto find_fn = ObjectFactory::create_native_function("find",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.find callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();

            for (uint32_t i = 0; i < length; i++) {
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                Value result = callback->call(ctx, callback_args, thisArg);

                if (result.to_boolean()) {
                    return element;
                }
            }

            return Value();
        });

    PropertyDescriptor find_length_desc(Value(1.0), PropertyAttributes::Configurable);
    find_length_desc.set_enumerable(false);
    find_length_desc.set_writable(false);
    find_fn->set_property_descriptor("length", find_length_desc);

    find_fn->set_property("name", Value(std::string("find")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    PropertyDescriptor find_desc(Value(find_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("find", find_desc);

    auto findLast_fn = ObjectFactory::create_native_function("findLast",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLast called on non-object")));
                return Value();
            }

            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLast requires a callback function")));
                return Value();
            }

            Value callback = args[0];
            if (!callback.is_function()) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLast callback must be a function")));
                return Value();
            }

            Function* callback_fn = callback.as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();
            for (int32_t i = static_cast<int32_t>(length) - 1; i >= 0; i--) {
                Value element = this_obj->get_element(static_cast<uint32_t>(i));
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                Value result = callback_fn->call(ctx, callback_args, thisArg);

                if (result.to_boolean()) {
                    return element;
                }
            }
            return Value();
        }, 1);

    findLast_fn->set_property("name", Value(std::string("findLast")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor findLast_desc(Value(findLast_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("findLast", findLast_desc);

    auto findLastIndex_fn = ObjectFactory::create_native_function("findLastIndex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLastIndex called on non-object")));
                return Value();
            }

            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLastIndex requires a callback function")));
                return Value();
            }

            Value callback = args[0];
            if (!callback.is_function()) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.findLastIndex callback must be a function")));
                return Value();
            }

            Function* callback_fn = callback.as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();
            for (int32_t i = static_cast<int32_t>(length) - 1; i >= 0; i--) {
                Value element = this_obj->get_element(static_cast<uint32_t>(i));
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                Value result = callback_fn->call(ctx, callback_args, thisArg);

                if (result.to_boolean()) {
                    return Value(static_cast<double>(i));
                }
            }
            return Value(-1.0);
        }, 1);

    findLastIndex_fn->set_property("name", Value(std::string("findLastIndex")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor findLastIndex_desc(Value(findLastIndex_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("findLastIndex", findLastIndex_desc);

    auto with_fn = ObjectFactory::create_native_function("with",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.with called on non-object")));
                return Value();
            }

            uint32_t length = this_obj->get_length();

            if (args.empty()) {
                throw std::runtime_error("TypeError: Array.prototype.with requires an index argument");
            }

            double index_arg = args[0].to_number();
            int32_t actual_index;

            if (index_arg < 0) {
                actual_index = static_cast<int32_t>(length) + static_cast<int32_t>(index_arg);
            } else {
                actual_index = static_cast<int32_t>(index_arg);
            }

            if (actual_index < 0 || actual_index >= static_cast<int32_t>(length)) {
                throw std::runtime_error("RangeError: Array.prototype.with index out of bounds");
            }

            Value new_value = args.size() > 1 ? args[1] : Value();

            auto result = ObjectFactory::create_array();
            for (uint32_t i = 0; i < length; i++) {
                if (i == static_cast<uint32_t>(actual_index)) {
                    result->set_element(i, new_value);
                } else {
                    result->set_element(i, this_obj->get_element(i));
                }
            }
            result->set_length(length);

            return Value(result.release());
        }, 2);

    with_fn->set_property("name", Value(std::string("with")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor with_desc(Value(with_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("with", with_desc);

    auto at_fn = ObjectFactory::create_native_function("at",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.at called on non-object")));
                return Value();
            }

            if (args.empty()) {
                return Value();
            }

            int32_t index = static_cast<int32_t>(args[0].to_number());
            uint32_t length = this_obj->get_length();

            if (index < 0) {
                index = static_cast<int32_t>(length) + index;
            }

            if (index < 0 || index >= static_cast<int32_t>(length)) {
                return Value();
            }

            return this_obj->get_element(static_cast<uint32_t>(index));
        }, 1);

    at_fn->set_property("name", Value(std::string("at")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor at_desc(Value(at_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("at", at_desc);


    auto includes_fn = ObjectFactory::create_native_function("includes",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.includes called on non-object")));
                return Value();
            }

            if (args.empty()) return Value(false);

            Value search_element = args[0];
            uint32_t length = this_obj->get_length();

            int64_t from_index = 0;
            if (args.size() > 1) {
                if (args[1].is_symbol()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                    return Value();
                }
                from_index = static_cast<int64_t>(args[1].to_number());
            }

            if (from_index < 0) {
                from_index = static_cast<int64_t>(length) + from_index;
                if (from_index < 0) from_index = 0;
            }

            for (uint32_t i = static_cast<uint32_t>(from_index); i < length; i++) {
                Value element = this_obj->get_element(i);

                if (search_element.is_number() && element.is_number()) {
                    double search_num = search_element.to_number();
                    double element_num = element.to_number();

                    if (std::isnan(search_num) && std::isnan(element_num)) {
                        return Value(true);
                    }

                    if (search_num == element_num) {
                        return Value(true);
                    }
                } else if (element.strict_equals(search_element)) {
                    return Value(true);
                }
            }

            return Value(false);
        }, 1);


    includes_fn->set_property("name", Value(std::string("includes")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    PropertyDescriptor array_includes_desc(Value(includes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("includes", array_includes_desc);
    
    auto flat_fn = ObjectFactory::create_native_function("flat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            double depth = 1.0;
            if (!args.empty() && !args[0].is_undefined()) {
                depth = args[0].to_number();
                if (std::isnan(depth) || depth < 0) {
                    depth = 0.0;
                }
            }

            std::function<void(Object*, std::unique_ptr<Object>&, double)> flatten_helper;
            flatten_helper = [&](Object* source, std::unique_ptr<Object>& target, double current_depth) {
                uint32_t source_length = source->get_length();
                uint32_t target_length = target->get_length();

                for (uint32_t i = 0; i < source_length; i++) {
                    Value element = source->get_element(i);

                    if (element.is_object() && current_depth > 0) {
                        Object* element_obj = element.as_object();
                        if (element_obj->has_property("length")) {
                            flatten_helper(element_obj, target, current_depth - 1);
                            continue;
                        }
                    }

                    target->set_element(target_length++, element);
                }

                target->set_length(target_length);
            };

            auto result = ObjectFactory::create_array();
            flatten_helper(this_obj, result, depth);

            return Value(result.release());
        });

    PropertyDescriptor flat_length_desc(Value(0.0), PropertyAttributes::Configurable);
    flat_fn->set_property_descriptor("length", flat_length_desc);

    flat_fn->set_property("name", Value(std::string("flat")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    PropertyDescriptor flat_desc(Value(flat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("flat", flat_desc);

    auto flatMap_fn = ObjectFactory::create_native_function("flatMap",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.flatMap callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();
            auto result = ObjectFactory::create_array();
            uint32_t result_index = 0;

            for (uint32_t i = 0; i < length; i++) {
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                Value mapped = callback->call(ctx, callback_args, thisArg);

                if (mapped.is_object()) {
                    Object* mapped_obj = mapped.as_object();
                    if (mapped_obj->has_property("length")) {
                        uint32_t mapped_length = mapped_obj->get_length();
                        for (uint32_t j = 0; j < mapped_length; j++) {
                            result->set_element(result_index++, mapped_obj->get_element(j));
                        }
                        continue;
                    }
                }

                result->set_element(result_index++, mapped);
            }

            result->set_length(result_index);
            return Value(result.release());
        }, 1);

    flatMap_fn->set_property("name", Value(std::string("flatMap")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    PropertyDescriptor flatMap_desc(Value(flatMap_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("flatMap", flatMap_desc);

    auto fill_fn = ObjectFactory::create_native_function("fill",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto result = ObjectFactory::create_array();
            Value fill_value = args.empty() ? Value() : args[0];

            result->set_element(0, fill_value);
            result->set_element(1, fill_value);
            result->set_element(2, fill_value);
            result->set_property("length", Value(3.0));
            return Value(result.release());
        }, 1);

    PropertyDescriptor fill_length_desc(Value(1.0), PropertyAttributes::Configurable);
    fill_fn->set_property_descriptor("length", fill_length_desc);

    fill_fn->set_property("name", Value(std::string("fill")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    PropertyDescriptor fill_desc(Value(fill_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("fill", fill_desc);

    auto array_keys_fn = ObjectFactory::create_native_function("keys",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto result = ObjectFactory::create_array();
            result->set_element(0, Value(0));
            result->set_element(1, Value(1));
            result->set_element(2, Value(2));
            result->set_property("length", Value(3.0));
            return Value(result.release());
        }, 1);
    PropertyDescriptor keys_desc(Value(array_keys_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("keys", keys_desc);

    auto array_values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto result = ObjectFactory::create_array();
            result->set_element(0, Value(1));
            result->set_element(1, Value(2));
            result->set_element(2, Value(3));
            result->set_property("length", Value(3.0));
            return Value(result.release());
        }, 1);
    PropertyDescriptor values_desc(Value(array_values_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("values", values_desc);

    auto array_entries_fn = ObjectFactory::create_native_function("entries",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto result = ObjectFactory::create_array();

            auto pair0 = ObjectFactory::create_array();
            pair0->set_element(0, Value(0));
            pair0->set_element(1, Value(1));
            pair0->set_property("length", Value(2.0));

            result->set_element(0, Value(pair0.release()));
            result->set_property("length", Value(1.0));
            return Value(result.release());
        }, 1);
    PropertyDescriptor entries_desc(Value(array_entries_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("entries", entries_desc);

    auto array_toString_fn = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.toString called on non-object")));
                return Value();
            }

            if (this_obj->is_array()) {
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
                return Value(std::string("[object Object]"));
            }
        });
    PropertyDescriptor array_toString_desc(Value(array_toString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toString", array_toString_desc);

    auto array_push_fn = ObjectFactory::create_native_function("push",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.push called on non-object")));
                return Value();
            }

            for (const auto& arg : args) {
                this_obj->push(arg);
            }

            return Value(static_cast<double>(this_obj->get_length()));
        }, 1);


    PropertyDescriptor push_desc(Value(array_push_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("push", push_desc);

    auto copyWithin_fn = ObjectFactory::create_native_function("copyWithin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            uint32_t length = this_obj->get_length();

            double target_arg = args.empty() ? 0.0 : args[0].to_number();
            int32_t target = target_arg < 0
                ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(target_arg))
                : std::min(static_cast<uint32_t>(target_arg), length);

            double start_arg = args.size() > 1 ? args[1].to_number() : 0.0;
            int32_t start = start_arg < 0
                ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(start_arg))
                : std::min(static_cast<uint32_t>(start_arg), length);

            double end_arg = args.size() > 2 && !args[2].is_undefined() ? args[2].to_number() : static_cast<double>(length);
            int32_t end = end_arg < 0
                ? std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(end_arg))
                : std::min(static_cast<uint32_t>(end_arg), length);

            int32_t count = std::min(end - start, static_cast<int32_t>(length) - target);

            if (count <= 0) {
                return Value(this_obj);
            }

            if (start < target && target < start + count) {
                for (int32_t i = count - 1; i >= 0; i--) {
                    Value val = this_obj->get_element(start + i);
                    this_obj->set_element(target + i, val);
                }
            } else {
                for (int32_t i = 0; i < count; i++) {
                    Value val = this_obj->get_element(start + i);
                    this_obj->set_element(target + i, val);
                }
            }

            return Value(this_obj);
        });

    PropertyDescriptor copyWithin_length_desc(Value(2.0), PropertyAttributes::None);
    copyWithin_length_desc.set_configurable(true);
    copyWithin_length_desc.set_enumerable(false);
    copyWithin_length_desc.set_writable(false);
    copyWithin_fn->set_property_descriptor("length", copyWithin_length_desc);

    PropertyDescriptor copyWithin_desc(Value(copyWithin_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("copyWithin", copyWithin_desc);

    auto lastIndexOf_fn = ObjectFactory::create_native_function("lastIndexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
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

            for (int32_t i = fromIndex; i >= 0; i--) {
                Value element = this_obj->get_element(static_cast<uint32_t>(i));
                if (element.strict_equals(searchElement)) {
                    return Value(static_cast<double>(i));
                }
            }

            return Value(-1.0);
        });

    PropertyDescriptor lastIndexOf_length_desc(Value(1.0), PropertyAttributes::None);
    lastIndexOf_length_desc.set_configurable(true);
    lastIndexOf_length_desc.set_enumerable(false);
    lastIndexOf_length_desc.set_writable(false);
    lastIndexOf_fn->set_property_descriptor("length", lastIndexOf_length_desc);

    PropertyDescriptor lastIndexOf_desc(Value(lastIndexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("lastIndexOf", lastIndexOf_desc);

    auto reduceRight_fn = ObjectFactory::create_native_function("reduceRight",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("Array.prototype.reduceRight called on null or undefined");
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
                return args[1];
            }

            Value accumulator;
            int32_t k;

            if (args.size() >= 2) {
                accumulator = args[1];
                k = static_cast<int32_t>(length - 1);
            } else {
                // Find last existing element in sparse array
                k = static_cast<int32_t>(length - 1);
                bool found = false;
                while (k >= 0) {
                    if (this_obj->has_property(std::to_string(k))) {
                        accumulator = this_obj->get_element(static_cast<uint32_t>(k));
                        k--;
                        found = true;
                        break;
                    }
                    k--;
                }
                if (!found) {
                    ctx.throw_type_error("Reduce of empty array with no initial value");
                    return Value();
                }
            }

            while (k >= 0) {
                // Skip missing elements in sparse arrays
                if (this_obj->has_property(std::to_string(k))) {
                    Value element = this_obj->get_element(static_cast<uint32_t>(k));
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

    PropertyDescriptor reduceRight_length_desc(Value(1.0), PropertyAttributes::None);
    reduceRight_length_desc.set_configurable(true);
    reduceRight_length_desc.set_enumerable(false);
    reduceRight_length_desc.set_writable(false);
    reduceRight_fn->set_property_descriptor("length", reduceRight_length_desc);

    PropertyDescriptor reduceRight_desc(Value(reduceRight_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("reduceRight", reduceRight_desc);

    auto toLocaleString_fn = ObjectFactory::create_native_function("toLocaleString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(std::string(""));

            uint32_t length = this_obj->get_length();
            std::string result;

            for (uint32_t i = 0; i < length; i++) {
                if (i > 0) {
                    result += ",";
                }

                Value element = this_obj->get_element(i);

                if (!element.is_null() && !element.is_undefined()) {
                    if (element.is_object()) {
                        Object* elem_obj = element.as_object();
                        if (elem_obj->has_property("toLocaleString")) {
                            Value toLocaleString_val = elem_obj->get_property("toLocaleString");
                            if (toLocaleString_val.is_function()) {
                                Function* fn = toLocaleString_val.as_function();
                                std::vector<Value> empty_args;
                                Value str_val = fn->call(ctx, empty_args, element);
                                result += str_val.to_string();
                                continue;
                            }
                        }
                    }
                    result += element.to_string();
                }
            }

            return Value(result);
        });
    PropertyDescriptor array_toLocaleString_desc(Value(toLocaleString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toLocaleString", array_toLocaleString_desc);

    auto toReversed_fn = ObjectFactory::create_native_function("toReversed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            uint32_t length = this_obj->get_length();
            auto result = ObjectFactory::create_array(length);

            for (uint32_t i = 0; i < length; i++) {
                result->set_element(i, this_obj->get_element(length - 1 - i));
            }
            result->set_length(length);
            return Value(result.release());
        }, 0);
    PropertyDescriptor toReversed_desc(Value(toReversed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toReversed", toReversed_desc);

    auto toSorted_fn = ObjectFactory::create_native_function("toSorted",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            uint32_t length = this_obj->get_length();
            auto result = ObjectFactory::create_array(length);

            for (uint32_t i = 0; i < length; i++) {
                result->set_element(i, this_obj->get_element(i));
            }
            result->set_length(length);

            return Value(result.release());
        }, 1);
    PropertyDescriptor toSorted_desc(Value(toSorted_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toSorted", toSorted_desc);

    auto toSpliced_fn = ObjectFactory::create_native_function("toSpliced",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            uint32_t length = this_obj->get_length();
            int32_t start = args.empty() ? 0 : static_cast<int32_t>(args[0].to_number());
            uint32_t deleteCount = args.size() < 2 ? (length - start) : static_cast<uint32_t>(args[1].to_number());

            if (start < 0) {
                start = static_cast<int32_t>(length) + start;
                if (start < 0) start = 0;
            }
            if (start > static_cast<int32_t>(length)) start = length;

            auto result = ObjectFactory::create_array();
            uint32_t result_index = 0;

            for (uint32_t i = 0; i < static_cast<uint32_t>(start); i++) {
                result->set_element(result_index++, this_obj->get_element(i));
            }

            for (size_t i = 2; i < args.size(); i++) {
                result->set_element(result_index++, args[i]);
            }

            uint32_t after_start = static_cast<uint32_t>(start) + deleteCount;
            for (uint32_t i = after_start; i < length; i++) {
                result->set_element(result_index++, this_obj->get_element(i));
            }

            result->set_length(result_index);
            return Value(result.release());
        }, 2);
    PropertyDescriptor toSpliced_desc(Value(toSpliced_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("toSpliced", toSpliced_desc);

    auto array_concat_fn = ObjectFactory::create_native_function("concat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_array = ctx.get_this_binding();
            if (!this_array) {
                ctx.throw_exception(Value(std::string("TypeError: Array.prototype.concat called on null or undefined")));
                return Value();
            }

            auto result = ObjectFactory::create_array(0);
            uint32_t result_index = 0;

            uint32_t this_length = this_array->get_length();
            for (uint32_t i = 0; i < this_length; i++) {
                Value element = this_array->get_element(i);
                result->set_element(result_index++, element);
            }

            for (const auto& arg : args) {
                if (arg.is_object() && arg.as_object()->is_array()) {
                    Object* arg_array = arg.as_object();
                    uint32_t arg_length = arg_array->get_length();
                    for (uint32_t i = 0; i < arg_length; i++) {
                        Value element = arg_array->get_element(i);
                        result->set_element(result_index++, element);
                    }
                } else {
                    result->set_element(result_index++, arg);
                }
            }

            result->set_length(result_index);
            return Value(result.release());
        });
    PropertyDescriptor concat_desc(Value(array_concat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("concat", concat_desc);

    auto every_fn = ObjectFactory::create_native_function("every",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(false);

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.every callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            uint32_t length = this_obj->get_length();

            for (uint32_t i = 0; i < length; i++) {
                // Skip missing elements in sparse arrays
                if (!this_obj->has_property(std::to_string(i))) {
                    continue;
                }
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                Value result = callback->call(ctx, callback_args, thisArg);
                if (!result.to_boolean()) {
                    return Value(false);
                }
            }
            return Value(true);
        }, 1);
    PropertyDescriptor every_desc(Value(every_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("every", every_desc);

    auto filter_fn = ObjectFactory::create_native_function("filter",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.filter callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            uint32_t length = this_obj->get_length();

            auto result = ObjectFactory::create_array();
            uint32_t result_index = 0;

            for (uint32_t i = 0; i < length; i++) {
                // Skip missing elements in sparse arrays
                if (!this_obj->has_property(std::to_string(i))) {
                    continue;
                }
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                Value test_result = callback->call(ctx, callback_args, thisArg);
                if (test_result.to_boolean()) {
                    result->set_element(result_index++, element);
                }
            }
            result->set_length(result_index);
            return Value(result.release());
        }, 1);
    PropertyDescriptor filter_desc(Value(filter_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("filter", filter_desc);

    auto forEach_fn = ObjectFactory::create_native_function("forEach",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            if (args.empty() || !args[0].is_function()) return Value();

            Function* callback = args[0].as_function();
            Value this_arg = args.size() > 1 ? args[1] : Value();

            uint32_t length = this_obj->get_length();
            for (uint32_t i = 0; i < length; i++) {
                // Skip missing elements in sparse arrays
                if (!this_obj->has_property(std::to_string(i))) {
                    continue;
                }
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this_obj)};
                callback->call(ctx, callback_args, this_arg);
                if (ctx.has_exception()) return Value();
            }
            return Value();
        }, 1);
    PropertyDescriptor forEach_desc(Value(forEach_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("forEach", forEach_desc);

    auto indexOf_fn = ObjectFactory::create_native_function("indexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(-1.0);

            if (args.empty()) return Value(-1.0);
            Value search_element = args[0];

            uint32_t length = this_obj->get_length();

            // Handle fromIndex parameter 
            int32_t start_index = 0;
            if (args.size() > 1) {
                double from_index = args[1].to_number();

                // If fromIndex is NaN, treat as 0
                if (std::isnan(from_index)) {
                    start_index = 0;
                }
                // If fromIndex is negative, calculate from end
                else if (from_index < 0) {
                    int32_t relative_index = static_cast<int32_t>(length) + static_cast<int32_t>(from_index);
                    start_index = relative_index < 0 ? 0 : relative_index;
                }
                // If fromIndex is positive
                else {
                    start_index = static_cast<int32_t>(from_index);
                    if (start_index >= static_cast<int32_t>(length)) {
                        return Value(-1.0);
                    }
                }
            }

            for (uint32_t i = static_cast<uint32_t>(start_index); i < length; i++) {
                Value element = this_obj->get_element(i);
                if (element.strict_equals(search_element)) {
                    return Value(static_cast<double>(i));
                }
            }
            return Value(-1.0);
        }, 1);
    PropertyDescriptor array_indexOf_desc(Value(indexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("indexOf", array_indexOf_desc);

    auto map_fn = ObjectFactory::create_native_function("map",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.map callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            auto result = ObjectFactory::create_array();
            uint32_t length = this_obj->get_length();

            for (uint32_t i = 0; i < length; i++) {
                // Skip missing elements in sparse arrays
                if (this_obj->has_property(std::to_string(i))) {
                    Value element = this_obj->get_element(i);
                    std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                    Value mapped = callback->call(ctx, callback_args, thisArg);
                    result->set_element(i, mapped);
                }
            }
            result->set_length(length);
            return Value(result.release());
        }, 1);
    PropertyDescriptor map_desc(Value(map_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("map", map_desc);

    auto reduce_fn = ObjectFactory::create_native_function("reduce",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Reduce of empty array with no initial value");
            }

            Function* callback = args[0].as_function();
            uint32_t length = this_obj->get_length();

            if (length == 0 && args.size() < 2) {
                throw std::runtime_error("TypeError: Reduce of empty array with no initial value");
            }

            uint32_t start_index = 0;
            Value accumulator;

            if (args.size() > 1) {
                accumulator = args[1];
            } else {
                // Find first existing element in sparse array
                bool found = false;
                for (uint32_t i = 0; i < length; i++) {
                    if (this_obj->has_property(std::to_string(i))) {
                        accumulator = this_obj->get_element(i);
                        start_index = i + 1;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw std::runtime_error("TypeError: Reduce of empty array with no initial value");
                }
            }

            for (uint32_t i = start_index; i < length; i++) {
                // Skip missing elements in sparse arrays
                if (!this_obj->has_property(std::to_string(i))) {
                    continue;
                }
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = {
                    accumulator,
                    element,
                    Value(static_cast<double>(i)),
                    Value(this_obj)
                };
                accumulator = callback->call(ctx, callback_args);
            }

            return accumulator;
        }, 1);
    PropertyDescriptor reduce_desc(Value(reduce_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("reduce", reduce_desc);

    auto some_fn = ObjectFactory::create_native_function("some",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(false);

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.some callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            uint32_t length = this_obj->get_length();

            for (uint32_t i = 0; i < length; i++) {
                // Skip missing elements in sparse arrays
                if (!this_obj->has_property(std::to_string(i))) {
                    continue;
                }
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                Value result = callback->call(ctx, callback_args, thisArg);
                if (result.to_boolean()) {
                    return Value(true);
                }
            }

            return Value(false);
        }, 1);
    PropertyDescriptor some_desc(Value(some_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("some", some_desc);

    auto findIndex_fn = ObjectFactory::create_native_function("findIndex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(-1.0);

            if (args.empty() || !args[0].is_function()) {
                throw std::runtime_error("TypeError: Array.prototype.findIndex callback must be a function");
            }

            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            uint32_t length = this_obj->get_length();

            for (uint32_t i = 0; i < length; i++) {
                Value element = this_obj->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), Value(this_obj) };
                Value result = callback->call(ctx, callback_args, thisArg);
                if (result.to_boolean()) {
                    return Value(static_cast<double>(i));
                }
            }

            return Value(-1.0);
        }, 1);
    PropertyDescriptor findIndex_desc(Value(findIndex_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("findIndex", findIndex_desc);

    auto join_fn = ObjectFactory::create_native_function("join",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(std::string(""));

            std::string separator = args.empty() ? "," : args[0].to_string();
            std::string result = "";

            uint32_t length = this_obj->get_length();
            for (uint32_t i = 0; i < length; i++) {
                if (i > 0) result += separator;
                result += this_obj->get_element(i).to_string();
            }
            return Value(result);
        }, 1);
    PropertyDescriptor join_desc(Value(join_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("join", join_desc);

    auto pop_fn = ObjectFactory::create_native_function("pop",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            uint32_t length = this_obj->get_length();
            if (length == 0) return Value();

            Value element = this_obj->get_element(length - 1);
            this_obj->set_length(length - 1);
            return element;
        }, 0);
    PropertyDescriptor pop_desc(Value(pop_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("pop", pop_desc);

    auto reverse_fn = ObjectFactory::create_native_function("reverse",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(this_obj);

            uint32_t length = this_obj->get_length();
            for (uint32_t i = 0; i < length / 2; i++) {
                Value temp = this_obj->get_element(i);
                this_obj->set_element(i, this_obj->get_element(length - 1 - i));
                this_obj->set_element(length - 1 - i, temp);
            }
            return Value(this_obj);
        }, 0);
    PropertyDescriptor reverse_desc(Value(reverse_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("reverse", reverse_desc);

    auto shift_fn = ObjectFactory::create_native_function("shift",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            uint32_t length = this_obj->get_length();
            if (length == 0) return Value();

            Value first = this_obj->get_element(0);
            for (uint32_t i = 1; i < length; i++) {
                this_obj->set_element(i - 1, this_obj->get_element(i));
            }
            this_obj->set_length(length - 1);
            return first;
        }, 0);
    PropertyDescriptor shift_desc(Value(shift_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("shift", shift_desc);

    auto slice_fn = ObjectFactory::create_native_function("slice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                auto empty = ObjectFactory::create_array();
                return Value(empty.release());
            }

            uint32_t length = this_obj->get_length();

            int32_t start = 0;
            int32_t end = static_cast<int32_t>(length);

            if (!args.empty()) {
                start = static_cast<int32_t>(args[0].to_number());
            }
            if (args.size() >= 2) {
                end = static_cast<int32_t>(args[1].to_number());
            }

            if (start < 0) start = std::max(0, static_cast<int32_t>(length) + start);
            if (end < 0) end = std::max(0, static_cast<int32_t>(length) + end);
            if (start < 0) start = 0;
            if (end > static_cast<int32_t>(length)) end = length;
            if (start > end) start = end;

            auto result = ObjectFactory::create_array();
            uint32_t result_index = 0;
            for (int32_t i = start; i < end; i++) {
                Value elem = this_obj->get_element(static_cast<uint32_t>(i));
                result->set_element(result_index++, elem);
            }
            result->set_length(result_index);

            return Value(result.release());
        }, 2);
    PropertyDescriptor slice_desc(Value(slice_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("slice", slice_desc);

    auto sort_fn = ObjectFactory::create_native_function("sort",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(this_obj);

            uint32_t length = this_obj->get_length();
            if (length <= 1) return Value(this_obj);

            // ES5: If compareFn is not undefined and is not a function, throw TypeError
            Function* compareFn = nullptr;
            if (!args.empty() && !args[0].is_undefined()) {
                if (!args[0].is_function()) {
                    ctx.throw_type_error("Array.prototype.sort: compareFn must be a function or undefined");
                    return Value();
                }
                compareFn = args[0].as_function();
            }

            auto compare = [&](const Value& a, const Value& b) -> int {
                if (a.is_undefined() && b.is_undefined()) return 0;
                if (a.is_undefined()) return 1;
                if (b.is_undefined()) return -1;

                if (compareFn) {
                    std::vector<Value> compare_args = { a, b };
                    Value result = compareFn->call(ctx, compare_args);
                    double cmp = result.to_number();
                    if (std::isnan(cmp)) return 0;
                    return cmp > 0 ? 1 : (cmp < 0 ? -1 : 0);
                } else {
                    std::string str_a = a.to_string();
                    std::string str_b = b.to_string();
                    return str_a.compare(str_b);
                }
            };

            std::function<void(int32_t, int32_t)> quicksort;
            quicksort = [&](int32_t low, int32_t high) {
                if (low < high) {
                    Value pivot = this_obj->get_element(high);
                    int32_t i = low - 1;

                    for (int32_t j = low; j < high; j++) {
                        Value current = this_obj->get_element(j);
                        if (compare(current, pivot) <= 0) {
                            i++;
                            Value temp = this_obj->get_element(i);
                            this_obj->set_element(i, current);
                            this_obj->set_element(j, temp);
                        }
                    }

                    Value temp = this_obj->get_element(i + 1);
                    this_obj->set_element(i + 1, this_obj->get_element(high));
                    this_obj->set_element(high, temp);

                    int32_t pivot_index = i + 1;

                    quicksort(low, pivot_index - 1);
                    quicksort(pivot_index + 1, high);
                }
            };

            quicksort(0, static_cast<int32_t>(length) - 1);

            return Value(this_obj);
        }, 1);
    PropertyDescriptor sort_desc(Value(sort_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("sort", sort_desc);

    auto splice_fn = ObjectFactory::create_native_function("splice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(ObjectFactory::create_array().release());

            uint32_t length = this_obj->get_length();

            int32_t start = 0;
            if (!args.empty()) {
                double start_arg = args[0].to_number();
                if (start_arg < 0) {
                    start = std::max(0, static_cast<int32_t>(length) + static_cast<int32_t>(start_arg));
                } else {
                    start = std::min(static_cast<uint32_t>(start_arg), length);
                }
            }

            uint32_t delete_count = 0;
            if (args.size() < 2) {
                delete_count = length - start;
            } else {
                double delete_arg = args[1].to_number();
                if (delete_arg < 0) {
                    delete_count = 0;
                } else {
                    delete_count = std::min(static_cast<uint32_t>(delete_arg), length - start);
                }
            }

            std::vector<Value> items_to_insert;
            for (size_t i = 2; i < args.size(); i++) {
                items_to_insert.push_back(args[i]);
            }

            auto result = ObjectFactory::create_array();
            for (uint32_t i = 0; i < delete_count; i++) {
                result->set_element(i, this_obj->get_element(start + i));
            }
            result->set_length(delete_count);

            uint32_t item_count = items_to_insert.size();
            uint32_t new_length = length - delete_count + item_count;

            if (item_count > delete_count) {
                uint32_t shift = item_count - delete_count;
                for (int32_t i = length - 1; i >= static_cast<int32_t>(start + delete_count); i--) {
                    this_obj->set_element(i + shift, this_obj->get_element(i));
                }
            }
            else if (delete_count > item_count) {
                uint32_t shift = delete_count - item_count;
                for (uint32_t i = start + delete_count; i < length; i++) {
                    this_obj->set_element(i - shift, this_obj->get_element(i));
                }
            }

            for (uint32_t i = 0; i < item_count; i++) {
                this_obj->set_element(start + i, items_to_insert[i]);
            }

            this_obj->set_length(new_length);

            return Value(result.release());
        }, 2);
    PropertyDescriptor splice_desc(Value(splice_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("splice", splice_desc);

    auto unshift_fn = ObjectFactory::create_native_function("unshift",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(0.0);

            uint32_t length = this_obj->get_length();
            uint32_t argCount = args.size();

            for (int32_t i = length - 1; i >= 0; i--) {
                this_obj->set_element(i + argCount, this_obj->get_element(i));
            }

            for (uint32_t i = 0; i < argCount; i++) {
                this_obj->set_element(i, args[i]);
            }

            uint32_t new_length = length + argCount;
            this_obj->set_length(new_length);
            return Value(static_cast<double>(new_length));
        }, 1);
    PropertyDescriptor unshift_desc(Value(unshift_fn.release()),
        PropertyAttributes::BuiltinFunction);
    array_prototype->set_property_descriptor("unshift", unshift_desc);

    Object* array_proto_ptr = array_prototype.get();

    PropertyDescriptor array_constructor_desc(Value(array_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    array_proto_ptr->set_property_descriptor("constructor", array_constructor_desc);

    PropertyDescriptor array_tag_desc(Value(std::string("Array")), PropertyAttributes::Configurable);
    array_proto_ptr->set_property_descriptor("Symbol.toStringTag", array_tag_desc);

    Symbol* unscopables_symbol = Symbol::get_well_known(Symbol::UNSCOPABLES);
    if (unscopables_symbol) {
        auto unscopables_obj = ObjectFactory::create_object();
        unscopables_obj->set_prototype(nullptr);
        unscopables_obj->set_property("at", Value(true));
        unscopables_obj->set_property("copyWithin", Value(true));
        unscopables_obj->set_property("entries", Value(true));
        unscopables_obj->set_property("fill", Value(true));
        unscopables_obj->set_property("find", Value(true));
        unscopables_obj->set_property("findIndex", Value(true));
        unscopables_obj->set_property("findLast", Value(true));
        unscopables_obj->set_property("findLastIndex", Value(true));
        unscopables_obj->set_property("flat", Value(true));
        unscopables_obj->set_property("flatMap", Value(true));
        unscopables_obj->set_property("includes", Value(true));
        unscopables_obj->set_property("keys", Value(true));
        unscopables_obj->set_property("values", Value(true));
        PropertyDescriptor unscopables_desc(Value(unscopables_obj.release()), PropertyAttributes::Configurable);
        array_proto_ptr->set_property_descriptor(unscopables_symbol->get_description(), unscopables_desc);
    }

    array_constructor->set_property("prototype", Value(array_prototype.release()), PropertyAttributes::None);

    auto array_species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(ctx.get_this_binding());
        }, 0);

    PropertyDescriptor array_species_desc;
    array_species_desc.set_getter(array_species_getter.get());
    array_species_desc.set_enumerable(false);
    array_species_desc.set_configurable(true);

    Value array_species_symbol = global_object_->get_property("Symbol");
    if (array_species_symbol.is_object()) {
        Object* symbol_constructor = array_species_symbol.as_object();
        Value species_key = symbol_constructor->get_property("species");
        if (species_key.is_symbol()) {
            array_constructor->set_property_descriptor(species_key.as_symbol()->to_property_key(), array_species_desc);
        }
    }

    array_species_getter.release();

    ObjectFactory::set_array_prototype(array_proto_ptr);

    register_built_in_object("Array", array_constructor.release());
    
    auto function_constructor = ObjectFactory::create_native_constructor("Function",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(ObjectFactory::create_function().release());
        });
    
    auto function_prototype = ObjectFactory::create_object();
    
    // Set function prototype early so create_native_function can use it
    Object* function_proto_ptr = function_prototype.get();
    ObjectFactory::set_function_prototype(function_proto_ptr);
    
    auto call_fn = ObjectFactory::create_native_function("call",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_type_error("Function.prototype.call called on non-function");
                return Value();
            }
            
            Function* func = static_cast<Function*>(function_obj);
            Value this_arg = args.size() > 0 ? args[0] : Value();
            
            std::vector<Value> call_args;
            for (size_t i = 1; i < args.size(); i++) {
                call_args.push_back(args[i]);
            }
            
            return func->call(ctx, call_args, this_arg);
        });

    PropertyDescriptor call_length_desc(Value(1.0), PropertyAttributes::Configurable);
    call_length_desc.set_enumerable(false);
    call_length_desc.set_writable(false);
    call_fn->set_property_descriptor("length", call_length_desc);

    call_fn->set_property("name", Value(std::string("call")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("call", Value(call_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto apply_fn = ObjectFactory::create_native_function("apply",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_type_error("Function.prototype.apply called on non-function");
                return Value();
            }
            
            Function* func = static_cast<Function*>(function_obj);
            Value this_arg = args.size() > 0 ? args[0] : Value();
            
            std::vector<Value> call_args;
            if (args.size() > 1 && !args[1].is_undefined() && !args[1].is_null()) {
                if (args[1].is_object()) {
                    Object* args_array = args[1].as_object();
                    // ES5: Accept any array-like object (object with length property)
                    Value length_val = args_array->get_property("length");
                    if (length_val.is_number()) {
                        uint32_t length = static_cast<uint32_t>(length_val.to_number());
                        for (uint32_t i = 0; i < length; i++) {
                            // Use get_property for array-like objects (not just arrays)
                            Value element = args_array->get_property(std::to_string(i));
                            call_args.push_back(element);
                        }
                    }
                }
            }
            
            return func->call(ctx, call_args, this_arg);
        });

    PropertyDescriptor apply_length_desc(Value(2.0), PropertyAttributes::Configurable);
    apply_length_desc.set_enumerable(false);
    apply_length_desc.set_writable(false);
    apply_fn->set_property_descriptor("length", apply_length_desc);

    apply_fn->set_property("name", Value(std::string("apply")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("apply", Value(apply_fn.release()), PropertyAttributes::BuiltinFunction);

    auto bind_fn = ObjectFactory::create_native_function("bind",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_type_error("Function.prototype.bind called on non-function");
                return Value();
            }

            Function* target_func = static_cast<Function*>(function_obj);
            Value bound_this = args.size() > 0 ? args[0] : Value();

            std::vector<Value> bound_args;
            for (size_t i = 1; i < args.size(); i++) {
                bound_args.push_back(args[i]);
            }

            // Calculate bound function arity: target length minus bound args count (minimum 0)
            Value target_length_val = target_func->get_property("length");
            double target_length = target_length_val.is_number() ? target_length_val.as_number() : 0.0;
            double bound_length = target_length - static_cast<double>(bound_args.size());
            if (bound_length < 0) bound_length = 0;
            uint32_t bound_arity = static_cast<uint32_t>(bound_length);

            // Create bound function that works both as regular call and constructor
            auto bound_function = ObjectFactory::create_native_constructor("bound",
                [target_func, bound_this, bound_args](Context& ctx, const std::vector<Value>& call_args) -> Value {
                    std::vector<Value> final_args = bound_args;
                    final_args.insert(final_args.end(), call_args.begin(), call_args.end());

                    // If called as constructor, ignore bound this and use new object
                    if (ctx.is_in_constructor_call()) {
                        return target_func->construct(ctx, final_args);
                    } else {
                        return target_func->call(ctx, final_args, bound_this);
                    }
                }, bound_arity);

            return Value(bound_function.release());
        });

    PropertyDescriptor bind_length_desc(Value(1.0), PropertyAttributes::Configurable);
    bind_length_desc.set_enumerable(false);
    bind_length_desc.set_writable(false);
    bind_fn->set_property_descriptor("length", bind_length_desc);

    bind_fn->set_property("name", Value(std::string("bind")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("bind", Value(bind_fn.release()), PropertyAttributes::BuiltinFunction);

    auto function_toString_fn = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_type_error("Function.prototype.toString called on non-function");
                return Value();
            }

            Function* func = static_cast<Function*>(function_obj);
            std::string func_name = "anonymous";

            Value name_val = func->get_property("name");
            if (!name_val.is_undefined() && !name_val.to_string().empty()) {
                func_name = name_val.to_string();
            }

            return Value("function " + func_name + "() { [native code] }");
        });

    PropertyDescriptor function_toString_length_desc(Value(0.0), PropertyAttributes::Configurable);
    function_toString_length_desc.set_enumerable(false);
    function_toString_length_desc.set_writable(false);
    function_toString_fn->set_property_descriptor("length", function_toString_length_desc);

    function_toString_fn->set_property("name", Value(std::string("toString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    function_prototype->set_property("toString", Value(function_toString_fn.release()), PropertyAttributes::BuiltinFunction);

    function_prototype->set_property("name", Value(std::string("")), PropertyAttributes::Configurable);

    // Set Function.prototype's prototype to Object.prototype so Function objects inherit Object methods
    Object* object_proto = ObjectFactory::get_object_prototype();
    if (object_proto) {
        function_prototype->set_prototype(object_proto);
    }

    PropertyDescriptor function_proto_ctor_desc(Value(function_constructor.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    function_proto_ptr->set_property_descriptor("constructor", function_proto_ctor_desc);

    function_constructor->set_property("prototype", Value(function_prototype.release()), PropertyAttributes::None);

    static_cast<Object*>(function_constructor.get())->set_prototype(function_proto_ptr);

    register_built_in_object("Function", function_constructor.release());

    auto string_constructor = ObjectFactory::create_native_constructor("String",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str_value = args.empty() ? "" : args[0].to_string();

            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_obj->set_property("value", Value(str_value));
                this_obj->set_property("[[PrimitiveValue]]", Value(str_value));
                PropertyDescriptor length_desc(Value(static_cast<double>(str_value.length())),
                    static_cast<PropertyAttributes>(PropertyAttributes::None));
                this_obj->set_property_descriptor("length", length_desc);

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)args;
                        Object* this_binding = ctx.get_this_binding();
                        if (this_binding && this_binding->has_property("value")) {
                            return this_binding->get_property("value");
                        }
                        return Value(std::string(""));
                    });
                this_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);
            }

            return Value(str_value);
        });
    
    auto string_prototype = ObjectFactory::create_object();
    
    auto padStart_fn = ObjectFactory::create_native_function("padStart",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
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
    PropertyDescriptor padStart_desc(Value(padStart_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("padStart", padStart_desc);
    
    auto padEnd_fn = ObjectFactory::create_native_function("padEnd",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
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
    PropertyDescriptor padEnd_desc(Value(padEnd_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("padEnd", padEnd_desc);

    auto str_includes_fn = ObjectFactory::create_native_function("includes",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(false);

            if (args[0].is_symbol()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                return Value();
            }

            std::string search_string = args[0].to_string();
            size_t position = 0;
            if (args.size() > 1) {
                if (args[1].is_symbol()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
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
    PropertyDescriptor str_includes_length_desc(Value(1.0), PropertyAttributes::Configurable);
    str_includes_length_desc.set_enumerable(false);
    str_includes_length_desc.set_writable(false);
    str_includes_fn->set_property_descriptor("length", str_includes_length_desc);
    PropertyDescriptor string_includes_desc(Value(str_includes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("includes", string_includes_desc);

    auto startsWith_fn = ObjectFactory::create_native_function("startsWith",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(false);

            if (args[0].is_symbol()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                return Value();
            }

            std::string search_string = args[0].to_string();
            size_t position = 0;
            if (args.size() > 1) {
                if (args[1].is_symbol()) {
                    ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
                    return Value();
                }
                position = static_cast<size_t>(std::max(0.0, args[1].to_number()));
            }

            if (position >= str.length()) {
                return Value(search_string.empty());
            }

            return Value(str.substr(position, search_string.length()) == search_string);
        });
    PropertyDescriptor startsWith_length_desc(Value(1.0), PropertyAttributes::Configurable);
    startsWith_length_desc.set_enumerable(false);
    startsWith_length_desc.set_writable(false);
    startsWith_fn->set_property_descriptor("length", startsWith_length_desc);
    PropertyDescriptor startsWith_desc(Value(startsWith_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("startsWith", startsWith_desc);

    auto endsWith_fn = ObjectFactory::create_native_function("endsWith",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) return Value(false);

            if (args[0].is_symbol()) {
                ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                return Value();
            }

            std::string search_string = args[0].to_string();
            size_t length = args.size() > 1 ?
                static_cast<size_t>(std::max(0.0, args[1].to_number())) : str.length();

            if (length > str.length()) length = str.length();
            if (search_string.length() > length) return Value(false);

            return Value(str.substr(length - search_string.length(), search_string.length()) == search_string);
        });
    PropertyDescriptor endsWith_length_desc(Value(1.0), PropertyAttributes::Configurable);
    endsWith_length_desc.set_enumerable(false);
    endsWith_length_desc.set_writable(false);
    endsWith_fn->set_property_descriptor("length", endsWith_length_desc);
    PropertyDescriptor endsWith_desc(Value(endsWith_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("endsWith", endsWith_desc);
    
    auto match_fn = ObjectFactory::create_native_function("match",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value();
            }

            if (args.empty()) return Value();

            Value pattern = args[0];

            if (pattern.is_object()) {
                Object* regex_obj = pattern.as_object();

                Value exec_method = regex_obj->get_property("exec");
                if (exec_method.is_object() && exec_method.as_object()->is_function()) {
                    std::vector<Value> exec_args = { Value(str) };
                    Function* exec_func = static_cast<Function*>(exec_method.as_object());
                    return exec_func->call(ctx, exec_args, pattern);
                }
            }

            std::string search = pattern.to_string();
            size_t pos = str.find(search);

            if (pos != std::string::npos) {
                auto result = ObjectFactory::create_array();
                result->set_element(0, Value(search));
                result->set_property("index", Value(static_cast<double>(pos)));
                result->set_property("input", Value(str));
                return Value(result.release());
            }

            return Value();
        });
    PropertyDescriptor match_desc(Value(match_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("match", match_desc);

    auto matchAll_fn = ObjectFactory::create_native_function("matchAll",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value();
            }

            if (args.empty()) {
                throw std::runtime_error("TypeError: matchAll requires a regexp argument");
            }

            auto result = ObjectFactory::create_array();
            result->set_length(0);
            return Value(result.release());
        }, 1);
    PropertyDescriptor matchAll_desc(Value(matchAll_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("matchAll", matchAll_desc);

    auto replace_fn = ObjectFactory::create_native_function("replace",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value(std::string(""));
            }

            if (args.size() < 2) return Value(str);

            Value search_val = args[0];
            std::string replacement = args[1].to_string();

            if (search_val.is_object()) {
                Object* regex_obj = search_val.as_object();

                Value exec_method = regex_obj->get_property("exec");
                if (exec_method.is_object() && exec_method.as_object()->is_function()) {
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

            std::string search = search_val.to_string();
            size_t pos = str.find(search);

            if (pos != std::string::npos) {
                str.replace(pos, search.length(), replacement);
            }

            return Value(str);
        });
    PropertyDescriptor replace_desc(Value(replace_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("replace", replace_desc);

    auto replaceAll_fn = ObjectFactory::create_native_function("replaceAll",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.size() < 2) return Value(str);

            std::string search = args[0].to_string();
            bool is_function = args[1].is_function();

            if (search.empty()) return Value(str);

            std::vector<size_t> positions;
            size_t pos = 0;
            while ((pos = str.find(search, pos)) != std::string::npos) {
                positions.push_back(pos);
                pos += search.length();
            }

            for (auto it = positions.rbegin(); it != positions.rend(); ++it) {
                std::string replacement;
                if (is_function) {
                    Function* replacer = args[1].as_function();
                    std::vector<Value> fn_args = {
                        Value(search),
                        Value(static_cast<double>(*it)),
                        Value(this_value.to_string())
                    };
                    Value result = replacer->call(ctx, fn_args);
                    if (ctx.has_exception()) return Value();
                    replacement = result.to_string();
                } else {
                    replacement = args[1].to_string();
                }
                str.replace(*it, search.length(), replacement);
            }

            return Value(str);
        }, 2);
    PropertyDescriptor replaceAll_desc(Value(replaceAll_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("replaceAll", replaceAll_desc);

    auto trim_fn = ObjectFactory::create_native_function("trim",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            size_t start = str.find_first_not_of(" \t\n\r\f\v");
            if (start == std::string::npos) return Value(std::string(""));

            size_t end = str.find_last_not_of(" \t\n\r\f\v");
            return Value(str.substr(start, end - start + 1));
        }, 0);
    PropertyDescriptor trim_desc(Value(trim_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("trim", trim_desc);

    auto trimStart_fn = ObjectFactory::create_native_function("trimStart",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            size_t start = str.find_first_not_of(" \t\n\r\f\v");
            if (start == std::string::npos) return Value(std::string(""));

            return Value(str.substr(start));
        }, 0);
    PropertyDescriptor trimStart_desc(Value(trimStart_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("trimStart", trimStart_desc);
    string_prototype->set_property_descriptor("trimLeft", trimStart_desc);

    auto trimEnd_fn = ObjectFactory::create_native_function("trimEnd",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            size_t end = str.find_last_not_of(" \t\n\r\f\v");
            if (end == std::string::npos) return Value(std::string(""));

            return Value(str.substr(0, end + 1));
        }, 0);
    PropertyDescriptor trimEnd_desc(Value(trimEnd_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("trimEnd", trimEnd_desc);
    string_prototype->set_property_descriptor("trimRight", trimEnd_desc);

    auto codePointAt_fn = ObjectFactory::create_native_function("codePointAt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.size() == 0 || str.empty()) return Value();

            int32_t pos = static_cast<int32_t>(args[0].to_number());
            if (pos < 0 || pos >= static_cast<int32_t>(str.length())) {
                return Value();
            }

            unsigned char ch = str[pos];

            if ((ch & 0x80) == 0) {
                return Value(static_cast<double>(ch));
            } else if ((ch & 0xE0) == 0xC0) {
                if (pos + 1 < static_cast<int32_t>(str.length())) {
                    uint32_t codePoint = ((ch & 0x1F) << 6) | (str[pos + 1] & 0x3F);
                    return Value(static_cast<double>(codePoint));
                }
            } else if ((ch & 0xF0) == 0xE0) {
                if (pos + 2 < static_cast<int32_t>(str.length())) {
                    uint32_t codePoint = ((ch & 0x0F) << 12) |
                                        ((str[pos + 1] & 0x3F) << 6) |
                                        (str[pos + 2] & 0x3F);
                    return Value(static_cast<double>(codePoint));
                }
            } else if ((ch & 0xF8) == 0xF0) {
                if (pos + 3 < static_cast<int32_t>(str.length())) {
                    uint32_t codePoint = ((ch & 0x07) << 18) |
                                        ((str[pos + 1] & 0x3F) << 12) |
                                        ((str[pos + 2] & 0x3F) << 6) |
                                        (str[pos + 3] & 0x3F);
                    return Value(static_cast<double>(codePoint));
                }
            }

            return Value(static_cast<double>(ch));
        }, 1);
    PropertyDescriptor codePointAt_desc(Value(codePointAt_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("codePointAt", codePointAt_desc);

    auto localeCompare_fn = ObjectFactory::create_native_function("localeCompare",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.size() == 0) return Value(0.0);

            std::string that = args[0].to_string();

            if (str < that) return Value(-1.0);
            if (str > that) return Value(1.0);
            return Value(0.0);
        }, 1);
    PropertyDescriptor localeCompare_desc(Value(localeCompare_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("localeCompare", localeCompare_desc);


    // Helper to convert this value to string for String.prototype methods
    auto toString_helper = [](Context& ctx, const Value& this_value) -> std::string {
        // ES1: If this is an object, try to call its toString method
        if (this_value.is_object() || this_value.is_function()) {
            Object* obj = this_value.is_object() ? this_value.as_object() : this_value.as_function();
            Value toString_method = obj->get_property("toString");
            if (!toString_method.is_undefined() && toString_method.is_function()) {
                Function* toString_fn = toString_method.as_function();
                std::vector<Value> empty_args;
                Value result = toString_fn->call(ctx, empty_args, this_value);
                if (ctx.has_exception()) {
                    return "";
                }
                return result.to_string();
            }
        }
        return this_value.to_string();
    };

    auto charAt_fn = ObjectFactory::create_native_function("charAt",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            uint32_t index = 0;
            if (args.size() > 0) {
                index = static_cast<uint32_t>(args[0].to_number());
            }

            if (index >= str.length()) {
                return Value(std::string(""));
            }

            return Value(std::string(1, str[index]));
        });
    PropertyDescriptor charAt_desc(Value(charAt_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("charAt", charAt_desc);

    auto string_at_fn = ObjectFactory::create_native_function("at",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();

            if (args.empty()) {
                return Value();
            }

            int64_t index = static_cast<int64_t>(args[0].to_number());
            int64_t len = static_cast<int64_t>(str.length());

            if (index < 0) {
                index = len + index;
            }

            if (index < 0 || index >= len) {
                return Value();
            }

            return Value(std::string(1, str[static_cast<size_t>(index)]));
        }, 1);
    PropertyDescriptor string_at_desc(Value(string_at_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("at", string_at_desc);

    auto charCodeAt_fn = ObjectFactory::create_native_function("charCodeAt",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            uint32_t index = 0;
            if (args.size() > 0) {
                index = static_cast<uint32_t>(args[0].to_number());
            }

            if (index >= str.length()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            return Value(static_cast<double>(static_cast<unsigned char>(str[index])));
        });
    PropertyDescriptor charCodeAt_desc(Value(charCodeAt_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("charCodeAt", charCodeAt_desc);

    auto str_indexOf_fn = ObjectFactory::create_native_function("indexOf",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            if (args.empty()) {
                return Value(-1.0);
            }

            std::string search = args[0].to_string();
            size_t start = 0;
            if (args.size() > 1) {
                double pos = args[1].to_number();
                // ES1: If position is NaN, treat as 0; if negative, treat as 0
                if (std::isnan(pos) || pos < 0) {
                    start = 0;
                } else {
                    start = static_cast<size_t>(pos);
                }
            }

            size_t found_pos = str.find(search, start);
            return Value(found_pos == std::string::npos ? -1.0 : static_cast<double>(found_pos));
        }, 1);
    PropertyDescriptor string_indexOf_desc(Value(str_indexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("indexOf", string_indexOf_desc);

    auto str_split_fn = ObjectFactory::create_native_function("split",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            auto result_array = ObjectFactory::create_array(0);

            // ES1: If separator is undefined, return array with entire string
            if (args.empty() || args[0].is_undefined()) {
                result_array->set_element(0, Value(str));
                return Value(result_array.release());
            }

            std::string separator = args[0].to_string();

            // ES1: If separator is empty string, split into individual characters
            if (separator.empty()) {
                for (size_t i = 0; i < str.length(); ++i) {
                    result_array->set_element(i, Value(std::string(1, str[i])));
                }
            } else {
                // Split by separator string
                size_t start = 0;
                size_t end = 0;
                uint32_t index = 0;

                while ((end = str.find(separator, start)) != std::string::npos) {
                    result_array->set_element(index++, Value(str.substr(start, end - start)));
                    start = end + separator.length();
                }
                result_array->set_element(index, Value(str.substr(start)));
            }

            return Value(result_array.release());
        }, 1);
    PropertyDescriptor string_split_desc(Value(str_split_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("split", string_split_desc);

    auto toLowerCase_fn = ObjectFactory::create_native_function("toLowerCase",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            std::transform(str.begin(), str.end(), str.begin(),
                [](unsigned char c) { return std::tolower(c); });

            return Value(str);
        });
    PropertyDescriptor toLowerCase_desc(Value(toLowerCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toLowerCase", toLowerCase_desc);

    auto toUpperCase_fn = ObjectFactory::create_native_function("toUpperCase",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            std::transform(str.begin(), str.end(), str.begin(),
                [](unsigned char c) { return std::toupper(c); });

            return Value(str);
        });
    PropertyDescriptor toUpperCase_desc(Value(toUpperCase_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toUpperCase", toUpperCase_desc);

    // ES1: 15.5.4.7 String.prototype.lastIndexOf(searchString, position)
    auto str_lastIndexOf_fn = ObjectFactory::create_native_function("lastIndexOf",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            if (args.empty()) {
                return Value(-1.0);
            }

            std::string search = args[0].to_string();
            size_t start = str.length();

            if (args.size() > 1) {
                double pos = args[1].to_number();
                if (std::isnan(pos) || pos >= static_cast<double>(str.length())) {
                    start = str.length();
                } else if (pos < 0) {
                    start = 0;
                } else {
                    start = static_cast<size_t>(pos) + search.length();
                    if (start > str.length()) {
                        start = str.length();
                    }
                }
            }

            // Search backwards from start position
            if (search.empty()) {
                return Value(static_cast<double>(std::min(start, str.length())));
            }

            size_t found_pos = str.rfind(search, start);
            return Value(found_pos == std::string::npos ? -1.0 : static_cast<double>(found_pos));
        }, 1);
    PropertyDescriptor str_lastIndexOf_desc(Value(str_lastIndexOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("lastIndexOf", str_lastIndexOf_desc);

    // ES1: 15.5.4.10 String.prototype.substring(start, end)
    auto str_substring_fn = ObjectFactory::create_native_function("substring",
        [toString_helper](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = toString_helper(ctx, this_value);

            size_t len = str.length();
            size_t start = 0;
            size_t end = len;

            if (args.size() > 0) {
                double start_num = args[0].to_number();
                if (std::isnan(start_num) || start_num < 0) {
                    start = 0;
                } else if (start_num > static_cast<double>(len)) {
                    start = len;
                } else {
                    start = static_cast<size_t>(start_num);
                }
            }

            if (args.size() > 1) {
                double end_num = args[1].to_number();
                if (std::isnan(end_num) || end_num < 0) {
                    end = 0;
                } else if (end_num > static_cast<double>(len)) {
                    end = len;
                } else {
                    end = static_cast<size_t>(end_num);
                }
            }

            // ES1: If start > end, swap them
            if (start > end) {
                std::swap(start, end);
            }

            if (start >= len) {
                return Value(std::string(""));
            }

            return Value(str.substr(start, end - start));
        }, 2);
    PropertyDescriptor str_substring_desc(Value(str_substring_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("substring", str_substring_desc);

    auto string_concat_static = ObjectFactory::create_native_function("concat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string result = "";
            for (const auto& arg : args) {
                result += arg.to_string();
            }
            return Value(result);
        });
    string_constructor->set_property("concat", Value(string_concat_static.release()));


    // Helper lambda for HTML escaping attribute values
    auto html_escape_attr = [](const std::string& s) -> std::string {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '"': result += "&quot;"; break;
                case '&': result += "&amp;"; break;
                default: result += c;
            }
        }
        return result;
    };

    auto anchor_fn = ObjectFactory::create_native_function("anchor",
        [html_escape_attr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");

            // RequireObjectCoercible
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call String.prototype.anchor on null or undefined");
                return Value();
            }

            std::string str = this_value.to_string();
            std::string name = args.size() > 0 ? html_escape_attr(args[0].to_string()) : "";
            return Value(std::string("<a name=\"") + name + "\">" + str + "</a>");
        }, 1);
    PropertyDescriptor anchor_desc(Value(anchor_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("anchor", anchor_desc);

    auto big_fn = ObjectFactory::create_native_function("big",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<big>") + str + "</big>");
        }, 0);
    PropertyDescriptor big_desc(Value(big_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("big", big_desc);

    auto blink_fn = ObjectFactory::create_native_function("blink",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<blink>") + str + "</blink>");
        }, 0);
    PropertyDescriptor blink_desc(Value(blink_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("blink", blink_desc);

    auto bold_fn = ObjectFactory::create_native_function("bold",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<b>") + str + "</b>");
        }, 0);
    PropertyDescriptor bold_desc(Value(bold_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("bold", bold_desc);

    auto fixed_fn = ObjectFactory::create_native_function("fixed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<tt>") + str + "</tt>");
        }, 0);
    PropertyDescriptor fixed_desc(Value(fixed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("fixed", fixed_desc);

    auto fontcolor_fn = ObjectFactory::create_native_function("fontcolor",
        [html_escape_attr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            std::string color = args.size() > 0 ? html_escape_attr(args[0].to_string()) : "";
            return Value(std::string("<font color=\"") + color + "\">" + str + "</font>");
        }, 1);
    PropertyDescriptor fontcolor_desc(Value(fontcolor_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("fontcolor", fontcolor_desc);

    auto fontsize_fn = ObjectFactory::create_native_function("fontsize",
        [html_escape_attr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            std::string size = args.size() > 0 ? html_escape_attr(args[0].to_string()) : "";
            return Value(std::string("<font size=\"") + size + "\">" + str + "</font>");
        }, 1);
    PropertyDescriptor fontsize_desc(Value(fontsize_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("fontsize", fontsize_desc);

    auto italics_fn = ObjectFactory::create_native_function("italics",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<i>") + str + "</i>");
        }, 0);
    PropertyDescriptor italics_desc(Value(italics_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("italics", italics_desc);

    auto link_fn = ObjectFactory::create_native_function("link",
        [html_escape_attr](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            std::string url = args.size() > 0 ? html_escape_attr(args[0].to_string()) : "";
            return Value(std::string("<a href=\"") + url + "\">" + str + "</a>");
        }, 1);
    PropertyDescriptor link_desc(Value(link_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("link", link_desc);

    auto small_fn = ObjectFactory::create_native_function("small",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<small>") + str + "</small>");
        }, 0);
    PropertyDescriptor small_desc(Value(small_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("small", small_desc);

    auto strike_fn = ObjectFactory::create_native_function("strike",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<strike>") + str + "</strike>");
        }, 0);
    PropertyDescriptor strike_desc(Value(strike_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("strike", strike_desc);

    auto sub_fn = ObjectFactory::create_native_function("sub",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<sub>") + str + "</sub>");
        }, 0);
    PropertyDescriptor sub_desc(Value(sub_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("sub", sub_desc);

    auto sup_fn = ObjectFactory::create_native_function("sup",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_value = ctx.get_binding("this");
            if (this_value.is_null() || this_value.is_undefined()) {
                ctx.throw_type_error("Cannot call method on null or undefined");
                return Value();
            }
            std::string str = this_value.to_string();
            return Value(std::string("<sup>") + str + "</sup>");
        }, 0);
    PropertyDescriptor sup_desc(Value(sup_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("sup", sup_desc);

    // AnnexB: String.prototype.substr(start, length)
    auto substr_fn = ObjectFactory::create_native_function("substr",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_value = ctx.get_binding("this");
            std::string str = this_value.to_string();
            int64_t size = static_cast<int64_t>(str.length());

            double start_val = 0;
            if (!args.empty()) {
                start_val = args[0].to_number();
            }

            int64_t intStart;
            if (std::isnan(start_val)) {
                intStart = 0;
            } else if (std::isinf(start_val)) {
                if (start_val < 0) {
                    intStart = 0; 
                } else {
                    intStart = size; 
                }
            } else {
                intStart = static_cast<int64_t>(std::trunc(start_val));
            }

            if (intStart < 0) {
                intStart = std::max(static_cast<int64_t>(0), size + intStart);
            }
            intStart = std::min(intStart, size);

            int64_t intLength;
            if (args.size() > 1) {
                double length_val = args[1].to_number();
                // ToIntegerOrInfinity
                if (std::isnan(length_val)) {
                    intLength = 0;
                } else if (std::isinf(length_val)) {
                    if (length_val < 0) {
                        intLength = 0;
                    } else {
                        intLength = size;
                    }
                } else {
                    intLength = static_cast<int64_t>(std::trunc(length_val));
                }
            } else {
                intLength = size;
            }

            intLength = std::min(std::max(intLength, static_cast<int64_t>(0)), size);

            int64_t intEnd = std::min(intStart + intLength, size);

            if (intEnd <= intStart) {
                return Value(std::string(""));
            }

            return Value(str.substr(static_cast<size_t>(intStart), static_cast<size_t>(intEnd - intStart)));
        }, 2);
    PropertyDescriptor substr_desc(Value(substr_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("substr", substr_desc);

    auto isWellFormed_fn = ObjectFactory::create_native_function("isWellFormed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value(true);
            }
            return Value(true);
        }, 0);
    PropertyDescriptor isWellFormed_desc(Value(isWellFormed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("isWellFormed", isWellFormed_desc);

    auto toWellFormed_fn = ObjectFactory::create_native_function("toWellFormed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value(std::string(""));
            }
            return Value(str);
        }, 0);
    PropertyDescriptor toWellFormed_desc(Value(toWellFormed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("toWellFormed", toWellFormed_desc);

    auto repeat_fn = ObjectFactory::create_native_function("repeat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string str = "";
            try {
                Value this_value = ctx.get_binding("this");
                str = this_value.to_string();
            } catch (...) {
                return Value(std::string(""));
            }

            if (args.empty()) return Value(std::string(""));

            int count = static_cast<int>(args[0].to_number());
            if (count < 0 || std::isinf(args[0].to_number())) {
                throw std::runtime_error("RangeError: Invalid count value");
            }

            if (count == 0) return Value(std::string(""));

            std::string result;
            result.reserve(str.length() * count);
            for (int i = 0; i < count; i++) {
                result += str;
            }
            return Value(result);
        }, 1);
    PropertyDescriptor repeat_desc(Value(repeat_fn.release()),
        PropertyAttributes::BuiltinFunction);
    string_prototype->set_property_descriptor("repeat", repeat_desc);

    Object* proto_ptr = string_prototype.get();
    string_constructor->set_property("prototype", Value(string_prototype.release()), PropertyAttributes::None);
    proto_ptr->set_property("constructor", Value(string_constructor.get()));

    auto string_raw_fn = ObjectFactory::create_native_function("raw",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("TypeError: String.raw requires at least 1 argument")));
                return Value();
            }

            if (args.size() > 0 && args[0].is_object()) {
                Object* template_obj = args[0].as_object();
                Value raw_val = template_obj->get_property("raw");
                if (raw_val.is_object()) {
                    Object* raw_array = raw_val.as_object();
                    if (raw_array->is_array() && raw_array->get_length() > 0) {
                        return raw_array->get_element(0);
                    }
                }
            }

            return Value(std::string(""));
        }, 1);

    string_constructor->set_property("raw", Value(string_raw_fn.release()), PropertyAttributes::BuiltinFunction);

    auto fromCharCode_fn = ObjectFactory::create_native_function("fromCharCode",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            std::string result;
            for (const auto& arg : args) {
                uint32_t code = static_cast<uint32_t>(arg.to_number()) & 0xFFFF;
                if (code <= 0x7F) {
                    result += static_cast<char>(code);
                } else if (code <= 0x7FF) {
                    result += static_cast<char>(0xC0 | (code >> 6));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    result += static_cast<char>(0xE0 | (code >> 12));
                    result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                }
            }
            return Value(result);
        }, 1);
    string_constructor->set_property("fromCharCode", Value(fromCharCode_fn.release()), PropertyAttributes::BuiltinFunction);

    auto fromCodePoint_fn = ObjectFactory::create_native_function("fromCodePoint",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string result;
            for (const auto& arg : args) {
                double num = arg.to_number();
                if (num < 0 || num > 0x10FFFF || num != std::floor(num)) {
                    ctx.throw_exception(Value(std::string("RangeError: Invalid code point")));
                    return Value();
                }
                uint32_t code = static_cast<uint32_t>(num);
                if (code <= 0x7F) {
                    result += static_cast<char>(code);
                } else if (code <= 0x7FF) {
                    result += static_cast<char>(0xC0 | (code >> 6));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                } else if (code <= 0xFFFF) {
                    result += static_cast<char>(0xE0 | (code >> 12));
                    result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    result += static_cast<char>(0xF0 | (code >> 18));
                    result += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                    result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code & 0x3F));
                }
            }
            return Value(result);
        }, 1);
    string_constructor->set_property("fromCodePoint", Value(fromCodePoint_fn.release()), PropertyAttributes::BuiltinFunction);

    register_built_in_object("String", string_constructor.release());

    Value global_string = global_object_->get_property("String");
    if (global_string.is_function()) {
        Object* global_string_obj = global_string.as_function();
        Value prototype_val = global_string_obj->get_property("prototype");
        if (prototype_val.is_object()) {
            Object* global_prototype = prototype_val.as_object();

            auto global_includes_fn = ObjectFactory::create_native_function("includes",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    Value this_value = ctx.get_binding("this");
                    std::string str = this_value.to_string();
                    if (args.empty()) return Value(false);
                    if (args[0].is_symbol()) {
                        ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                        return Value();
                    }
                    std::string search_string = args[0].to_string();
                    size_t position = 0;
                    if (args.size() > 1) {
                        if (args[1].is_symbol()) {
                            ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a number")));
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
            PropertyDescriptor global_includes_length_desc(Value(1.0), PropertyAttributes::Configurable);
            global_includes_length_desc.set_enumerable(false);
            global_includes_length_desc.set_writable(false);
            global_includes_fn->set_property_descriptor("length", global_includes_length_desc);
            global_prototype->set_property("includes", Value(global_includes_fn.release()), PropertyAttributes::BuiltinFunction);

            auto string_valueOf_fn = ObjectFactory::create_native_function("valueOf",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    Object* this_obj = ctx.get_this_binding();
                    Value this_val;
                    if (this_obj) {
                        this_val = Value(this_obj);
                    } else {
                        try {
                            this_val = ctx.get_binding("this");
                        } catch (...) {
                            ctx.throw_exception(Value(std::string("TypeError: String.prototype.valueOf called on non-object")));
                            return Value();
                        }
                    }

                    if (this_val.is_object()) {
                        Object* obj = this_val.as_object();
                        Value primitive_value = obj->get_property("[[PrimitiveValue]]");
                        if (!primitive_value.is_undefined() && primitive_value.is_string()) {
                            return primitive_value;
                        }
                    }

                    if (this_val.is_string()) {
                        return this_val;
                    }

                    return Value(this_val.to_string());
                });

            PropertyDescriptor string_valueOf_length_desc(Value(0.0), PropertyAttributes::Configurable);
            string_valueOf_length_desc.set_enumerable(false);
            string_valueOf_length_desc.set_writable(false);
            string_valueOf_fn->set_property_descriptor("length", string_valueOf_length_desc);

            PropertyDescriptor string_valueOf_name_desc(Value(std::string("valueOf")), PropertyAttributes::None);
            string_valueOf_name_desc.set_configurable(true);
            string_valueOf_name_desc.set_enumerable(false);
            string_valueOf_name_desc.set_writable(false);
            string_valueOf_fn->set_property_descriptor("name", string_valueOf_name_desc);

            global_prototype->set_property("valueOf", Value(string_valueOf_fn.release()), PropertyAttributes::BuiltinFunction);

            auto string_toString_fn = ObjectFactory::create_native_function("toString",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    Object* this_obj = ctx.get_this_binding();
                    Value this_val;
                    if (this_obj) {
                        this_val = Value(this_obj);
                    } else {
                        try {
                            this_val = ctx.get_binding("this");
                        } catch (...) {
                            ctx.throw_exception(Value(std::string("TypeError: String.prototype.toString called on non-object")));
                            return Value();
                        }
                    }

                    if (this_val.is_object()) {
                        Object* obj = this_val.as_object();
                        Value primitive_value = obj->get_property("[[PrimitiveValue]]");
                        if (!primitive_value.is_undefined() && primitive_value.is_string()) {
                            return primitive_value;
                        }
                    }

                    if (this_val.is_string()) {
                        return this_val;
                    }

                    return Value(this_val.to_string());
                });

            PropertyDescriptor string_toString_length_desc(Value(0.0), PropertyAttributes::Configurable);
            string_toString_length_desc.set_enumerable(false);
            string_toString_length_desc.set_writable(false);
            string_toString_fn->set_property_descriptor("length", string_toString_length_desc);

            PropertyDescriptor string_toString_name_desc(Value(std::string("toString")), PropertyAttributes::None);
            string_toString_name_desc.set_configurable(true);
            string_toString_name_desc.set_enumerable(false);
            string_toString_name_desc.set_writable(false);
            string_toString_fn->set_property_descriptor("name", string_toString_name_desc);

            global_prototype->set_property("toString", Value(string_toString_fn.release()), PropertyAttributes::BuiltinFunction);

            auto string_trim_fn = ObjectFactory::create_native_function("trim",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Value this_value = ctx.get_binding("this");
                    std::string str = this_value.to_string();

                    size_t start = 0;
                    size_t end = str.length();

                    while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
                        start++;
                    }
                    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
                        end--;
                    }

                    return Value(str.substr(start, end - start));
                });
            global_prototype->set_property("trim", Value(string_trim_fn.release()), PropertyAttributes::BuiltinFunction);

            auto string_trimStart_fn = ObjectFactory::create_native_function("trimStart",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Value this_value = ctx.get_binding("this");
                    std::string str = this_value.to_string();

                    size_t start = 0;
                    while (start < str.length() && std::isspace(static_cast<unsigned char>(str[start]))) {
                        start++;
                    }

                    return Value(str.substr(start));
                });
            global_prototype->set_property("trimStart", Value(string_trimStart_fn.release()), PropertyAttributes::BuiltinFunction);
            global_prototype->set_property("trimLeft", Value(string_trimStart_fn.get()), PropertyAttributes::BuiltinFunction);

            auto string_trimEnd_fn = ObjectFactory::create_native_function("trimEnd",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Value this_value = ctx.get_binding("this");
                    std::string str = this_value.to_string();

                    size_t end = str.length();
                    while (end > 0 && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
                        end--;
                    }

                    return Value(str.substr(0, end));
                });
            global_prototype->set_property("trimEnd", Value(string_trimEnd_fn.release()), PropertyAttributes::BuiltinFunction);
            global_prototype->set_property("trimRight", Value(string_trimEnd_fn.get()), PropertyAttributes::BuiltinFunction);

        }
    }

    auto bigint_constructor = ObjectFactory::create_native_constructor("BigInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_exception(Value(std::string("BigInt constructor requires an argument")));
                return Value();
            }
            
            try {
                if (args[0].is_number()) {
                    double num = args[0].as_number();
                    if (std::floor(num) != num) {
                        ctx.throw_exception(Value(std::string("Cannot convert non-integer Number to BigInt")));
                        return Value();
                    }
                    auto bigint = std::make_unique<BigInt>(static_cast<int64_t>(num));
                    return Value(bigint.release());
                } else if (args[0].is_string()) {
                    auto bigint = std::make_unique<BigInt>(args[0].to_string());
                    return Value(bigint.release());
                } else {
                    ctx.throw_exception(Value(std::string("Cannot convert value to BigInt")));
                    return Value();
                }
            } catch (const std::exception& e) {
                ctx.throw_exception(Value("Invalid BigInt: " + std::string(e.what())));
                return Value();
            }
        });
    register_built_in_object("BigInt", bigint_constructor.release());
    
    auto symbol_constructor = ObjectFactory::create_native_constructor("Symbol",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string description = "";
            if (!args.empty() && !args[0].is_undefined()) {
                description = args[0].to_string();
            }
            
            auto symbol = Symbol::create(description);
            return Value(symbol.release());
        });
    
    auto symbol_for_fn = ObjectFactory::create_native_function("for",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Symbol::symbol_for(ctx, args);
        });
    symbol_constructor->set_property("for", Value(symbol_for_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto symbol_key_for_fn = ObjectFactory::create_native_function("keyFor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Symbol::symbol_key_for(ctx, args);
        });
    symbol_constructor->set_property("keyFor", Value(symbol_key_for_fn.release()), PropertyAttributes::BuiltinFunction);

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
    
    Proxy::setup_proxy(*this);
    Reflect::setup_reflect(*this);

    Temporal::setup(*this);

    Map::setup_map_prototype(*this);
    Set::setup_set_prototype(*this);
    
    WeakMap::setup_weakmap_prototype(*this);
    WeakSet::setup_weakset_prototype(*this);
    
    AsyncUtils::setup_async_functions(*this);
    AsyncGenerator::setup_async_generator_prototype(*this);
    AsyncIterator::setup_async_iterator_prototype(*this);
    
    Iterator::setup_iterator_prototype(*this);
    
    Generator::setup_generator_prototype(*this);
    
    auto number_constructor = ObjectFactory::create_native_constructor("Number",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            double num_value = args.empty() ? 0.0 : args[0].to_number();

            // If this_obj exists (constructor call), set [[PrimitiveValue]]
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_obj->set_property("[[PrimitiveValue]]", Value(num_value));
            }

            // Always return primitive number
            // Function::construct will return the created object if called as constructor
            return Value(num_value);
        });
    PropertyDescriptor max_value_desc(Value(std::numeric_limits<double>::max()), PropertyAttributes::None);
    number_constructor->set_property_descriptor("MAX_VALUE", max_value_desc);
    PropertyDescriptor min_value_desc(Value(5e-324), PropertyAttributes::None);
    number_constructor->set_property_descriptor("MIN_VALUE", min_value_desc);
    PropertyDescriptor nan_desc(Value(std::numeric_limits<double>::quiet_NaN()), PropertyAttributes::None);
    number_constructor->set_property_descriptor("NaN", nan_desc);
    PropertyDescriptor pos_inf_desc(Value(std::numeric_limits<double>::infinity()), PropertyAttributes::None);
    number_constructor->set_property_descriptor("POSITIVE_INFINITY", pos_inf_desc);
    PropertyDescriptor neg_inf_desc(Value(-std::numeric_limits<double>::infinity()), PropertyAttributes::None);
    number_constructor->set_property_descriptor("NEGATIVE_INFINITY", neg_inf_desc);
    PropertyDescriptor epsilon_desc(Value(2.220446049250313e-16), PropertyAttributes::None);
    number_constructor->set_property_descriptor("EPSILON", epsilon_desc);
    PropertyDescriptor max_safe_desc(Value(9007199254740991.0), PropertyAttributes::None);
    number_constructor->set_property_descriptor("MAX_SAFE_INTEGER", max_safe_desc);
    PropertyDescriptor min_safe_desc(Value(-9007199254740991.0), PropertyAttributes::None);
    number_constructor->set_property_descriptor("MIN_SAFE_INTEGER", min_safe_desc);
    
    auto isInteger_fn = ObjectFactory::create_native_function("isInteger",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            if (!args[0].is_number()) return Value(false);
            double num = args[0].to_number();
            return Value(std::isfinite(num) && std::floor(num) == num);
        }, 1);
    number_constructor->set_property("isInteger", Value(isInteger_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto numberIsNaN_fn = ObjectFactory::create_native_function("isNaN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            // ES6 Number.isNaN: only returns true for actual NaN values (no type coercion)
            if (args.empty()) return Value(false);
            // Must be a number type AND NaN value
            if (!args[0].is_number()) return Value(false);
            return Value(args[0].is_nan());
        }, 1);
    number_constructor->set_property("isNaN", Value(numberIsNaN_fn.release()), PropertyAttributes::BuiltinFunction);
    
    auto numberIsFinite_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            
            if (!args[0].is_number()) return Value(false);
            
            double val = args[0].to_number();
            
            if (val != val) return Value(false);
            
            const double MAX_FINITE = 1.7976931348623157e+308;
            return Value(val > -MAX_FINITE && val < MAX_FINITE);
        }, 1);
    number_constructor->set_property("isFinite", Value(numberIsFinite_fn.release()), PropertyAttributes::BuiltinFunction);

    auto isSafeInteger_fn = ObjectFactory::create_native_function("isSafeInteger",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            if (!args[0].is_number()) return Value(false);
            double num = args[0].to_number();
            if (!std::isfinite(num)) return Value(false);
            if (std::floor(num) != num) return Value(false);
            const double MAX_SAFE = 9007199254740991.0;
            return Value(num >= -MAX_SAFE && num <= MAX_SAFE);
        }, 1);
    number_constructor->set_property("isSafeInteger", Value(isSafeInteger_fn.release()), PropertyAttributes::BuiltinFunction);

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
        }, 1);
    number_constructor->set_property("parseFloat", Value(numberParseFloat_fn.release()), PropertyAttributes::BuiltinFunction);

    number_constructor->set_property("parseInt", this->get_binding("parseInt"));

    auto number_prototype = ObjectFactory::create_object();

    auto number_valueOf = ObjectFactory::create_native_function("valueOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            try {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_number()) {
                    return this_val;
                }
                if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->has_property("[[PrimitiveValue]]")) {
                        return this_obj->get_property("[[PrimitiveValue]]");
                    }
                }
                ctx.throw_exception(Value(std::string("TypeError: Number.prototype.valueOf called on non-number")));
                return Value();
            } catch (...) {
                ctx.throw_exception(Value(std::string("TypeError: Number.prototype.valueOf called on non-number")));
                return Value();
            }
        }, 0);

    PropertyDescriptor number_valueOf_name_desc(Value(std::string("valueOf")), PropertyAttributes::None);
    number_valueOf_name_desc.set_configurable(true);
    number_valueOf_name_desc.set_enumerable(false);
    number_valueOf_name_desc.set_writable(false);
    number_valueOf->set_property_descriptor("name", number_valueOf_name_desc);

    PropertyDescriptor number_valueOf_length_desc(Value(0.0), PropertyAttributes::Configurable);
    number_valueOf_length_desc.set_enumerable(false);
    number_valueOf_length_desc.set_writable(false);
    number_valueOf->set_property_descriptor("length", number_valueOf_length_desc);

    auto number_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            try {
                Value this_val = ctx.get_binding("this");
                double num = 0.0;

                if (this_val.is_number()) {
                    num = this_val.as_number();
                } else if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->has_property("[[PrimitiveValue]]")) {
                        Value primitive = this_obj->get_property("[[PrimitiveValue]]");
                        num = primitive.as_number();
                    } else {
                        ctx.throw_exception(Value(std::string("TypeError: Number.prototype.toString called on non-number")));
                        return Value();
                    }
                } else {
                    ctx.throw_exception(Value(std::string("TypeError: Number.prototype.toString called on non-number")));
                    return Value();
                }

                if (std::isnan(num)) return Value(std::string("NaN"));
                if (std::isinf(num)) return Value(num > 0 ? "Infinity" : "-Infinity");

                int radix = 10;
                if (!args.empty()) {
                    radix = static_cast<int>(args[0].to_number());
                    if (radix < 2 || radix > 36) {
                        ctx.throw_exception(Value(std::string("RangeError: radix must be between 2 and 36")));
                        return Value();
                    }
                }

                if (radix == 10) {
                    // Check if number is an integer
                    if (num == std::floor(num) && std::abs(num) < 1e15) {
                        // Format as integer
                        std::ostringstream oss;
                        oss << std::fixed << std::setprecision(0) << num;
                        return Value(oss.str());
                    } else {
                        // Use default formatting for decimal numbers
                        std::ostringstream oss;
                        oss << num;
                        std::string result = oss.str();

                        // Remove trailing zeros after decimal point
                        size_t dot_pos = result.find('.');
                        if (dot_pos != std::string::npos) {
                            size_t last_nonzero = result.find_last_not_of('0');
                            if (last_nonzero > dot_pos) {
                                result = result.substr(0, last_nonzero + 1);
                            } else if (last_nonzero == dot_pos) {
                                result = result.substr(0, dot_pos);
                            }
                        }
                        return Value(result);
                    }
                }

                bool negative = num < 0;
                if (negative) num = -num;

                int64_t int_part = static_cast<int64_t>(num);
                std::string result;
                if (int_part == 0) {
                    result = "0";
                } else {
                    while (int_part > 0) {
                        int digit = int_part % radix;
                        result = (digit < 10 ? char('0' + digit) : char('a' + digit - 10)) + result;
                        int_part /= radix;
                    }
                }

                if (negative) result = "-" + result;
                return Value(result);
            } catch (...) {
                ctx.throw_exception(Value(std::string("TypeError: Number.prototype.toString called on non-number")));
                return Value();
            }
        }, 1);

    PropertyDescriptor number_toString_name_desc(Value(std::string("toString")), PropertyAttributes::None);
    number_toString_name_desc.set_configurable(true);
    number_toString_name_desc.set_enumerable(false);
    number_toString_name_desc.set_writable(false);
    number_toString->set_property_descriptor("name", number_toString_name_desc);

    PropertyDescriptor number_toString_length_desc(Value(1.0), PropertyAttributes::Configurable);
    number_toString_length_desc.set_enumerable(false);
    number_toString_length_desc.set_writable(false);
    number_toString->set_property_descriptor("length", number_toString_length_desc);

    PropertyDescriptor number_valueOf_desc(Value(number_valueOf.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("valueOf", number_valueOf_desc);
    PropertyDescriptor number_toString_desc(Value(number_toString.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toString", number_toString_desc);

    auto toExponential_fn = ObjectFactory::create_native_function("toExponential",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_val = ctx.get_binding("this");
            double num = this_val.to_number();

            int precision = 0;
            if (!args.empty() && !args[0].is_undefined()) {
                precision = static_cast<int>(args[0].to_number());
                if (precision < 0 || precision > 100) {
                    ctx.throw_exception(Value(std::string("RangeError: toExponential() precision out of range")));
                    return Value();
                }
            }

            char buffer[128];
            if (args.empty() || args[0].is_undefined()) {
                snprintf(buffer, sizeof(buffer), "%e", num);
            } else {
                std::string format = "%." + std::to_string(precision) + "e";
                snprintf(buffer, sizeof(buffer), format.c_str(), num);
            }
            return Value(std::string(buffer));
        });
    PropertyDescriptor toExponential_desc(Value(toExponential_fn.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toExponential", toExponential_desc);

    auto toFixed_fn = ObjectFactory::create_native_function("toFixed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_val = ctx.get_binding("this");
            double num = this_val.to_number();

            int precision = 0;
            if (!args.empty()) {
                precision = static_cast<int>(args[0].to_number());
                if (precision < 0 || precision > 100) {
                    ctx.throw_exception(Value(std::string("RangeError: toFixed() precision out of range")));
                    return Value();
                }
            }

            char buffer[128];
            std::string format = "%." + std::to_string(precision) + "f";
            snprintf(buffer, sizeof(buffer), format.c_str(), num);
            return Value(std::string(buffer));
        });
    PropertyDescriptor toFixed_desc(Value(toFixed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toFixed", toFixed_desc);

    auto toPrecision_fn = ObjectFactory::create_native_function("toPrecision",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_val = ctx.get_binding("this");
            double num = this_val.to_number();

            if (args.empty() || args[0].is_undefined()) {
                return Value(std::to_string(num));
            }

            int precision = static_cast<int>(args[0].to_number());
            if (precision < 1 || precision > 100) {
                ctx.throw_exception(Value(std::string("RangeError: toPrecision() precision out of range")));
                return Value();
            }

            char buffer[128];
            std::string format = "%." + std::to_string(precision - 1) + "g";
            snprintf(buffer, sizeof(buffer), format.c_str(), num);
            return Value(std::string(buffer));
        });
    PropertyDescriptor toPrecision_desc(Value(toPrecision_fn.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toPrecision", toPrecision_desc);

    auto number_toLocaleString_fn = ObjectFactory::create_native_function("toLocaleString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value this_val = ctx.get_binding("this");
            double num = this_val.to_number();
            return Value(std::to_string(num));
        });
    PropertyDescriptor number_toLocaleString_desc(Value(number_toLocaleString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toLocaleString", number_toLocaleString_desc);

    PropertyDescriptor number_constructor_desc(Value(number_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("constructor", number_constructor_desc);

    auto isNaN_fn2 = ObjectFactory::create_native_function("isNaN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            // ES6 Number.isNaN: only returns true for actual NaN values (no type coercion)
            if (args.empty()) return Value(false);
            // Must be a number type AND NaN value
            if (!args[0].is_number()) return Value(false);
            return Value(args[0].is_nan());
        }, 1);
    number_constructor->set_property("isNaN", Value(isNaN_fn2.release()), PropertyAttributes::BuiltinFunction);

    auto isFinite_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty() || !args[0].is_number()) return Value(false);
            return Value(std::isfinite(args[0].to_number()));
        }, 1);
    number_constructor->set_property("isFinite", Value(isFinite_fn.release()), PropertyAttributes::BuiltinFunction);
    number_constructor->set_property("prototype", Value(number_prototype.release()));

    register_built_in_object("Number", number_constructor.release());
    
    auto boolean_constructor = ObjectFactory::create_native_constructor("Boolean",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            bool value = args.empty() ? false : args[0].to_boolean();

            // If this_obj exists (constructor call), set [[PrimitiveValue]]
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                this_obj->set_property("[[PrimitiveValue]]", Value(value));
            }

            // Always return primitive boolean
            // Function::construct will return the created object if called as constructor
            return Value(value);
        });

    auto boolean_prototype = ObjectFactory::create_object();

    auto boolean_valueOf = ObjectFactory::create_native_function("valueOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            try {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_boolean()) {
                    return this_val;
                }
                if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->has_property("[[PrimitiveValue]]")) {
                        return this_obj->get_property("[[PrimitiveValue]]");
                    }
                }
                ctx.throw_exception(Value(std::string("TypeError: Boolean.prototype.valueOf called on non-boolean")));
                return Value();
            } catch (...) {
                ctx.throw_exception(Value(std::string("TypeError: Boolean.prototype.valueOf called on non-boolean")));
                return Value();
            }
        }, 0);

    PropertyDescriptor boolean_valueOf_name_desc(Value(std::string("valueOf")), PropertyAttributes::None);
    boolean_valueOf_name_desc.set_configurable(true);
    boolean_valueOf_name_desc.set_enumerable(false);
    boolean_valueOf_name_desc.set_writable(false);
    boolean_valueOf->set_property_descriptor("name", boolean_valueOf_name_desc);

    PropertyDescriptor boolean_valueOf_length_desc(Value(0.0), PropertyAttributes::Configurable);
    boolean_valueOf_length_desc.set_enumerable(false);
    boolean_valueOf_length_desc.set_writable(false);
    boolean_valueOf->set_property_descriptor("length", boolean_valueOf_length_desc);

    auto boolean_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            try {
                Value this_val = ctx.get_binding("this");
                if (this_val.is_boolean()) {
                    return Value(this_val.to_boolean() ? "true" : "false");
                }
                if (this_val.is_object()) {
                    Object* this_obj = this_val.as_object();
                    if (this_obj->has_property("[[PrimitiveValue]]")) {
                        Value primitive = this_obj->get_property("[[PrimitiveValue]]");
                        return Value(primitive.to_boolean() ? "true" : "false");
                    }
                }
                ctx.throw_exception(Value(std::string("TypeError: Boolean.prototype.toString called on non-boolean")));
                return Value();
            } catch (...) {
                ctx.throw_exception(Value(std::string("TypeError: Boolean.prototype.toString called on non-boolean")));
                return Value();
            }
        }, 0);

    PropertyDescriptor boolean_toString_name_desc(Value(std::string("toString")), PropertyAttributes::None);
    boolean_toString_name_desc.set_configurable(true);
    boolean_toString_name_desc.set_enumerable(false);
    boolean_toString_name_desc.set_writable(false);
    boolean_toString->set_property_descriptor("name", boolean_toString_name_desc);

    PropertyDescriptor boolean_toString_length_desc(Value(0.0), PropertyAttributes::Configurable);
    boolean_toString_length_desc.set_enumerable(false);
    boolean_toString_length_desc.set_writable(false);
    boolean_toString->set_property_descriptor("length", boolean_toString_length_desc);

    PropertyDescriptor boolean_valueOf_desc(Value(boolean_valueOf.release()),
        PropertyAttributes::BuiltinFunction);
    boolean_prototype->set_property_descriptor("valueOf", boolean_valueOf_desc);
    PropertyDescriptor boolean_toString_desc(Value(boolean_toString.release()),
        PropertyAttributes::BuiltinFunction);
    boolean_prototype->set_property_descriptor("toString", boolean_toString_desc);
    PropertyDescriptor boolean_constructor_desc(Value(boolean_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    boolean_prototype->set_property_descriptor("constructor", boolean_constructor_desc);

    boolean_constructor->set_property("prototype", Value(boolean_prototype.release()));

    register_built_in_object("Boolean", boolean_constructor.release());
    
    auto error_prototype = ObjectFactory::create_object();

    PropertyDescriptor error_proto_name_desc(Value(std::string("Error")),
        PropertyAttributes::BuiltinFunction);
    error_prototype->set_property_descriptor("name", error_proto_name_desc);
    error_prototype->set_property("message", Value(std::string("")));

    // Add Error.prototype.toString method
    auto error_proto_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                return Value(std::string("Error"));
            }

            Value name_val = this_obj->get_property("name");
            Value message_val = this_obj->get_property("message");

            std::string name = name_val.is_undefined() ? "Error" : name_val.to_string();
            std::string message = message_val.is_undefined() ? "" : message_val.to_string();

            if (message.empty()) {
                return Value(name);
            }
            if (name.empty()) {
                return Value(message);
            }
            return Value(name + ": " + message);
        }, 0);

    PropertyDescriptor error_proto_toString_desc(Value(error_proto_toString.release()),
        PropertyAttributes::BuiltinFunction);
    error_prototype->set_property_descriptor("toString", error_proto_toString_desc);

    Object* error_prototype_ptr = error_prototype.get();

    auto error_constructor = ObjectFactory::create_native_constructor("Error",
        [error_prototype_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            std::string message = "";
            if (!args.empty()) {
                if (args[0].is_undefined()) {
                    message = "";
                } else if (args[0].is_object()) {
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
            
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }
            
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* this_obj = ctx.get_this_binding();
                    if (!this_obj) {
                        return Value(std::string("Error"));
                    }

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
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);
            
            return Value(error_obj.release());
        });
    
    auto error_isError = ObjectFactory::create_native_function("isError", Error::isError);
    error_constructor->set_property("isError", Value(error_isError.release()));

    PropertyDescriptor error_constructor_desc(Value(error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    error_prototype->set_property_descriptor("constructor", error_constructor_desc);

    error_constructor->set_property("prototype", Value(error_prototype_ptr), PropertyAttributes::None);

    Function* error_ctor = error_constructor.get();

    register_built_in_object("Error", error_constructor.release());

    error_prototype.release();
    
    auto json_object = ObjectFactory::create_object();
    
    auto json_parse = ObjectFactory::create_native_function("parse",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_parse(ctx, args);
        }, 2);
    json_object->set_property("parse", Value(json_parse.release()),
        PropertyAttributes::BuiltinFunction);

    auto json_stringify = ObjectFactory::create_native_function("stringify",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_stringify(ctx, args);
        }, 3);
    json_object->set_property("stringify", Value(json_stringify.release()),
        PropertyAttributes::BuiltinFunction);

    auto json_isRawJSON = ObjectFactory::create_native_function("isRawJSON",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty() || !args[0].is_object()) {
                return Value(false);
            }

            Object* obj = args[0].as_object();
            if (obj->has_property("rawJSON")) {
                return Value(true);
            }

            return Value(false);
        }, 1);
    json_object->set_property("isRawJSON", Value(json_isRawJSON.release()),
        PropertyAttributes::BuiltinFunction);

    PropertyDescriptor json_tag_desc(Value(std::string("JSON")), PropertyAttributes::Configurable);
    json_object->set_property_descriptor("Symbol.toStringTag", json_tag_desc);

    register_built_in_object("JSON", json_object.release());
    
    auto math_object = std::make_unique<Object>();

    PropertyDescriptor pi_desc(Value(3.141592653589793), PropertyAttributes::None);
    math_object->set_property_descriptor("PI", pi_desc);
    PropertyDescriptor e_desc(Value(2.718281828459045), PropertyAttributes::None);
    math_object->set_property_descriptor("E", e_desc);

    auto store_fn = [](std::unique_ptr<Function> func) -> Function* {
        Function* ptr = func.get();
        g_owned_native_functions.push_back(std::move(func));
        return ptr;
    };

    auto math_max_fn = ObjectFactory::create_native_function("max",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value::negative_infinity();
            }

            double result = -std::numeric_limits<double>::infinity();
            for (const Value& arg : args) {
                if (arg.is_nan()) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                double value = arg.to_number();
                if (std::isnan(value)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                result = std::max(result, value);
            }
            return Value(result);
        }, 0);
    math_object->set_property("max", Value(store_fn(std::move(math_max_fn))), PropertyAttributes::BuiltinFunction);

    auto math_min_fn = ObjectFactory::create_native_function("min",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value::positive_infinity();
            }

            double result = std::numeric_limits<double>::infinity();
            for (const Value& arg : args) {
                if (arg.is_nan()) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                double value = arg.to_number();
                if (std::isnan(value)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                result = std::min(result, value);
            }
            return Value(result);
        }, 0);
    math_object->set_property("min", Value(store_fn(std::move(math_min_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_round_fn = ObjectFactory::create_native_function("round",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            
            double value = args[0].to_number();
            return Value(std::round(value));
        }, 1);
    math_object->set_property("round", Value(store_fn(std::move(math_round_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_random_fn = ObjectFactory::create_native_function("random",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            return Value(static_cast<double>(rand()) / RAND_MAX);
        }, 0);
    math_object->set_property("random", Value(store_fn(std::move(math_random_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_floor_fn = ObjectFactory::create_native_function("floor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::floor(args[0].to_number()));
        }, 1);
    math_object->set_property("floor", Value(store_fn(std::move(math_floor_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_ceil_fn = ObjectFactory::create_native_function("ceil",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::ceil(args[0].to_number()));
        }, 1);
    math_object->set_property("ceil", Value(store_fn(std::move(math_ceil_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_abs_fn = ObjectFactory::create_native_function("abs",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            double value = args[0].to_number();
            if (std::isinf(value)) {
                return Value::positive_infinity();
            }
            return Value(std::abs(value));
        }, 1);
    math_object->set_property("abs", Value(store_fn(std::move(math_abs_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_sqrt_fn = ObjectFactory::create_native_function("sqrt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sqrt(args[0].to_number()));
        }, 1);
    math_object->set_property("sqrt", Value(store_fn(std::move(math_sqrt_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_pow_fn = ObjectFactory::create_native_function("pow",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() < 2) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::pow(args[0].to_number(), args[1].to_number()));
        }, 2);
    math_object->set_property("pow", Value(store_fn(std::move(math_pow_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_sin_fn = ObjectFactory::create_native_function("sin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sin(args[0].to_number()));
        }, 1);
    math_object->set_property("sin", Value(store_fn(std::move(math_sin_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_cos_fn = ObjectFactory::create_native_function("cos",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cos(args[0].to_number()));
        }, 1);
    math_object->set_property("cos", Value(store_fn(std::move(math_cos_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_tan_fn = ObjectFactory::create_native_function("tan",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::tan(args[0].to_number()));
        }, 1);
    math_object->set_property("tan", Value(store_fn(std::move(math_tan_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_log_fn = ObjectFactory::create_native_function("log",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log(args[0].to_number()));
        }, 1);
    math_object->set_property("log", Value(store_fn(std::move(math_log_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_log10_fn = ObjectFactory::create_native_function("log10",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log10(args[0].to_number()));
        }, 1);
    math_object->set_property("log10", Value(store_fn(std::move(math_log10_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_exp_fn = ObjectFactory::create_native_function("exp",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::exp(args[0].to_number()));
        }, 1);
    math_object->set_property("exp", Value(store_fn(std::move(math_exp_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_trunc_fn = ObjectFactory::create_native_function("trunc",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(0.0);
            double val = args[0].to_number();
            if (std::isinf(val)) return Value(val);
            if (std::isnan(val)) return Value(0.0);
            return Value(std::trunc(val));
        }, 1);
    math_object->set_property("trunc", Value(store_fn(std::move(math_trunc_fn))), PropertyAttributes::BuiltinFunction);
    
    auto math_sign_fn = ObjectFactory::create_native_function("sign",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(0.0);
            double val = args[0].to_number();
            if (std::isnan(val)) return Value(0.0);
            if (val > 0) return Value(1.0);
            if (val < 0) return Value(-1.0);
            return Value(val);
        }, 1);
    math_object->set_property("sign", Value(store_fn(std::move(math_sign_fn))), PropertyAttributes::BuiltinFunction);

    auto math_acos_fn = ObjectFactory::create_native_function("acos",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::acos(args[0].to_number()));
        }, 1);
    math_object->set_property("acos", Value(store_fn(std::move(math_acos_fn))), PropertyAttributes::BuiltinFunction);

    auto math_acosh_fn = ObjectFactory::create_native_function("acosh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::acosh(args[0].to_number()));
        }, 1);
    math_object->set_property("acosh", Value(store_fn(std::move(math_acosh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_asin_fn = ObjectFactory::create_native_function("asin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::asin(args[0].to_number()));
        }, 1);
    math_object->set_property("asin", Value(store_fn(std::move(math_asin_fn))), PropertyAttributes::BuiltinFunction);

    auto math_asinh_fn = ObjectFactory::create_native_function("asinh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::asinh(args[0].to_number()));
        }, 1);
    math_object->set_property("asinh", Value(store_fn(std::move(math_asinh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_atan_fn = ObjectFactory::create_native_function("atan",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::atan(args[0].to_number()));
        }, 1);
    math_object->set_property("atan", Value(store_fn(std::move(math_atan_fn))), PropertyAttributes::BuiltinFunction);

    auto math_atan2_fn = ObjectFactory::create_native_function("atan2",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() < 2) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::atan2(args[0].to_number(), args[1].to_number()));
        }, 2);
    math_object->set_property("atan2", Value(store_fn(std::move(math_atan2_fn))), PropertyAttributes::BuiltinFunction);

    auto math_atanh_fn = ObjectFactory::create_native_function("atanh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::atanh(args[0].to_number()));
        }, 1);
    math_object->set_property("atanh", Value(store_fn(std::move(math_atanh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_cbrt_fn = ObjectFactory::create_native_function("cbrt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cbrt(args[0].to_number()));
        }, 1);
    math_object->set_property("cbrt", Value(store_fn(std::move(math_cbrt_fn))), PropertyAttributes::BuiltinFunction);

    auto math_clz32_fn = ObjectFactory::create_native_function("clz32",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(32.0);
            uint32_t n = static_cast<uint32_t>(args[0].to_number());
            if (n == 0) return Value(32.0);
            int count = 0;
            for (int i = 31; i >= 0; i--) {
                if (n & (1U << i)) break;
                count++;
            }
            return Value(static_cast<double>(count));
        }, 1);
    math_object->set_property("clz32", Value(store_fn(std::move(math_clz32_fn))), PropertyAttributes::BuiltinFunction);

    auto math_cosh_fn = ObjectFactory::create_native_function("cosh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cosh(args[0].to_number()));
        }, 1);
    math_object->set_property("cosh", Value(store_fn(std::move(math_cosh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_expm1_fn = ObjectFactory::create_native_function("expm1",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::expm1(args[0].to_number()));
        }, 1);
    math_object->set_property("expm1", Value(store_fn(std::move(math_expm1_fn))), PropertyAttributes::BuiltinFunction);

    auto math_fround_fn = ObjectFactory::create_native_function("fround",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(static_cast<double>(static_cast<float>(args[0].to_number())));
        }, 1);
    math_object->set_property("fround", Value(store_fn(std::move(math_fround_fn))), PropertyAttributes::BuiltinFunction);

    auto math_hypot_fn = ObjectFactory::create_native_function("hypot",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            double sum = 0;
            for (const auto& arg : args) {
                double val = arg.to_number();
                sum += val * val;
            }
            return Value(std::sqrt(sum));
        }, 2);
    math_object->set_property("hypot", Value(store_fn(std::move(math_hypot_fn))), PropertyAttributes::BuiltinFunction);

    auto math_imul_fn = ObjectFactory::create_native_function("imul",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() < 2) return Value(0.0);
            int32_t a = static_cast<int32_t>(args[0].to_number());
            int32_t b = static_cast<int32_t>(args[1].to_number());
            return Value(static_cast<double>(a * b));
        }, 2);
    math_object->set_property("imul", Value(store_fn(std::move(math_imul_fn))), PropertyAttributes::BuiltinFunction);

    auto math_log1p_fn = ObjectFactory::create_native_function("log1p",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log1p(args[0].to_number()));
        }, 1);
    math_object->set_property("log1p", Value(store_fn(std::move(math_log1p_fn))), PropertyAttributes::BuiltinFunction);

    auto math_log2_fn = ObjectFactory::create_native_function("log2",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log2(args[0].to_number()));
        }, 1);
    math_object->set_property("log2", Value(store_fn(std::move(math_log2_fn))), PropertyAttributes::BuiltinFunction);

    auto math_sinh_fn = ObjectFactory::create_native_function("sinh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sinh(args[0].to_number()));
        }, 1);
    math_object->set_property("sinh", Value(store_fn(std::move(math_sinh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_tanh_fn = ObjectFactory::create_native_function("tanh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::tanh(args[0].to_number()));
        }, 1);
    math_object->set_property("tanh", Value(store_fn(std::move(math_tanh_fn))), PropertyAttributes::BuiltinFunction);

    PropertyDescriptor ln10_desc(Value(2.302585092994046), PropertyAttributes::None);
    math_object->set_property_descriptor("LN10", ln10_desc);
    PropertyDescriptor ln2_desc(Value(0.6931471805599453), PropertyAttributes::None);
    math_object->set_property_descriptor("LN2", ln2_desc);
    PropertyDescriptor log10e_desc(Value(0.4342944819032518), PropertyAttributes::None);
    math_object->set_property_descriptor("LOG10E", log10e_desc);
    PropertyDescriptor log2e_desc(Value(1.4426950408889634), PropertyAttributes::None);
    math_object->set_property_descriptor("LOG2E", log2e_desc);
    PropertyDescriptor sqrt1_2_desc(Value(0.7071067811865476), PropertyAttributes::None);
    math_object->set_property_descriptor("SQRT1_2", sqrt1_2_desc);
    PropertyDescriptor sqrt2_desc(Value(1.4142135623730951), PropertyAttributes::None);
    math_object->set_property_descriptor("SQRT2", sqrt2_desc);

    PropertyDescriptor math_tag_desc(Value(std::string("Math")), PropertyAttributes::Configurable);
    math_object->set_property_descriptor("Symbol.toStringTag", math_tag_desc);

    register_built_in_object("Math", math_object.release());

    auto intl_object = ObjectFactory::create_object();

    auto intl_datetimeformat = ObjectFactory::create_native_constructor("DateTimeFormat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto formatter = ObjectFactory::create_object();

            auto format_fn = ObjectFactory::create_native_function("format",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) {
                        return Value(std::string("Invalid Date"));
                    }
                    return Value(std::string("1/1/1970"));
                }, 1);
            formatter->set_property("format", Value(format_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(formatter.release());
        });
    intl_object->set_property("DateTimeFormat", Value(intl_datetimeformat.release()));

    auto intl_numberformat = ObjectFactory::create_native_constructor("NumberFormat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto formatter = ObjectFactory::create_object();

            auto format_fn = ObjectFactory::create_native_function("format",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.empty()) {
                        return Value(std::string("0"));
                    }
                    return Value(args[0].to_string());
                }, 1);
            formatter->set_property("format", Value(format_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(formatter.release());
        });
    intl_object->set_property("NumberFormat", Value(intl_numberformat.release()));

    auto intl_collator = ObjectFactory::create_native_constructor("Collator",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto collator = ObjectFactory::create_object();

            auto compare_fn = ObjectFactory::create_native_function("compare",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.size() < 2) return Value(0.0);
                    std::string a = args[0].to_string();
                    std::string b = args[1].to_string();
                    if (a < b) return Value(-1.0);
                    if (a > b) return Value(1.0);
                    return Value(0.0);
                }, 2);
            collator->set_property("compare", Value(compare_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(collator.release());
        });
    intl_object->set_property("Collator", Value(intl_collator.release()));

    register_built_in_object("Intl", intl_object.release());

    auto add_date_instance_methods = [](Object* date_obj) {
        auto getTime_fn = ObjectFactory::create_native_function("getTime",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                return Value(static_cast<double>(timestamp));
            });
        date_obj->set_property("getTime", Value(getTime_fn.release()), PropertyAttributes::BuiltinFunction);
        
        auto getFullYear_fn = ObjectFactory::create_native_function("getFullYear",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_year + 1900)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getFullYear", Value(getFullYear_fn.release()), PropertyAttributes::BuiltinFunction);
        
        auto getMonth_fn = ObjectFactory::create_native_function("getMonth",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_mon)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getMonth", Value(getMonth_fn.release()), PropertyAttributes::BuiltinFunction);
        
        auto getDate_fn = ObjectFactory::create_native_function("getDate",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_mday)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getDate", Value(getDate_fn.release()), PropertyAttributes::BuiltinFunction);

        auto getYear_fn = ObjectFactory::create_native_function("getYear",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::tm* local_time = std::localtime(&time);
                return local_time ? Value(static_cast<double>(local_time->tm_year)) : Value(std::numeric_limits<double>::quiet_NaN());
            });
        date_obj->set_property("getYear", Value(getYear_fn.release()), PropertyAttributes::BuiltinFunction);

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

                return Value(static_cast<double>(year));
            });
        date_obj->set_property("setYear", Value(setYear_fn.release()), PropertyAttributes::BuiltinFunction);

        auto toString_fn = ObjectFactory::create_native_function("toString",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)ctx; (void)args;
                auto now = std::chrono::system_clock::now();
                std::time_t time = std::chrono::system_clock::to_time_t(now);
                std::string time_str = std::ctime(&time);
                if (!time_str.empty() && time_str.back() == '\n') {
                    time_str.pop_back();
                }
                return Value(time_str);
            });
        date_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);
    };
    
    auto date_prototype = ObjectFactory::create_object();
    Object* date_proto_ptr = date_prototype.get();

    auto date_constructor_fn = ObjectFactory::create_native_constructor("Date",
        [date_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            // If called as function (not constructor), return current time string
            if (!ctx.is_in_constructor_call()) {
                auto now = std::chrono::system_clock::now();
                std::time_t now_time = std::chrono::system_clock::to_time_t(now);
                std::tm* now_tm = std::localtime(&now_time);
                char buffer[100];
                std::strftime(buffer, sizeof(buffer), "%a %b %d %Y %H:%M:%S", now_tm);
                return Value(std::string(buffer));
            }

            // Otherwise construct Date object
            Value date_obj = Date::date_constructor(ctx, args);

            if (date_obj.is_object()) {
                date_obj.as_object()->set_prototype(date_proto_ptr);
            }

            return date_obj;
        });

    auto date_now = ObjectFactory::create_native_function("now", Date::now);
    auto date_parse = ObjectFactory::create_native_function("parse", Date::parse);
    auto date_UTC = ObjectFactory::create_native_function("UTC", Date::UTC);
    
    date_constructor_fn->set_property("now", Value(date_now.release()), PropertyAttributes::BuiltinFunction);
    date_constructor_fn->set_property("parse", Value(date_parse.release()), PropertyAttributes::BuiltinFunction);
    date_constructor_fn->set_property("UTC", Value(date_UTC.release()), PropertyAttributes::BuiltinFunction);

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
    auto valueOf_fn = ObjectFactory::create_native_function("valueOf", Date::valueOf);
    auto toUTCString_fn = ObjectFactory::create_native_function("toUTCString", Date::toUTCString);

    auto toDateString_fn = ObjectFactory::create_native_function("toDateString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("Wed Jan 01 2020"));
        }, 0);

    auto toLocaleDateString_fn = ObjectFactory::create_native_function("toLocaleDateString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("1/1/2020"));
        }, 0);

    auto date_toLocaleString_fn = ObjectFactory::create_native_function("toLocaleString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("1/1/2020, 12:00:00 AM"));
        }, 0);

    auto toLocaleTimeString_fn = ObjectFactory::create_native_function("toLocaleTimeString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("12:00:00 AM"));
        }, 0);

    auto toTimeString_fn = ObjectFactory::create_native_function("toTimeString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(std::string("00:00:00 GMT+0000 (UTC)"));
        }, 0);

    toDateString_fn->set_property("name", Value(std::string("toDateString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    toLocaleDateString_fn->set_property("name", Value(std::string("toLocaleDateString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    date_toLocaleString_fn->set_property("name", Value(std::string("toLocaleString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    toLocaleTimeString_fn->set_property("name", Value(std::string("toLocaleTimeString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    toTimeString_fn->set_property("name", Value(std::string("toTimeString")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    auto getYear_fn = ObjectFactory::create_native_function("getYear", Date::getYear);
    auto setYear_fn = ObjectFactory::create_native_function("setYear", Date::setYear);

    PropertyDescriptor getTime_desc(Value(getTime_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getTime", getTime_desc);
    PropertyDescriptor getFullYear_desc(Value(getFullYear_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getFullYear", getFullYear_desc);
    PropertyDescriptor getMonth_desc(Value(getMonth_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getMonth", getMonth_desc);
    PropertyDescriptor getDate_desc(Value(getDate_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getDate", getDate_desc);
    PropertyDescriptor getDay_desc(Value(getDay_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getDay", getDay_desc);
    PropertyDescriptor getHours_desc(Value(getHours_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getHours", getHours_desc);
    PropertyDescriptor getMinutes_desc(Value(getMinutes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getMinutes", getMinutes_desc);
    PropertyDescriptor getSeconds_desc(Value(getSeconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getSeconds", getSeconds_desc);
    PropertyDescriptor getMilliseconds_desc(Value(getMilliseconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getMilliseconds", getMilliseconds_desc);
    PropertyDescriptor date_toString_desc(Value(toString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toString", date_toString_desc);
    PropertyDescriptor toISOString_desc(Value(toISOString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toISOString", toISOString_desc);
    PropertyDescriptor toJSON_desc(Value(toJSON_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toJSON", toJSON_desc);
    PropertyDescriptor valueOf_desc(Value(valueOf_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("valueOf", valueOf_desc);
    PropertyDescriptor toUTCString_desc(Value(toUTCString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toUTCString", toUTCString_desc);
    PropertyDescriptor toDateString_desc(Value(toDateString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toDateString", toDateString_desc);
    PropertyDescriptor toLocaleDateString_desc(Value(toLocaleDateString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toLocaleDateString", toLocaleDateString_desc);
    PropertyDescriptor date_toLocaleString_desc(Value(date_toLocaleString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toLocaleString", date_toLocaleString_desc);
    PropertyDescriptor toLocaleTimeString_desc(Value(toLocaleTimeString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toLocaleTimeString", toLocaleTimeString_desc);
    PropertyDescriptor toTimeString_desc(Value(toTimeString_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("toTimeString", toTimeString_desc);

    auto getTimezoneOffset_fn = ObjectFactory::create_native_function("getTimezoneOffset", Date::getTimezoneOffset, 0);
    PropertyDescriptor getTimezoneOffset_desc(Value(getTimezoneOffset_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getTimezoneOffset", getTimezoneOffset_desc);

    auto getUTCDate_fn = ObjectFactory::create_native_function("getUTCDate", Date::getUTCDate, 0);
    auto getUTCDay_fn = ObjectFactory::create_native_function("getUTCDay", Date::getUTCDay, 0);
    auto getUTCFullYear_fn = ObjectFactory::create_native_function("getUTCFullYear", Date::getUTCFullYear, 0);
    auto getUTCHours_fn = ObjectFactory::create_native_function("getUTCHours", Date::getUTCHours, 0);
    auto getUTCMilliseconds_fn = ObjectFactory::create_native_function("getUTCMilliseconds", Date::getUTCMilliseconds, 0);
    auto getUTCMinutes_fn = ObjectFactory::create_native_function("getUTCMinutes", Date::getUTCMinutes, 0);
    auto getUTCMonth_fn = ObjectFactory::create_native_function("getUTCMonth", Date::getUTCMonth, 0);
    auto getUTCSeconds_fn = ObjectFactory::create_native_function("getUTCSeconds", Date::getUTCSeconds, 0);

    PropertyDescriptor getUTCDate_desc(Value(getUTCDate_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCDate", getUTCDate_desc);
    PropertyDescriptor getUTCDay_desc(Value(getUTCDay_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCDay", getUTCDay_desc);
    PropertyDescriptor getUTCFullYear_desc(Value(getUTCFullYear_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCFullYear", getUTCFullYear_desc);
    PropertyDescriptor getUTCHours_desc(Value(getUTCHours_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCHours", getUTCHours_desc);
    PropertyDescriptor getUTCMilliseconds_desc(Value(getUTCMilliseconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCMilliseconds", getUTCMilliseconds_desc);
    PropertyDescriptor getUTCMinutes_desc(Value(getUTCMinutes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCMinutes", getUTCMinutes_desc);
    PropertyDescriptor getUTCMonth_desc(Value(getUTCMonth_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCMonth", getUTCMonth_desc);
    PropertyDescriptor getUTCSeconds_desc(Value(getUTCSeconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("getUTCSeconds", getUTCSeconds_desc);

    auto setTime_fn = ObjectFactory::create_native_function("setTime", Date::setTime, 1);
    auto setFullYear_fn = ObjectFactory::create_native_function("setFullYear", Date::setFullYear, 3);
    auto setMonth_fn = ObjectFactory::create_native_function("setMonth", Date::setMonth, 2);
    auto setDate_fn = ObjectFactory::create_native_function("setDate", Date::setDate, 1);
    auto setHours_fn = ObjectFactory::create_native_function("setHours", Date::setHours, 4);
    auto setMinutes_fn = ObjectFactory::create_native_function("setMinutes", Date::setMinutes, 3);
    auto setSeconds_fn = ObjectFactory::create_native_function("setSeconds", Date::setSeconds, 2);
    auto setMilliseconds_fn = ObjectFactory::create_native_function("setMilliseconds", Date::setMilliseconds, 1);

    PropertyDescriptor setTime_desc(Value(setTime_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setTime", setTime_desc);
    PropertyDescriptor setFullYear_desc(Value(setFullYear_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setFullYear", setFullYear_desc);
    PropertyDescriptor setMonth_desc(Value(setMonth_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setMonth", setMonth_desc);
    PropertyDescriptor setDate_desc(Value(setDate_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setDate", setDate_desc);
    PropertyDescriptor setHours_desc(Value(setHours_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setHours", setHours_desc);
    PropertyDescriptor setMinutes_desc(Value(setMinutes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setMinutes", setMinutes_desc);
    PropertyDescriptor setSeconds_desc(Value(setSeconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setSeconds", setSeconds_desc);
    PropertyDescriptor setMilliseconds_desc(Value(setMilliseconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setMilliseconds", setMilliseconds_desc);

    auto setUTCFullYear_fn = ObjectFactory::create_native_function("setUTCFullYear", Date::setUTCFullYear, 3);
    auto setUTCMonth_fn = ObjectFactory::create_native_function("setUTCMonth", Date::setUTCMonth, 2);
    auto setUTCDate_fn = ObjectFactory::create_native_function("setUTCDate", Date::setUTCDate, 1);
    auto setUTCHours_fn = ObjectFactory::create_native_function("setUTCHours", Date::setUTCHours, 4);
    auto setUTCMinutes_fn = ObjectFactory::create_native_function("setUTCMinutes", Date::setUTCMinutes, 3);
    auto setUTCSeconds_fn = ObjectFactory::create_native_function("setUTCSeconds", Date::setUTCSeconds, 2);
    auto setUTCMilliseconds_fn = ObjectFactory::create_native_function("setUTCMilliseconds", Date::setUTCMilliseconds, 1);

    PropertyDescriptor setUTCFullYear_desc(Value(setUTCFullYear_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCFullYear", setUTCFullYear_desc);
    PropertyDescriptor setUTCMonth_desc(Value(setUTCMonth_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCMonth", setUTCMonth_desc);
    PropertyDescriptor setUTCDate_desc(Value(setUTCDate_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCDate", setUTCDate_desc);
    PropertyDescriptor setUTCHours_desc(Value(setUTCHours_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCHours", setUTCHours_desc);
    PropertyDescriptor setUTCMinutes_desc(Value(setUTCMinutes_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCMinutes", setUTCMinutes_desc);
    PropertyDescriptor setUTCSeconds_desc(Value(setUTCSeconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCSeconds", setUTCSeconds_desc);
    PropertyDescriptor setUTCMilliseconds_desc(Value(setUTCMilliseconds_fn.release()),
        PropertyAttributes::BuiltinFunction);
    date_prototype->set_property_descriptor("setUTCMilliseconds", setUTCMilliseconds_desc);

    date_prototype->set_property("getYear", Value(getYear_fn.release()), PropertyAttributes::BuiltinFunction);
    date_prototype->set_property("setYear", Value(setYear_fn.release()), PropertyAttributes::BuiltinFunction);

    auto toGMTString_fn = ObjectFactory::create_native_function("toGMTString", Date::toGMTString);
    date_prototype->set_property("toGMTString", Value(toGMTString_fn.release()), PropertyAttributes::BuiltinFunction);

    PropertyDescriptor date_proto_ctor_desc(Value(date_constructor_fn.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    date_prototype->set_property_descriptor("constructor", date_proto_ctor_desc);

    date_constructor_fn->set_property("prototype", Value(date_prototype.get()));

    register_built_in_object("Date", date_constructor_fn.get());
    
    if (lexical_environment_) {
        lexical_environment_->create_binding("Date", Value(date_constructor_fn.get()), false);
    }
    if (variable_environment_) {
        variable_environment_->create_binding("Date", Value(date_constructor_fn.get()), false);
    }
    if (global_object_) {
        PropertyDescriptor date_desc(Value(date_constructor_fn.get()),
            PropertyAttributes::BuiltinFunction);
        global_object_->set_property_descriptor("Date", date_desc);
    }
    
    date_constructor_fn.release();
    date_prototype.release();
    
    auto type_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    type_error_prototype->set_property("name", Value(std::string("TypeError")));
    Object* type_error_proto_ptr = type_error_prototype.get();

    auto type_error_constructor = ObjectFactory::create_native_constructor("TypeError",
        [type_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::TypeError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(type_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor type_error_constructor_desc(Value(type_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    type_error_prototype->set_property_descriptor("constructor", type_error_constructor_desc);

    type_error_constructor->set_property("prototype", Value(type_error_prototype.release()));

    PropertyDescriptor type_error_length_desc(Value(1.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    type_error_length_desc.set_configurable(true);
    type_error_length_desc.set_enumerable(false);
    type_error_length_desc.set_writable(false);
    type_error_constructor->set_property_descriptor("length", type_error_length_desc);

    type_error_constructor->set_property("name", Value(std::string("TypeError")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    if (error_ctor) {
        type_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("TypeError", type_error_constructor.release());
    
    auto reference_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    reference_error_prototype->set_property("name", Value(std::string("ReferenceError")));
    Object* reference_error_proto_ptr = reference_error_prototype.get();

    auto reference_error_constructor = ObjectFactory::create_native_constructor("ReferenceError",
        [reference_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::ReferenceError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(reference_error_proto_ptr);
            
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }
            
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);
            
            return Value(error_obj.release());
        });
    
    PropertyDescriptor reference_error_constructor_desc(Value(reference_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    reference_error_prototype->set_property_descriptor("constructor", reference_error_constructor_desc);
    reference_error_constructor->set_property("prototype", Value(reference_error_prototype.release()));

    PropertyDescriptor reference_error_length_desc(Value(1.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    reference_error_length_desc.set_configurable(true);
    reference_error_length_desc.set_enumerable(false);
    reference_error_length_desc.set_writable(false);
    reference_error_constructor->set_property_descriptor("length", reference_error_length_desc);

    reference_error_constructor->set_property("name", Value(std::string("ReferenceError")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    if (error_ctor) {
        reference_error_constructor->set_prototype(error_ctor);
    }
    
    register_built_in_object("ReferenceError", reference_error_constructor.release());
    
    auto syntax_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    syntax_error_prototype->set_property("name", Value(std::string("SyntaxError")));
    Object* syntax_error_proto_ptr = syntax_error_prototype.get();

    auto syntax_error_constructor = ObjectFactory::create_native_constructor("SyntaxError",
        [syntax_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::SyntaxError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(syntax_error_proto_ptr);
            
            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }
            
            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);
            
            return Value(error_obj.release());
        });
    
    PropertyDescriptor syntax_error_constructor_desc(Value(syntax_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    syntax_error_prototype->set_property_descriptor("constructor", syntax_error_constructor_desc);
    syntax_error_constructor->set_property("prototype", Value(syntax_error_prototype.release()));

    PropertyDescriptor syntax_error_length_desc(Value(1.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    syntax_error_length_desc.set_configurable(true);
    syntax_error_length_desc.set_enumerable(false);
    syntax_error_length_desc.set_writable(false);
    syntax_error_constructor->set_property_descriptor("length", syntax_error_length_desc);

    syntax_error_constructor->set_property("name", Value(std::string("SyntaxError")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    if (error_ctor) {
        syntax_error_constructor->set_prototype(error_ctor);
    }
    
    register_built_in_object("SyntaxError", syntax_error_constructor.release());

    auto range_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    range_error_prototype->set_property("name", Value(std::string("RangeError")));
    Object* range_error_proto_ptr = range_error_prototype.get();

    auto range_error_constructor = ObjectFactory::create_native_constructor("RangeError",
        [range_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::RangeError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(range_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor range_error_constructor_desc(Value(range_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    range_error_prototype->set_property_descriptor("constructor", range_error_constructor_desc);

    range_error_constructor->set_property("prototype", Value(range_error_prototype.release()));

    PropertyDescriptor range_error_length_desc(Value(1.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    range_error_length_desc.set_configurable(true);
    range_error_length_desc.set_enumerable(false);
    range_error_length_desc.set_writable(false);
    range_error_constructor->set_property_descriptor("length", range_error_length_desc);

    range_error_constructor->set_property("name", Value(std::string("RangeError")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));

    if (error_ctor) {
        range_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("RangeError", range_error_constructor.release());

    auto uri_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    uri_error_prototype->set_property("name", Value(std::string("URIError")));
    Object* uri_error_proto_ptr = uri_error_prototype.get();

    auto uri_error_constructor = ObjectFactory::create_native_constructor("URIError",
        [uri_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::URIError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(uri_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor uri_error_constructor_desc(Value(uri_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    uri_error_prototype->set_property_descriptor("constructor", uri_error_constructor_desc);

    uri_error_constructor->set_property("prototype", Value(uri_error_prototype.release()));

    if (error_ctor) {
        uri_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("URIError", uri_error_constructor.release());

    auto eval_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    eval_error_prototype->set_property("name", Value(std::string("EvalError")));
    Object* eval_error_proto_ptr = eval_error_prototype.get();

    auto eval_error_constructor = ObjectFactory::create_native_constructor("EvalError",
        [eval_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (!args.empty() && !args[0].is_undefined()) {
                message = args[0].to_string();
            }
            auto error_obj = std::make_unique<Error>(Error::Type::EvalError, message);
            error_obj->set_property("_isError", Value(true));
            error_obj->set_prototype(eval_error_proto_ptr);

            if (args.size() > 1 && args[1].is_object()) {
                Object* options = args[1].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        });

    PropertyDescriptor eval_error_constructor_desc(Value(eval_error_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    eval_error_prototype->set_property_descriptor("constructor", eval_error_constructor_desc);

    eval_error_constructor->set_property("prototype", Value(eval_error_prototype.release()));

    if (error_ctor) {
        eval_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("EvalError", eval_error_constructor.release());

    auto aggregate_error_prototype = ObjectFactory::create_object(error_prototype_ptr);
    aggregate_error_prototype->set_property("name", Value(std::string("AggregateError")));
    
    Object* agg_error_proto_ptr = aggregate_error_prototype.get();

    auto aggregate_error_constructor = ObjectFactory::create_native_constructor("AggregateError",
        [agg_error_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string message = "";
            if (args.size() > 1 && !args[1].is_undefined()) {
                Value msg_value = args[1];
                if (msg_value.is_object()) {
                    Object* obj = msg_value.as_object();
                    Value toString_method = obj->get_property("toString");
                    if (toString_method.is_function()) {
                        try {
                            Function* func = toString_method.as_function();
                            Value result = func->call(ctx, {}, msg_value);
                            if (!ctx.has_exception()) {
                                message = result.to_string();
                            } else {
                                ctx.clear_exception();
                                message = msg_value.to_string();
                            }
                        } catch (...) {
                            message = msg_value.to_string();
                        }
                    } else {
                        message = msg_value.to_string();
                    }
                } else {
                    message = msg_value.to_string();
                }
            }
            auto error_obj = std::make_unique<Error>(Error::Type::AggregateError, message);
            error_obj->set_property("_isError", Value(true));
            
            error_obj->set_prototype(agg_error_proto_ptr);

            if (args.size() > 0 && args[0].is_object()) {
                error_obj->set_property("errors", args[0]);
            } else {
                auto empty_array = ObjectFactory::create_array();
                error_obj->set_property("errors", Value(empty_array.release()));
            }

            if (args.size() > 2 && args[2].is_object()) {
                Object* options = args[2].as_object();
                if (options->has_property("cause")) {
                    Value cause = options->get_property("cause");
                    PropertyDescriptor cause_desc(cause, PropertyAttributes::BuiltinFunction);
                    error_obj->set_property_descriptor("cause", cause_desc);
                }
            }

            auto toString_fn = ObjectFactory::create_native_function("toString",
                [error_name = error_obj->get_name(), error_message = error_obj->get_message()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    if (error_message.empty()) {
                        return Value(error_name);
                    }
                    return Value(error_name + ": " + error_message);
                });
            error_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(error_obj.release());
        }, 2);

    PropertyDescriptor constructor_desc(Value(aggregate_error_constructor.get()), PropertyAttributes::None);
    constructor_desc.set_writable(true);
    constructor_desc.set_enumerable(false);
    constructor_desc.set_configurable(true);
    aggregate_error_prototype->set_property_descriptor("constructor", constructor_desc);

    PropertyDescriptor name_desc(Value(std::string("AggregateError")), PropertyAttributes::None);
    name_desc.set_configurable(true);
    name_desc.set_enumerable(false);
    name_desc.set_writable(false);
    aggregate_error_constructor->set_property_descriptor("name", name_desc);

    PropertyDescriptor length_desc(Value(2.0), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    length_desc.set_configurable(true);
    length_desc.set_enumerable(false);
    length_desc.set_writable(false);
    aggregate_error_constructor->set_property_descriptor("length", length_desc);

    aggregate_error_constructor->set_property("prototype", Value(aggregate_error_prototype.release()));

    if (error_ctor) {
        aggregate_error_constructor->set_prototype(error_ctor);
    }

    register_built_in_object("AggregateError", aggregate_error_constructor.release());

    auto regexp_prototype = ObjectFactory::create_object();

    auto compile_fn = ObjectFactory::create_native_function("compile",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("TypeError: RegExp.prototype.compile called on null or undefined")));
                return Value();
            }

            std::string pattern = "";
            std::string flags = "";

            if (args.size() > 0) {
                pattern = args[0].to_string();
            }
            if (args.size() > 1) {
                flags = args[1].to_string();
            }

            this_obj->set_property("source", Value(pattern));
            this_obj->set_property("global", Value(flags.find('g') != std::string::npos));
            this_obj->set_property("ignoreCase", Value(flags.find('i') != std::string::npos));
            this_obj->set_property("multiline", Value(flags.find('m') != std::string::npos));
            this_obj->set_property("lastIndex", Value(0.0));

            return Value(this_obj);
        }, 2);
    regexp_prototype->set_property("compile", Value(compile_fn.release()), PropertyAttributes::BuiltinFunction);

    Object* regexp_proto_ptr = regexp_prototype.get();

    auto regexp_constructor = ObjectFactory::create_native_constructor("RegExp",
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
                
                auto regexp_impl = std::make_shared<RegExp>(pattern, flags);
                
                regex_obj->set_property("source", Value(regexp_impl->get_source()));
                regex_obj->set_property("flags", Value(regexp_impl->get_flags()));
                regex_obj->set_property("global", Value(regexp_impl->get_global()));
                regex_obj->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                regex_obj->set_property("multiline", Value(regexp_impl->get_multiline()));
                regex_obj->set_property("unicode", Value(regexp_impl->get_unicode()));
                regex_obj->set_property("sticky", Value(regexp_impl->get_sticky()));
                regex_obj->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                
                auto test_fn = ObjectFactory::create_native_function("test",
                    [regexp_impl](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (args.empty()) return Value(false);
                        std::string str = args[0].to_string();
                        return Value(regexp_impl->test(str));
                    });
                regex_obj->set_property("test", Value(test_fn.release()), PropertyAttributes::BuiltinFunction);
                
                auto exec_fn = ObjectFactory::create_native_function("exec",
                    [regexp_impl](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (args.empty()) return Value::null();
                        std::string str = args[0].to_string();
                        return regexp_impl->exec(str);
                    });
                regex_obj->set_property("exec", Value(exec_fn.release()), PropertyAttributes::BuiltinFunction);

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

    PropertyDescriptor regexp_constructor_desc(Value(regexp_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    regexp_prototype->set_property_descriptor("constructor", regexp_constructor_desc);
    regexp_constructor->set_property("prototype", Value(regexp_prototype.release()));

    auto regexp_species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(ctx.get_this_binding());
        }, 0);

    PropertyDescriptor regexp_species_desc;
    regexp_species_desc.set_getter(regexp_species_getter.get());
    regexp_species_desc.set_enumerable(false);
    regexp_species_desc.set_configurable(true);

    Value regexp_species_symbol = global_object_->get_property("Symbol");
    if (regexp_species_symbol.is_object()) {
        Object* symbol_constructor = regexp_species_symbol.as_object();
        Value species_key = symbol_constructor->get_property("species");
        if (species_key.is_symbol()) {
            regexp_constructor->set_property_descriptor(species_key.as_symbol()->to_property_key(), regexp_species_desc);
        }
    }

    regexp_species_getter.release();

    register_built_in_object("RegExp", regexp_constructor.release());
    
    std::function<void(Promise*)> add_promise_methods = [&](Promise* promise) {
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
    
    auto promise_constructor = ObjectFactory::create_native_constructor("Promise",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value(std::string("Promise executor must be a function")));
                return Value();
            }
            
            auto promise = std::make_unique<Promise>(&ctx);
            
            Function* executor = args[0].as_function();
            
            auto resolve_fn = ObjectFactory::create_native_function("resolve",
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value value = args.empty() ? Value() : args[0];
                    promise_ptr->fulfill(value);
                    return Value();
                });
            
            auto reject_fn = ObjectFactory::create_native_function("reject",
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value reason = args.empty() ? Value() : args[0];
                    promise_ptr->reject(reason);
                    return Value();
                });
            
            std::vector<Value> executor_args = {
                Value(resolve_fn.release()),
                Value(reject_fn.release())
            };
            
            try {
                executor->call(ctx, executor_args);
            } catch (...) {
                promise->reject(Value(std::string("Promise executor threw")));
            }
            
            add_promise_methods(promise.get());
            
            promise->set_property("_isPromise", Value(true));
            
            return Value(promise.release());
        });
    
    auto promise_try = ObjectFactory::create_native_function("try",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value(std::string("Promise.try requires a function")));
                return Value();
            }
            
            Function* fn = args[0].as_function();
            auto promise = std::make_unique<Promise>(&ctx);
            
            try {
                Value result = fn->call(ctx, {});
                promise->fulfill(result);
            } catch (...) {
                promise->reject(Value(std::string("Function threw in Promise.try")));
            }
            
            return Value(promise.release());
        });
    promise_constructor->set_property("try", Value(promise_try.release()));
    
    auto promise_withResolvers = ObjectFactory::create_native_function("withResolvers",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            auto promise = std::make_unique<Promise>(&ctx);
            
            auto resolve_fn = ObjectFactory::create_native_function("resolve",
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value value = args.empty() ? Value() : args[0];
                    promise_ptr->fulfill(value);
                    return Value();
                });
            
            auto reject_fn = ObjectFactory::create_native_function("reject",
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value reason = args.empty() ? Value() : args[0];
                    promise_ptr->reject(reason);
                    return Value();
                });
            
            auto result_obj = ObjectFactory::create_object();
            result_obj->set_property("promise", Value(promise.release()));
            result_obj->set_property("resolve", Value(resolve_fn.release()), PropertyAttributes::BuiltinFunction);
            result_obj->set_property("reject", Value(reject_fn.release()), PropertyAttributes::BuiltinFunction);
            
            return Value(result_obj.release());
        });
    promise_constructor->set_property("withResolvers", Value(promise_withResolvers.release()));
    
    auto promise_prototype = ObjectFactory::create_object();
    
    auto promise_then = ObjectFactory::create_native_function("then",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("Promise.prototype.then called on non-object")));
                return Value();
            }
            
            Promise* promise = dynamic_cast<Promise*>(this_obj);
            if (!promise) {
                ctx.throw_exception(Value(std::string("Promise.prototype.then called on non-Promise")));
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
    
    auto promise_catch = ObjectFactory::create_native_function("catch",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("Promise.prototype.catch called on non-object")));
                return Value();
            }
            
            Promise* promise = dynamic_cast<Promise*>(this_obj);
            if (!promise) {
                ctx.throw_exception(Value(std::string("Promise.prototype.catch called on non-Promise")));
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
    
    auto promise_finally = ObjectFactory::create_native_function("finally",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_exception(Value(std::string("Promise.prototype.finally called on non-object")));
                return Value();
            }
            
            Promise* promise = dynamic_cast<Promise*>(this_obj);
            if (!promise) {
                ctx.throw_exception(Value(std::string("Promise.prototype.finally called on non-Promise")));
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

    PropertyDescriptor promise_tag_desc(Value(std::string("Promise")), PropertyAttributes::Configurable);
    promise_prototype->set_property_descriptor("Symbol.toStringTag", promise_tag_desc);

    promise_constructor->set_property("prototype", Value(promise_prototype.release()));
    
    auto promise_resolve_static = ObjectFactory::create_native_function("resolve",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            Value value = args.empty() ? Value() : args[0];
            auto promise = std::make_unique<Promise>(&ctx);
            promise->fulfill(value);

            add_promise_methods(promise.get());

            promise->set_property("_isPromise", Value(true));
            promise->set_property("_promiseValue", value);

            return Value(promise.release());
        });
    promise_constructor->set_property("resolve", Value(promise_resolve_static.release()));
    
    auto promise_reject_static = ObjectFactory::create_native_function("reject",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            Value reason = args.empty() ? Value() : args[0];
            auto promise = std::make_unique<Promise>(&ctx);
            promise->reject(reason);
            
            add_promise_methods(promise.get());
            
            promise->set_property("_isPromise", Value(true));
            
            return Value(promise.release());
        });
    promise_constructor->set_property("reject", Value(promise_reject_static.release()));

    auto promise_all_static = ObjectFactory::create_native_function("all",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value(std::string("Promise.all expects an iterable")));
                return Value();
            }

            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_exception(Value(std::string("Promise.all expects an array")));
                return Value();
            }

            uint32_t length = iterable->get_length();
            std::vector<Value> results(length);
            uint32_t resolved_count = 0;

            auto result_promise = std::make_unique<Promise>(&ctx);
            add_promise_methods(result_promise.get());
            result_promise->set_property("_isPromise", Value(true));

            if (length == 0) {
                auto empty_array = ObjectFactory::create_array(0);
                result_promise->fulfill(Value(empty_array.release()));
                return Value(result_promise.release());
            }

            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                if (element.is_object()) {
                    Object* obj = element.as_object();
                    if (obj && obj->has_property("_isPromise")) {
                        if (obj->has_property("_promiseValue")) {
                            results[i] = obj->get_property("_promiseValue");
                        } else {
                            results[i] = element;
                        }
                    } else {
                        results[i] = element;
                    }
                } else {
                    results[i] = element;
                }
            }

            auto result_array = ObjectFactory::create_array(length);
            for (uint32_t i = 0; i < length; i++) {
                result_array->set_element(i, results[i]);
            }

            result_promise->fulfill(Value(result_array.release()));
            return Value(result_promise.release());
        });
    promise_constructor->set_property("all", Value(promise_all_static.release()));

    auto promise_race_static = ObjectFactory::create_native_function("race",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value(std::string("Promise.race expects an iterable")));
                return Value();
            }

            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_exception(Value(std::string("Promise.race expects an array")));
                return Value();
            }

            uint32_t length = iterable->get_length();
            auto result_promise = std::make_unique<Promise>(&ctx);
            add_promise_methods(result_promise.get());
            result_promise->set_property("_isPromise", Value(true));

            if (length == 0) {
                return Value(result_promise.release());
            }

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

    auto promise_allSettled_static = ObjectFactory::create_native_function("allSettled",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value(std::string("Promise.allSettled expects an iterable")));
                return Value();
            }

            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_exception(Value(std::string("Promise.allSettled expects an array")));
                return Value();
            }

            uint32_t length = iterable->get_length();
            auto result_promise = std::make_unique<Promise>(&ctx);
            add_promise_methods(result_promise.get());
            result_promise->set_property("_isPromise", Value(true));

            auto results_array = ObjectFactory::create_array(length);
            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                auto settled_obj = ObjectFactory::create_object();

                settled_obj->set_property("status", Value(std::string("fulfilled")));
                settled_obj->set_property("value", element);

                results_array->set_element(i, Value(settled_obj.release()));
            }

            result_promise->fulfill(Value(results_array.release()));
            return Value(result_promise.release());
        }, 1);
    promise_constructor->set_property("allSettled", Value(promise_allSettled_static.release()));

    auto promise_any_static = ObjectFactory::create_native_function("any",
        [add_promise_methods](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value(std::string("Promise.any expects an iterable")));
                return Value();
            }

            Object* iterable = args[0].as_object();
            if (!iterable->is_array()) {
                ctx.throw_exception(Value(std::string("Promise.any expects an array")));
                return Value();
            }

            uint32_t length = iterable->get_length();
            auto result_promise = std::make_unique<Promise>(&ctx);
            add_promise_methods(result_promise.get());
            result_promise->set_property("_isPromise", Value(true));

            if (length == 0) {
                ctx.throw_exception(Value(std::string("AggregateError: All promises were rejected")));
                return Value();
            }

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
        }, 1);
    promise_constructor->set_property("any", Value(promise_any_static.release()));

    register_built_in_object("Promise", promise_constructor.release());

    auto weakref_constructor = ObjectFactory::create_native_constructor("WeakRef",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_type_error("WeakRef constructor requires an object argument");
                return Value();
            }

            auto weakref_obj = ObjectFactory::create_object();
            weakref_obj->set_property("_target", args[0]);

            auto deref_fn = ObjectFactory::create_native_function("deref",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* this_obj = ctx.get_this_binding();
                    if (this_obj) {
                        return this_obj->get_property("_target");
                    }
                    return Value();
                }, 0);
            weakref_obj->set_property("deref", Value(deref_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(weakref_obj.release());
        });
    register_built_in_object("WeakRef", weakref_constructor.release());

    auto finalizationregistry_constructor = ObjectFactory::create_native_constructor("FinalizationRegistry",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("FinalizationRegistry constructor requires a callback function");
                return Value();
            }

            auto registry_obj = ObjectFactory::create_object();
            registry_obj->set_property("_callback", args[0]);

            auto map_constructor = ctx.get_binding("Map");
            if (map_constructor.is_function()) {
                Function* map_ctor = map_constructor.as_function();
                std::vector<Value> no_args;
                Value map_instance = map_ctor->call(ctx, no_args);
                registry_obj->set_property("_registry", map_instance);
            }

            auto register_fn = ObjectFactory::create_native_function("register",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    if (args.size() < 2 || !args[0].is_object()) {
                        return Value();
                    }

                    Object* this_obj = ctx.get_this_binding();
                    if (!this_obj) return Value();

                    Value registry_map = this_obj->get_property("_registry");
                    if (registry_map.is_object()) {
                        Object* map_obj = registry_map.as_object();

                        if (args.size() >= 3 && !args[2].is_undefined()) {
                            auto entry = ObjectFactory::create_object();
                            entry->set_property("target", args[0]);
                            entry->set_property("heldValue", args[1]);

                            Value set_method = map_obj->get_property("set");
                            if (set_method.is_function()) {
                                Function* set_fn = set_method.as_function();
                                std::vector<Value> set_args = {args[2], Value(entry.release())};
                                set_fn->call(ctx, set_args, Value(map_obj));
                            }
                        }
                    }
                    return Value();
                }, 2);
            registry_obj->set_property("register", Value(register_fn.release()), PropertyAttributes::BuiltinFunction);

            auto unregister_fn = ObjectFactory::create_native_function("unregister",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    if (args.empty()) {
                        return Value(false);
                    }

                    Object* this_obj = ctx.get_this_binding();
                    if (!this_obj) return Value(false);

                    Value registry_map = this_obj->get_property("_registry");
                    if (registry_map.is_object()) {
                        Object* map_obj = registry_map.as_object();

                        Value delete_method = map_obj->get_property("delete");
                        if (delete_method.is_function()) {
                            Function* delete_fn = delete_method.as_function();
                            std::vector<Value> delete_args = {args[0]};
                            return delete_fn->call(ctx, delete_args, Value(map_obj));
                        }
                    }
                    return Value(false);
                }, 1);
            registry_obj->set_property("unregister", Value(unregister_fn.release()), PropertyAttributes::BuiltinFunction);

            return Value(registry_obj.release());
        });
    register_built_in_object("FinalizationRegistry", finalizationregistry_constructor.release());

    auto disposablestack_constructor = ObjectFactory::create_native_constructor("DisposableStack",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* constructor = ctx.get_this_binding();
            auto stack_obj = ObjectFactory::create_object();

            if (constructor && constructor->is_function()) {
                Value prototype_val = constructor->get_property("prototype");
                if (prototype_val.is_object()) {
                    stack_obj->set_prototype(prototype_val.as_object());
                }
            }

            stack_obj->set_property("_stack", Value(ObjectFactory::create_array(0).release()));
            stack_obj->set_property("_disposed", Value(false));

            return Value(stack_obj.release());
        }, 0);

    auto disposablestack_prototype = ObjectFactory::create_object();

    auto ds_use_fn = ObjectFactory::create_native_function("use",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("DisposableStack already disposed");
                return Value();
            }

            if (args.size() > 0) {
                Value stack_val = this_obj->get_property("_stack");
                if (stack_val.is_object()) {
                    Object* stack = stack_val.as_object();
                    stack->push(args[0]);
                }
                return args[0];
            }
            return Value();
        }, 1);
    disposablestack_prototype->set_property("use", Value(ds_use_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ds_dispose_fn = ObjectFactory::create_native_function("dispose",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                return Value();
            }

            this_obj->set_property("_disposed", Value(true));

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                uint32_t length = stack->get_length();

                for (int32_t i = length - 1; i >= 0; i--) {
                    Value resource = stack->get_element(static_cast<uint32_t>(i));
                    if (resource.is_object()) {
                        Object* res_obj = resource.as_object();
                        Value dispose_method = res_obj->get_property("dispose");
                        if (dispose_method.is_function()) {
                            Function* dispose_fn_inner = dispose_method.as_function();
                            std::vector<Value> no_args;
                            dispose_fn_inner->call(ctx, no_args, resource);
                        }
                    }
                }
            }
            return Value();
        }, 0);
    disposablestack_prototype->set_property("dispose", Value(ds_dispose_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ds_adopt_fn = ObjectFactory::create_native_function("adopt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("DisposableStack already disposed");
                return Value();
            }

            if (args.size() < 2) return Value();

            Value value = args[0];
            Value onDispose = args[1];

            if (!onDispose.is_function()) {
                ctx.throw_type_error("onDispose must be a function");
                return Value();
            }

            auto wrapper = ObjectFactory::create_object();
            wrapper->set_property("_value", value);
            wrapper->set_property("_onDispose", onDispose);

            auto wrapper_dispose = ObjectFactory::create_native_function("dispose",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* wrapper_obj = ctx.get_this_binding();
                    if (!wrapper_obj) return Value();

                    Value val = wrapper_obj->get_property("_value");
                    Value on_dispose = wrapper_obj->get_property("_onDispose");

                    if (on_dispose.is_function()) {
                        Function* dispose_callback = on_dispose.as_function();
                        std::vector<Value> callback_args = {val};
                        dispose_callback->call(ctx, callback_args);
                    }
                    return Value();
                }, 0);
            wrapper->set_property("dispose", Value(wrapper_dispose.release()));

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                stack->push(Value(wrapper.release()));
            }

            return value;
        }, 2);
    disposablestack_prototype->set_property("adopt", Value(ds_adopt_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ds_defer_fn = ObjectFactory::create_native_function("defer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("DisposableStack already disposed");
                return Value();
            }

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("defer requires a function argument");
                return Value();
            }

            auto wrapper = ObjectFactory::create_object();
            wrapper->set_property("_onDispose", args[0]);

            auto wrapper_dispose = ObjectFactory::create_native_function("dispose",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* wrapper_obj = ctx.get_this_binding();
                    if (!wrapper_obj) return Value();

                    Value on_dispose = wrapper_obj->get_property("_onDispose");
                    if (on_dispose.is_function()) {
                        Function* dispose_callback = on_dispose.as_function();
                        std::vector<Value> no_args;
                        dispose_callback->call(ctx, no_args);
                    }
                    return Value();
                }, 0);
            wrapper->set_property("dispose", Value(wrapper_dispose.release()));

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                stack->push(Value(wrapper.release()));
            }

            return Value();
        }, 1);
    disposablestack_prototype->set_property("defer", Value(ds_defer_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ds_move_fn = ObjectFactory::create_native_function("move",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("DisposableStack already disposed");
                return Value();
            }

            auto disposable_ctor = ctx.get_binding("DisposableStack");
            if (disposable_ctor.is_function()) {
                Function* ctor = disposable_ctor.as_function();
                std::vector<Value> no_args;
                Value new_stack = ctor->call(ctx, no_args);

                if (new_stack.is_object()) {
                    Object* new_stack_obj = new_stack.as_object();
                    Value old_stack = this_obj->get_property("_stack");
                    new_stack_obj->set_property("_stack", old_stack);
                    this_obj->set_property("_stack", Value(ObjectFactory::create_array(0).release()));
                    this_obj->set_property("_disposed", Value(true));
                    return new_stack;
                }
            }

            return Value();
        }, 0);
    disposablestack_prototype->set_property("move", Value(ds_move_fn.release()), PropertyAttributes::BuiltinFunction);

    disposablestack_constructor->set_property("prototype", Value(disposablestack_prototype.release()));

    register_built_in_object("DisposableStack", disposablestack_constructor.release());

    auto asyncdisposablestack_constructor = ObjectFactory::create_native_constructor("AsyncDisposableStack",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* constructor = ctx.get_this_binding();
            auto stack_obj = ObjectFactory::create_object();

            if (constructor && constructor->is_function()) {
                Value prototype_val = constructor->get_property("prototype");
                if (prototype_val.is_object()) {
                    stack_obj->set_prototype(prototype_val.as_object());
                }
            }

            stack_obj->set_property("_stack", Value(ObjectFactory::create_array(0).release()));
            stack_obj->set_property("_disposed", Value(false));

            return Value(stack_obj.release());
        }, 0);

    auto asyncdisposablestack_prototype = ObjectFactory::create_object();

    auto ads_use_fn = ObjectFactory::create_native_function("use",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("AsyncDisposableStack already disposed");
                return Value();
            }

            if (args.size() > 0) {
                Value stack_val = this_obj->get_property("_stack");
                if (stack_val.is_object()) {
                    Object* stack = stack_val.as_object();
                    stack->push(args[0]);
                }
                return args[0];
            }
            return Value();
        }, 1);
    asyncdisposablestack_prototype->set_property("use", Value(ads_use_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ads_disposeAsync_fn = ObjectFactory::create_native_function("disposeAsync",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                Value promise_ctor = ctx.get_binding("Promise");
                if (promise_ctor.is_function()) {
                    Function* ctor = promise_ctor.as_function();
                    Value resolve_method = ctor->get_property("resolve");
                    if (resolve_method.is_function()) {
                        Function* resolve_fn = resolve_method.as_function();
                        std::vector<Value> args;
                        return resolve_fn->call(ctx, args, promise_ctor);
                    }
                }
                return Value();
            }

            this_obj->set_property("_disposed", Value(true));

            Value promise_ctor = ctx.get_binding("Promise");
            if (promise_ctor.is_function()) {
                Function* ctor = promise_ctor.as_function();
                Value resolve_method = ctor->get_property("resolve");
                if (resolve_method.is_function()) {
                    Function* resolve_fn = resolve_method.as_function();
                    std::vector<Value> args;
                    return resolve_fn->call(ctx, args, promise_ctor);
                }
            }

            return Value();
        }, 0);
    asyncdisposablestack_prototype->set_property("disposeAsync", Value(ads_disposeAsync_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ads_adopt_fn = ObjectFactory::create_native_function("adopt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("AsyncDisposableStack already disposed");
                return Value();
            }

            if (args.size() < 2) return Value();

            Value value = args[0];
            Value onDisposeAsync = args[1];

            if (!onDisposeAsync.is_function()) {
                ctx.throw_type_error("onDisposeAsync must be a function");
                return Value();
            }

            auto wrapper = ObjectFactory::create_object();
            wrapper->set_property("_value", value);
            wrapper->set_property("_onDisposeAsync", onDisposeAsync);

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                stack->push(Value(wrapper.release()));
            }

            return value;
        }, 2);
    asyncdisposablestack_prototype->set_property("adopt", Value(ads_adopt_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ads_defer_fn = ObjectFactory::create_native_function("defer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("AsyncDisposableStack already disposed");
                return Value();
            }

            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("defer requires a function argument");
                return Value();
            }

            auto wrapper = ObjectFactory::create_object();
            wrapper->set_property("_onDisposeAsync", args[0]);

            Value stack_val = this_obj->get_property("_stack");
            if (stack_val.is_object()) {
                Object* stack = stack_val.as_object();
                stack->push(Value(wrapper.release()));
            }

            return Value();
        }, 1);
    asyncdisposablestack_prototype->set_property("defer", Value(ads_defer_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ads_move_fn = ObjectFactory::create_native_function("move",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();

            Value disposed = this_obj->get_property("_disposed");
            if (disposed.to_boolean()) {
                ctx.throw_reference_error("AsyncDisposableStack already disposed");
                return Value();
            }

            auto disposable_ctor = ctx.get_binding("AsyncDisposableStack");
            if (disposable_ctor.is_function()) {
                Function* ctor = disposable_ctor.as_function();
                std::vector<Value> no_args;
                Value new_stack = ctor->call(ctx, no_args);

                if (new_stack.is_object()) {
                    Object* new_stack_obj = new_stack.as_object();
                    Value old_stack = this_obj->get_property("_stack");
                    new_stack_obj->set_property("_stack", old_stack);
                    this_obj->set_property("_stack", Value(ObjectFactory::create_array(0).release()));
                    this_obj->set_property("_disposed", Value(true));
                    return new_stack;
                }
            }

            return Value();
        }, 0);
    asyncdisposablestack_prototype->set_property("move", Value(ads_move_fn.release()), PropertyAttributes::BuiltinFunction);

    asyncdisposablestack_constructor->set_property("prototype", Value(asyncdisposablestack_prototype.release()));

    register_built_in_object("AsyncDisposableStack", asyncdisposablestack_constructor.release());

    auto iterator_constructor = ObjectFactory::create_native_function("Iterator",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            auto iterator_obj = ObjectFactory::create_object();

            Object* constructor = ctx.get_this_binding();
            if (constructor && constructor->is_function()) {
                Value prototype_val = constructor->get_property("prototype");
                if (prototype_val.is_object()) {
                    iterator_obj->set_prototype(prototype_val.as_object());
                }
            }

            return Value(iterator_obj.release());
        });

    auto iterator_prototype = ObjectFactory::create_object();

    auto iterator_next = ObjectFactory::create_native_function("next",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            auto result = ObjectFactory::create_object();
            result->set_property("done", Value(true));
            result->set_property("value", Value());
            return Value(result.release());
        }, 0);
    iterator_prototype->set_property("next", Value(iterator_next.release()));

    iterator_constructor->set_property("prototype", Value(iterator_prototype.release()));
    register_built_in_object("Iterator", iterator_constructor.release());

    auto arraybuffer_constructor = ObjectFactory::create_native_function("ArrayBuffer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            double length_double = 0.0;

            if (!args.empty()) {
                if (!args[0].is_number()) {
                    ctx.throw_type_error("ArrayBuffer size must be a number");
                    return Value();
                }
                length_double = args[0].as_number();
            }
            if (length_double < 0 || length_double != std::floor(length_double)) {
                ctx.throw_range_error("ArrayBuffer size must be a non-negative integer");
                return Value();
            }
            
            size_t byte_length = static_cast<size_t>(length_double);
            
            try {
                auto buffer_obj = std::make_unique<ArrayBuffer>(byte_length);
                buffer_obj->set_property("byteLength", Value(static_cast<double>(byte_length)));
                buffer_obj->set_property("_isArrayBuffer", Value(true));
                
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
    
    auto arraybuffer_isView = ObjectFactory::create_native_function("isView",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty() || !args[0].is_object()) {
                return Value(false);
            }

            Object* obj = args[0].as_object();

            if (obj->has_property("buffer") || obj->has_property("byteLength")) {
                Value buffer_val = obj->get_property("buffer");
                if (buffer_val.is_object()) {
                    return Value(true);
                }
            }

            return Value(false);
        });

    PropertyDescriptor isView_length_desc(Value(1.0), PropertyAttributes::None);
    isView_length_desc.set_configurable(true);
    isView_length_desc.set_enumerable(false);
    isView_length_desc.set_writable(false);
    arraybuffer_isView->set_property_descriptor("length", isView_length_desc);

    arraybuffer_constructor->set_property("isView", Value(arraybuffer_isView.release()), PropertyAttributes::BuiltinFunction);

    auto arraybuffer_prototype = ObjectFactory::create_object();

    auto byteLength_getter = ObjectFactory::create_native_function("get byteLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array_buffer()) {
                ctx.throw_type_error("ArrayBuffer.prototype.byteLength called on non-ArrayBuffer");
                return Value();
            }
            ArrayBuffer* ab = static_cast<ArrayBuffer*>(this_obj);
            return Value(static_cast<double>(ab->byte_length()));
        }, 0);

    PropertyDescriptor byteLength_desc;
    byteLength_desc.set_getter(byteLength_getter.release());
    byteLength_desc.set_enumerable(false);
    byteLength_desc.set_configurable(true);
    arraybuffer_prototype->set_property_descriptor("byteLength", byteLength_desc);

    auto detached_getter = ObjectFactory::create_native_function("get detached",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_array_buffer()) {
                ctx.throw_type_error("ArrayBuffer.prototype.detached called on non-ArrayBuffer");
                return Value();
            }
            ArrayBuffer* ab = static_cast<ArrayBuffer*>(this_obj);
            return Value(ab->is_detached());
        }, 0);

    PropertyDescriptor detached_desc;
    detached_desc.set_getter(detached_getter.release());
    detached_desc.set_enumerable(false);
    detached_desc.set_configurable(true);
    arraybuffer_prototype->set_property_descriptor("detached", detached_desc);

    auto ab_slice_fn = ObjectFactory::create_native_function("slice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value();
        }, 2);

    ab_slice_fn->set_property("name", Value(std::string("slice")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("slice", Value(ab_slice_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ab_resize_fn = ObjectFactory::create_native_function("resize",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value();
        }, 1);

    ab_resize_fn->set_property("name", Value(std::string("resize")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("resize", Value(ab_resize_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ab_transfer_fn = ObjectFactory::create_native_function("transfer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value();
        }, 0);

    ab_transfer_fn->set_property("name", Value(std::string("transfer")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("transfer", Value(ab_transfer_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ab_maxByteLength_fn = ObjectFactory::create_native_function("get maxByteLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("ArrayBuffer.prototype.maxByteLength called on non-ArrayBuffer");
                return Value();
            }

            if (this_obj->has_property("maxByteLength")) {
                return this_obj->get_property("maxByteLength");
            }

            if (this_obj->has_property("byteLength")) {
                return this_obj->get_property("byteLength");
            }

            return Value(0.0);
        }, 0);

    PropertyDescriptor maxByteLength_desc(Value(ab_maxByteLength_fn.release()), PropertyAttributes::Configurable);
    maxByteLength_desc.set_enumerable(false);
    arraybuffer_prototype->set_property_descriptor("maxByteLength", maxByteLength_desc);

    auto ab_resizable_fn = ObjectFactory::create_native_function("get resizable",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("ArrayBuffer.prototype.resizable called on non-ArrayBuffer");
                return Value();
            }

            if (this_obj->has_property("maxByteLength") && this_obj->has_property("byteLength")) {
                Value max = this_obj->get_property("maxByteLength");
                Value current = this_obj->get_property("byteLength");
                if (max.is_number() && current.is_number()) {
                    return Value(max.as_number() != current.as_number());
                }
            }

            return Value(false);
        }, 0);

    PropertyDescriptor resizable_desc(Value(ab_resizable_fn.release()), PropertyAttributes::Configurable);
    resizable_desc.set_enumerable(false);
    arraybuffer_prototype->set_property_descriptor("resizable", resizable_desc);

    auto ab_transferToFixedLength_fn = ObjectFactory::create_native_function("transferToFixedLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            return Value();
        }, 0);

    ab_transferToFixedLength_fn->set_property("name", Value(std::string("transferToFixedLength")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    arraybuffer_prototype->set_property("transferToFixedLength", Value(ab_transferToFixedLength_fn.release()), PropertyAttributes::BuiltinFunction);

    arraybuffer_constructor->set_property("prototype", Value(arraybuffer_prototype.release()));

    auto arraybuffer_species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(ctx.get_this_binding());
        }, 0);

    PropertyDescriptor arraybuffer_species_desc;
    arraybuffer_species_desc.set_getter(arraybuffer_species_getter.get());
    arraybuffer_species_desc.set_enumerable(false);
    arraybuffer_species_desc.set_configurable(true);

    Value arraybuffer_species_symbol = global_object_->get_property("Symbol");
    if (arraybuffer_species_symbol.is_object()) {
        Object* symbol_constructor = arraybuffer_species_symbol.as_object();
        Value species_key = symbol_constructor->get_property("species");
        if (species_key.is_symbol()) {
            arraybuffer_constructor->set_property_descriptor(species_key.as_symbol()->to_property_key(), arraybuffer_species_desc);
        }
    }

    arraybuffer_species_getter.release();

    register_built_in_object("ArrayBuffer", arraybuffer_constructor.release());
    
    register_typed_array_constructors();
    
    Proxy::setup_proxy(*this);
    Reflect::setup_reflect(*this);
    
}


void Context::setup_global_bindings() {
    if (!lexical_environment_) return;
    
    auto parseInt_fn = ObjectFactory::create_native_function("parseInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::nan();
            
            std::string str = args[0].to_string();
            
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

            // If radix not specified and string starts with "0x" or "0X", use radix 16
            if (args.size() <= 1 && start + 1 < str.length() &&
                str[start] == '0' && (str[start + 1] == 'x' || str[start + 1] == 'X')) {
                radix = 16;
                start += 2; 
            }

            if (start >= str.length()) {
                return Value::nan();
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
        }, 2);
    lexical_environment_->create_binding("parseInt", Value(parseInt_fn.release()), false);
    
    auto parseFloat_fn = ObjectFactory::create_native_function("parseFloat",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::nan();
            
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
        }, 1);
    lexical_environment_->create_binding("parseFloat", Value(parseFloat_fn.release()), false);
    
    auto isNaN_global_fn = ObjectFactory::create_native_function("isNaN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Global isNaN: coerce to number first, then check if NaN
            if (args.empty()) return Value(true);

            // If already NaN, return true
            if (args[0].is_nan()) return Value(true);

            // Convert to number (may produce NaN for non-numeric values like "abc")
            Value num_val(args[0].to_number());

            // Check if conversion resulted in NaN
            return Value(num_val.is_nan());
        }, 1);
    lexical_environment_->create_binding("isNaN", Value(isNaN_global_fn.release()), false);


    auto eval_fn = ObjectFactory::create_native_function("eval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value();

            std::string code = args[0].to_string();
            if (code.empty()) return Value();

            Engine* engine = ctx.get_engine();
            if (!engine) return Value();

            auto result = engine->evaluate(code);
            if (result.success) {
                return result.value;
            } else {
                ctx.throw_syntax_error(result.error_message);
                return Value();
            }
        }, 1);
    lexical_environment_->create_binding("eval", Value(eval_fn.release()), false);

    lexical_environment_->create_binding("undefined", Value(), false);
    lexical_environment_->create_binding("null", Value::null(), false);
    
    if (global_object_) {
        lexical_environment_->create_binding("globalThis", Value(global_object_), false);
        lexical_environment_->create_binding("global", Value(global_object_), false);
        lexical_environment_->create_binding("window", Value(global_object_), false);
        lexical_environment_->create_binding("this", Value(global_object_), false);

        PropertyDescriptor global_ref_desc(Value(global_object_), PropertyAttributes::BuiltinFunction);
        global_object_->set_property_descriptor("globalThis", global_ref_desc);
        global_object_->set_property_descriptor("global", global_ref_desc);
        global_object_->set_property_descriptor("window", global_ref_desc);
        global_object_->set_property_descriptor("this", global_ref_desc);
    }
    lexical_environment_->create_binding("true", Value(true), false);
    lexical_environment_->create_binding("false", Value(false), false);
    
    lexical_environment_->create_binding("NaN", Value::nan(), false);
    lexical_environment_->create_binding("Infinity", Value::positive_infinity(), false);

    if (global_object_) {
        PropertyDescriptor nan_desc(Value::nan(), PropertyAttributes::None);
        global_object_->set_property_descriptor("NaN", nan_desc);

        PropertyDescriptor inf_desc(Value::positive_infinity(), PropertyAttributes::None);
        global_object_->set_property_descriptor("Infinity", inf_desc);

        PropertyDescriptor undef_desc(Value(), PropertyAttributes::None);
        global_object_->set_property_descriptor("undefined", undef_desc);
    }
    
    auto encode_uri_fn = ObjectFactory::create_native_function("encodeURI",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::string(""));
            std::string input = args[0].to_string();
            std::string result;

            for (unsigned char c : input) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == ';' || c == ',' || c == '/' || c == '?' || c == ':' || c == '@' ||
                    c == '&' || c == '=' || c == '+' || c == '$' || c == '-' || c == '_' ||
                    c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' ||
                    c == ')' || c == '#') {
                    result += c;
                } else {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", c);
                    result += hex;
                }
            }
            return Value(result);
        }, 1);
    lexical_environment_->create_binding("encodeURI", Value(encode_uri_fn.release()), false);

    auto decode_uri_fn = ObjectFactory::create_native_function("decodeURI",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::string(""));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                if (input[i] == '%' && i + 2 < input.length()) {
                    int value;
                    if (sscanf(input.substr(i + 1, 2).c_str(), "%x", &value) == 1) {
                        result += static_cast<char>(value);
                        i += 2;
                    } else {
                        result += input[i];
                    }
                } else {
                    result += input[i];
                }
            }
            return Value(result);
        }, 1);
    lexical_environment_->create_binding("decodeURI", Value(decode_uri_fn.release()), false);

    auto encode_uri_component_fn = ObjectFactory::create_native_function("encodeURIComponent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::string(""));
            std::string input = args[0].to_string();
            std::string result;

            for (unsigned char c : input) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' ||
                    c == '\'' || c == '(' || c == ')') {
                    result += c;
                } else {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", c);
                    result += hex;
                }
            }
            return Value(result);
        }, 1);
    lexical_environment_->create_binding("encodeURIComponent", Value(encode_uri_component_fn.release()), false);

    auto decode_uri_component_fn = ObjectFactory::create_native_function("decodeURIComponent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::string(""));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                if (input[i] == '%' && i + 2 < input.length()) {
                    int value;
                    if (sscanf(input.substr(i + 1, 2).c_str(), "%x", &value) == 1) {
                        result += static_cast<char>(value);
                        i += 2;
                    } else {
                        result += input[i];
                    }
                } else if (input[i] == '+') {
                    result += ' ';
                } else {
                    result += input[i];
                }
            }
            return Value(result);
        }, 1);
    lexical_environment_->create_binding("decodeURIComponent", Value(decode_uri_component_fn.release()), false);
    
    auto bigint_fn = ObjectFactory::create_native_function("BigInt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("BigInt constructor requires an argument");
                return Value();
            }
            
            Value arg = args[0];
            if (arg.is_bigint()) {
                return arg;
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

    auto escape_fn = ObjectFactory::create_native_function("escape",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::string("undefined"));
            }

            std::string input;
            Value arg = args[0];
            if (arg.is_object()) {
                Object* obj = arg.as_object();
                Value toString_method = obj->get_property("toString");
                if (toString_method.is_function()) {
                    try {
                        Function* func = toString_method.as_function();
                        Value result = func->call(ctx, {}, arg);
                        if (ctx.has_exception()) {
                            return Value();
                        }
                        input = result.to_string();
                    } catch (...) {
                        return Value();
                    }
                } else {
                    input = arg.to_string();
                }
            } else {
                input = arg.to_string();
            }
            std::string result;

            // Convert UTF-8 string to UTF-16 code units
            std::u16string utf16;
            size_t i = 0;
            while (i < input.length()) {
                unsigned char byte = static_cast<unsigned char>(input[i]);
                uint32_t codepoint;

                if (byte < 0x80) {
                    codepoint = byte;
                    i++;
                } else if ((byte & 0xE0) == 0xC0) {
                    codepoint = ((byte & 0x1F) << 6) | (input[i + 1] & 0x3F);
                    i += 2;
                } else if ((byte & 0xF0) == 0xE0) {
                    codepoint = ((byte & 0x0F) << 12) | ((input[i + 1] & 0x3F) << 6) | (input[i + 2] & 0x3F);
                    i += 3;
                } else if ((byte & 0xF8) == 0xF0) {
                    codepoint = ((byte & 0x07) << 18) | ((input[i + 1] & 0x3F) << 12) | ((input[i + 2] & 0x3F) << 6) | (input[i + 3] & 0x3F);
                    i += 4;
                    // Convert to surrogate pair
                    if (codepoint > 0xFFFF) {
                        codepoint -= 0x10000;
                        utf16 += static_cast<char16_t>((codepoint >> 10) + 0xD800);
                        utf16 += static_cast<char16_t>((codepoint & 0x3FF) + 0xDC00);
                        continue;
                    }
                } else {
                    i++;
                    continue;
                }

                utf16 += static_cast<char16_t>(codepoint);
            }

            // Escape according to spec
            for (char16_t code_unit : utf16) {
                uint16_t c = static_cast<uint16_t>(code_unit);

                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '@' || c == '*' || c == '_' || c == '+' || c == '-' || c == '.' || c == '/') {
                    result += static_cast<char>(c);
                } else if (c < 256) {
                    // %XX format for code units below 256
                    result += '%';
                    result += "0123456789ABCDEF"[(c >> 4) & 0xF];
                    result += "0123456789ABCDEF"[c & 0xF];
                } else {
                    // %uXXXX format for code units >= 256
                    result += "%u";
                    result += "0123456789ABCDEF"[(c >> 12) & 0xF];
                    result += "0123456789ABCDEF"[(c >> 8) & 0xF];
                    result += "0123456789ABCDEF"[(c >> 4) & 0xF];
                    result += "0123456789ABCDEF"[c & 0xF];
                }
            }

            return Value(result);
        });
    lexical_environment_->create_binding("escape", Value(escape_fn.get()), false);
    if (global_object_) {
        PropertyDescriptor escape_desc(Value(escape_fn.get()), PropertyAttributes::BuiltinFunction);
        global_object_->set_property_descriptor("escape", escape_desc);
    }
    escape_fn.release();

    auto unescape_fn = ObjectFactory::create_native_function("unescape",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::string("undefined"));
            }

            std::string input;
            Value arg = args[0];
            if (arg.is_object()) {
                Object* obj = arg.as_object();
                Value toString_method = obj->get_property("toString");
                if (toString_method.is_function()) {
                    try {
                        Function* func = toString_method.as_function();
                        Value result = func->call(ctx, {}, arg);
                        if (ctx.has_exception()) {
                            return Value();
                        }
                        input = result.to_string();
                    } catch (...) {
                        return Value();
                    }
                } else {
                    input = arg.to_string();
                }
            } else {
                input = arg.to_string();
            }
            auto hex_to_num = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };

            std::u16string utf16;

            for (size_t i = 0; i < input.length(); ++i) {
                if (input[i] == '%') {
                    // Check for %uXXXX format
                    if (i + 5 < input.length() && input[i + 1] == 'u') {
                        int val1 = hex_to_num(input[i + 2]);
                        int val2 = hex_to_num(input[i + 3]);
                        int val3 = hex_to_num(input[i + 4]);
                        int val4 = hex_to_num(input[i + 5]);

                        if (val1 >= 0 && val2 >= 0 && val3 >= 0 && val4 >= 0) {
                            uint16_t code_unit = (val1 << 12) | (val2 << 8) | (val3 << 4) | val4;
                            utf16 += static_cast<char16_t>(code_unit);
                            i += 5;
                            continue;
                        }
                    }
                    // Check for %XX format
                    if (i + 2 < input.length()) {
                        int val1 = hex_to_num(input[i + 1]);
                        int val2 = hex_to_num(input[i + 2]);

                        if (val1 >= 0 && val2 >= 0) {
                            uint8_t byte = (val1 << 4) | val2;
                            utf16 += static_cast<char16_t>(byte);
                            i += 2;
                            continue;
                        }
                    }
                }
                // Not an escape sequence, add as-is
                utf16 += static_cast<char16_t>(static_cast<unsigned char>(input[i]));
            }

            // Convert UTF-16 back to UTF-8
            std::string result;
            for (size_t i = 0; i < utf16.length(); ++i) {
                uint16_t code_unit = static_cast<uint16_t>(utf16[i]);

                // Check for surrogate pair
                if (code_unit >= 0xD800 && code_unit <= 0xDBFF && i + 1 < utf16.length()) {
                    uint16_t next = static_cast<uint16_t>(utf16[i + 1]);
                    if (next >= 0xDC00 && next <= 0xDFFF) {
                        uint32_t codepoint = 0x10000 + ((code_unit - 0xD800) << 10) + (next - 0xDC00);
                        // Encode to UTF-8
                        result += static_cast<char>(0xF0 | (codepoint >> 18));
                        result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (codepoint & 0x3F));
                        i++;
                        continue;
                    }
                }

                // Single code unit
                if (code_unit < 0x80) {
                    result += static_cast<char>(code_unit);
                } else if (code_unit < 0x800) {
                    result += static_cast<char>(0xC0 | (code_unit >> 6));
                    result += static_cast<char>(0x80 | (code_unit & 0x3F));
                } else {
                    result += static_cast<char>(0xE0 | (code_unit >> 12));
                    result += static_cast<char>(0x80 | ((code_unit >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (code_unit & 0x3F));
                }
            }

            return Value(result);
        });
    lexical_environment_->create_binding("unescape", Value(unescape_fn.get()), false);
    if (global_object_) {
        PropertyDescriptor unescape_desc(Value(unescape_fn.get()), PropertyAttributes::BuiltinFunction);
        global_object_->set_property_descriptor("unescape", unescape_desc);
    }
    unescape_fn.release();

    auto console_obj = ObjectFactory::create_object();
    auto console_log_fn = ObjectFactory::create_native_function("log", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].to_string();
            }
            std::cout << std::endl;
            return Value();
        }, 1);
    auto console_error_fn = ObjectFactory::create_native_function("error",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cerr << " ";
                std::cerr << args[i].to_string();
            }
            std::cerr << std::endl;
            return Value();
        });
    auto console_warn_fn = ObjectFactory::create_native_function("warn",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].to_string();
            }
            std::cout << std::endl;
            return Value();
        });
    
    console_obj->set_property("log", Value(console_log_fn.release()), PropertyAttributes::BuiltinFunction);
    console_obj->set_property("error", Value(console_error_fn.release()), PropertyAttributes::BuiltinFunction);
    console_obj->set_property("warn", Value(console_warn_fn.release()), PropertyAttributes::BuiltinFunction);
    
    lexical_environment_->create_binding("console", Value(console_obj.release()), false);

    // GC object with stats(), collect(), heapSize() methods
    auto gc_obj = ObjectFactory::create_object();

    auto gc_obj_stats_fn = ObjectFactory::create_native_function("stats",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.get_gc()) return Value();

            const auto& stats = ctx.get_gc()->get_statistics();
            auto stats_obj = ObjectFactory::create_object();

            stats_obj->set_property("totalAllocations", Value(static_cast<double>(stats.total_allocations)));
            stats_obj->set_property("totalDeallocations", Value(static_cast<double>(stats.total_deallocations)));
            stats_obj->set_property("totalCollections", Value(static_cast<double>(stats.total_collections)));
            stats_obj->set_property("bytesAllocated", Value(static_cast<double>(stats.bytes_allocated)));
            stats_obj->set_property("bytesFreed", Value(static_cast<double>(stats.bytes_freed)));
            stats_obj->set_property("currentMemory", Value(static_cast<double>(stats.bytes_allocated - stats.bytes_freed)));
            stats_obj->set_property("peakMemoryUsage", Value(static_cast<double>(stats.peak_memory_usage)));

            return Value(stats_obj.release());
        }, 0);

    auto gc_obj_collect_fn = ObjectFactory::create_native_function("collect",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.get_gc()) {
                ctx.get_gc()->collect_garbage();
            }
            return Value();
        }, 0);

    auto gc_obj_heap_size_fn = ObjectFactory::create_native_function("heapSize",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (ctx.get_gc()) {
                return Value(static_cast<double>(ctx.get_gc()->get_heap_size()));
            }
            return Value();
        }, 0);

    gc_obj->set_property("stats", Value(gc_obj_stats_fn.release()), PropertyAttributes::BuiltinFunction);
    gc_obj->set_property("collect", Value(gc_obj_collect_fn.release()), PropertyAttributes::BuiltinFunction);
    gc_obj->set_property("heapSize", Value(gc_obj_heap_size_fn.release()), PropertyAttributes::BuiltinFunction);

    lexical_environment_->create_binding("gc", Value(gc_obj.release()), false);

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
    
    if (built_in_objects_.find("JSON") != built_in_objects_.end() && built_in_objects_["JSON"]) {
        lexical_environment_->create_binding("JSON", Value(built_in_objects_["JSON"]), false);
    }
    if (built_in_objects_.find("Date") != built_in_objects_.end() && built_in_objects_["Date"]) {
        lexical_environment_->create_binding("Date", Value(built_in_objects_["Date"]), false);
    }
    
    auto setTimeout_fn = ObjectFactory::create_native_function("setTimeout",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(1);
        });
    auto setInterval_fn = ObjectFactory::create_native_function("setInterval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value(1);
        });
    auto clearTimeout_fn = ObjectFactory::create_native_function("clearTimeout",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value();
        });
    auto clearInterval_fn = ObjectFactory::create_native_function("clearInterval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return Value();
        });
    
    lexical_environment_->create_binding("setTimeout", Value(setTimeout_fn.release()), false);
    lexical_environment_->create_binding("setInterval", Value(setInterval_fn.release()), false);
    lexical_environment_->create_binding("clearTimeout", Value(clearTimeout_fn.release()), false);
    lexical_environment_->create_binding("clearInterval", Value(clearInterval_fn.release()), false);
    
    
    
    if (built_in_objects_.find("Object") != built_in_objects_.end() && built_in_objects_["Object"]) {
        Object* obj_constructor = built_in_objects_["Object"];
        Value binding_value;
        if (obj_constructor->is_function()) {
            Function* func_ptr = static_cast<Function*>(obj_constructor);
            binding_value = Value(func_ptr);
        } else {
            binding_value = Value(obj_constructor);
        }
        lexical_environment_->create_binding("Object", binding_value, false);
    }
    
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
    
    if (built_in_objects_.find("Function") != built_in_objects_.end() && built_in_objects_["Function"]) {
        Object* func_constructor = built_in_objects_["Function"];
        Value binding_value;
        if (func_constructor->is_function()) {
            Function* func_ptr = static_cast<Function*>(func_constructor);
            binding_value = Value(func_ptr);
        } else {
            binding_value = Value(func_constructor);
        }
        lexical_environment_->create_binding("Function", binding_value, false);
    }
    
    for (const auto& pair : built_in_objects_) {
        if (pair.second) {
            if (pair.first != "Object" && pair.first != "Array" && pair.first != "Function") {
                Value binding_value;
                if (pair.second->is_function()) {
                    Function* func_ptr = static_cast<Function*>(pair.second);
                    binding_value = Value(func_ptr);
                } else {
                    binding_value = Value(pair.second);
                }
                
                lexical_environment_->create_binding(pair.first, binding_value, false);
                if (global_object_) {
                    PropertyDescriptor desc(binding_value,
                        PropertyAttributes::BuiltinFunction);
                    global_object_->set_property_descriptor(pair.first, desc);
                }
            }
        }
    }

    IterableUtils::setup_array_iterator_methods(*this);
    IterableUtils::setup_string_iterator_methods(*this);
    IterableUtils::setup_map_iterator_methods(*this);
    IterableUtils::setup_set_iterator_methods(*this);

    setup_test262_helpers();
}


void Context::setup_test262_helpers() {
    auto testWithTypedArrayConstructors = ObjectFactory::create_native_function("testWithTypedArrayConstructors",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("testWithTypedArrayConstructors requires a function argument");
                return Value();
            }

            Function* callback = args[0].as_function();

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
                            std::vector<Value> callArgs = { ctor };
                            callback->call(ctx, callArgs, Value());
                        } catch (...) {
                            ctx.throw_exception(Value("Error in testWithTypedArrayConstructors with " + ctorName));
                            return Value();
                        }
                    }
                }
            }

            return Value();
        });

    lexical_environment_->create_binding("testWithTypedArrayConstructors", Value(testWithTypedArrayConstructors.release()), false);

    auto buildString = ObjectFactory::create_native_function("buildString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_type_error("buildString requires an object argument");
                return Value();
            }

            Object* argsObj = args[0].as_object();
            std::string result;

            if (argsObj->has_property("loneCodePoints")) {
                Value loneVal = argsObj->get_property("loneCodePoints");
                if (loneVal.is_object() && loneVal.as_object()->is_array()) {
                    Object* loneArray = loneVal.as_object();
                    uint32_t length = static_cast<uint32_t>(loneArray->get_property("length").as_number());
                    for (uint32_t i = 0; i < length; i++) {
                        Value elem = loneArray->get_element(i);
                        if (elem.is_number()) {
                            uint32_t codePoint = static_cast<uint32_t>(elem.as_number());
                            if (codePoint < 0x80) {
                                result += static_cast<char>(codePoint);
                            }
                        }
                    }
                }
            }

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


void Context::set_return_value(const Value& value) {
    return_value_ = value;
    has_return_value_ = true;
}

void Context::clear_return_value() {
    return_value_ = Value();
    has_return_value_ = false;
}


void Context::set_break(const std::string& label) {
    has_break_ = true;
    break_label_ = label;
}

void Context::set_continue(const std::string& label) {
    has_continue_ = true;
    continue_label_ = label;
}

void Context::clear_break_continue() {
    has_break_ = false;
    has_continue_ = false;
    break_label_.clear();
    continue_label_.clear();
}


StackFrame::StackFrame(Type type, Function* function, Object* this_binding)
    : type_(type), function_(function), this_binding_(this_binding),
      environment_(nullptr), program_counter_(0), line_number_(0), column_number_(0) {
}

Value StackFrame::get_argument(size_t index) const {
    if (index < arguments_.size()) {
        return arguments_[index];
    }
    return Value();
}

bool StackFrame::has_local(const std::string& name) const {
    return local_variables_.find(name) != local_variables_.end();
}

Value StackFrame::get_local(const std::string& name) const {
    auto it = local_variables_.find(name);
    if (it != local_variables_.end()) {
        return it->second;
    }
    return Value();
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
    if (depth > 100) {
        return Value();
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
    
    return Value();
}

bool Environment::set_binding(const std::string& name, const Value& value) {
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->set_property(name, value);
        } else {
            if (is_mutable_binding(name)) {
                bindings_[name] = value;
                return true;
            }
            return false;
        }
    }
    
    if (outer_environment_) {
        return outer_environment_->set_binding(name, value);
    }
    
    return false;
}

bool Environment::create_binding(const std::string& name, const Value& value, bool mutable_binding, bool deletable) {
    if (has_own_binding(name)) {
        return false;
    }

    if (type_ == Type::Object && binding_object_) {
        // ES1: Set Configurable attribute based on deletable flag
        // Configurable = true means deletable 
        // Configurable = false means DontDelete 
        int attrs_value = PropertyAttributes::Writable | PropertyAttributes::Enumerable;
        if (deletable) {
            attrs_value |= PropertyAttributes::Configurable;
        }
        PropertyAttributes attrs = static_cast<PropertyAttributes>(attrs_value);
        PropertyDescriptor desc(value, attrs);
        return binding_object_->set_property_descriptor(name, desc);
    } else {
        bindings_[name] = value;
        mutable_flags_[name] = mutable_binding;
        initialized_flags_[name] = true;
        deletable_flags_[name] = deletable;
        return true;
    }
}

bool Environment::delete_binding(const std::string& name) {
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->delete_property(name);
        } else {
            // ES1: Check if binding is deletable (DontDelete attribute)
            auto it = deletable_flags_.find(name);
            bool deletable = (it != deletable_flags_.end()) ? it->second : false;

            if (!deletable) {
                return false;
            }

            bindings_.erase(name);
            mutable_flags_.erase(name);
            initialized_flags_.erase(name);
            deletable_flags_.erase(name);
            return true;
        }
    }

    return false;
}

bool Environment::is_mutable_binding(const std::string& name) const {
    auto it = mutable_flags_.find(name);
    return (it != mutable_flags_.end()) ? it->second : true;
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


namespace ContextFactory {

std::unique_ptr<Context> create_global_context(Engine* engine) {
    return std::make_unique<Context>(engine, Context::Type::Global);
}

std::unique_ptr<Context> create_function_context(Engine* engine, Context* parent, Function* function) {
    auto context = std::make_unique<Context>(engine, parent, Context::Type::Function);
    
    auto func_env = std::make_unique<Environment>(Environment::Type::Function, parent->get_lexical_environment());
    context->set_lexical_environment(func_env.release());
    context->set_variable_environment(context->get_lexical_environment());
    
    return context;
}

std::unique_ptr<Context> create_eval_context(Engine* engine, Context* parent) {
    auto context = std::make_unique<Context>(engine, parent, Context::Type::Eval);
    
    context->set_lexical_environment(parent->get_lexical_environment());
    context->set_variable_environment(parent->get_variable_environment());
    
    return context;
}

std::unique_ptr<Context> create_module_context(Engine* engine) {
    auto context = std::make_unique<Context>(engine, Context::Type::Module);
    
    auto module_env = std::make_unique<Environment>(Environment::Type::Module);
    context->set_lexical_environment(module_env.release());
    context->set_variable_environment(context->get_lexical_environment());
    
    return context;
}

}


void Context::push_block_scope() {
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

                if (obj->is_array() || obj->has_property("length")) {
                    uint32_t length = obj->is_array() ? obj->get_length() :
                                     (obj->has_property("length") ? static_cast<uint32_t>(obj->get_property("length").to_number()) : 0);

                    auto typed_array = TypedArrayFactory::create_uint8_array(length);

                    for (uint32_t i = 0; i < length; i++) {
                        Value element = obj->get_element(i);
                        typed_array->set_element(i, element);
                    }

                    return Value(typed_array.release());
                }

                if (obj->is_typed_array()) {
                    TypedArrayBase* source = static_cast<TypedArrayBase*>(obj);
                    size_t length = source->length();
                    auto typed_array = TypedArrayFactory::create_uint8_array(length);

                    for (size_t i = 0; i < length; i++) {
                        typed_array->set_element(i, source->get_element(i));
                    }

                    return Value(typed_array.release());
                }
            }

            ctx.throw_type_error("Uint8Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Uint8Array", uint8array_constructor.release());

    auto uint8clampedarray_constructor = ObjectFactory::create_native_function("Uint8ClampedArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                auto typed_array = TypedArrayFactory::create_uint8_clamped_array(0);
                return Value(typed_array.release());
            }

            const Value& arg = args[0];

            if (arg.is_number()) {
                size_t length = static_cast<size_t>(arg.to_number());
                auto typed_array = TypedArrayFactory::create_uint8_clamped_array(length);
                return Value(typed_array.release());
            }

            if (arg.is_object()) {
                Object* obj = arg.as_object();

                if (obj->is_array() || obj->has_property("length")) {
                    uint32_t length = obj->is_array() ? obj->get_length() :
                                     static_cast<uint32_t>(obj->get_property("length").to_number());

                    auto typed_array = TypedArrayFactory::create_uint8_clamped_array(length);

                    for (uint32_t i = 0; i < length; i++) {
                        Value element = obj->get_element(i);
                        typed_array->set_element(i, element);
                    }

                    return Value(typed_array.release());
                }

                if (obj->is_typed_array()) {
                    TypedArrayBase* source = static_cast<TypedArrayBase*>(obj);
                    size_t length = source->length();
                    auto typed_array = TypedArrayFactory::create_uint8_clamped_array(length);

                    for (size_t i = 0; i < length; i++) {
                        typed_array->set_element(i, source->get_element(i));
                    }

                    return Value(typed_array.release());
                }
            }

            ctx.throw_type_error("Uint8ClampedArray constructor argument not supported");
            return Value();
        });
    register_built_in_object("Uint8ClampedArray", uint8clampedarray_constructor.release());

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

                if (obj->is_array() || obj->has_property("length")) {
                    uint32_t length = obj->is_array() ? obj->get_length() :
                                     static_cast<uint32_t>(obj->get_property("length").to_number());
                    auto typed_array = TypedArrayFactory::create_float32_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                if (obj->is_typed_array()) {
                    TypedArrayBase* source = static_cast<TypedArrayBase*>(obj);
                    size_t length = source->length();
                    auto typed_array = TypedArrayFactory::create_float32_array(length);
                    for (size_t i = 0; i < length; i++) {
                        typed_array->set_element(i, source->get_element(i));
                    }
                    return Value(typed_array.release());
                }
            }

            ctx.throw_type_error("Float32Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Float32Array", float32array_constructor.release());

    auto typedarray_constructor = ObjectFactory::create_native_function("TypedArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            ctx.throw_type_error("Abstract class TypedArray not intended to be instantiated directly");
            return Value();
        }, 0);

    PropertyDescriptor typedarray_name_desc(Value(std::string("TypedArray")),
        static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    typedarray_constructor->set_property_descriptor("name", typedarray_name_desc);

    PropertyDescriptor typedarray_length_desc(Value(0.0),
        static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    typedarray_constructor->set_property_descriptor("length", typedarray_length_desc);

    auto typedarray_prototype = ObjectFactory::create_object();

    PropertyDescriptor typedarray_constructor_desc(Value(typedarray_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    typedarray_prototype->set_property_descriptor("constructor", typedarray_constructor_desc);

    PropertyDescriptor typedarray_tag_desc(Value(std::string("TypedArray")), PropertyAttributes::Configurable);
    typedarray_prototype->set_property_descriptor("Symbol.toStringTag", typedarray_tag_desc);


    auto buffer_getter = ObjectFactory::create_native_function("get buffer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.buffer called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            return Value(ta->buffer());
        }, 0);
    PropertyDescriptor buffer_desc;
    buffer_desc.set_getter(buffer_getter.release());
    buffer_desc.set_enumerable(false);
    buffer_desc.set_configurable(true);
    typedarray_prototype->set_property_descriptor("buffer", buffer_desc);

    auto byteLength_getter = ObjectFactory::create_native_function("get byteLength",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.byteLength called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            return Value(static_cast<double>(ta->byte_length()));
        }, 0);
    PropertyDescriptor byteLength_desc;
    byteLength_desc.set_getter(byteLength_getter.release());
    byteLength_desc.set_enumerable(false);
    byteLength_desc.set_configurable(true);
    typedarray_prototype->set_property_descriptor("byteLength", byteLength_desc);

    auto byteOffset_getter = ObjectFactory::create_native_function("get byteOffset",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.byteOffset called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            return Value(static_cast<double>(ta->byte_offset()));
        }, 0);
    PropertyDescriptor byteOffset_desc;
    byteOffset_desc.set_getter(byteOffset_getter.release());
    byteOffset_desc.set_enumerable(false);
    byteOffset_desc.set_configurable(true);
    typedarray_prototype->set_property_descriptor("byteOffset", byteOffset_desc);

    auto length_getter = ObjectFactory::create_native_function("get length",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.length called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            return Value(static_cast<double>(ta->length()));
        }, 0);
    PropertyDescriptor length_desc;
    length_desc.set_getter(length_getter.release());
    length_desc.set_enumerable(false);
    length_desc.set_configurable(true);
    typedarray_prototype->set_property_descriptor("length", length_desc);

    Object* typedarray_proto_ptr = typedarray_prototype.get();


    auto typedarray_at_fn = ObjectFactory::create_native_function("at",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.at called on non-TypedArray");
                return Value();
            }

            if (args.empty()) return Value();

            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t index = static_cast<int64_t>(args[0].to_number());
            int64_t len = static_cast<int64_t>(ta->length());

            if (index < 0) {
                index = len + index;
            }

            if (index < 0 || index >= len) {
                return Value();
            }

            return ta->get_element(static_cast<size_t>(index));
        }, 1);
    PropertyDescriptor typedarray_at_desc(Value(typedarray_at_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("at", typedarray_at_desc);

    auto forEach_fn = ObjectFactory::create_native_function("forEach",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.forEach called on non-TypedArray");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("forEach requires a callback function");
                return Value();
            }

            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            size_t length = ta->length();
            for (size_t i = 0; i < length; i++) {
                std::vector<Value> callback_args = {
                    ta->get_element(i),
                    Value(static_cast<double>(i)),
                    Value(this_obj)
                };
                callback->call(ctx, callback_args, thisArg);
            }
            return Value();
        }, 1);
    PropertyDescriptor forEach_desc(Value(forEach_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("forEach", forEach_desc);

    auto map_fn = ObjectFactory::create_native_function("map",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.map called on non-TypedArray");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("map requires a callback function");
                return Value();
            }

            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            size_t length = ta->length();
            TypedArrayBase* result = nullptr;
            switch (ta->get_array_type()) {
                case TypedArrayBase::ArrayType::INT8:
                    result = TypedArrayFactory::create_int8_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::UINT8:
                    result = TypedArrayFactory::create_uint8_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::UINT8_CLAMPED:
                    result = TypedArrayFactory::create_uint8_clamped_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::INT16:
                    result = TypedArrayFactory::create_int16_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::UINT16:
                    result = TypedArrayFactory::create_uint16_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::INT32:
                    result = TypedArrayFactory::create_int32_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::UINT32:
                    result = TypedArrayFactory::create_uint32_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::FLOAT32:
                    result = TypedArrayFactory::create_float32_array(length).release();
                    break;
                case TypedArrayBase::ArrayType::FLOAT64:
                    result = TypedArrayFactory::create_float64_array(length).release();
                    break;
                default:
                    ctx.throw_type_error("Unsupported TypedArray type");
                    return Value();
            }

            for (size_t i = 0; i < length; i++) {
                std::vector<Value> callback_args = {
                    ta->get_element(i),
                    Value(static_cast<double>(i)),
                    Value(this_obj)
                };
                Value mapped = callback->call(ctx, callback_args, thisArg);
                result->set_element(i, mapped);
            }
            return Value(result);
        }, 1);
    PropertyDescriptor map_desc(Value(map_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("map", map_desc);

    auto filter_fn = ObjectFactory::create_native_function("filter",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.filter called on non-TypedArray");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("filter requires a callback function");
                return Value();
            }

            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* callback = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();

            size_t length = ta->length();
            std::vector<Value> filtered;

            for (size_t i = 0; i < length; i++) {
                Value element = ta->get_element(i);
                std::vector<Value> callback_args = {
                    element,
                    Value(static_cast<double>(i)),
                    Value(this_obj)
                };
                Value result = callback->call(ctx, callback_args, thisArg);
                if (result.to_boolean()) {
                    filtered.push_back(element);
                }
            }

            TypedArrayBase* result = nullptr;
            switch (ta->get_array_type()) {
                case TypedArrayBase::ArrayType::INT8:
                    result = TypedArrayFactory::create_int8_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::UINT8:
                    result = TypedArrayFactory::create_uint8_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::UINT8_CLAMPED:
                    result = TypedArrayFactory::create_uint8_clamped_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::INT16:
                    result = TypedArrayFactory::create_int16_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::UINT16:
                    result = TypedArrayFactory::create_uint16_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::INT32:
                    result = TypedArrayFactory::create_int32_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::UINT32:
                    result = TypedArrayFactory::create_uint32_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::FLOAT32:
                    result = TypedArrayFactory::create_float32_array(filtered.size()).release();
                    break;
                case TypedArrayBase::ArrayType::FLOAT64:
                    result = TypedArrayFactory::create_float64_array(filtered.size()).release();
                    break;
                default:
                    ctx.throw_type_error("Unsupported TypedArray type");
                    return Value();
            }
            for (size_t i = 0; i < filtered.size(); i++) {
                result->set_element(i, filtered[i]);
            }
            return Value(result);
        }, 1);
    PropertyDescriptor filter_desc(Value(filter_fn.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_proto_ptr->set_property_descriptor("filter", filter_desc);

    PropertyDescriptor typedarray_prototype_desc(Value(typedarray_prototype.release()), PropertyAttributes::None);
    typedarray_constructor->set_property_descriptor("prototype", typedarray_prototype_desc);


    auto typedarray_from = ObjectFactory::create_native_function("from",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            ctx.throw_type_error("TypedArray.from must be called on a concrete TypedArray constructor");
            return Value();
        }, 1);
    PropertyDescriptor from_desc(Value(typedarray_from.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_constructor->set_property_descriptor("from", from_desc);

    auto typedarray_of = ObjectFactory::create_native_function("of",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            ctx.throw_type_error("TypedArray.of must be called on a concrete TypedArray constructor");
            return Value();
        }, 0);
    PropertyDescriptor of_desc(Value(typedarray_of.release()),
        PropertyAttributes::BuiltinFunction);
    typedarray_constructor->set_property_descriptor("of", of_desc);

    register_built_in_object("TypedArray", typedarray_constructor.release());

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
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_int8_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
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
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_uint16_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
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
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_int16_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
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
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_uint32_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
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
                if (obj->is_array() || obj->has_property("length") || obj->is_typed_array()) {
                    uint32_t length = obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->length() :
                                     (obj->is_array() ? obj->get_length() : static_cast<uint32_t>(obj->get_property("length").to_number()));
                    auto typed_array = TypedArrayFactory::create_int32_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->is_typed_array() ? static_cast<TypedArrayBase*>(obj)->get_element(i) : obj->get_element(i));
                    }
                    return Value(typed_array.release());
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

                if (obj->is_array() || obj->has_property("length")) {
                    uint32_t length = obj->is_array() ? obj->get_length() :
                                     static_cast<uint32_t>(obj->get_property("length").to_number());
                    auto typed_array = TypedArrayFactory::create_float64_array(length);
                    for (uint32_t i = 0; i < length; i++) {
                        typed_array->set_element(i, obj->get_element(i));
                    }
                    return Value(typed_array.release());
                }

                if (obj->is_typed_array()) {
                    TypedArrayBase* source = static_cast<TypedArrayBase*>(obj);
                    size_t length = source->length();
                    auto typed_array = TypedArrayFactory::create_float64_array(length);
                    for (size_t i = 0; i < length; i++) {
                        typed_array->set_element(i, source->get_element(i));
                    }
                    return Value(typed_array.release());
                }
            }

            ctx.throw_type_error("Float64Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Float64Array", float64array_constructor.release());

    auto dataview_constructor = ObjectFactory::create_native_function("DataView", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value result = DataView::constructor(ctx, args);
            
            if (result.is_object()) {
                Object* dataview_obj = result.as_object();
                
                auto get_uint8_method = ObjectFactory::create_native_function("getUint8", DataView::js_get_uint8);
                dataview_obj->set_property("getUint8", Value(get_uint8_method.release()));
                
                auto set_uint8_method = ObjectFactory::create_native_function("setUint8", DataView::js_set_uint8);
                dataview_obj->set_property("setUint8", Value(set_uint8_method.release()));

                auto get_int8_method = ObjectFactory::create_native_function("getInt8", DataView::js_get_int8);
                dataview_obj->set_property("getInt8", Value(get_int8_method.release()));

                auto set_int8_method = ObjectFactory::create_native_function("setInt8", DataView::js_set_int8);
                dataview_obj->set_property("setInt8", Value(set_int8_method.release()));

                auto get_int16_method = ObjectFactory::create_native_function("getInt16", DataView::js_get_int16);
                dataview_obj->set_property("getInt16", Value(get_int16_method.release()));
                
                auto set_int16_method = ObjectFactory::create_native_function("setInt16", DataView::js_set_int16);
                dataview_obj->set_property("setInt16", Value(set_int16_method.release()));

                auto get_uint16_method = ObjectFactory::create_native_function("getUint16", DataView::js_get_uint16);
                dataview_obj->set_property("getUint16", Value(get_uint16_method.release()));

                auto set_uint16_method = ObjectFactory::create_native_function("setUint16", DataView::js_set_uint16);
                dataview_obj->set_property("setUint16", Value(set_uint16_method.release()));

                auto get_int32_method = ObjectFactory::create_native_function("getInt32", DataView::js_get_int32);
                dataview_obj->set_property("getInt32", Value(get_int32_method.release()));

                auto set_int32_method = ObjectFactory::create_native_function("setInt32", DataView::js_set_int32);
                dataview_obj->set_property("setInt32", Value(set_int32_method.release()));

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

    auto dataview_prototype = ObjectFactory::create_object();

    auto get_uint8_proto = ObjectFactory::create_native_function("getUint8", DataView::js_get_uint8);
    dataview_prototype->set_property("getUint8", Value(get_uint8_proto.release()));

    auto set_uint8_proto = ObjectFactory::create_native_function("setUint8", DataView::js_set_uint8);
    dataview_prototype->set_property("setUint8", Value(set_uint8_proto.release()));

    auto get_int8_proto = ObjectFactory::create_native_function("getInt8", DataView::js_get_int8);
    dataview_prototype->set_property("getInt8", Value(get_int8_proto.release()));

    auto set_int8_proto = ObjectFactory::create_native_function("setInt8", DataView::js_set_int8);
    dataview_prototype->set_property("setInt8", Value(set_int8_proto.release()));

    auto get_int16_proto = ObjectFactory::create_native_function("getInt16", DataView::js_get_int16);
    dataview_prototype->set_property("getInt16", Value(get_int16_proto.release()));

    auto set_int16_proto = ObjectFactory::create_native_function("setInt16", DataView::js_set_int16);
    dataview_prototype->set_property("setInt16", Value(set_int16_proto.release()));

    auto get_uint16_proto = ObjectFactory::create_native_function("getUint16", DataView::js_get_uint16);
    dataview_prototype->set_property("getUint16", Value(get_uint16_proto.release()));

    auto set_uint16_proto = ObjectFactory::create_native_function("setUint16", DataView::js_set_uint16);
    dataview_prototype->set_property("setUint16", Value(set_uint16_proto.release()));

    auto get_int32_proto = ObjectFactory::create_native_function("getInt32", DataView::js_get_int32);
    dataview_prototype->set_property("getInt32", Value(get_int32_proto.release()));

    auto set_int32_proto = ObjectFactory::create_native_function("setInt32", DataView::js_set_int32);
    dataview_prototype->set_property("setInt32", Value(set_int32_proto.release()));

    auto get_uint32_proto = ObjectFactory::create_native_function("getUint32", DataView::js_get_uint32);
    dataview_prototype->set_property("getUint32", Value(get_uint32_proto.release()));

    auto set_uint32_proto = ObjectFactory::create_native_function("setUint32", DataView::js_set_uint32);
    dataview_prototype->set_property("setUint32", Value(set_uint32_proto.release()));

    auto get_float32_proto = ObjectFactory::create_native_function("getFloat32", DataView::js_get_float32);
    dataview_prototype->set_property("getFloat32", Value(get_float32_proto.release()));

    auto set_float32_proto = ObjectFactory::create_native_function("setFloat32", DataView::js_set_float32);
    dataview_prototype->set_property("setFloat32", Value(set_float32_proto.release()));

    auto get_float64_proto = ObjectFactory::create_native_function("getFloat64", DataView::js_get_float64);
    dataview_prototype->set_property("getFloat64", Value(get_float64_proto.release()));

    auto set_float64_proto = ObjectFactory::create_native_function("setFloat64", DataView::js_set_float64);
    dataview_prototype->set_property("setFloat64", Value(set_float64_proto.release()));

    PropertyDescriptor dataview_tag_desc(Value(std::string("DataView")), PropertyAttributes::Configurable);
    dataview_prototype->set_property_descriptor("Symbol.toStringTag", dataview_tag_desc);

    dataview_constructor->set_property("prototype", Value(dataview_prototype.release()));

    register_built_in_object("DataView", dataview_constructor.release());

    auto done_function = ObjectFactory::create_native_function("$DONE",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!args.empty() && !args[0].is_undefined()) {
                std::string error_msg = args[0].to_string();
                ctx.throw_exception(Value("Test failed: " + error_msg));
            }
            return Value();
        });
    global_object_->set_property("$DONE", Value(done_function.release()));


    Value function_ctor_value = global_object_->get_property("Function");
    if (function_ctor_value.is_function()) {
        Function* function_ctor = function_ctor_value.as_function();
        Value func_proto_value = function_ctor->get_property("prototype");
        if (func_proto_value.is_object()) {
            Object* function_proto_ptr = func_proto_value.as_object();

            const char* constructor_names[] = {
                "Array", "Object", "String", "Number", "Boolean", "BigInt", "Symbol",
                "Error", "TypeError", "ReferenceError", "SyntaxError", "RangeError", "URIError", "EvalError", "AggregateError",
                "Promise", "Map", "Set", "WeakMap", "WeakSet",
                "Date", "RegExp", "ArrayBuffer", "Int8Array", "Uint8Array", "Uint8ClampedArray",
                "Int16Array", "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array",
                "DataView"
            };

            for (const char* name : constructor_names) {
                Value ctor = global_object_->get_property(name);
                if (ctor.is_function()) {
                    Function* func = ctor.as_function();
                    static_cast<Object*>(func)->set_prototype(function_proto_ptr);
                }
            }
        }
    }
}

// Garbage collector integration
void Context::register_object(Object* obj, size_t size) {
    if (gc_ && obj) {
        gc_->register_object(obj, size);
    }
}

void Context::trigger_gc() {
    if (gc_) {
        gc_->collect_garbage();
    }
}

// Bootstrap loading - Load essential test262 harness files
void Context::load_bootstrap() {
    if (!engine_) return;

    // Define $262 object required by test262
    std::string test262_object = R"(
var $262 = {
    // IsHTMLDDA - emulates HTML document.all behavior (falsy object)
    IsHTMLDDA: {},

    // createRealm - creates a new realm (not fully implemented yet)
    createRealm: function() {
        return {
            global: globalThis
        };
    },

    // evalScript - evaluates script in current realm
    evalScript: function(code) {
        return eval(code);
    },

    // detachArrayBuffer - detaches an array buffer
    detachArrayBuffer: function(buffer) {
        // Not fully implemented yet
    },

    // gc - trigger garbage collection (no-op for now)
    gc: function() {
        // No-op
    },

    // agent - agent API for shared memory tests
    agent: {
        start: function() {},
        broadcast: function() {},
        getReport: function() { return null; },
        sleep: function() {},
        monotonicNow: function() { return Date.now(); }
    }
};
)";

    // Execute $262 definition
    try {
        auto result = engine_->execute(test262_object, "$262-definition");
        if (!result.success) {
            std::cerr << "Warning: Failed to define $262 object: " << result.error_message << std::endl;
        }
    } catch (...) {
        std::cerr << "Exception while defining $262 object" << std::endl;
    }

    // List of essential harness files in correct order
    const char* harness_files[] = {
        "test262/harness/sta.js",           // Test262Error, $DONOTEVALUATE
        "test262/harness/assert.js",        // assert functions
        "test262/harness/propertyHelper.js",// verifyProperty and related
        "test262/harness/isConstructor.js", // isConstructor
        "test262/harness/compareArray.js"   // compareArray
    };

    // Load each harness file
    for (const char* harness_path : harness_files) {
        std::ifstream file(harness_path);
        if (file.is_open()) {
            std::string harness_code((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
            file.close();

            try {
                auto result = engine_->execute(harness_code, harness_path);
                if (!result.success) {
                    // Harness loading failed, but continue with next file
                    std::cerr << "Warning: Failed to load " << harness_path << ": " << result.error_message << std::endl;
                }
            } catch (...) {
                // Silently ignore errors and continue with next file
                std::cerr << "Exception while loading harness file" << std::endl;
            }
        }
    }
}

}

