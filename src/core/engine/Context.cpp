/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/builtins/ArrayBuiltin.h"
#include "quanta/core/engine/builtins/StringBuiltin.h"
#include "quanta/core/engine/builtins/ObjectBuiltin.h"
#include "quanta/core/engine/builtins/NumberBuiltin.h"
#include "quanta/core/engine/builtins/BooleanBuiltin.h"
#include "quanta/core/engine/builtins/FunctionBuiltin.h"
#include "quanta/core/engine/builtins/BigIntBuiltin.h"
#include "quanta/core/engine/builtins/SymbolBuiltin.h"
#include "quanta/core/engine/builtins/ErrorBuiltin.h"
#include "quanta/core/engine/builtins/JsonBuiltin.h"
#include "quanta/core/engine/builtins/MathBuiltin.h"
#include "quanta/core/engine/builtins/IntlBuiltin.h"
#include "quanta/core/engine/builtins/DateBuiltin.h"
#include "quanta/core/engine/builtins/RegExpBuiltin.h"
#include "quanta/core/engine/builtins/PromiseBuiltin.h"
#include "quanta/core/engine/builtins/DisposableBuiltin.h"
#include "quanta/core/engine/builtins/IteratorBuiltin.h"
#include "quanta/core/engine/builtins/ArrayBufferBuiltin.h"
#include "quanta/core/engine/builtins/TypedArrayBuiltin.h"
#include "quanta/core/engine/builtins/GlobalsBuiltin.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/Temporal.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/MapSet.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/parser/AST.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace Quanta {



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
    register_function_builtins(*this);

    register_bigint_builtins(*this);

    register_symbol_builtins(*this);
    
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

    register_iterator_helpers(*this);
    
    Generator::setup_generator_prototype(*this);
    
    register_number_builtins(*this);
    register_boolean_builtins(*this);
    
    register_error_builtins(*this);
    
    register_json_builtins(*this);
    
    register_math_builtins(*this);

    register_intl_builtins(*this);

    register_date_builtins(*this);
    
    // Error subtypes registered in register_error_builtins

    register_regexp_builtins(*this);
    
    register_promise_builtins(*this);

    register_disposable_builtins(*this);

    register_iterator_constructor(*this);

    register_arraybuffer_builtins(*this);
    Proxy::setup_proxy(*this);
    Reflect::setup_reflect(*this);

}


void Context::setup_global_bindings() {
    register_global_builtins(*this);
}


void Context::setup_test262_helpers() {
    register_test262_builtins(*this);
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
    register_typed_array_builtins(*this);
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

