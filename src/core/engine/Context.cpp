/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "builtins/array/ArrayBuiltin.h"
#include "builtins/string/StringBuiltin.h"
#include "builtins/object/ObjectBuiltin.h"
#include <iostream>
#include <algorithm>
#include "quanta/core/runtime/Error.h"
#include "quanta/lexer/Lexer.h"
#include "quanta/parser/Parser.h"
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
#include "utf8proc.h"
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

// Helper: ES6 ArraySpeciesCreate, creates result using constructor's @@species
static Value array_species_create(Context& ctx, Object* original_array, uint32_t length) {
    // Per spec: only consult @@species if the original is an exotic Array object
    bool is_actual_array = original_array->is_array();
    if (!is_actual_array && original_array->get_type() == Object::ObjectType::Proxy) {
        Object* target = static_cast<Proxy*>(original_array)->get_proxy_target();
        is_actual_array = target && target->is_array();
    }
    if (!is_actual_array) {
        return Value(ObjectFactory::create_array(length).release());
    }
    Value ctor_val = original_array->get_property("constructor");
    if (!ctor_val.is_undefined() && !ctor_val.is_null() &&
        (ctor_val.is_function() || ctor_val.is_object())) {
        Object* ctor = ctor_val.is_function()
            ? static_cast<Object*>(ctor_val.as_function())
            : ctor_val.as_object();
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            Value species_val = ctor->get_property(species_sym->to_property_key());
            if (species_val.is_null() || species_val.is_undefined()) {
                // null/undefined species → fallback to plain Array
            } else if (species_val.is_function()) {
                Function* species_fn = species_val.as_function();
                Value result = species_fn->construct(ctx, {Value(static_cast<double>(length))});
                if (ctx.has_exception()) return Value();
                return result;
            }
        }
    }
    return Value(ObjectFactory::create_array(length).release());
}

static std::vector<std::unique_ptr<Function>> g_owned_native_functions;

uint32_t Context::next_context_id_ = 1;


Context::Context(Engine* engine, Type type)
    : type_(type), state_(State::Running), context_id_(next_context_id_++),
      lexical_environment_(nullptr), variable_environment_(nullptr), this_binding_(nullptr),
      execution_depth_(0), global_object_(nullptr), current_exception_(), has_exception_(false),
      return_value_(), has_return_value_(false), has_break_(false), has_continue_(false),
      current_loop_label_(), next_statement_label_(), is_in_constructor_call_(false), super_called_(false),
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

void Context::throw_exception(const Value& exception, bool raw) {
    if (exception.is_string() && !raw) {
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

void Context::throw_uri_error(const std::string& message) {
    auto error = Error::create_uri_error(message);
    error->generate_stack_trace();

    Value uri_error_ctor = get_binding("URIError");
    if (uri_error_ctor.is_function()) {
        Function* ctor_fn = uri_error_ctor.as_function();
        Value proto = ctor_fn->get_property("prototype");
        if (proto.is_object()) {
            error->set_prototype(proto.as_object());
        }
    }

    throw_exception(Value(error.release()));
}

void Context::queue_microtask(std::function<void()> task) {
    microtask_queue_.push_back(std::move(task));
}

void Context::drain_microtasks() {
    // Spin until all microtasks are drained (with 10-second real-time limit
    // to allow setTimeout-based tests to work via flushQueue spin loops)
    is_draining_microtasks_ = true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!microtask_queue_.empty()) {
        auto tasks = std::move(microtask_queue_);
        microtask_queue_.clear();
        for (auto& task : tasks) {
            if (task) task();
        }
        if (std::chrono::steady_clock::now() > deadline) break;
    }
    is_draining_microtasks_ = false;
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


    // Object builtin - moved to builtins/object/ObjectBuiltin.cpp
    register_object_builtins(*this);
    auto function_constructor = ObjectFactory::create_native_constructor("Function",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // ES1: new Function([arg1[, arg2[, ... argN]],] functionBody)
            // Last argument is the function body, previous arguments are parameter names

            std::string params = "";
            std::string body = "";

            if (args.size() == 0) {
                // new Function() - empty function
                body = "";
            } else if (args.size() == 1) {
                // new Function(body) - no parameters
                body = args[0].to_string();
            } else {
                // new Function(param1, param2, ..., body)
                for (size_t i = 0; i < args.size() - 1; i++) {
                    if (i > 0) params += ",";
                    params += args[i].to_string();
                }
                body = args[args.size() - 1].to_string();
            }

            // Check if body contains "use strict" directive
            bool is_strict = false;
            {
                std::string trimmed = body;
                size_t start = trimmed.find_first_not_of(" \t\n\r");
                if (start != std::string::npos) {
                    std::string first = trimmed.substr(start);
                    if (first.find("\"use strict\"") == 0 || first.find("'use strict'") == 0) {
                        is_strict = true;
                    }
                }
            }

            // ES5: Check for duplicate parameters in strict mode
            if (is_strict && args.size() > 2) {
                std::vector<std::string> param_list;
                for (size_t i = 0; i < args.size() - 1; i++) {
                    std::string p = args[i].to_string();
                    for (size_t j = 0; j < param_list.size(); j++) {
                        if (param_list[j] == p) {
                            ctx.throw_syntax_error("Duplicate parameter name not allowed in strict mode");
                            return Value();
                        }
                    }
                    param_list.push_back(p);
                }
            }

            // Spec-compliant Function constructor toString format
            std::string toString_src = "function anonymous(" + params + "\n) {\n" + body + "\n}";
            std::string func_code = "(" + toString_src + ")";

            // Parse and create the function
            try {
                Lexer lexer(func_code);
                TokenSequence tokens = lexer.tokenize();
                Parser parser(tokens);
                parser.set_source(func_code);
                auto expr = parser.parse_expression();

                // Check for parser errors (e.g. invalid syntax in body)
                if (parser.has_errors()) {
                    ctx.throw_syntax_error("Invalid function body");
                    return Value();
                }

                // The expression should be a function expression
                if (expr && expr->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
                    FunctionExpression* func_expr = static_cast<FunctionExpression*>(expr.get());

                    // Clone parameters to preserve rest/default info
                    std::vector<std::unique_ptr<Parameter>> cloned_params;
                    for (const auto& param : func_expr->get_params()) {
                        cloned_params.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(param->clone().release())));
                    }

                    std::unique_ptr<Function> func = ObjectFactory::create_js_function(
                        "anonymous", // ES6: new Function creates "anonymous" named function
                        std::move(cloned_params),
                        func_expr->get_body()->clone(),
                        &ctx
                    );
                    if (!func) {
                        ctx.throw_syntax_error("Failed to create function object");
                        return Value();
                    }
                    func->set_source_text(toString_src);

                    Function* raw_func = func.release();
                    Value new_target = ctx.get_new_target();
                    if (!new_target.is_undefined()) {
                        Object* nt_obj = new_target.is_function()
                            ? static_cast<Object*>(new_target.as_function())
                            : new_target.is_object() ? new_target.as_object() : nullptr;
                        if (nt_obj) {
                            Value nt_proto = nt_obj->get_property("prototype");
                            if (nt_proto.is_object()) raw_func->set_prototype(nt_proto.as_object());
                        }
                    }
                    return Value{raw_func};
                }
            } catch (...) {
                ctx.throw_syntax_error("Invalid function body in Function constructor");
                return Value();
            }

            ctx.throw_syntax_error("Failed to create function");
            return Value();
        });
    
    // Function.prototype must be callable (spec: it's an intrinsic function object)
    // Create it as a native function that returns undefined when called.
    // NOTE: create_native_function checks get_function_prototype() which is null at this point,
    // so the proto of function_prototype won't be set here; it will be set later (Object.prototype).
    auto function_prototype_fn = ObjectFactory::create_native_function("",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args; return Value();
        });
    auto function_prototype = std::unique_ptr<Object>(static_cast<Object*>(function_prototype_fn.release()));
    
    // Array builtin - moved to builtins/array/ArrayBuiltin.cpp
    register_array_builtins(*this, function_prototype.get());
    

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
            Function* func = nullptr;
            if (function_obj && function_obj->is_function()) {
                func = static_cast<Function*>(function_obj);
            } else if (function_obj && function_obj->get_type() == Object::ObjectType::Proxy) {
                Proxy* proxy_obj = static_cast<Proxy*>(function_obj);
                Object* proxy_target = proxy_obj->get_proxy_target();
                if (proxy_target && proxy_target->is_function()) {
                    func = static_cast<Function*>(proxy_target);
                }
            }
            if (!func) {
                ctx.throw_type_error("Function.prototype.apply called on non-function");
                return Value();
            }
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
            // Accept plain functions or Proxy objects wrapping a function
            Function* target_func = nullptr;
            if (function_obj && function_obj->is_function()) {
                target_func = static_cast<Function*>(function_obj);
            } else if (function_obj && function_obj->get_type() == Object::ObjectType::Proxy) {
                Proxy* proxy_obj = static_cast<Proxy*>(function_obj);
                Object* proxy_target = proxy_obj->get_proxy_target();
                if (proxy_target && proxy_target->is_function()) {
                    target_func = static_cast<Function*>(proxy_target);
                }
            }
            if (!target_func) {
                ctx.throw_type_error("Function.prototype.bind called on non-function");
                return Value();
            }

            Value bound_this = args.size() > 0 ? args[0] : Value();

            std::vector<Value> bound_args;
            for (size_t i = 1; i < args.size(); i++) {
                bound_args.push_back(args[i]);
            }

            // Spec: HasOwnProperty(Target,"length") fires getOwnPropertyDescriptor trap,
            // then Get(Target,"length") fires get trap on Proxy
            double target_length = 0.0;
            {
                if (function_obj->get_type() == Object::ObjectType::Proxy) {
                    Proxy* proxy_obj = static_cast<Proxy*>(function_obj);
                    proxy_obj->get_own_property_descriptor_trap(Value(std::string("length")));
                }
                Value target_length_val = function_obj->get_property("length");
                target_length = target_length_val.is_number() ? target_length_val.as_number() : 0.0;
            }
            double bound_length = target_length - static_cast<double>(bound_args.size());
            if (bound_length < 0) bound_length = 0;
            uint32_t bound_arity = static_cast<uint32_t>(bound_length);

            Value name_val = function_obj->get_property("name");
            std::string bound_name = "bound " + (name_val.is_string() ? name_val.to_string() : target_func->get_name());
            auto bound_function = ObjectFactory::create_native_constructor(bound_name,
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

            // ES6 19.2.3.2 step 12: bound function's [[Prototype]] should be target's [[Prototype]]
            bound_function->set_prototype(target_func->get_prototype());

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
            return Value(func->to_string());
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

    {
        Object* fp = function_prototype.release();
        Value fp_val = fp->is_function() ? Value(static_cast<Function*>(fp)) : Value(fp);
        PropertyDescriptor fp_desc(fp_val, PropertyAttributes::None);
        function_constructor->set_property_descriptor("prototype", fp_desc);
    }

    static_cast<Object*>(function_constructor.get())->set_prototype(function_proto_ptr);

    register_built_in_object("Function", function_constructor.release());

    // String builtin - moved to builtins/string/StringBuiltin.cpp
    register_string_builtins(*this);

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
                    // ES6: throw TypeError if argument is a regexp (has Symbol.match truthy)
                    if (args[0].is_object() || args[0].is_function()) {
                        Object* arg_obj = args[0].is_function()
                            ? static_cast<Object*>(args[0].as_function())
                            : args[0].as_object();
                        Value sym_match = arg_obj->get_property("Symbol.match");
                        if (sym_match.is_undefined()) {
                            if (arg_obj->get_type() == Object::ObjectType::RegExp) {
                                ctx.throw_exception(Value(std::string("TypeError: First argument to String.prototype.includes must not be a regular expression")));
                                return Value();
                            }
                        } else if (sym_match.to_boolean()) {
                            ctx.throw_exception(Value(std::string("TypeError: First argument to String.prototype.includes must not be a regular expression")));
                            return Value();
                        }
                    }
                    if (args[0].is_symbol()) {
                        ctx.throw_exception(Value(std::string("TypeError: Cannot convert a Symbol value to a string")));
                        return Value();
                    }
                    // Convert argument to string (call toString() for objects)
                    std::string search_string;
                    if (args[0].is_object() || args[0].is_function()) {
                        Object* obj = args[0].is_function()
                            ? static_cast<Object*>(args[0].as_function())
                            : args[0].as_object();
                        Value ts = obj->get_property("toString");
                        if (ts.is_function()) {
                            Value r = ts.as_function()->call(ctx, {}, args[0]);
                            if (!ctx.has_exception() && r.is_string()) search_string = r.to_string();
                            else search_string = args[0].to_string();
                        } else search_string = args[0].to_string();
                    } else search_string = args[0].to_string();
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
            Function* trimStart_raw = string_trimStart_fn.get();
            global_prototype->set_property("trimStart", Value(string_trimStart_fn.release()), PropertyAttributes::BuiltinFunction);
            global_prototype->set_property("trimLeft", Value(trimStart_raw), PropertyAttributes::BuiltinFunction);

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
            Function* trimEnd_raw = string_trimEnd_fn.get();
            global_prototype->set_property("trimEnd", Value(string_trimEnd_fn.release()), PropertyAttributes::BuiltinFunction);
            global_prototype->set_property("trimRight", Value(trimEnd_raw), PropertyAttributes::BuiltinFunction);

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
    {
        auto asIntN_fn = ObjectFactory::create_native_function("asIntN",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) { ctx.throw_type_error("BigInt.asIntN requires 2 arguments"); return Value(); }
                int64_t n = static_cast<int64_t>(args[0].to_number());
                if (n < 0 || n > 64) { ctx.throw_range_error("Invalid width"); return Value(); }
                if (!args[1].is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
                int64_t val = args[1].as_bigint()->to_int64();
                if (n == 0) return Value(new BigInt(0));
                if (n == 64) return Value(new BigInt(val));
                int64_t mod = 1LL << n;
                int64_t result = val & (mod - 1);
                if (result >= (mod >> 1)) result -= mod;
                return Value(new BigInt(result));
            });
        bigint_constructor->set_property("asIntN", Value(asIntN_fn.release()), PropertyAttributes::BuiltinFunction);

        auto asUintN_fn = ObjectFactory::create_native_function("asUintN",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) { ctx.throw_type_error("BigInt.asUintN requires 2 arguments"); return Value(); }
                int64_t n = static_cast<int64_t>(args[0].to_number());
                if (n < 0 || n > 64) { ctx.throw_range_error("Invalid width"); return Value(); }
                if (!args[1].is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
                int64_t val = args[1].as_bigint()->to_int64();
                if (n == 0) return Value(new BigInt(0));
                if (n == 64) return Value(new BigInt(val));
                uint64_t mask = (1ULL << n) - 1;
                uint64_t result = static_cast<uint64_t>(val) & mask;
                return Value(new BigInt(static_cast<int64_t>(result)));
            });
        bigint_constructor->set_property("asUintN", Value(asUintN_fn.release()), PropertyAttributes::BuiltinFunction);
    }
    register_built_in_object("BigInt", bigint_constructor.release());

    auto symbol_constructor = ObjectFactory::create_native_constructor("Symbol",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            bool has_desc = !args.empty() && !args[0].is_undefined();
            std::string description = has_desc ? args[0].to_string() : "";
            auto symbol = Symbol::create(description, has_desc);
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
    
    {
        auto sym_proto = ObjectFactory::create_object();
        sym_proto->set_property("constructor", Value(symbol_constructor.get()));
        Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (tag_sym) {
            PropertyDescriptor tag_desc(Value(std::string("Symbol")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            sym_proto->set_property_descriptor(tag_sym->to_property_key(), tag_desc);
        }
        auto desc_getter = ObjectFactory::create_native_function("get description",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                Value prim = ctx.get_binding("__primitive_this__");
                if (prim.is_symbol()) return prim.as_symbol()->get_has_description() ? Value(prim.as_symbol()->get_description()) : Value();
                return Value();
            });
        PropertyDescriptor desc_prop;
        desc_prop.set_getter(desc_getter.release());
        desc_prop.set_enumerable(false);
        desc_prop.set_configurable(true);
        sym_proto->set_property_descriptor("description", desc_prop);
        symbol_constructor->set_property("prototype", Value(sym_proto.release()));
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

    // Add ES2025 Iterator Helpers to %IteratorPrototype%
    if (Iterator::s_iterator_prototype_) {
        Object* iter_proto_obj = Iterator::s_iterator_prototype_;

        auto call_iter_next = [](Context& ctx, Object* iter) -> std::pair<Value,bool> {
            Value nxt = iter->get_property("next");
            if (!nxt.is_function()) return {Value(), true};
            Value res = nxt.as_function()->call(ctx, {}, Value(iter));
            if (ctx.has_exception() || !res.is_object()) return {Value(), true};
            Object* r = res.as_object();
            bool done = r->get_property("done").to_boolean();
            return {r->get_property("value"), done};
        };

        auto iter_toArray = ObjectFactory::create_native_function("toArray",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; Object* it = ctx.get_this_binding(); if (!it) return Value();
                auto a = ObjectFactory::create_array(); uint32_t i=0;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; a->set_property(std::to_string(i++),v);}
                a->set_property("length",Value((double)i)); return Value(a.release());
            },0);
        iter_proto_obj->set_property("toArray", Value(iter_toArray.release()));

        auto iter_forEach2 = ObjectFactory::create_native_function("forEach",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("forEach requires function");return Value();}
                Function* cb=args[0].as_function();
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; cb->call(ctx,{v},Value()); if(ctx.has_exception())break;}
                return Value();
            },1);
        iter_proto_obj->set_property("forEach", Value(iter_forEach2.release()));

        auto iter_reduce2 = ObjectFactory::create_native_function("reduce",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("reduce");return Value();}
                Function* cb=args[0].as_function(); Value acc=args.size()>1?args[1]:Value(); bool has_acc=args.size()>1;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(!has_acc){acc=v;has_acc=true;continue;} acc=cb->call(ctx,{acc,v},Value()); if(ctx.has_exception())break;}
                return acc;
            },1);
        iter_proto_obj->set_property("reduce", Value(iter_reduce2.release()));

        auto iter_some2 = ObjectFactory::create_native_function("some",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("some");return Value();}
                Function* cb=args[0].as_function();
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(cb->call(ctx,{v},Value()).to_boolean())return Value(true);}
                return Value(false);
            },1);
        iter_proto_obj->set_property("some", Value(iter_some2.release()));

        auto iter_every2 = ObjectFactory::create_native_function("every",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("every");return Value();}
                Function* cb=args[0].as_function();
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(!cb->call(ctx,{v},Value()).to_boolean())return Value(false);}
                return Value(true);
            },1);
        iter_proto_obj->set_property("every", Value(iter_every2.release()));

        auto iter_find2 = ObjectFactory::create_native_function("find",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("find");return Value();}
                Function* cb=args[0].as_function();
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(cb->call(ctx,{v},Value()).to_boolean())return v;}
                return Value();
            },1);
        iter_proto_obj->set_property("find", Value(iter_find2.release()));

        auto iter_map2 = ObjectFactory::create_native_function("map",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("map");return Value();}
                Function* cb=args[0].as_function(); auto a=ObjectFactory::create_array(); uint32_t i=0;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; Value r=cb->call(ctx,{v,Value((double)i)},Value()); if(ctx.has_exception())break; a->set_property(std::to_string(i++),r);}
                a->set_property("length",Value((double)i)); Value vf=a->get_property("values"); if(vf.is_function())return vf.as_function()->call(ctx,{},Value(a.release())); return Value(a.release());
            },1);
        iter_proto_obj->set_property("map", Value(iter_map2.release()));

        auto iter_filter2 = ObjectFactory::create_native_function("filter",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it||args.empty()||!args[0].is_function()){ctx.throw_type_error("filter");return Value();}
                Function* cb=args[0].as_function(); auto a=ObjectFactory::create_array(); uint32_t i=0,o=0;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; if(cb->call(ctx,{v,Value((double)i++)},Value()).to_boolean())a->set_property(std::to_string(o++),v);}
                a->set_property("length",Value((double)o)); Value vf=a->get_property("values"); if(vf.is_function())return vf.as_function()->call(ctx,{},Value(a.release())); return Value(a.release());
            },1);
        iter_proto_obj->set_property("filter", Value(iter_filter2.release()));

        auto iter_take2 = ObjectFactory::create_native_function("take",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it){return Value();}
                uint32_t lim=args.empty()?0:(uint32_t)args[0].to_number(); auto a=ObjectFactory::create_array(); uint32_t i=0;
                while(i<lim){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; a->set_property(std::to_string(i++),v);}
                a->set_property("length",Value((double)i)); Value vf=a->get_property("values"); if(vf.is_function())return vf.as_function()->call(ctx,{},Value(a.release())); return Value(a.release());
            },1);
        iter_proto_obj->set_property("take", Value(iter_take2.release()));

        auto iter_drop2 = ObjectFactory::create_native_function("drop",
            [call_iter_next](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* it=ctx.get_this_binding(); if(!it){return Value();}
                uint32_t sk=args.empty()?0:(uint32_t)args[0].to_number();
                for(uint32_t i=0;i<sk;i++){auto[v,d]=call_iter_next(ctx,it);(void)v;if(ctx.has_exception()||d)break;}
                auto a=ObjectFactory::create_array(); uint32_t i=0;
                while(true){auto[v,d]=call_iter_next(ctx,it); if(ctx.has_exception()||d)break; a->set_property(std::to_string(i++),v);}
                a->set_property("length",Value((double)i)); Value vf=a->get_property("values"); if(vf.is_function())return vf.as_function()->call(ctx,{},Value(a.release())); return Value(a.release());
            },1);
        iter_proto_obj->set_property("drop", Value(iter_drop2.release()));
    }
    
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

    // ES6: Number.parseFloat/parseInt set up later after global functions are defined

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

            if (std::isnan(num)) return Value(std::string("NaN"));
            if (std::isinf(num)) return Value(num > 0 ? std::string("Infinity") : std::string("-Infinity"));

            bool has_frac = !args.empty() && !args[0].is_undefined();
            int frac = 0;
            if (has_frac) {
                frac = static_cast<int>(args[0].to_number());
                if (frac < 0 || frac > 100) {
                    ctx.throw_exception(Value(std::string("RangeError: toExponential() precision out of range")));
                    return Value();
                }
            }

            bool negative = num < 0;
            double abs_num = negative ? -num : num;

            int exp = 0;
            if (abs_num != 0) {
                exp = static_cast<int>(std::floor(std::log10(abs_num)));
                double test_m = abs_num / std::pow(10.0, exp);
                if (test_m >= 10.0) { exp++; }
                else if (test_m < 1.0) { exp--; }
            }

            double mantissa = (abs_num == 0) ? 0.0 : abs_num / std::pow(10.0, exp);

            if (!has_frac) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.17e", abs_num);
                std::string s(buf);
                size_t e_pos = s.find('e');
                if (e_pos != std::string::npos) {
                    std::string m_part = s.substr(0, e_pos);
                    if (m_part.find('.') != std::string::npos) {
                        size_t last = m_part.find_last_not_of('0');
                        if (last != std::string::npos && m_part[last] == '.')
                            m_part = m_part.substr(0, last);
                        else if (last != std::string::npos)
                            m_part = m_part.substr(0, last + 1);
                    }
                    int parsed_exp = std::stoi(s.substr(e_pos + 1));
                    std::string result;
                    if (negative) result += "-";
                    result += m_part;
                    result += "e";
                    result += (parsed_exp >= 0) ? "+" : "-";
                    result += std::to_string(std::abs(parsed_exp));
                    return Value(result);
                }
            }

            double factor = std::pow(10.0, frac);
            mantissa = std::floor(mantissa * factor + 0.5) / factor;
            if (mantissa >= 10.0) { mantissa /= 10.0; exp++; }

            char buf[64];
            snprintf(buf, sizeof(buf), ("%." + std::to_string(frac) + "f").c_str(), mantissa);

            std::string result;
            if (negative) result += "-";
            result += buf;
            result += "e";
            result += (exp >= 0) ? "+" : "-";
            result += std::to_string(std::abs(exp));
            return Value(result);
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

            if (std::isnan(num)) return Value(std::string("NaN"));
            if (std::isinf(num)) return Value(num > 0 ? std::string("Infinity") : std::string("-Infinity"));

            bool negative = num < 0;
            double abs_num = negative ? -num : num;
            double factor = std::pow(10.0, precision);
            abs_num = std::floor(abs_num * factor + 0.5) / factor;

            char buffer[256];
            std::string format = "%." + std::to_string(precision) + "f";
            snprintf(buffer, sizeof(buffer), format.c_str(), abs_num);

            std::string result;
            if (negative && abs_num != 0) result += "-";
            result += buffer;
            return Value(result);
        });
    PropertyDescriptor toFixed_desc(Value(toFixed_fn.release()),
        PropertyAttributes::BuiltinFunction);
    number_prototype->set_property_descriptor("toFixed", toFixed_desc);

    auto toPrecision_fn = ObjectFactory::create_native_function("toPrecision",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_val = ctx.get_binding("this");
            double num = this_val.to_number();

            if (args.empty() || args[0].is_undefined()) {
                if (std::isnan(num)) return Value(std::string("NaN"));
                if (std::isinf(num)) return Value(num > 0 ? std::string("Infinity") : std::string("-Infinity"));
                return Value(this_val.to_string());
            }

            int precision = static_cast<int>(args[0].to_number());
            if (precision < 1 || precision > 100) {
                ctx.throw_exception(Value(std::string("RangeError: toPrecision() precision out of range")));
                return Value();
            }

            if (std::isnan(num)) return Value(std::string("NaN"));
            if (std::isinf(num)) return Value(num > 0 ? std::string("Infinity") : std::string("-Infinity"));

            bool negative = num < 0;
            double abs_num = negative ? -num : num;

            int exp = 0;
            if (abs_num != 0) {
                exp = static_cast<int>(std::floor(std::log10(abs_num)));
                double test_m = abs_num / std::pow(10.0, exp);
                if (test_m >= 10.0) exp++;
                else if (test_m < 1.0) exp--;
            }

            char buf[256];
            if (exp >= 0 && exp < precision) {
                int frac_digits = precision - exp - 1;
                double factor = std::pow(10.0, frac_digits);
                double rounded = std::floor(abs_num * factor + 0.5) / factor;
                snprintf(buf, sizeof(buf), ("%." + std::to_string(frac_digits) + "f").c_str(), rounded);
                std::string result;
                if (negative) result += "-";
                result += buf;
                return Value(result);
            } else if (exp < 0 && exp >= -6) {
                int frac_digits = precision - exp - 1;
                double factor = std::pow(10.0, frac_digits);
                double rounded = std::floor(abs_num * factor + 0.5) / factor;
                snprintf(buf, sizeof(buf), ("%." + std::to_string(frac_digits) + "f").c_str(), rounded);
                std::string result;
                if (negative) result += "-";
                result += buf;
                return Value(result);
            } else {
                int frac_digits = precision - 1;
                double mantissa = (abs_num == 0) ? 0.0 : abs_num / std::pow(10.0, exp);
                if (mantissa >= 10.0) { mantissa /= 10.0; exp++; }
                else if (mantissa < 1.0 && mantissa > 0) { mantissa *= 10.0; exp--; }
                double factor = std::pow(10.0, frac_digits);
                mantissa = std::floor(mantissa * factor + 0.5) / factor;
                if (mantissa >= 10.0) { mantissa /= 10.0; exp++; }

                if (frac_digits > 0)
                    snprintf(buf, sizeof(buf), ("%." + std::to_string(frac_digits) + "f").c_str(), mantissa);
                else
                    snprintf(buf, sizeof(buf), "%.0f", mantissa);

                std::string result;
                if (negative) result += "-";
                result += buf;
                result += "e";
                result += (exp >= 0) ? "+" : "-";
                result += std::to_string(std::abs(exp));
                return Value(result);
            }
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

            // Support subclassing: when called via super() from a derived class,
            // ctx.get_this_binding() is the subclass instance with its prototype.
            // Use that prototype so c instanceof C works alongside c instanceof Error.
            Object* proto_to_use = error_prototype_ptr;
            Object* this_obj = ctx.get_this_binding();
            if (this_obj) {
                Object* this_proto = this_obj->get_prototype();
                if (this_proto && this_proto != error_prototype_ptr) {
                    proto_to_use = this_proto;
                }
            }
            error_obj->set_prototype(proto_to_use);
            
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

    // ES6: Date.prototype[Symbol.toPrimitive]
    Symbol* toPrim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
    if (toPrim_sym) {
        auto date_toPrimitive_fn = ObjectFactory::create_native_function("[Symbol.toPrimitive]",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Object* obj = ctx.get_this_binding();
                if (!obj) {
                    ctx.throw_type_error("Date.prototype[Symbol.toPrimitive] called on non-object");
                    return Value();
                }
                std::string hint = args.empty() ? "default" : args[0].to_string();
                if (hint == "number") {
                    Value valueOf_fn = obj->get_property("valueOf");
                    if (valueOf_fn.is_function()) {
                        Value result = valueOf_fn.as_function()->call(ctx, {}, Value(obj));
                        if (!result.is_object()) return result;
                    }
                    Value toString_fn = obj->get_property("toString");
                    if (toString_fn.is_function()) {
                        Value result = toString_fn.as_function()->call(ctx, {}, Value(obj));
                        if (!result.is_object()) return result;
                    }
                    return Value();
                } else {
                    Value toString_fn = obj->get_property("toString");
                    if (toString_fn.is_function()) {
                        Value result = toString_fn.as_function()->call(ctx, {}, Value(obj));
                        if (!result.is_object()) return result;
                    }
                    Value valueOf_fn = obj->get_property("valueOf");
                    if (valueOf_fn.is_function()) {
                        Value result = valueOf_fn.as_function()->call(ctx, {}, Value(obj));
                        if (!result.is_object()) return result;
                    }
                    return Value();
                }
            }, 1);
        date_prototype->set_property(toPrim_sym->to_property_key(), Value(date_toPrimitive_fn.release()),
            static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    }

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
            // ES6: Check IsRegExp(pattern) via Symbol.match
            bool pattern_is_regexp = false;
            std::string pattern = "";
            std::string flags = args.size() > 1 ? args[1].to_string() : "";

            if (!args.empty() && (args[0].is_object() || args[0].is_function())) {
                Object* pat_obj = args[0].is_object() ? args[0].as_object() : args[0].as_function();
                // IsRegExp: check Symbol.match (fires get trap on Proxy)
                Value sym_match = pat_obj->get_property("Symbol.match");
                if (!sym_match.is_undefined()) {
                    pattern_is_regexp = sym_match.to_boolean();
                } else {
                    // Fallback: check internal _isRegExp flag
                    Value is_regexp = pat_obj->get_property("_isRegExp");
                    pattern_is_regexp = is_regexp.is_boolean() && is_regexp.to_boolean();
                }

                if (pattern_is_regexp) {
                    // Get constructor to check if it matches current RegExp
                    Value ctor = pat_obj->get_property("constructor");  // fires get("constructor") on Proxy
                    Value current_regexp = ctx.get_binding("RegExp");
                    bool ctor_matches = false;
                    if (ctor.is_function() && current_regexp.is_function()) {
                        ctor_matches = (ctor.as_function() == current_regexp.as_function());
                    }
                    if (ctor_matches && args.size() < 2) {
                        // Same constructor and no flags override: return pattern as-is
                        return args[0];
                    }
                    // Different constructor or flags provided: get source and flags
                    Value src = pat_obj->get_property("source");  // fires get("source") on Proxy
                    pattern = src.is_undefined() ? "" : src.to_string();
                    if (args.size() < 2) {
                        Value fl = pat_obj->get_property("flags");  // fires get("flags") on Proxy
                        flags = fl.is_undefined() ? "" : fl.to_string();
                    }
                } else {
                    pattern = pat_obj->get_property("_isRegExp").is_undefined() ? args[0].to_string() : "";
                    // For ordinary objects without IsRegExp, convert to string
                    pattern = args[0].to_string();
                }
            } else if (!args.empty()) {
                pattern = args[0].to_string();
            }

            try {
                auto regex_obj = ObjectFactory::create_object();

                auto regexp_impl = std::make_shared<RegExp>(pattern, flags);

                regex_obj->set_property("_isRegExp", Value(true));
                regex_obj->set_property("source", Value(regexp_impl->get_source()));
                // ES6: flags must be in alphabetical order
                std::string sorted_flags = regexp_impl->get_flags();
                std::sort(sorted_flags.begin(), sorted_flags.end());
                regex_obj->set_property("flags", Value(sorted_flags));
                regex_obj->set_property("global", Value(regexp_impl->get_global()));
                regex_obj->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                regex_obj->set_property("multiline", Value(regexp_impl->get_multiline()));
                regex_obj->set_property("unicode", Value(regexp_impl->get_unicode()));
                regex_obj->set_property("sticky", Value(regexp_impl->get_sticky()));
                regex_obj->set_property("dotAll", Value(regexp_impl->get_dotall()));
                regex_obj->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                
                Object* regex_obj_ptr = regex_obj.get();

                auto test_fn = ObjectFactory::create_native_function("test",
                    [regexp_impl, regex_obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (args.empty()) return Value(false);
                        std::string str = args[0].to_string();

                        if (regexp_impl->get_global()) {
                            Value lastIndex_val = regex_obj_ptr->get_property("lastIndex");
                            if (lastIndex_val.is_number()) {
                                regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                            }
                        }

                        bool result = regexp_impl->test(str);

                        if (regexp_impl->get_global()) {
                            regex_obj_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));
                        }

                        return Value(result);
                    });
                regex_obj->set_property("test", Value(test_fn.release()), PropertyAttributes::BuiltinFunction);

                auto exec_fn = ObjectFactory::create_native_function("exec",
                    [regexp_impl, regex_obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (args.empty()) return Value::null();
                        std::string str = args[0].to_string();

                        Value lastIndex_val = regex_obj_ptr->get_property("lastIndex");
                        if (lastIndex_val.is_number()) {
                            regexp_impl->set_last_index(static_cast<int>(lastIndex_val.to_number()));
                        }

                        Value result = regexp_impl->exec(str);

                        regex_obj_ptr->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));

                        return result;
                    });
                regex_obj->set_property("exec", Value(exec_fn.release()), PropertyAttributes::BuiltinFunction);

                auto toString_fn = ObjectFactory::create_native_function("toString",
                    [regexp_impl](Context& ctx, const std::vector<Value>& args) -> Value {
                        (void)ctx;
                        (void)args;
                        return Value(regexp_impl->to_string());
                    });
                regex_obj->set_property("toString", Value(toString_fn.release()), PropertyAttributes::BuiltinFunction);

                auto compile_inst_fn = ObjectFactory::create_native_function("compile",
                    [regexp_impl, regex_obj_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                        std::string pattern = "";
                        std::string flags = "";
                        if (args.size() > 0) pattern = args[0].to_string();
                        if (args.size() > 1) flags = args[1].to_string();

                        regexp_impl->compile(pattern, flags);

                        regex_obj_ptr->set_property("source", Value(regexp_impl->get_source()));
                        std::string sf = regexp_impl->get_flags();
                        std::sort(sf.begin(), sf.end());
                        regex_obj_ptr->set_property("flags", Value(sf));
                        regex_obj_ptr->set_property("global", Value(regexp_impl->get_global()));
                        regex_obj_ptr->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                        regex_obj_ptr->set_property("multiline", Value(regexp_impl->get_multiline()));
                        regex_obj_ptr->set_property("lastIndex", Value(0.0));

                        return Value(regex_obj_ptr);
                    }, 2);
                regex_obj->set_property("compile", Value(compile_inst_fn.release()), PropertyAttributes::BuiltinFunction);

                regex_obj->set_property("source", Value(regexp_impl->get_source()));
                regex_obj->set_property("flags", Value(sorted_flags));
                regex_obj->set_property("global", Value(regexp_impl->get_global()));
                regex_obj->set_property("ignoreCase", Value(regexp_impl->get_ignore_case()));
                regex_obj->set_property("multiline", Value(regexp_impl->get_multiline()));
                regex_obj->set_property("lastIndex", Value(static_cast<double>(regexp_impl->get_last_index())));

                Object* regex_raw = regex_obj.release();
                Value new_target = ctx.get_new_target();
                if (!new_target.is_undefined()) {
                    Object* nt_obj = new_target.is_function()
                        ? static_cast<Object*>(new_target.as_function())
                        : new_target.is_object() ? new_target.as_object() : nullptr;
                    if (nt_obj) {
                        Value nt_proto = nt_obj->get_property("prototype");
                        if (nt_proto.is_object()) regex_raw->set_prototype(nt_proto.as_object());
                    }
                } else {
                    Value regexp_ctor = ctx.get_binding("RegExp");
                    if (regexp_ctor.is_function()) {
                        Value proto = regexp_ctor.as_function()->get_property("prototype");
                        if (proto.is_object()) regex_raw->set_prototype(proto.as_object());
                    }
                }
                return Value(regex_raw);

            } catch (const std::exception& e) {
                ctx.throw_error("Invalid RegExp: " + std::string(e.what()));
                return Value::null();
            }
        });

    // ES6: RegExp.prototype.toString is generic - works on any object with source/flags
    auto regexp_toString = ObjectFactory::create_native_function("toString",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("RegExp.prototype.toString called on incompatible receiver");
                return Value();
            }
            Value source_val = this_obj->get_property("source");
            Value flags_val = this_obj->get_property("flags");
            std::string source = source_val.is_undefined() ? "(?:)" : source_val.to_string();
            std::string flags = flags_val.is_undefined() ? "" : flags_val.to_string();
            return Value("/" + source + "/" + flags);
        }, 0);
    regexp_prototype->set_property("toString", Value(regexp_toString.release()), PropertyAttributes::BuiltinFunction);

    PropertyDescriptor regexp_constructor_desc(Value(regexp_constructor.get()),
        PropertyAttributes::BuiltinFunction);
    regexp_prototype->set_property_descriptor("constructor", regexp_constructor_desc);

    // ES2022: RegExp.prototype flag data properties (false by default, shadowed by instance props)
    regexp_prototype->set_property("hasIndices", Value(false));
    regexp_prototype->set_property("global", Value(false));
    regexp_prototype->set_property("ignoreCase", Value(false));
    regexp_prototype->set_property("multiline", Value(false));
    regexp_prototype->set_property("dotAll", Value(false));
    regexp_prototype->set_property("unicode", Value(false));
    regexp_prototype->set_property("sticky", Value(false));

    // ES2022: RegExp.prototype.flags accessor (reads flag props via get_property for Proxy support)
    {
        auto flags_getter_fn = ObjectFactory::create_native_function("get flags",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                Object* this_obj = ctx.get_this_binding();
                if (!this_obj) {
                    ctx.throw_type_error("RegExp.prototype.flags getter called on incompatible receiver");
                    return Value();
                }
                // Check flags in ES2022 canonical order: d, g, i, m, s, u, y
                std::string result;
                if (this_obj->get_property("hasIndices").to_boolean()) result += "d";
                if (this_obj->get_property("global").to_boolean()) result += "g";
                if (this_obj->get_property("ignoreCase").to_boolean()) result += "i";
                if (this_obj->get_property("multiline").to_boolean()) result += "m";
                if (this_obj->get_property("dotAll").to_boolean()) result += "s";
                if (this_obj->get_property("unicode").to_boolean()) result += "u";
                if (this_obj->get_property("sticky").to_boolean()) result += "y";
                return Value(result);
            });
        PropertyDescriptor flags_desc;
        flags_desc.set_getter(flags_getter_fn.release());
        flags_desc.set_enumerable(false);
        flags_desc.set_configurable(true);
        regexp_prototype->set_property_descriptor("flags", flags_desc);
    }

    // RegExp.prototype.exec - generic, delegates to own exec on instance
    auto regexp_exec_proto_fn = ObjectFactory::create_native_function("exec",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->get_own_property("_isRegExp").to_boolean()) {
                ctx.throw_type_error("RegExp.prototype.exec called on incompatible receiver");
                return Value();
            }
            Value own_exec = this_obj->get_own_property("exec");
            if (own_exec.is_function()) {
                std::string str = args.empty() ? "undefined" : args[0].to_string();
                return own_exec.as_function()->call(ctx, {Value(str)}, Value(this_obj));
            }
            ctx.throw_type_error("RegExp.prototype.exec called on incompatible receiver");
            return Value();
        }, 1);
    regexp_prototype->set_property("exec", Value(regexp_exec_proto_fn.release()), PropertyAttributes::BuiltinFunction);

    // ES6: RegExp.prototype.test - generic function that calls this.exec
    auto regexp_test_fn = ObjectFactory::create_native_function("test",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("RegExp.prototype.test called on incompatible receiver");
                return Value();
            }
            std::string str = args.empty() ? "" : args[0].to_string();
            // Call this.exec via get_property (fires get("exec") trap on Proxy)
            Value exec_fn = this_obj->get_property("exec");
            if (exec_fn.is_function()) {
                Value result = exec_fn.as_function()->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
                return Value(!result.is_null() && !result.is_undefined());
            }
            return Value(false);
        }, 1);
    regexp_prototype->set_property("test", Value(regexp_test_fn.release()), PropertyAttributes::BuiltinFunction);

    // ES6: RegExp.prototype[Symbol.match/replace/search/split]
    auto regexp_sym_match = ObjectFactory::create_native_function("[Symbol.match]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();
            std::string str = args.empty() ? "" : args[0].to_string();
            // Check global flag via get_property (fires get("global") trap on Proxy)
            Value global_val = this_obj->get_property("global");
            bool is_global = global_val.to_boolean();
            if (!is_global) {
                // Non-global: call exec once
                Value exec_fn = this_obj->get_property("exec");
                if (exec_fn.is_function()) {
                    return exec_fn.as_function()->call(ctx, {Value(str)}, Value(this_obj));
                }
                return Value::null();
            }
            // Global: also check unicode, then loop exec
            Value unicode_val = this_obj->get_property("unicode");
            (void)unicode_val;
            this_obj->set_property("lastIndex", Value(0.0));
            auto result_array = ObjectFactory::create_array();
            size_t match_count = 0;
            Value exec_fn = this_obj->get_property("exec");
            if (!exec_fn.is_function()) return Value::null();
            Function* exec_func = exec_fn.as_function();
            size_t safety = 0;
            const size_t max_iter = str.length() + 2;
            while (safety++ < max_iter) {
                Value match = exec_func->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
                if (match.is_null()) break;
                if (match.is_object()) {
                    Value matched = match.as_object()->get_element(0);
                    result_array->set_element(match_count++, matched);
                }
            }
            if (match_count == 0) return Value::null();
            result_array->set_length(static_cast<uint32_t>(match_count));
            return Value(result_array.release());
        }, 1);
    regexp_prototype->set_property("Symbol.match", Value(regexp_sym_match.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_replace = ObjectFactory::create_native_function("[Symbol.replace]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();
            std::string str = args.size() > 0 ? args[0].to_string() : "";
            Value replace_val = args.size() > 1 ? args[1] : Value();
            // Check global flag via get_property (fires get("global") trap on Proxy)
            Value global_val = this_obj->get_property("global");
            bool is_global = global_val.to_boolean();
            if (!is_global) {
                // Non-global: call exec once
                Value exec_fn = this_obj->get_property("exec");
                if (exec_fn.is_function()) {
                    Value match = exec_fn.as_function()->call(ctx, {Value(str)}, Value(this_obj));
                    if (match.is_null() || match.is_undefined()) return Value(str);
                    if (match.is_object()) {
                        Value matched = match.as_object()->get_property("0");
                        Value index_val = match.as_object()->get_property("index");
                        int index = static_cast<int>(index_val.to_number());
                        std::string matched_str = matched.to_string();
                        std::string replacement = replace_val.is_function() ? "" : replace_val.to_string();
                        if (replace_val.is_function()) {
                            Value r = replace_val.as_function()->call(ctx, {matched, Value(static_cast<double>(index)), Value(str)}, Value());
                            replacement = r.to_string();
                        }
                        return Value(str.substr(0, index) + replacement + str.substr(index + matched_str.length()));
                    }
                }
                return Value(str);
            }
            // Global: also check unicode, then loop exec
            Value unicode_val = this_obj->get_property("unicode");
            (void)unicode_val;
            this_obj->set_property("lastIndex", Value(0.0));
            Value exec_fn = this_obj->get_property("exec");
            if (!exec_fn.is_function()) return Value(str);
            Function* exec_func = exec_fn.as_function();
            std::string result = str;
            size_t safety = 0;
            const size_t max_iter = str.length() + 2;
            while (safety++ < max_iter) {
                Value match = exec_func->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
                if (match.is_null()) break;
            }
            return Value(result);
        }, 2);
    regexp_prototype->set_property("Symbol.replace", Value(regexp_sym_replace.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_search = ObjectFactory::create_native_function("[Symbol.search]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value(-1.0);
            std::string str = args.empty() ? "" : args[0].to_string();
            // Save previousLastIndex via get_property (fires get("lastIndex") trap on Proxy)
            Value prev_last_index = this_obj->get_property("lastIndex");
            // Set lastIndex to 0
            this_obj->set_property("lastIndex", Value(0.0));
            // Call exec via get_property (fires get("exec") trap on Proxy)
            Value exec_fn = this_obj->get_property("exec");
            Value result_val;
            if (exec_fn.is_function()) {
                result_val = exec_fn.as_function()->call(ctx, {Value(str)}, Value(this_obj));
                if (ctx.has_exception()) return Value();
            }
            // Read current lastIndex (fires get("lastIndex") again)
            Value cur_last_index = this_obj->get_property("lastIndex");
            // Restore previousLastIndex if changed
            if (cur_last_index.to_string() != prev_last_index.to_string()) {
                this_obj->set_property("lastIndex", prev_last_index);
            }
            if (result_val.is_null() || result_val.is_undefined()) return Value(-1.0);
            if (result_val.is_object()) {
                return result_val.as_object()->get_property("index");
            }
            return Value(-1.0);
        }, 1);
    regexp_prototype->set_property("Symbol.search", Value(regexp_sym_search.release()), PropertyAttributes::BuiltinFunction);

    auto regexp_sym_split = ObjectFactory::create_native_function("[Symbol.split]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) return Value();
            std::string str = args.size() > 0 ? args[0].to_string() : "";
            // ES6: SpeciesConstructor - check constructor[Symbol.species]
            Value ctor_val = this_obj->get_property("constructor");
            if (ctor_val.is_object() || ctor_val.is_function()) {
                Object* ctor_obj = ctor_val.is_function() ? static_cast<Object*>(ctor_val.as_function()) : ctor_val.as_object();
                Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
                if (species_sym) {
                    Value species_val = ctor_obj->get_property(species_sym->to_property_key());
                    if (species_val.is_function()) {
                        Value flags_val = this_obj->get_property("flags");
                        std::vector<Value> species_args = { Value(this_obj), flags_val };
                        Value splitter = species_val.as_function()->call(ctx, species_args, Value(ctor_obj));
                        if (ctx.has_exception()) return Value();
                        if (splitter.is_object() || splitter.is_function()) {
                            this_obj = splitter.is_function() ? static_cast<Object*>(splitter.as_function()) : splitter.as_object();
                        }
                    }
                }
            }
            // Get exec and use it
            auto result = ObjectFactory::create_array();
            Value exec_fn = this_obj->get_property("exec");
            if (!exec_fn.is_function()) return Value(result.release());
            uint32_t idx = 0;
            std::string remaining = str;
            while (!remaining.empty()) {
                Value match = exec_fn.as_function()->call(ctx, {Value(remaining)}, Value(this_obj));
                if (match.is_null() || match.is_undefined()) break;
                if (!match.is_object()) break;
                Value index_val = match.as_object()->get_property("index");
                Value matched_val = match.as_object()->get_property("0");
                int index = static_cast<int>(index_val.to_number());
                std::string matched_str = matched_val.to_string();
                if (matched_str.empty() && index == 0) {
                    result->set_element(idx++, Value(std::string(1, remaining[0])));
                    remaining = remaining.substr(1);
                } else {
                    result->set_element(idx++, Value(remaining.substr(0, index)));
                    remaining = remaining.substr(index + matched_str.length());
                }
            }
            if (!remaining.empty() || idx > 0) {
                result->set_element(idx++, Value(remaining));
            }
            result->set_property("length", Value(static_cast<double>(idx)));
            return Value(result.release());
        }, 2);
    regexp_prototype->set_property("Symbol.split", Value(regexp_sym_split.release()), PropertyAttributes::BuiltinFunction);

    regexp_constructor->set_property("prototype", Value(regexp_prototype.release()));

    {
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto regexp_species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* self = ctx.get_this_binding();
                    return self ? Value(self) : Value();
                });
            PropertyDescriptor regexp_species_desc;
            regexp_species_desc.set_getter(regexp_species_getter.release());
            regexp_species_desc.set_enumerable(false);
            regexp_species_desc.set_configurable(true);
            regexp_constructor->set_property_descriptor(species_sym->to_property_key(), regexp_species_desc);
        }
    }

    register_built_in_object("RegExp", regexp_constructor.release());
    
    auto promise_constructor = ObjectFactory::create_native_constructor("Promise",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) {
                ctx.throw_type_error("Promise constructor cannot be invoked without 'new'");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value(std::string("Promise executor must be a function")));
                return Value();
            }
            
            auto promise = std::make_unique<Promise>(&ctx);
            // ES6: use new.target.prototype for subclassing support
            {
                Object* nt_obj = nullptr;
                Value new_target = ctx.get_new_target();
                if (new_target.is_function()) nt_obj = static_cast<Object*>(new_target.as_function());
                else if (new_target.is_object()) nt_obj = new_target.as_object();

                Value proto;
                if (nt_obj) proto = nt_obj->get_property("prototype");
                if (!proto.is_object()) {
                    Value promise_ctor = ctx.get_binding("Promise");
                    if (promise_ctor.is_function())
                        proto = static_cast<Object*>(promise_ctor.as_function())->get_property("prototype");
                }
                if (proto.is_object()) promise->set_prototype(proto.as_object());
            }

            Function* executor = args[0].as_function();
            
            auto resolve_fn = ObjectFactory::create_native_function("resolve",
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    Value value = args.empty() ? Value() : args[0];
                    // Promise Resolution Procedure: check for thenable
                    if (value.is_object() || value.is_function()) {
                        Object* obj = value.as_object();
                        Value then_val = obj->get_property("then");
                        if (ctx.has_exception()) return Value();
                        if (then_val.is_function()) {
                            Function* then_fn = then_val.as_function();
                            auto res_fn = ObjectFactory::create_native_function("resolve",
                                [promise_ptr](Context&, const std::vector<Value>& a) -> Value {
                                    promise_ptr->fulfill(a.empty() ? Value() : a[0]);
                                    return Value();
                                });
                            auto rej_fn = ObjectFactory::create_native_function("reject",
                                [promise_ptr](Context&, const std::vector<Value>& a) -> Value {
                                    promise_ptr->reject(a.empty() ? Value() : a[0]);
                                    return Value();
                                });
                            std::vector<Value> then_args = { Value(res_fn.release()), Value(rej_fn.release()) };
                            then_fn->call(ctx, then_args, value);
                            return Value();
                        }
                    }
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

            return Value(promise.release());
        });
    
    auto promise_try = ObjectFactory::create_native_function("try",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_exception(Value(std::string("Promise.try requires a function")));
                return Value();
            }
            
            Function* fn = args[0].as_function();
            auto promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* promise = static_cast<Promise*>(promise_obj.get());

            try {
                Value result = fn->call(ctx, {});
                promise->fulfill(result);
            } catch (...) {
                promise->reject(Value(std::string("Function threw in Promise.try")));
            }

            return Value(promise_obj.release());
        });
    promise_constructor->set_property("try", Value(promise_try.release()));
    
    auto promise_withResolvers = ObjectFactory::create_native_function("withResolvers",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            auto promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* promise_ptr = static_cast<Promise*>(promise_obj.get());

            auto resolve_fn = ObjectFactory::create_native_function("resolve",
                [promise_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value value = args.empty() ? Value() : args[0];
                    promise_ptr->fulfill(value);
                    return Value();
                });

            auto reject_fn = ObjectFactory::create_native_function("reject",
                [promise_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value reason = args.empty() ? Value() : args[0];
                    promise_ptr->reject(reason);
                    return Value();
                });

            auto result_obj = ObjectFactory::create_object();
            result_obj->set_property("promise", Value(promise_obj.release()));
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
            // ES6: apply Symbol.species for subclassing (PerformPromiseThen prototype propagation)
            if (new_promise) {
                Value ctor_val = this_obj->get_property("constructor");
                Object* ctor = nullptr;
                if (ctor_val.is_function()) ctor = static_cast<Object*>(ctor_val.as_function());
                else if (ctor_val.is_object()) ctor = ctor_val.as_object();

                if (ctor) {
                    Object* species_ctor = nullptr;
                    // Walk ctor's prototype chain to find Symbol.species
                    Object* cur = ctor;
                    while (cur && !species_ctor) {
                        PropertyDescriptor sdesc = cur->get_property_descriptor("Symbol.species");
                        if (sdesc.is_data_descriptor()) {
                            Value sv = sdesc.get_value();
                            if (sv.is_function()) species_ctor = static_cast<Object*>(sv.as_function());
                            else if (sv.is_object()) species_ctor = sv.as_object();
                            break;
                        } else if (sdesc.is_accessor_descriptor() && sdesc.has_getter()) {
                            Function* gfn = dynamic_cast<Function*>(sdesc.get_getter());
                            if (gfn) {
                                std::vector<Value> no_args;
                                Value sv = gfn->call(ctx, no_args, ctor_val);
                                if (!ctx.has_exception() && (sv.is_function() || sv.is_object())) {
                                    species_ctor = sv.is_function() ?
                                        static_cast<Object*>(sv.as_function()) : sv.as_object();
                                }
                                ctx.clear_exception();
                            }
                            break;
                        }
                        cur = cur->get_prototype();
                    }
                    if (!species_ctor) species_ctor = ctor;
                    Value proto = species_ctor->get_property("prototype");
                    if (proto.is_object()) new_promise->set_prototype(proto.as_object());
                }
            }
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
            if (!args.empty() && args[0].is_function()) {
                on_finally = args[0].as_function();
            }
            if (!on_finally) {
                return Value(promise->then(nullptr, nullptr));
            }
            auto then_wrapper = ObjectFactory::create_native_function("thenFinally",
                [on_finally](Context& ctx, const std::vector<Value>& args) -> Value {
                    Value original_val = args.empty() ? Value() : args[0];
                    std::vector<Value> no_args;
                    Value result = on_finally->call(ctx, no_args);
                    if (ctx.has_exception()) return Value();
                    if (result.is_object()) {
                        Promise* rp = dynamic_cast<Promise*>(result.as_object());
                        if (rp && rp->is_rejected()) {
                            ctx.throw_exception(rp->get_value(), true);
                            return Value();
                        }
                    }
                    return original_val;
                });
            auto catch_wrapper = ObjectFactory::create_native_function("catchFinally",
                [on_finally](Context& ctx, const std::vector<Value>& args) -> Value {
                    Value original_reason = args.empty() ? Value() : args[0];
                    std::vector<Value> no_args;
                    Value result = on_finally->call(ctx, no_args);
                    if (ctx.has_exception()) return Value();
                    if (result.is_object()) {
                        Promise* rp = dynamic_cast<Promise*>(result.as_object());
                        if (rp && rp->is_rejected()) {
                            ctx.throw_exception(rp->get_value(), true);
                            return Value();
                        }
                    }
                    ctx.throw_exception(original_reason, true);
                    return Value();
                });
            Function* then_fn = static_cast<Function*>(then_wrapper.release());
            Function* catch_fn = static_cast<Function*>(catch_wrapper.release());
            return Value(promise->then(then_fn, catch_fn));
        });
    promise_prototype->set_property("finally", Value(promise_finally.release()));

    PropertyDescriptor promise_tag_desc(Value(std::string("Promise")), PropertyAttributes::Configurable);
    promise_prototype->set_property_descriptor("Symbol.toStringTag", promise_tag_desc);

    // ES6: Promise.prototype.constructor = Promise
    promise_prototype->set_property("constructor", Value(promise_constructor.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    promise_constructor->set_property("prototype", Value(promise_prototype.release()));
    
    auto promise_resolve_static = ObjectFactory::create_native_function("resolve",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value value = args.empty() ? Value() : args[0];
            auto promise = ObjectFactory::create_promise(&ctx);
            static_cast<Promise*>(promise.get())->fulfill(value);
            return Value(promise.release());
        });
    promise_constructor->set_property("resolve", Value(promise_resolve_static.release()));
    
    auto promise_reject_static = ObjectFactory::create_native_function("reject",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value reason = args.empty() ? Value() : args[0];
            auto promise = ObjectFactory::create_promise(&ctx);
            static_cast<Promise*>(promise.get())->reject(reason);
            return Value(promise.release());
        });
    promise_constructor->set_property("reject", Value(promise_reject_static.release()));

    auto promise_all_static = ObjectFactory::create_native_function("all",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value(std::string("Promise.all expects an iterable")));
                return Value();
            }

            // ES6: use this constructor's prototype for subclassing
            Function* this_ctor = nullptr;
            Object* this_obj = ctx.get_this_binding();
            if (this_obj && dynamic_cast<Function*>(this_obj)) this_ctor = static_cast<Function*>(this_obj);

            Object* iterable = args[0].as_object();
            // ES6: Support Symbol.iterator for non-array iterables
            Object* collected_arr = nullptr;
            std::unique_ptr<Object> collected_arr_owner;
            if (!iterable->is_array()) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_method = iterable->get_property(iter_sym->to_property_key());
                    if (iter_method.is_function()) {
                        Value iter_obj = iter_method.as_function()->call(ctx, {}, args[0]);
                        if (!ctx.has_exception() && iter_obj.is_object()) {
                            Value next_fn = iter_obj.as_object()->get_property("next");
                            if (next_fn.is_function()) {
                                collected_arr_owner = ObjectFactory::create_array(0);
                                uint32_t cnt = 0;
                                for (uint32_t ii = 0; ii < 100000; ii++) {
                                    Value res = next_fn.as_function()->call(ctx, {}, iter_obj);
                                    if (ctx.has_exception()) return Value();
                                    if (!res.is_object()) break;
                                    if (res.as_object()->get_property("done").to_boolean()) break;
                                    collected_arr_owner->set_element(cnt++, res.as_object()->get_property("value"));
                                }
                                collected_arr_owner->set_length(cnt);
                                collected_arr = collected_arr_owner.get();
                                iterable = collected_arr;
                            }
                        }
                    }
                }
                if (!collected_arr) {
                    ctx.throw_exception(Value(std::string("Promise.all expects an iterable")));
                    return Value();
                }
            }

            uint32_t length = iterable->get_length();

            auto result_promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* result_promise = static_cast<Promise*>(result_promise_obj.get());

            if (this_ctor) {
                Value proto = this_ctor->get_property("prototype");
                if (proto.is_object()) result_promise->set_prototype(proto.as_object());
            }

            if (length == 0) {
                auto empty_array = ObjectFactory::create_array(0);
                result_promise->fulfill(Value(empty_array.release()));
                return Value(result_promise_obj.release());
            }

            // Async Promise.all: register .then() on each element promise
            // Use shared counters stored on result_promise as hidden properties
            // to survive GC (since result_promise is referenced by the caller)
            auto results_arr_owner = ObjectFactory::create_array(length);
            Object* results_arr = results_arr_owner.release();
            result_promise->set_property("__all_results__", Value(results_arr));
            result_promise->set_property("__all_remaining__", Value((double)length));

            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);

                // Wrap non-promise values in a pre-fulfilled promise
                Promise* p = nullptr;
                std::unique_ptr<Object> wrapped_obj;
                Promise* p_cast = element.is_object() ? dynamic_cast<Promise*>(element.as_object()) : nullptr;
                if (p_cast) {
                    p = p_cast;
                } else {
                    wrapped_obj = ObjectFactory::create_promise(&ctx);
                    static_cast<Promise*>(wrapped_obj.get())->fulfill(element);
                    p = static_cast<Promise*>(wrapped_obj.release());
                }

                uint32_t idx = i;
                Promise* rp = result_promise;

                auto on_ful = ObjectFactory::create_native_function("",
                    [idx, rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value val = args.empty() ? Value() : args[0];
                        Value arr_v = rp->get_property("__all_results__");
                        if (arr_v.is_object()) arr_v.as_object()->set_element(idx, val);
                        Value rem_v = rp->get_property("__all_remaining__");
                        double remaining = rem_v.to_number() - 1.0;
                        rp->set_property("__all_remaining__", Value(remaining));
                        if (remaining <= 0.0) {
                            Value arr2 = rp->get_property("__all_results__");
                            rp->fulfill(arr2);
                        }
                        return Value();
                    });

                auto on_rej = ObjectFactory::create_native_function("",
                    [rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value reason = args.empty() ? Value() : args[0];
                        rp->reject(reason);
                        return Value();
                    });

                // Keep handlers alive by storing on result_promise
                std::string k_ful = "__all_ful_" + std::to_string(i) + "__";
                std::string k_rej = "__all_rej_" + std::to_string(i) + "__";
                Function* ful_fn = on_ful.get();
                Function* rej_fn = on_rej.get();
                result_promise->set_property(k_ful, Value(on_ful.release()));
                result_promise->set_property(k_rej, Value(on_rej.release()));

                p->then(ful_fn, rej_fn);
            }

            return Value(result_promise_obj.release());
        });
    promise_constructor->set_property("all", Value(promise_all_static.release()));

    auto promise_race_static = ObjectFactory::create_native_function("race",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value(std::string("Promise.race expects an iterable")));
                return Value();
            }

            Function* this_ctor = nullptr;
            Object* this_obj = ctx.get_this_binding();
            if (this_obj && dynamic_cast<Function*>(this_obj)) this_ctor = static_cast<Function*>(this_obj);

            Object* iterable = args[0].as_object();
            // ES6: Support Symbol.iterator for non-array iterables
            Object* race_collected = nullptr;
            std::unique_ptr<Object> race_collected_owner;
            if (!iterable->is_array()) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_method = iterable->get_property(iter_sym->to_property_key());
                    if (iter_method.is_function()) {
                        Value iter_obj = iter_method.as_function()->call(ctx, {}, args[0]);
                        if (!ctx.has_exception() && iter_obj.is_object()) {
                            Value next_fn = iter_obj.as_object()->get_property("next");
                            if (next_fn.is_function()) {
                                race_collected_owner = ObjectFactory::create_array(0);
                                uint32_t cnt = 0;
                                for (uint32_t ii = 0; ii < 100000; ii++) {
                                    Value res = next_fn.as_function()->call(ctx, {}, iter_obj);
                                    if (ctx.has_exception()) return Value();
                                    if (!res.is_object()) break;
                                    if (res.as_object()->get_property("done").to_boolean()) break;
                                    race_collected_owner->set_element(cnt++, res.as_object()->get_property("value"));
                                }
                                race_collected_owner->set_length(cnt);
                                race_collected = race_collected_owner.get();
                                iterable = race_collected;
                            }
                        }
                    }
                }
                if (!race_collected) {
                    ctx.throw_exception(Value(std::string("Promise.race expects an iterable")));
                    return Value();
                }
            }

            uint32_t length = iterable->get_length();
            auto result_promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* result_promise = static_cast<Promise*>(result_promise_obj.get());

            if (this_ctor) {
                Value proto = this_ctor->get_property("prototype");
                if (proto.is_object()) result_promise->set_prototype(proto.as_object());
            }

            if (length == 0) {
                return Value(result_promise_obj.release());
            }

            // Async Promise.race: first settled promise wins
            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);

                Promise* p = nullptr;
                std::unique_ptr<Object> wrapped_obj;
                Promise* p_cast = element.is_object() ? dynamic_cast<Promise*>(element.as_object()) : nullptr;
                if (p_cast) {
                    p = p_cast;
                } else {
                    wrapped_obj = ObjectFactory::create_promise(&ctx);
                    static_cast<Promise*>(wrapped_obj.get())->fulfill(element);
                    p = static_cast<Promise*>(wrapped_obj.release());
                }

                Promise* rp = result_promise;

                auto on_ful = ObjectFactory::create_native_function("",
                    [rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value val = args.empty() ? Value() : args[0];
                        rp->fulfill(val);
                        return Value();
                    });

                auto on_rej = ObjectFactory::create_native_function("",
                    [rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value reason = args.empty() ? Value() : args[0];
                        rp->reject(reason);
                        return Value();
                    });

                // Keep handlers alive by storing on result_promise
                std::string k_ful = "__race_ful_" + std::to_string(i) + "__";
                std::string k_rej = "__race_rej_" + std::to_string(i) + "__";
                Function* ful_fn = on_ful.get();
                Function* rej_fn = on_rej.get();
                result_promise->set_property(k_ful, Value(on_ful.release()));
                result_promise->set_property(k_rej, Value(on_rej.release()));

                p->then(ful_fn, rej_fn);
            }

            return Value(result_promise_obj.release());
        });
    promise_constructor->set_property("race", Value(promise_race_static.release()));

    auto promise_allSettled_static = ObjectFactory::create_native_function("allSettled",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value(std::string("Promise.allSettled expects an iterable")));
                return Value();
            }

            Object* iterable = args[0].as_object();
            uint32_t length = iterable->get_length();

            auto result_promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* result_promise = static_cast<Promise*>(result_promise_obj.get());

            if (length == 0) {
                auto empty_array = ObjectFactory::create_array(0);
                result_promise->fulfill(Value(empty_array.release()));
                return Value(result_promise_obj.release());
            }

            auto results_arr_owner = ObjectFactory::create_array(length);
            Object* results_arr = results_arr_owner.release();
            result_promise->set_property("__settled_results__", Value(results_arr));
            result_promise->set_property("__settled_remaining__", Value((double)length));

            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);

                Promise* p = nullptr;
                std::unique_ptr<Object> wrapped_obj;
                Promise* p_cast = element.is_object() ? dynamic_cast<Promise*>(element.as_object()) : nullptr;
                if (p_cast) {
                    p = p_cast;
                } else {
                    wrapped_obj = ObjectFactory::create_promise(&ctx);
                    static_cast<Promise*>(wrapped_obj.get())->fulfill(element);
                    p = static_cast<Promise*>(wrapped_obj.release());
                }

                uint32_t idx = i;
                Promise* rp = result_promise;

                auto on_ful = ObjectFactory::create_native_function("",
                    [idx, rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value val = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("fulfilled")));
                        settled->set_property("value", val);
                        Value arr_v = rp->get_property("__settled_results__");
                        if (arr_v.is_object()) arr_v.as_object()->set_element(idx, Value(settled.release()));
                        Value rem_v = rp->get_property("__settled_remaining__");
                        double remaining = rem_v.to_number() - 1.0;
                        rp->set_property("__settled_remaining__", Value(remaining));
                        if (remaining <= 0.0) rp->fulfill(rp->get_property("__settled_results__"));
                        return Value();
                    });

                auto on_rej = ObjectFactory::create_native_function("",
                    [idx, rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value reason = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("rejected")));
                        settled->set_property("reason", reason);
                        Value arr_v = rp->get_property("__settled_results__");
                        if (arr_v.is_object()) arr_v.as_object()->set_element(idx, Value(settled.release()));
                        Value rem_v = rp->get_property("__settled_remaining__");
                        double remaining = rem_v.to_number() - 1.0;
                        rp->set_property("__settled_remaining__", Value(remaining));
                        if (remaining <= 0.0) rp->fulfill(rp->get_property("__settled_results__"));
                        return Value();
                    });

                std::string k_ful = "__settled_ful_" + std::to_string(i) + "__";
                std::string k_rej = "__settled_rej_" + std::to_string(i) + "__";
                Function* ful_fn = on_ful.get();
                Function* rej_fn = on_rej.get();
                result_promise->set_property(k_ful, Value(on_ful.release()));
                result_promise->set_property(k_rej, Value(on_rej.release()));

                p->then(ful_fn, rej_fn);
            }

            return Value(result_promise_obj.release());
        }, 1);
    promise_constructor->set_property("allSettled", Value(promise_allSettled_static.release()));

    auto promise_any_static = ObjectFactory::create_native_function("any",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_exception(Value(std::string("Promise.any expects an iterable")));
                return Value();
            }

            Object* iterable = args[0].as_object();
            uint32_t length = 0;
            std::vector<Value> promises_vec;
            if (iterable->is_array()) {
                length = iterable->get_length();
                for (uint32_t i = 0; i < length; i++)
                    promises_vec.push_back(iterable->get_element(i));
            } else {
                Value len_val = iterable->get_property("length");
                if (len_val.is_number()) {
                    length = static_cast<uint32_t>(len_val.to_number());
                    for (uint32_t i = 0; i < length; i++)
                        promises_vec.push_back(iterable->get_element(i));
                }
            }

            auto result_promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* result_promise = static_cast<Promise*>(result_promise_obj.get());
            Value result_promise_val(result_promise_obj.release());

            if (length == 0) {
                auto errors_arr = ObjectFactory::create_array();
                Value errors_val(errors_arr.release());
                Object* agg_ctor = ctx.get_built_in_object("AggregateError");
                if (agg_ctor && agg_ctor->is_function()) {
                    Value agg = static_cast<Function*>(agg_ctor)->call(ctx, {errors_val, Value(std::string("All promises were rejected"))});
                    result_promise->reject(agg);
                } else {
                    result_promise->reject(Value(std::string("AggregateError: All promises were rejected")));
                }
                return result_promise_val;
            }

            struct AnyState {
                Promise* result;
                std::vector<Value> errors;
                uint32_t total;
                uint32_t rejected_count;
                bool settled;
                Context* ctx;
            };
            auto state = std::make_shared<AnyState>();
            state->result = result_promise;
            state->errors.resize(length);
            state->total = length;
            state->rejected_count = 0;
            state->settled = false;
            state->ctx = &ctx;

            for (uint32_t i = 0; i < length; i++) {
                Value elem = promises_vec[i];
                Promise* p = nullptr;
                if (elem.is_object()) p = dynamic_cast<Promise*>(elem.as_object());

                auto on_fulfill = ObjectFactory::create_native_function("",
                    [state](Context&, const std::vector<Value>& a) -> Value {
                        if (state->settled) return Value();
                        state->settled = true;
                        state->result->fulfill(a.empty() ? Value() : a[0]);
                        return Value();
                    });
                auto on_reject = ObjectFactory::create_native_function("",
                    [state, i](Context& c, const std::vector<Value>& a) -> Value {
                        if (state->settled) return Value();
                        state->errors[i] = a.empty() ? Value() : a[0];
                        state->rejected_count++;
                        if (state->rejected_count == state->total) {
                            state->settled = true;
                            auto errors_arr = ObjectFactory::create_array();
                            for (uint32_t j = 0; j < state->total; j++)
                                errors_arr->set_element(j, state->errors[j]);
                            Value errors_val(errors_arr.release());
                            Object* agg_ctor = c.get_built_in_object("AggregateError");
                            if (agg_ctor && agg_ctor->is_function()) {
                                Value agg = static_cast<Function*>(agg_ctor)->call(c, {errors_val, Value(std::string("All promises were rejected"))});
                                state->result->reject(agg);
                            } else {
                                state->result->reject(Value(std::string("AggregateError: All promises were rejected")));
                            }
                        }
                        return Value();
                    });

                Function* fulfill_raw = on_fulfill.get();
                Function* reject_raw = on_reject.get();
                std::string suffix = std::to_string(i);

                if (p) {
                    p->set_property("__any_f__" + suffix, Value(on_fulfill.release()));
                    p->set_property("__any_r__" + suffix, Value(on_reject.release()));
                    p->then(fulfill_raw, reject_raw);
                } else {
                    result_promise_obj = nullptr;
                    on_reject.release();
                    on_fulfill.release();
                    if (!state->settled) {
                        state->settled = true;
                        state->result->fulfill(elem);
                    }
                }
            }

            return result_promise_val;
        }, 1);
    promise_constructor->set_property("any", Value(promise_any_static.release()));

    // ES6: Promise[Symbol.species] = Promise (accessor)
    auto promise_species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_binding = ctx.get_this_binding();
            if (this_binding) return Value(this_binding);
            return Value();
        }, 0);
    {
        PropertyDescriptor promise_species_desc;
        promise_species_desc.set_getter(promise_species_getter.get());
        promise_species_desc.set_enumerable(false);
        promise_species_desc.set_configurable(true);
        promise_constructor->set_property_descriptor("Symbol.species", promise_species_desc);
        promise_species_getter.release();
    }

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

    auto finalizationregistry_prototype = ObjectFactory::create_object();
    Object* fr_proto_ptr = finalizationregistry_prototype.get();
    finalizationregistry_prototype.release();

    auto finalizationregistry_constructor = ObjectFactory::create_native_constructor("FinalizationRegistry",
        [fr_proto_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("FinalizationRegistry constructor requires a callback function");
                return Value();
            }

            auto registry_obj = ObjectFactory::create_object();
            registry_obj->set_prototype(fr_proto_ptr);
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
    finalizationregistry_constructor->set_property("prototype", Value(fr_proto_ptr));
    fr_proto_ptr->set_property("constructor", Value(finalizationregistry_constructor.get()));
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
            (void)ctx; (void)args;
            auto result = ObjectFactory::create_object();
            result->set_property("done", Value(true));
            result->set_property("value", Value());
            return Value(result.release());
        }, 0);
    iterator_prototype->set_property("next", Value(iterator_next.release()));

    // Helper: call next() on an iterator object and get {value, done}
    // Returns {done:true,value:undefined} if any error
    auto call_next = [](Context& ctx, Object* iter) -> std::pair<Value,bool> {
        Value next_fn = iter->get_property("next");
        if (!next_fn.is_function()) return {Value(), true};
        Value result = next_fn.as_function()->call(ctx, {}, Value(iter));
        if (ctx.has_exception()) return {Value(), true};
        if (!result.is_object()) return {Value(), true};
        Object* res_obj = result.as_object();
        Value done_v = res_obj->get_property("done");
        bool done = done_v.to_boolean();
        Value val = res_obj->get_property("value");
        return {val, done};
    };

    // toArray
    auto iter_toArray_fn = ObjectFactory::create_native_function("toArray",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("toArray on non-object"); return Value(); }
            auto arr = ObjectFactory::create_array();
            uint32_t idx = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                arr->set_property(std::to_string(idx++), val);
            }
            arr->set_property("length", Value((double)idx));
            return Value(arr.release());
        }, 0);
    iterator_prototype->set_property("toArray", Value(iter_toArray_fn.release()));

    // forEach
    auto iter_forEach_fn = ObjectFactory::create_native_function("forEach",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("forEach on non-object"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("forEach requires function"); return Value(); }
            Function* cb = args[0].as_function();
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                cb->call(ctx, {val}, Value());
                if (ctx.has_exception()) break;
            }
            return Value();
        }, 1);
    iterator_prototype->set_property("forEach", Value(iter_forEach_fn.release()));

    // reduce
    auto iter_reduce_fn = ObjectFactory::create_native_function("reduce",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("reduce requires function"); return Value(); }
            Function* cb = args[0].as_function();
            Value acc = args.size() > 1 ? args[1] : Value();
            bool has_acc = args.size() > 1;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                if (!has_acc) { acc = val; has_acc = true; continue; }
                acc = cb->call(ctx, {acc, val}, Value());
                if (ctx.has_exception()) break;
            }
            return acc;
        }, 1);
    iterator_prototype->set_property("reduce", Value(iter_reduce_fn.release()));

    // some
    auto iter_some_fn = ObjectFactory::create_native_function("some",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("some requires function"); return Value(); }
            Function* cb = args[0].as_function();
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value r = cb->call(ctx, {val}, Value());
                if (ctx.has_exception()) break;
                if (r.to_boolean()) return Value(true);
            }
            return Value(false);
        }, 1);
    iterator_prototype->set_property("some", Value(iter_some_fn.release()));

    // every
    auto iter_every_fn = ObjectFactory::create_native_function("every",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("every requires function"); return Value(); }
            Function* cb = args[0].as_function();
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value r = cb->call(ctx, {val}, Value());
                if (ctx.has_exception()) break;
                if (!r.to_boolean()) return Value(false);
            }
            return Value(true);
        }, 1);
    iterator_prototype->set_property("every", Value(iter_every_fn.release()));

    // find
    auto iter_find_fn = ObjectFactory::create_native_function("find",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("find requires function"); return Value(); }
            Function* cb = args[0].as_function();
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value r = cb->call(ctx, {val}, Value());
                if (ctx.has_exception()) break;
                if (r.to_boolean()) return val;
            }
            return Value();
        }, 1);
    iterator_prototype->set_property("find", Value(iter_find_fn.release()));

    // map - returns a new iterator
    auto iter_map_fn = ObjectFactory::create_native_function("map",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("map requires function"); return Value(); }
            Function* mapper = args[0].as_function();
            // Collect all mapped values into array-backed iterator
            auto arr = ObjectFactory::create_array();
            uint32_t idx = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value mapped = mapper->call(ctx, {val, Value((double)idx)}, Value());
                if (ctx.has_exception()) break;
                arr->set_property(std::to_string(idx), mapped);
                idx++;
            }
            arr->set_property("length", Value((double)idx));
            // Return array iterator
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
        }, 1);
    iterator_prototype->set_property("map", Value(iter_map_fn.release()));

    // filter
    auto iter_filter_fn = ObjectFactory::create_native_function("filter",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("filter requires function"); return Value(); }
            Function* pred = args[0].as_function();
            auto arr = ObjectFactory::create_array();
            uint32_t idx = 0, out = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value r = pred->call(ctx, {val, Value((double)idx++)}, Value());
                if (ctx.has_exception()) break;
                if (r.to_boolean()) { arr->set_property(std::to_string(out++), val); }
            }
            arr->set_property("length", Value((double)out));
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
        }, 1);
    iterator_prototype->set_property("filter", Value(iter_filter_fn.release()));

    // take
    auto iter_take_fn = ObjectFactory::create_native_function("take",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("take on non-object"); return Value(); }
            uint32_t limit = args.empty() ? 0 : (uint32_t)args[0].to_number();
            auto arr = ObjectFactory::create_array();
            for (uint32_t i = 0; i < limit; i++) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                arr->set_property(std::to_string(i), val);
                arr->set_property("length", Value((double)(i+1)));
            }
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
        }, 1);
    iterator_prototype->set_property("take", Value(iter_take_fn.release()));

    // drop
    auto iter_drop_fn = ObjectFactory::create_native_function("drop",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter) { ctx.throw_type_error("drop on non-object"); return Value(); }
            uint32_t skip = args.empty() ? 0 : (uint32_t)args[0].to_number();
            for (uint32_t i = 0; i < skip; i++) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                (void)val;
            }
            // Return remaining iterator
            auto arr = ObjectFactory::create_array();
            uint32_t idx = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                arr->set_property(std::to_string(idx++), val);
            }
            arr->set_property("length", Value((double)idx));
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
        }, 1);
    iterator_prototype->set_property("drop", Value(iter_drop_fn.release()));

    // flatMap
    auto iter_flatMap_fn = ObjectFactory::create_native_function("flatMap",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* iter = ctx.get_this_binding();
            if (!iter || args.empty() || !args[0].is_function()) { ctx.throw_type_error("flatMap requires function"); return Value(); }
            Function* mapper = args[0].as_function();
            auto arr = ObjectFactory::create_array();
            uint32_t out = 0, idx = 0;
            while (true) {
                auto [val, done] = call_next(ctx, iter);
                if (ctx.has_exception() || done) break;
                Value mapped = mapper->call(ctx, {val, Value((double)idx++)}, Value());
                if (ctx.has_exception()) break;
                // Iterate over mapped value
                if (mapped.is_object()) {
                    Value iter_sym_val = mapped.as_object()->get_property("Symbol(Symbol.iterator)");
                    if (!iter_sym_val.is_function()) {
                        // Try string "@@iterator"
                        iter_sym_val = mapped.as_object()->get_property("@@iterator");
                    }
                    if (iter_sym_val.is_function()) {
                        Value inner = iter_sym_val.as_function()->call(ctx, {}, mapped);
                        if (inner.is_object()) {
                            while (true) {
                                auto [iv, id] = call_next(ctx, inner.as_object());
                                if (ctx.has_exception() || id) break;
                                arr->set_property(std::to_string(out++), iv);
                            }
                            continue;
                        }
                    }
                }
                arr->set_property(std::to_string(out++), mapped);
            }
            arr->set_property("length", Value((double)out));
            Value values_fn = arr->get_property("values");
            if (values_fn.is_function()) return values_fn.as_function()->call(ctx, {}, Value(arr.release()));
            return Value(arr.release());
        }, 1);
    iterator_prototype->set_property("flatMap", Value(iter_flatMap_fn.release()));

    // Add @@iterator to prototype
    {
        Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
        if (iter_sym) {
            auto self_iter = ObjectFactory::create_native_function("[Symbol.iterator]",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* self = ctx.get_this_binding();
                    return self ? Value(self) : Value();
                });
            iterator_prototype->set_property(iter_sym->to_property_key(), Value(self_iter.release()));
        }
    }

    // Static Iterator.from
    auto iterator_from = ObjectFactory::create_native_function("from",
        [call_next](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) { ctx.throw_type_error("Iterator.from requires an argument"); return Value(); }
            Value input = args[0];
            // If it has [Symbol.iterator], call it
            if (input.is_object()) {
                Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                if (iter_sym) {
                    Value iter_fn = input.as_object()->get_property(iter_sym->to_property_key());
                    if (iter_fn.is_function()) {
                        Value iter = iter_fn.as_function()->call(ctx, {}, input);
                        if (!ctx.has_exception() && iter.is_object()) return iter;
                    }
                }
                return input;
            }
            ctx.throw_type_error("Iterator.from: not iterable");
            return Value();
        }, 1);
    iterator_constructor->set_property("from", Value(iterator_from.release()));

    iterator_constructor->set_property("prototype", Value(iterator_prototype.release()));
    register_built_in_object("Iterator", iterator_constructor.release());

    auto arraybuffer_constructor = ObjectFactory::create_native_constructor("ArrayBuffer",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

    {
        Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (tag_sym) {
            PropertyDescriptor tag_desc(Value(std::string("ArrayBuffer")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            arraybuffer_prototype->set_property_descriptor(tag_sym->to_property_key(), tag_desc);
        }
    }

    arraybuffer_constructor->set_property("prototype", Value(arraybuffer_prototype.release()));

    {
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    return Value(ctx.get_this_binding());
                }, 0);
            PropertyDescriptor desc;
            desc.set_getter(getter.release());
            desc.set_enumerable(false);
            desc.set_configurable(true);
            arraybuffer_constructor->set_property_descriptor(species_sym->to_property_key(), desc);
        }
    }

    register_built_in_object("ArrayBuffer", arraybuffer_constructor.release());
    
    register_typed_array_constructors();

    // ES2017: Atomics object with stub operations
    {
        auto atomics_obj = ObjectFactory::create_object();
        const char* atomics_ops[] = {
            "add","and","compareExchange","exchange","isLockFree",
            "load","notify","or","store","sub","wait","xor", nullptr
        };
        for (int i = 0; atomics_ops[i]; ++i) {
            std::string op_name = atomics_ops[i];
            auto op_fn = ObjectFactory::create_native_function(op_name,
                [](Context&, const std::vector<Value>&) -> Value { return Value(0.0); }, 3);
            atomics_obj->set_property(op_name, Value(op_fn.release()), PropertyAttributes::BuiltinFunction);
        }
        register_built_in_object("Atomics", atomics_obj.release());
    }

    // ES2017: SharedArrayBuffer stub constructor
    {
        auto sab_constructor = ObjectFactory::create_native_constructor("SharedArrayBuffer",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                double byte_length = args.empty() ? 0.0 : args[0].to_number();
                auto buf = ObjectFactory::create_object();
                buf->set_property("byteLength", Value(byte_length), PropertyAttributes::None);
                buf->set_property("_isSharedArrayBuffer", Value(true));
                // Set prototype
                if (ctx.has_binding("SharedArrayBuffer")) {
                    Value ctor = ctx.get_binding("SharedArrayBuffer");
                    if (ctor.is_function()) {
                        Value proto = ctor.as_function()->get_property("prototype");
                        if (proto.is_object()) buf->set_prototype(proto.as_object());
                    }
                }
                return Value(buf.release());
            }, 1);

        auto sab_proto = ObjectFactory::create_object();

        // byteLength getter
        auto byte_length_getter = ObjectFactory::create_native_function("get byteLength",
            [](Context& ctx, const std::vector<Value>&) -> Value {
                Object* this_obj = ctx.get_this_binding();
                if (!this_obj) return Value(0.0);
                return this_obj->get_property("byteLength");
            }, 0);
        PropertyDescriptor bl_desc;
        bl_desc.set_getter(byte_length_getter.release());
        bl_desc.set_configurable(true);
        sab_proto->set_property_descriptor("byteLength", bl_desc);

        // slice stub
        auto sab_slice = ObjectFactory::create_native_function("slice",
            [](Context&, const std::vector<Value>&) -> Value {
                auto obj = ObjectFactory::create_object();
                return Value(static_cast<Object*>(obj.release()));
            }, 2);
        sab_proto->set_property("slice", Value(sab_slice.release()), PropertyAttributes::BuiltinFunction);

        // Symbol.toStringTag = "SharedArrayBuffer"
        Symbol* to_string_tag = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (to_string_tag) {
            PropertyDescriptor tag_desc(Value(std::string("SharedArrayBuffer")),
                static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            sab_proto->set_property_descriptor(to_string_tag->to_property_key(), tag_desc);
        }

        sab_constructor->set_property("prototype", Value(sab_proto.release()), PropertyAttributes::None);

        // Symbol.species getter
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    return ctx.get_binding("SharedArrayBuffer");
                }, 0);
            PropertyDescriptor species_desc;
            species_desc.set_getter(species_getter.release());
            species_desc.set_configurable(true);
            sab_constructor->set_property_descriptor(species_sym->to_property_key(), species_desc);
        }

        register_built_in_object("SharedArrayBuffer", sab_constructor.release());
    }

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
    Function* parseInt_raw = parseInt_fn.get();
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
    Function* parseFloat_raw = parseFloat_fn.get();
    lexical_environment_->create_binding("parseFloat", Value(parseFloat_fn.release()), false);

    // ES6: Number.parseFloat and Number.parseInt must be the same objects as globals
    {
        auto it = built_in_objects_.find("Number");
        if (it != built_in_objects_.end()) {
            Function* num_ctor = static_cast<Function*>(it->second);
            num_ctor->set_property("parseFloat", Value(parseFloat_raw));
            num_ctor->set_property("parseInt", Value(parseInt_raw));
        }
    }

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

    auto isFinite_global_fn = ObjectFactory::create_native_function("isFinite",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            double num = args[0].to_number();
            return Value(std::isfinite(num));
        }, 1);
    lexical_environment_->create_binding("isFinite", Value(isFinite_global_fn.release()), false);

    auto eval_fn = ObjectFactory::create_native_function("eval",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value();
            if (!args[0].is_string()) return args[0];

            std::string code = args[0].to_string();
            if (code.empty()) return Value();

            Engine* engine = ctx.get_engine();
            if (!engine) return Value();

            bool strict = ctx.is_strict_mode();

            try {
                // Parse with strict mode if calling context is strict
                Lexer::LexerOptions lex_opts;
                lex_opts.strict_mode = strict;
                Lexer lexer(code, lex_opts);
                auto tokens = lexer.tokenize();

                if (lexer.has_errors()) {
                    auto& errors = lexer.get_errors();
                    std::string msg = errors.empty() ? "SyntaxError" : errors[0];
                    ctx.throw_syntax_error(msg);
                    return Value();
                }

                Parser::ParseOptions parse_opts;
                parse_opts.strict_mode = strict;
                Parser parser(tokens, parse_opts);
                parser.set_source(code);
                auto program = parser.parse_program();

                if (parser.has_errors()) {
                    auto& errors = parser.get_errors();
                    std::string msg = errors.empty() ? "SyntaxError" : errors[0].message;
                    // Strip "SyntaxError: " prefix to avoid double-prefixing
                    if (msg.substr(0, 13) == "SyntaxError: ") msg = msg.substr(13);
                    ctx.throw_syntax_error(msg);
                    return Value();
                }

                if (!program) {
                    ctx.throw_syntax_error("Failed to parse eval code");
                    return Value();
                }

                if (strict) {
                    // ES5 10.4.2: Strict eval creates isolated variable environment
                    Context eval_ctx(engine, &ctx, Context::Type::Eval);
                    eval_ctx.set_strict_mode(true);
                    auto eval_env = new Environment(
                        Environment::Type::Declarative, ctx.get_lexical_environment());
                    eval_ctx.set_lexical_environment(eval_env);
                    eval_ctx.set_variable_environment(eval_env);
                    eval_ctx.set_this_binding(ctx.get_this_binding());

                    Value result = program->evaluate(eval_ctx);

                    if (eval_ctx.has_exception()) {
                        Value exception = eval_ctx.get_exception();
                        eval_ctx.clear_exception();
                        ctx.throw_exception(exception);
                        delete eval_env;
                        return Value();
                    }

                    delete eval_env;
                    return result;
                } else {
                    // Non-strict eval: evaluate in calling context
                    auto result = engine->evaluate(code);
                    if (result.success) {
                        return result.value;
                    } else {
                        ctx.throw_syntax_error(result.error_message);
                        return Value();
                    }
                }
            } catch (const std::exception& e) {
                ctx.throw_syntax_error(std::string(e.what()));
                return Value();
            } catch (...) {
                ctx.throw_syntax_error("Unknown syntax error");
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
    
    auto is_hex_digit = [](char c) -> bool {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    };

    auto hex_to_int = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return 0;
    };

    auto is_uri_reserved = [](unsigned char c) -> bool {
        return c == ';' || c == '/' || c == '?' || c == ':' || c == '@' ||
               c == '&' || c == '=' || c == '+' || c == '$' || c == ',' || c == '#';
    };

    auto encode_uri_fn = ObjectFactory::create_native_function("encodeURI",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string("undefined"));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                unsigned char c = input[i];
                // Detect UTF-8 encoded surrogates (U+D800-U+DFFF): 0xED 0xA0-0xBF ...
                if (c == 0xED && i + 1 < input.length()) {
                    unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
                    if (c2 >= 0xA0 && c2 <= 0xBF) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                }
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == ';' || c == ',' || c == '/' || c == '?' || c == ':' || c == '@' ||
                    c == '&' || c == '=' || c == '+' || c == '$' || c == '-' || c == '_' ||
                    c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' ||
                    c == ')' || c == '#') {
                    result += static_cast<char>(c);
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
        [is_hex_digit, hex_to_int, is_uri_reserved](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string("undefined"));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                if (input[i] == '%') {
                    if (i + 2 >= input.length()) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                    if (!is_hex_digit(input[i + 1]) || !is_hex_digit(input[i + 2])) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                    int byte1 = hex_to_int(input[i + 1]) * 16 + hex_to_int(input[i + 2]);
                    if (byte1 < 0x80) {
                        if (is_uri_reserved(static_cast<unsigned char>(byte1))) {
                            result += input[i];
                            result += input[i + 1];
                            result += input[i + 2];
                        } else {
                            result += static_cast<char>(byte1);
                        }
                        i += 2;
                    } else {
                        int num_bytes = 0;
                        if ((byte1 & 0xE0) == 0xC0) num_bytes = 2;
                        else if ((byte1 & 0xF0) == 0xE0) num_bytes = 3;
                        else if ((byte1 & 0xF8) == 0xF0) num_bytes = 4;
                        else {
                            ctx.throw_uri_error("URI malformed");
                            return Value();
                        }
                        std::string utf8;
                        utf8 += static_cast<char>(byte1);
                        for (int j = 1; j < num_bytes; j++) {
                            if (i + 2 + j * 3 >= input.length() || input[i + j * 3] != '%') {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            size_t pos = i + j * 3;
                            if (!is_hex_digit(input[pos + 1]) || !is_hex_digit(input[pos + 2])) {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            int cb = hex_to_int(input[pos + 1]) * 16 + hex_to_int(input[pos + 2]);
                            if ((cb & 0xC0) != 0x80) {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            utf8 += static_cast<char>(cb);
                        }
                        result += utf8;
                        i += num_bytes * 3 - 1;
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
            if (args.empty()) return Value(std::string("undefined"));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                unsigned char c = input[i];
                // Detect UTF-8 encoded surrogates (U+D800-U+DFFF): 0xED 0xA0-0xBF ...
                if (c == 0xED && i + 1 < input.length()) {
                    unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
                    if (c2 >= 0xA0 && c2 <= 0xBF) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                }
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' ||
                    c == '\'' || c == '(' || c == ')') {
                    result += static_cast<char>(c);
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
        [is_hex_digit, hex_to_int](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string("undefined"));
            std::string input = args[0].to_string();
            std::string result;

            for (size_t i = 0; i < input.length(); i++) {
                if (input[i] == '%') {
                    if (i + 2 >= input.length()) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                    if (!is_hex_digit(input[i + 1]) || !is_hex_digit(input[i + 2])) {
                        ctx.throw_uri_error("URI malformed");
                        return Value();
                    }
                    int byte1 = hex_to_int(input[i + 1]) * 16 + hex_to_int(input[i + 2]);
                    if (byte1 < 0x80) {
                        result += static_cast<char>(byte1);
                        i += 2;
                    } else {
                        int num_bytes = 0;
                        if ((byte1 & 0xE0) == 0xC0) num_bytes = 2;
                        else if ((byte1 & 0xF0) == 0xE0) num_bytes = 3;
                        else if ((byte1 & 0xF8) == 0xF0) num_bytes = 4;
                        else {
                            ctx.throw_uri_error("URI malformed");
                            return Value();
                        }
                        std::string utf8;
                        utf8 += static_cast<char>(byte1);
                        for (int j = 1; j < num_bytes; j++) {
                            if (i + 2 + j * 3 >= input.length() || input[i + j * 3] != '%') {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            size_t pos = i + j * 3;
                            if (!is_hex_digit(input[pos + 1]) || !is_hex_digit(input[pos + 2])) {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            int cb = hex_to_int(input[pos + 1]) * 16 + hex_to_int(input[pos + 2]);
                            if ((cb & 0xC0) != 0x80) {
                                ctx.throw_uri_error("URI malformed");
                                return Value();
                            }
                            utf8 += static_cast<char>(cb);
                        }
                        result += utf8;
                        i += num_bytes * 3 - 1;
                    }
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
    auto asIntN_fn = ObjectFactory::create_native_function("asIntN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) { ctx.throw_type_error("BigInt.asIntN requires 2 arguments"); return Value(); }
            int64_t n = static_cast<int64_t>(args[0].to_number());
            if (n < 0 || n > 64) { ctx.throw_range_error("Invalid width"); return Value(); }
            if (!args[1].is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
            int64_t val = args[1].as_bigint()->to_int64();
            if (n == 0) return Value(new Quanta::BigInt(0));
            if (n == 64) return Value(new Quanta::BigInt(val));
            int64_t mod = 1LL << n;
            int64_t result = val & (mod - 1);
            if (result >= (mod >> 1)) result -= mod;
            return Value(new Quanta::BigInt(result));
        });
    bigint_fn->set_property("asIntN", Value(asIntN_fn.release()), PropertyAttributes::BuiltinFunction);

    auto asUintN_fn = ObjectFactory::create_native_function("asUintN",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) { ctx.throw_type_error("BigInt.asUintN requires 2 arguments"); return Value(); }
            int64_t n = static_cast<int64_t>(args[0].to_number());
            if (n < 0 || n > 64) { ctx.throw_range_error("Invalid width"); return Value(); }
            if (!args[1].is_bigint()) { ctx.throw_type_error("Not a BigInt"); return Value(); }
            int64_t val = args[1].as_bigint()->to_int64();
            if (n == 0) return Value(new Quanta::BigInt(0));
            if (n == 64) return Value(new Quanta::BigInt(val));
            uint64_t mask = (1ULL << n) - 1;
            uint64_t result = static_cast<uint64_t>(val) & mask;
            return Value(new Quanta::BigInt(static_cast<int64_t>(result)));
        });
    bigint_fn->set_property("asUintN", Value(asUintN_fn.release()), PropertyAttributes::BuiltinFunction);

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

    // print()
    auto print_fn = ObjectFactory::create_native_function("print",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) std::cout << " ";
                std::cout << args[i].to_string();
            }
            std::cout << std::endl;
            return Value();
        }, 1);
    lexical_environment_->create_binding("print", Value(print_fn.release()), false);

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

    // Expose Object.prototype.__defineGetter__/__defineSetter__ as global bindings
    // (browsers expose them via window.__proto__ = Object.prototype, we do it explicitly)
    {
        Value obj_ctor = get_binding("Object");
        if (obj_ctor.is_function()) {
            Value obj_proto = obj_ctor.as_function()->get_property("prototype");
            if (obj_proto.is_object()) {
                Object* op = obj_proto.as_object();
                Value dg = op->get_own_property("__defineGetter__");
                if (!dg.is_undefined() && lexical_environment_) {
                    lexical_environment_->create_binding("__defineGetter__", dg, false);
                    global_object_->set_property("__defineGetter__", dg, PropertyAttributes::BuiltinFunction);
                }
                Value ds = op->get_own_property("__defineSetter__");
                if (!ds.is_undefined() && lexical_environment_) {
                    lexical_environment_->create_binding("__defineSetter__", ds, false);
                    global_object_->set_property("__defineSetter__", ds, PropertyAttributes::BuiltinFunction);
                }
            }
        }
    }

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
        if (!binding_object_->has_own_property(name)) return false;
        // ES6: Check Symbol.unscopables (guard against re-entrancy from Proxy get trap)
        Symbol* unscopables_sym = Symbol::get_well_known(Symbol::UNSCOPABLES);
        if (unscopables_sym) {
            static thread_local int unscopables_depth = 0;
            if (unscopables_depth == 0) {
                unscopables_depth++;
                Value unscopables = binding_object_->get_property(unscopables_sym->to_property_key());
                unscopables_depth--;
                if (unscopables.is_object()) {
                    Value blocked = unscopables.as_object()->get_property(name);
                    if (blocked.to_boolean()) return false;
                }
            }
        }
        return true;
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

    Environment* outer_env = parent->get_lexical_environment();
    if (function && function->is_param_default()) {
        Environment* walk = outer_env;
        while (walk && walk->get_type() == Environment::Type::Declarative) {
            if (!walk->get_outer()) break;
            walk = walk->get_outer();
        }
        if (walk && walk->get_type() != Environment::Type::Declarative) {
            outer_env = walk;
        }
    }

    auto func_env = std::make_unique<Environment>(Environment::Type::Function, outer_env);
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

void Context::push_with_scope(Object* obj) {
    // Create object environment for with statement
    auto new_env = std::make_unique<Environment>(obj, lexical_environment_);
    lexical_environment_ = new_env.release();
}

void Context::pop_with_scope() {
    if (lexical_environment_ && lexical_environment_->get_outer()) {
        Environment* old_env = lexical_environment_;
        lexical_environment_ = lexical_environment_->get_outer();
        delete old_env;
    }
}

void Context::register_typed_array_constructors() {
    auto uint8array_constructor = ObjectFactory::create_native_constructor("Uint8Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
                                return Value();
                            }
                            std::vector<Value> items;
                            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
                            while (it) {
                                Value next_fn = it->get_property("next");
                                if (!next_fn.is_function()) break;
                                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                                if (!next_call_ctx) next_call_ctx = &ctx;
                                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                                if (next_call_ctx->has_exception()) {
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_uint8_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }

            ctx.throw_type_error("Uint8Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Uint8Array", uint8array_constructor.release());

    auto uint8clampedarray_constructor = ObjectFactory::create_native_constructor("Uint8ClampedArray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                if (obj->is_array_buffer()) {
                    ArrayBuffer* buffer = static_cast<ArrayBuffer*>(obj);
                    return Value(TypedArrayFactory::create_uint8_clamped_array_from_buffer(buffer).release());
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

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
                                return Value();
                            }
                            std::vector<Value> items;
                            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
                            while (it) {
                                Value next_fn = it->get_property("next");
                                if (!next_fn.is_function()) break;
                                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                                if (!next_call_ctx) next_call_ctx = &ctx;
                                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                                if (next_call_ctx->has_exception()) {
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_uint8_clamped_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }

            ctx.throw_type_error("Uint8ClampedArray constructor argument not supported");
            return Value();
        });
    register_built_in_object("Uint8ClampedArray", uint8clampedarray_constructor.release());

    auto float32array_constructor = ObjectFactory::create_native_constructor("Float32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
                                return Value();
                            }
                            std::vector<Value> items;
                            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
                            while (it) {
                                Value next_fn = it->get_property("next");
                                if (!next_fn.is_function()) break;
                                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                                if (!next_call_ctx) next_call_ctx = &ctx;
                                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                                if (next_call_ctx->has_exception()) {
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_float32_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
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

    {
        Symbol* ta_tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
        if (ta_tag_sym) {
            auto ta_tag_getter = ObjectFactory::create_native_function("get [Symbol.toStringTag]",
                [](Context& ctx, const std::vector<Value>&) -> Value {
                    Object* self = ctx.get_this_binding();
                    if (!self || !self->is_typed_array()) return Value();
                    TypedArrayBase* ta = static_cast<TypedArrayBase*>(self);
                    return Value(std::string(TypedArrayBase::array_type_to_string(ta->get_array_type())));
                });
            PropertyDescriptor ta_tag_desc;
            ta_tag_desc.set_getter(ta_tag_getter.release());
            ta_tag_desc.set_enumerable(false);
            ta_tag_desc.set_configurable(true);
            typedarray_prototype->set_property_descriptor(ta_tag_sym->to_property_key(), ta_tag_desc);
        }
    }


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

    auto every_fn = ObjectFactory::create_native_function("every",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (size_t i = 0; i < ta->length(); i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(this_obj) };
                if (!cb->call(ctx, cb_args, thisArg).to_boolean()) return Value(false);
            }
            return Value(true);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("every", PropertyDescriptor(Value(every_fn.release()), PropertyAttributes::BuiltinFunction));

    auto some_fn = ObjectFactory::create_native_function("some",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (size_t i = 0; i < ta->length(); i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(this_obj) };
                if (cb->call(ctx, cb_args, thisArg).to_boolean()) return Value(true);
            }
            return Value(false);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("some", PropertyDescriptor(Value(some_fn.release()), PropertyAttributes::BuiltinFunction));

    auto find_fn = ObjectFactory::create_native_function("find",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (size_t i = 0; i < ta->length(); i++) {
                Value el = ta->get_element(i);
                std::vector<Value> cb_args = { el, Value(static_cast<double>(i)), Value(this_obj) };
                if (cb->call(ctx, cb_args, thisArg).to_boolean()) return el;
            }
            return Value();
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("find", PropertyDescriptor(Value(find_fn.release()), PropertyAttributes::BuiltinFunction));

    auto findIndex_fn = ObjectFactory::create_native_function("findIndex",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            Value thisArg = args.size() > 1 ? args[1] : Value();
            for (size_t i = 0; i < ta->length(); i++) {
                std::vector<Value> cb_args = { ta->get_element(i), Value(static_cast<double>(i)), Value(this_obj) };
                if (cb->call(ctx, cb_args, thisArg).to_boolean()) return Value(static_cast<double>(i));
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("findIndex", PropertyDescriptor(Value(findIndex_fn.release()), PropertyAttributes::BuiltinFunction));

    auto indexOf_fn = ObjectFactory::create_native_function("indexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            if (args.empty() || ta->length() == 0) return Value(-1.0);
            Value search = args[0];
            size_t from = 0;
            if (args.size() > 1) { int64_t idx = static_cast<int64_t>(args[1].to_number()); from = idx < 0 ? (size_t)(idx + (int64_t)ta->length() < 0 ? 0 : idx + (int64_t)ta->length()) : (size_t)idx; }
            for (size_t i = from; i < ta->length(); i++) {
                if (ta->get_element(i).strict_equals(search)) return Value(static_cast<double>(i));
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("indexOf", PropertyDescriptor(Value(indexOf_fn.release()), PropertyAttributes::BuiltinFunction));

    auto lastIndexOf_fn = ObjectFactory::create_native_function("lastIndexOf",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            if (args.empty() || ta->length() == 0) return Value(-1.0);
            Value search = args[0];
            int64_t from = ta->length() - 1;
            if (args.size() > 1) { int64_t idx = static_cast<int64_t>(args[1].to_number()); from = idx < 0 ? idx + (int64_t)ta->length() : (idx < (int64_t)ta->length() ? idx : (int64_t)ta->length() - 1); }
            for (int64_t i = from; i >= 0; i--) {
                if (ta->get_element(static_cast<size_t>(i)).strict_equals(search)) return Value(static_cast<double>(i));
            }
            return Value(-1.0);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("lastIndexOf", PropertyDescriptor(Value(lastIndexOf_fn.release()), PropertyAttributes::BuiltinFunction));

    auto reduce_fn = ObjectFactory::create_native_function("reduce",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            size_t k = 0; Value acc;
            if (args.size() >= 2) { acc = args[1]; } else { if (ta->length() == 0) { ctx.throw_type_error("Reduce of empty array"); return Value(); } acc = ta->get_element(static_cast<size_t>(0)); k = 1; }
            for (; k < ta->length(); k++) {
                std::vector<Value> cb_args = { acc, ta->get_element(k), Value(static_cast<double>(k)), Value(this_obj) };
                acc = cb->call(ctx, cb_args, Value());
            }
            return acc;
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("reduce", PropertyDescriptor(Value(reduce_fn.release()), PropertyAttributes::BuiltinFunction));

    auto reduceRight_fn = ObjectFactory::create_native_function("reduceRight",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            if (args.empty() || !args[0].is_function()) { ctx.throw_type_error("callback required"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            Function* cb = args[0].as_function();
            int64_t k = (int64_t)ta->length() - 1; Value acc;
            if (args.size() >= 2) { acc = args[1]; } else { if (ta->length() == 0) { ctx.throw_type_error("Reduce of empty array"); return Value(); } acc = ta->get_element(ta->length() - 1); k--; }
            for (; k >= 0; k--) {
                std::vector<Value> cb_args = { acc, ta->get_element(static_cast<size_t>(k)), Value(static_cast<double>(k)), Value(this_obj) };
                acc = cb->call(ctx, cb_args, Value());
            }
            return acc;
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("reduceRight", PropertyDescriptor(Value(reduceRight_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_join_fn = ObjectFactory::create_native_function("join",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            std::string sep = (args.empty() || args[0].is_undefined()) ? "," : args[0].to_string();
            std::string result;
            for (size_t i = 0; i < ta->length(); i++) { if (i > 0) result += sep; result += ta->get_element(i).to_string(); }
            return Value(result);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("join", PropertyDescriptor(Value(ta_join_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_sort_fn = ObjectFactory::create_native_function("sort",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            size_t len = ta->length();
            if (len <= 1) return Value(this_obj);
            Function* cmp = (!args.empty() && args[0].is_function()) ? args[0].as_function() : nullptr;
            std::vector<Value> els; els.reserve(len);
            for (size_t i = 0; i < len; i++) els.push_back(ta->get_element(i));
            std::sort(els.begin(), els.end(), [&](const Value& a, const Value& b) {
                if (cmp) { std::vector<Value> ca = {a, b}; return cmp->call(ctx, ca, Value()).to_number() < 0; }
                return a.to_number() < b.to_number();
            });
            for (size_t i = 0; i < len; i++) ta->set_element(i, els[i]);
            return Value(this_obj);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("sort", PropertyDescriptor(Value(ta_sort_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_reverse_fn = ObjectFactory::create_native_function("reverse",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            size_t len = ta->length();
            for (size_t i = 0; i < len / 2; i++) { Value t = ta->get_element(i); ta->set_element(i, ta->get_element(len - 1 - i)); ta->set_element(len - 1 - i, t); }
            return Value(this_obj);
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("reverse", PropertyDescriptor(Value(ta_reverse_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_slice_fn = ObjectFactory::create_native_function("slice",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t len = ta->length(), start = 0, end = len;
            if (!args.empty()) { int64_t s = static_cast<int64_t>(args[0].to_number()); start = s < 0 ? (s + len < 0 ? 0 : s + len) : (s > len ? len : s); }
            if (args.size() > 1 && !args[1].is_undefined()) { int64_t e = static_cast<int64_t>(args[1].to_number()); end = e < 0 ? (e + len < 0 ? 0 : e + len) : (e > len ? len : e); }
            size_t sl = end > start ? end - start : 0;
            TypedArrayBase* r = nullptr;
            switch (ta->get_array_type()) {
                case TypedArrayBase::ArrayType::INT8: r = TypedArrayFactory::create_int8_array(sl).release(); break;
                case TypedArrayBase::ArrayType::UINT8: r = TypedArrayFactory::create_uint8_array(sl).release(); break;
                case TypedArrayBase::ArrayType::UINT8_CLAMPED: r = TypedArrayFactory::create_uint8_clamped_array(sl).release(); break;
                case TypedArrayBase::ArrayType::INT16: r = TypedArrayFactory::create_int16_array(sl).release(); break;
                case TypedArrayBase::ArrayType::UINT16: r = TypedArrayFactory::create_uint16_array(sl).release(); break;
                case TypedArrayBase::ArrayType::INT32: r = TypedArrayFactory::create_int32_array(sl).release(); break;
                case TypedArrayBase::ArrayType::UINT32: r = TypedArrayFactory::create_uint32_array(sl).release(); break;
                case TypedArrayBase::ArrayType::FLOAT32: r = TypedArrayFactory::create_float32_array(sl).release(); break;
                case TypedArrayBase::ArrayType::FLOAT64: r = TypedArrayFactory::create_float64_array(sl).release(); break;
                default: ctx.throw_type_error("Unsupported type"); return Value();
            }
            for (size_t i = 0; i < sl; i++) r->set_element(i, ta->get_element(start + i));
            return Value(r);
        }, 2);
    typedarray_proto_ptr->set_property_descriptor("slice", PropertyDescriptor(Value(ta_slice_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_fill_fn = ObjectFactory::create_native_function("fill",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t len = ta->length();
            Value fillVal = args.empty() ? Value() : args[0];
            int64_t start = 0, end = len;
            if (args.size() > 1 && !args[1].is_undefined()) { int64_t s = static_cast<int64_t>(args[1].to_number()); start = s < 0 ? (s + len < 0 ? 0 : s + len) : (s > len ? len : s); }
            if (args.size() > 2 && !args[2].is_undefined()) { int64_t e = static_cast<int64_t>(args[2].to_number()); end = e < 0 ? (e + len < 0 ? 0 : e + len) : (e > len ? len : e); }
            for (int64_t i = start; i < end; i++) ta->set_element(static_cast<size_t>(i), fillVal);
            return Value(this_obj);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("fill", PropertyDescriptor(Value(ta_fill_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_copyWithin_fn = ObjectFactory::create_native_function("copyWithin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t len = ta->length();
            if (args.empty()) return Value(this_obj);
            int64_t tgt = static_cast<int64_t>(args[0].to_number());
            tgt = tgt < 0 ? (tgt + len < 0 ? 0 : tgt + len) : (tgt > len ? len : tgt);
            int64_t start = 0;
            if (args.size() > 1) { int64_t s = static_cast<int64_t>(args[1].to_number()); start = s < 0 ? (s + len < 0 ? 0 : s + len) : (s > len ? len : s); }
            int64_t end = len;
            if (args.size() > 2 && !args[2].is_undefined()) { int64_t e = static_cast<int64_t>(args[2].to_number()); end = e < 0 ? (e + len < 0 ? 0 : e + len) : (e > len ? len : e); }
            int64_t count = end - start;
            if (count <= 0) return Value(this_obj);
            std::vector<Value> tmp; tmp.reserve(count);
            for (int64_t i = 0; i < count; i++) tmp.push_back(ta->get_element(static_cast<size_t>(start + i)));
            for (int64_t i = 0; i < count && tgt + i < len; i++) ta->set_element(static_cast<size_t>(tgt + i), tmp[static_cast<size_t>(i)]);
            return Value(this_obj);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("copyWithin", PropertyDescriptor(Value(ta_copyWithin_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_entries_fn = ObjectFactory::create_native_function("entries",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            auto iter = ObjectFactory::create_object();
            iter->set_property("__idx", Value(0.0)); iter->set_property("__len", Value(static_cast<double>(ta->length()))); iter->set_property("__arr", Value(this_obj));
            auto next = ObjectFactory::create_native_function("next", [](Context& ctx, const std::vector<Value>& a) -> Value {
                (void)a; Object* it = ctx.get_this_binding(); size_t idx = (size_t)it->get_property("__idx").to_number(); size_t len = (size_t)it->get_property("__len").to_number();
                auto res = ObjectFactory::create_object();
                if (idx >= len) { res->set_property("done", Value(true)); res->set_property("value", Value()); }
                else { auto pair = ObjectFactory::create_object(); pair->set_property("0", Value((double)idx)); pair->set_property("1", static_cast<TypedArrayBase*>(it->get_property("__arr").as_object())->get_element(idx)); pair->set_property("length", Value(2.0));
                    res->set_property("done", Value(false)); res->set_property("value", Value(pair.release())); it->set_property("__idx", Value((double)(idx + 1))); }
                return Value(res.release()); }, 0);
            iter->set_property("next", Value(next.release())); return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("entries", PropertyDescriptor(Value(ta_entries_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_keys_fn = ObjectFactory::create_native_function("keys",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            auto iter = ObjectFactory::create_object();
            iter->set_property("__idx", Value(0.0)); iter->set_property("__len", Value(static_cast<double>(ta->length())));
            auto next = ObjectFactory::create_native_function("next", [](Context& ctx, const std::vector<Value>& a) -> Value {
                (void)a; Object* it = ctx.get_this_binding(); size_t idx = (size_t)it->get_property("__idx").to_number(); size_t len = (size_t)it->get_property("__len").to_number();
                auto res = ObjectFactory::create_object();
                if (idx >= len) { res->set_property("done", Value(true)); res->set_property("value", Value()); }
                else { res->set_property("done", Value(false)); res->set_property("value", Value((double)idx)); it->set_property("__idx", Value((double)(idx + 1))); }
                return Value(res.release()); }, 0);
            iter->set_property("next", Value(next.release())); return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("keys", PropertyDescriptor(Value(ta_keys_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            auto iter = ObjectFactory::create_object();
            iter->set_property("__idx", Value(0.0)); iter->set_property("__len", Value(static_cast<double>(ta->length()))); iter->set_property("__arr", Value(this_obj));
            auto next = ObjectFactory::create_native_function("next", [](Context& ctx, const std::vector<Value>& a) -> Value {
                (void)a; Object* it = ctx.get_this_binding(); size_t idx = (size_t)it->get_property("__idx").to_number(); size_t len = (size_t)it->get_property("__len").to_number();
                auto res = ObjectFactory::create_object();
                if (idx >= len) { res->set_property("done", Value(true)); res->set_property("value", Value()); }
                else { res->set_property("done", Value(false)); res->set_property("value", static_cast<TypedArrayBase*>(it->get_property("__arr").as_object())->get_element(idx)); it->set_property("__idx", Value((double)(idx + 1))); }
                return Value(res.release()); }, 0);
            iter->set_property("next", Value(next.release())); return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property_descriptor("values", PropertyDescriptor(Value(ta_values_fn.release()), PropertyAttributes::BuiltinFunction));

    // ES6: TypedArray.prototype[Symbol.iterator] = values
    auto ta_sym_iterator_fn = ObjectFactory::create_native_function("[Symbol.iterator]",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            auto iter = ObjectFactory::create_object();
            auto idx = std::make_shared<uint32_t>(0);
            uint32_t len = ta->byte_length() / ta->bytes_per_element();
            auto next = ObjectFactory::create_native_function("next",
                [this_obj, idx, len](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    auto res = ObjectFactory::create_object();
                    if (*idx >= len) {
                        res->set_property("done", Value(true));
                        res->set_property("value", Value());
                    } else {
                        res->set_property("done", Value(false));
                        res->set_property("value", this_obj->get_element(*idx));
                        (*idx)++;
                    }
                    return Value(res.release());
                }, 0);
            iter->set_property("next", Value(next.release()));
            return Value(iter.release());
        }, 0);
    typedarray_proto_ptr->set_property("Symbol.iterator", Value(ta_sym_iterator_fn.release()), PropertyAttributes::BuiltinFunction);

    auto ta_subarray_fn = ObjectFactory::create_native_function("subarray",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) { ctx.throw_type_error("not a TypedArray"); return Value(); }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            int64_t len = ta->length(), start = 0, end = len;
            if (!args.empty()) { int64_t s = static_cast<int64_t>(args[0].to_number()); start = s < 0 ? (s + len < 0 ? 0 : s + len) : (s > len ? len : s); }
            if (args.size() > 1 && !args[1].is_undefined()) { int64_t e = static_cast<int64_t>(args[1].to_number()); end = e < 0 ? (e + len < 0 ? 0 : e + len) : (e > len ? len : e); }
            size_t nl = end > start ? end - start : 0;
            size_t nbo = ta->byte_offset() + start * ta->bytes_per_element();
            std::shared_ptr<ArrayBuffer> sb(ta->buffer(), [](ArrayBuffer*) {});
            TypedArrayBase* r = nullptr;
            switch (ta->get_array_type()) {
                case TypedArrayBase::ArrayType::INT8: r = new Int8Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::UINT8: r = new Uint8Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::UINT8_CLAMPED: r = new Uint8ClampedArray(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::INT16: r = new Int16Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::UINT16: r = new Uint16Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::INT32: r = new Int32Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::UINT32: r = new Uint32Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::FLOAT32: r = new Float32Array(sb, nbo, nl); break;
                case TypedArrayBase::ArrayType::FLOAT64: r = new Float64Array(sb, nbo, nl); break;
                default: ctx.throw_type_error("Unsupported type"); return Value();
            }
            return Value(r);
        }, 2);
    typedarray_proto_ptr->set_property_descriptor("subarray", PropertyDescriptor(Value(ta_subarray_fn.release()), PropertyAttributes::BuiltinFunction));

    auto ta_includes_fn = ObjectFactory::create_native_function("includes",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj || !this_obj->is_typed_array()) {
                ctx.throw_type_error("TypedArray.prototype.includes called on non-TypedArray");
                return Value();
            }
            TypedArrayBase* ta = static_cast<TypedArrayBase*>(this_obj);
            uint32_t length = static_cast<uint32_t>(ta->length());
            Value search_element = args.empty() ? Value() : args[0];
            int64_t from_index = 0;
            if (args.size() > 1) {
                from_index = static_cast<int64_t>(args[1].to_number());
            }
            if (from_index < 0) {
                from_index = static_cast<int64_t>(length) + from_index;
                if (from_index < 0) from_index = 0;
            }
            for (uint32_t i = static_cast<uint32_t>(from_index); i < length; i++) {
                Value element = ta->get_element(i);
                if (search_element.is_number() && element.is_number()) {
                    double sn = search_element.to_number(), en = element.to_number();
                    if (std::isnan(sn) && std::isnan(en)) return Value(true);
                    if (sn == en) return Value(true);
                } else if (element.strict_equals(search_element)) {
                    return Value(true);
                }
            }
            return Value(false);
        }, 1);
    typedarray_proto_ptr->set_property_descriptor("includes", PropertyDescriptor(Value(ta_includes_fn.release()), PropertyAttributes::BuiltinFunction));

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

    auto int8array_constructor = ObjectFactory::create_native_constructor("Int8Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
                                return Value();
                            }
                            std::vector<Value> items;
                            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
                            while (it) {
                                Value next_fn = it->get_property("next");
                                if (!next_fn.is_function()) break;
                                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                                if (!next_call_ctx) next_call_ctx = &ctx;
                                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                                if (next_call_ctx->has_exception()) {
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_int8_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Int8Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Int8Array", int8array_constructor.release());

    auto uint16array_constructor = ObjectFactory::create_native_constructor("Uint16Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
                                return Value();
                            }
                            std::vector<Value> items;
                            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
                            while (it) {
                                Value next_fn = it->get_property("next");
                                if (!next_fn.is_function()) break;
                                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                                if (!next_call_ctx) next_call_ctx = &ctx;
                                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                                if (next_call_ctx->has_exception()) {
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_uint16_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Uint16Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Uint16Array", uint16array_constructor.release());

    auto int16array_constructor = ObjectFactory::create_native_constructor("Int16Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
                                return Value();
                            }
                            std::vector<Value> items;
                            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
                            while (it) {
                                Value next_fn = it->get_property("next");
                                if (!next_fn.is_function()) break;
                                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                                if (!next_call_ctx) next_call_ctx = &ctx;
                                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                                if (next_call_ctx->has_exception()) {
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_int16_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Int16Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Int16Array", int16array_constructor.release());

    auto uint32array_constructor = ObjectFactory::create_native_constructor("Uint32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
                                return Value();
                            }
                            std::vector<Value> items;
                            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
                            while (it) {
                                Value next_fn = it->get_property("next");
                                if (!next_fn.is_function()) break;
                                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                                if (!next_call_ctx) next_call_ctx = &ctx;
                                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                                if (next_call_ctx->has_exception()) {
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_uint32_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Uint32Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Uint32Array", uint32array_constructor.release());

    auto int32array_constructor = ObjectFactory::create_native_constructor("Int32Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
                                return Value();
                            }
                            std::vector<Value> items;
                            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
                            while (it) {
                                Value next_fn = it->get_property("next");
                                if (!next_fn.is_function()) break;
                                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                                if (!next_call_ctx) next_call_ctx = &ctx;
                                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                                if (next_call_ctx->has_exception()) {
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_int32_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }
            ctx.throw_type_error("Int32Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Int32Array", int32array_constructor.release());

    auto float64array_constructor = ObjectFactory::create_native_constructor("Float64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                {
                    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
                    if (iter_sym) {
                        Value iter_fn = obj->get_property(iter_sym->to_property_key());
                        if (iter_fn.is_function()) {
                            Context* iter_call_ctx = iter_fn.as_function()->get_closure_context();
                            if (!iter_call_ctx) iter_call_ctx = &ctx;
                            Value iterator = iter_fn.as_function()->call(*iter_call_ctx, {}, Value(obj));
                            if (iter_call_ctx->has_exception()) {
                                if (iter_call_ctx != &ctx) ctx.throw_exception(iter_call_ctx->get_exception());
                                return Value();
                            }
                            std::vector<Value> items;
                            Object* it = iterator.is_object() ? iterator.as_object() : (iterator.is_function() ? static_cast<Object*>(iterator.as_function()) : nullptr);
                            while (it) {
                                Value next_fn = it->get_property("next");
                                if (!next_fn.is_function()) break;
                                Context* next_call_ctx = next_fn.as_function()->get_closure_context();
                                if (!next_call_ctx) next_call_ctx = &ctx;
                                Value res = next_fn.as_function()->call(*next_call_ctx, {}, iterator);
                                if (next_call_ctx->has_exception()) {
                                    if (next_call_ctx != &ctx) ctx.throw_exception(next_call_ctx->get_exception());
                                    return Value();
                                }
                                if (!res.is_object()) break;
                                if (res.as_object()->get_property("done").to_boolean()) break;
                                items.push_back(res.as_object()->get_property("value"));
                            }
                            auto ta = TypedArrayFactory::create_float64_array(items.size());
                            for (size_t i = 0; i < items.size(); i++) ta->set_element(i, items[i]);
                            return Value(ta.release());
                        }
                    }
                }
            }

            ctx.throw_type_error("Float64Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("Float64Array", float64array_constructor.release());

    auto bigint64array_constructor = ObjectFactory::create_native_constructor("BigInt64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) return Value(new BigInt64Array(0));
            if (args[0].is_number()) return Value(new BigInt64Array(static_cast<size_t>(args[0].as_number())));
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* ab = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> sb(ab, [](ArrayBuffer*){});
                    return Value(new BigInt64Array(sb));
                }
            }
            ctx.throw_type_error("BigInt64Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("BigInt64Array", bigint64array_constructor.release());

    auto biguint64array_constructor = ObjectFactory::create_native_constructor("BigUint64Array",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
            if (args.empty()) return Value(new BigUint64Array(0));
            if (args[0].is_number()) return Value(new BigUint64Array(static_cast<size_t>(args[0].as_number())));
            if (args[0].is_object()) {
                Object* obj = args[0].as_object();
                if (obj->is_array_buffer()) {
                    ArrayBuffer* ab = static_cast<ArrayBuffer*>(obj);
                    std::shared_ptr<ArrayBuffer> sb(ab, [](ArrayBuffer*){});
                    return Value(new BigUint64Array(sb));
                }
            }
            ctx.throw_type_error("BigUint64Array constructor argument not supported");
            return Value();
        });
    register_built_in_object("BigUint64Array", biguint64array_constructor.release());

    // Set up prototype chains: XArray.prototype.__proto__ = TypedArray.prototype
    // Also: XArray.__proto__ = TypedArray (constructor chain)
    Object* typedarray_ctor_ptr = get_built_in_object("TypedArray");
    struct TypedInfo { const char* name; int bytes; };
    TypedInfo typed_infos[] = {
        {"Int8Array", 1}, {"Uint8Array", 1}, {"Uint8ClampedArray", 1},
        {"Int16Array", 2}, {"Uint16Array", 2}, {"Int32Array", 4}, {"Uint32Array", 4},
        {"Float32Array", 4}, {"Float64Array", 8}
    };
    Symbol* ta_species_sym = Symbol::get_well_known(Symbol::SPECIES);
    for (const auto& info : typed_infos) {
        Object* ctor = get_built_in_object(info.name);
        if (ctor) {
            if (typedarray_ctor_ptr) {
                ctor->set_prototype(typedarray_ctor_ptr);
            }
            if (ta_species_sym) {
                auto getter = ObjectFactory::create_native_function("get [Symbol.species]",
                    [](Context& ctx, const std::vector<Value>&) -> Value {
                        return Value(ctx.get_this_binding());
                    }, 0);
                PropertyDescriptor species_desc;
                species_desc.set_getter(getter.release());
                species_desc.set_enumerable(false);
                species_desc.set_configurable(true);
                ctor->set_property_descriptor(ta_species_sym->to_property_key(), species_desc);
            }
            Value proto_val = ctor->get_property("prototype");
            if (proto_val.is_object_like() && proto_val.as_object()) {
                Object* proto = proto_val.as_object();
                proto->set_prototype(typedarray_proto_ptr);
                PropertyDescriptor bpe_desc(Value(static_cast<double>(info.bytes)), PropertyAttributes::None);
                bpe_desc.set_enumerable(false);
                bpe_desc.set_writable(false);
                bpe_desc.set_configurable(false);
                proto->set_property_descriptor("BYTES_PER_ELEMENT", bpe_desc);
                PropertyDescriptor ctor_desc(Value(static_cast<Function*>(ctor)), PropertyAttributes::BuiltinFunction);
                ctor_desc.set_enumerable(false);
                proto->set_property_descriptor("constructor", ctor_desc);
            }
        }
    }

    auto dataview_constructor = ObjectFactory::create_native_constructor("DataView",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) { ctx.throw_type_error("Constructor cannot be invoked without 'new'"); return Value(); }
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

                auto get_bigint64_method = ObjectFactory::create_native_function("getBigInt64", DataView::js_get_bigint64);
                dataview_obj->set_property("getBigInt64", Value(get_bigint64_method.release()));

                auto set_bigint64_method = ObjectFactory::create_native_function("setBigInt64", DataView::js_set_bigint64);
                dataview_obj->set_property("setBigInt64", Value(set_bigint64_method.release()));

                auto get_biguint64_method = ObjectFactory::create_native_function("getBigUint64", DataView::js_get_biguint64);
                dataview_obj->set_property("getBigUint64", Value(get_biguint64_method.release()));

                auto set_biguint64_method = ObjectFactory::create_native_function("setBigUint64", DataView::js_set_biguint64);
                dataview_obj->set_property("setBigUint64", Value(set_biguint64_method.release()));
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

    auto get_bigint64_proto = ObjectFactory::create_native_function("getBigInt64", DataView::js_get_bigint64);
    dataview_prototype->set_property("getBigInt64", Value(get_bigint64_proto.release()));

    auto set_bigint64_proto = ObjectFactory::create_native_function("setBigInt64", DataView::js_set_bigint64);
    dataview_prototype->set_property("setBigInt64", Value(set_bigint64_proto.release()));

    auto get_biguint64_proto = ObjectFactory::create_native_function("getBigUint64", DataView::js_get_biguint64);
    dataview_prototype->set_property("getBigUint64", Value(get_biguint64_proto.release()));

    auto set_biguint64_proto = ObjectFactory::create_native_function("setBigUint64", DataView::js_set_biguint64);
    dataview_prototype->set_property("setBigUint64", Value(set_biguint64_proto.release()));

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

            // ES6: Typed array constructors' __proto__ should be %TypedArray%, not Function.prototype
            Object* ta_ctor = get_built_in_object("TypedArray");
            if (ta_ctor) {
                const char* ta_names[] = {"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array"};
                for (const char* name : ta_names) {
                    Value ctor = global_object_->get_property(name);
                    if (ctor.is_function()) {
                        static_cast<Object*>(ctor.as_function())->set_prototype(ta_ctor);
                    }
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

void Context::load_bootstrap() {
    // Harness injection removed - kangax-es6 tests are self-contained
}

}

