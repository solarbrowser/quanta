/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Context.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/gc/Visitor.h"
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



thread_local uint32_t Context::next_context_id_ = 1;

ContextSurvivorGuard::~ContextSurvivorGuard() {
    if (!ptr) return;
    // Provably unreachable from anywhere -- see the doc comment above the
    // struct. Let it destruct normally instead of paying survivor-pool
    // registration (which also forces extra major GC cycles, Heap.cpp).
    if (!ptr->exposed_to_escape() &&
        (!ptr->get_owned_env() || !ptr->get_owned_env()->is_escaped())) {
        return;
    }
    if (eng) eng->add_survivor_context(ptr.release());
}

void Environment::gc_trace(Visitor& v) const {
    for (const auto& kv : slots_) v.visit(kv.second.value);
    v.visit_object(binding_object_);
    v.visit_environment(outer_environment_);
}

void StackFrame::gc_trace(Visitor& v) const {
    v.visit_object(function_);
    v.visit_object(this_binding_);
    for (const auto& a : arguments_) v.visit(a);
    for (const auto& lv : local_variables_) v.visit(lv.second);
    v.visit_environment(environment_);
}

void Context::gc_trace(Visitor& v) const {
    v.visit_environment(lexical_environment_);
    v.visit_environment(variable_environment_);
    v.visit_object(this_binding_);
    v.visit_object(global_object_);
    for (const auto& e : built_in_objects_) v.visit_object(e.second);
    for (const auto& e : built_in_functions_) v.visit_object(e.second);
    v.visit(current_exception_);
    v.visit(return_value_);
    v.visit(new_target_);
    v.visit(import_meta_);
    for (const auto& frame : call_stack_) {
        if (frame) frame->gc_trace(v);
    }
    for (const auto& entry : microtask_queue_) {
        for (const auto& kept : entry.keep_alive) v.visit(kept);
    }
    for (const auto& entry : draining_queue_) {
        for (const auto& kept : entry.keep_alive) v.visit(kept);
    }
}


namespace {
// Freed Context blocks awaiting reuse. Never subclassed, so every block is
// exactly sizeof(Context) -- no per-block size bookkeeping needed.
constexpr size_t kContextPoolCap = 512;
thread_local std::vector<void*> g_context_pool;
}

void* Context::operator new(size_t size) {
    if (!g_context_pool.empty()) {
        void* p = g_context_pool.back();
        g_context_pool.pop_back();
        return p;
    }
    return ::operator new(size);
}

void Context::operator delete(void* ptr) {
    if (!ptr) return;
    if (g_context_pool.size() < kContextPoolCap) {
        g_context_pool.push_back(ptr);
        return;
    }
    ::operator delete(ptr);
}

Context::Context(Engine* engine, Type type)
    : type_(type), state_(State::Running), context_id_(next_context_id_++),
      lexical_environment_(nullptr), variable_environment_(nullptr), this_binding_(nullptr),
      execution_depth_(0), global_object_(nullptr), current_exception_(), has_exception_(false),
      return_value_(), has_return_value_(false), has_break_(false), has_continue_(false),
      current_loop_label_(), next_statement_label_(), is_in_constructor_call_(false), super_called_(false), this_needs_super_(false),
      strict_mode_(false), engine_(engine), current_filename_("<unknown>") {

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
      is_in_constructor_call_(false), super_called_(false), this_needs_super_(false), strict_mode_(parent && type != Type::Function ? parent->strict_mode_ : false),
      engine_(engine), current_filename_(parent ? parent->current_filename_ : "<unknown>") {



    // Builtins are registered once on the global context; children resolve
    // through that root instead of copying both maps on every call (the
    // copies dominated call-heavy profiles). The root is the engine's
    // global context, which outlives every child -- a parent pointer would
    // dangle for contexts that outlive their creator (generators/fibers).
    builtins_root_ = parent ? (parent->builtins_root_ ? parent->builtins_root_ : parent) : nullptr;
}

Context::~Context() {
    call_stack_.clear();
    if (owned_env_ && !owned_env_->is_escaped()) {
        Collector::release_env(owned_env_);
    }
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

Environment* Context::find_binding_env(const std::string& name) const {
    Environment* env = lexical_environment_;
    while (env) {
        if (env->has_own_binding(name)) return env;
        env = env->get_outer();
    }
    return nullptr;
}

Environment* Environment::find_binding_env(const std::string& name) {
    if (has_own_binding(name)) return this;
    if (outer_environment_) return outer_environment_->find_binding_env(name);
    return nullptr;
}

std::unordered_map<std::string, Value> Context::snapshot_bindings() const {
    std::unordered_map<std::string, Value> snap;
    Environment* env = lexical_environment_;
    while (env) {
        for (const auto& name : env->get_binding_names()) {
            if (snap.find(name) == snap.end()) {
                Value v = env->get_binding(name);
                if (!v.is_undefined()) snap[name] = v;
            }
        }
        env = env->get_outer();
    }
    return snap;
}

void Context::restore_bindings(const std::unordered_map<std::string, Value>& snapshot) {
    for (const auto& pair : snapshot) {
        // Use set_binding which walks the chain to find and update the binding
        if (lexical_environment_ && lexical_environment_->has_binding(pair.first)) {
            lexical_environment_->set_binding(pair.first, pair.second);
        }
    }
}

bool Context::is_lexical_const(const std::string& name) const {
    Environment* env = lexical_environment_;
    while (env) {
        if (env->get_type() == Environment::Type::Object) {
            if (env->is_const_binding(name)) return true;
            if (env->get_binding_object() && env->get_binding_object()->has_property(name)) return false;
        } else if (env->has_mutable_flag(name)) {
            return !env->is_mutable_binding(name);
        }
        env = env->get_outer();
    }
    return false;
}

// Only returns true for explicit `const` bindings -- NOT for named function expression name bindings which are immutable but not marked const.
bool Context::is_strict_const(const std::string& name) const {
    Environment* env = lexical_environment_;
    while (env) {
        if (env->get_type() == Environment::Type::Object) {
            if (env->is_const_binding(name)) return true;
            if (env->get_binding_object() && env->get_binding_object()->has_property(name)) return false;
        } else if (env->has_mutable_flag(name)) {
            return !env->is_mutable_binding(name) && env->is_const_binding(name);
        }
        env = env->get_outer();
    }
    return false;
}

bool Context::create_binding(const std::string& name, const Value& value, bool mutable_binding, bool deletable, bool enumerable) {
    if (variable_environment_) {
        return variable_environment_->create_binding(name, value, mutable_binding, deletable, enumerable);
    }
    return false;
}

void Context::create_binding_force(const std::string& name, const Value& value) {
    if (variable_environment_) {
        variable_environment_->force_set_binding(name, value);
    }
}

void Context::create_lexical_binding_force(const std::string& name, const Value& value) {
    if (lexical_environment_) {
        lexical_environment_->force_set_binding(name, value);
    }
}

bool Context::create_var_binding(const std::string& name, const Value& value, bool mutable_binding) {
    if (variable_environment_) {
        // Regular var has DontDelete (non-deletable). Variables created by eval are deletable
        // per spec 18.2.1.3 CreateGlobalVarBinding(N, true).
        bool deletable = (type_ == Type::Eval);
        return variable_environment_->create_binding(name, value, mutable_binding, deletable);
    }
    return false;
}

bool Context::create_lexical_binding(const std::string& name, const Value& value, bool mutable_binding) {
    if (lexical_environment_) {
        bool ok = lexical_environment_->create_binding(name, value, mutable_binding, false); // lexical bindings are never deletable
        if (ok) {
            lexical_environment_->mark_lexical_declaration(name);
            if (!mutable_binding) lexical_environment_->mark_const_binding(name);
        }
        return ok;
    }
    return false;
}

void Context::create_global_function_binding(const std::string& name, const Value& value, bool configurable) {
    if (variable_environment_) {
        variable_environment_->create_global_function_binding(name, value, configurable);
    }
}

bool Context::is_in_tdz(const std::string& name) const {
    Environment* env = lexical_environment_;
    while (env) {
        if (env->get_type() != Environment::Type::Object && env->has_own_binding(name)) {
            return !env->is_initialized_binding(name);
        }
        env = env->get_outer();
    }
    return false;
}

bool Context::delete_binding(const std::string& name) {
    // Walk the lexical environment chain to find the first env that owns this binding,
    // so that `delete x` inside a with(obj) block deletes from obj, not the global.
    Environment* env = lexical_environment_;
    while (env) {
        if (env->has_own_binding(name)) {
            return env->delete_binding(name);
        }
        env = env->get_outer();
    }
    // Unresolvable reference: spec 13.5.1.2 step 4c -- return true
    return true;
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
    Value error_ctor = get_binding("Error");
    if (error_ctor.is_function()) {
        Value proto = error_ctor.as_function()->get_property("prototype");
        if (proto.is_object()) error->set_prototype(proto.as_object());
    }
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

Value Context::get_import_meta() {
    if (import_meta_.is_undefined()) {
        auto meta_obj = ObjectFactory::create_object();
        meta_obj->set_property("url", Value(std::string("file://") + current_filename_));
        import_meta_ = Value(meta_obj.release());
    }
    return import_meta_;
}

void Context::queue_microtask(std::function<void()> task, std::vector<Value> keep_alive) {
    microtask_queue_.push_back({std::move(task), std::move(keep_alive)});
}

void Context::drain_microtasks() {
    // Loops until empty (a job can enqueue more). The 10s cap guards against a runaway microtask chain -- unrelated to setTimeout/setInterval, which run through EventLoop's timer heap instead.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!microtask_queue_.empty()) {
        draining_queue_ = std::move(microtask_queue_);
        microtask_queue_.clear();
        for (auto& entry : draining_queue_) {
            if (entry.task) entry.task();
        }
        draining_queue_.clear();
        if (std::chrono::steady_clock::now() > deadline) break;
    }
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
    if (it != built_in_objects_.end()) return it->second;
    if (builtins_root_) {
        auto rit = builtins_root_->built_in_objects_.find(name);
        if (rit != builtins_root_->built_in_objects_.end()) return rit->second;
    }
    return nullptr;
}

Function* Context::get_built_in_function(const std::string& name) const {
    auto it = built_in_functions_.find(name);
    if (it != built_in_functions_.end()) return it->second;
    if (builtins_root_) {
        auto rit = builtins_root_->built_in_functions_.find(name);
        if (rit != builtins_root_->built_in_functions_.end()) return rit->second;
    }
    return nullptr;
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

    // global_object_ predates Object.prototype's setup above; patch it now.
    if (!global_object_->get_prototype()) {
        Object* object_proto = ObjectFactory::get_object_prototype();
        if (object_proto) global_object_->set_prototype(object_proto);
    }

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
    WeakRef::setup_weakref_prototype(*this);
    FinalizationRegistry::setup_finalization_registry_prototype(*this);

    AsyncUtils::setup_async_functions(*this);
    AsyncIterator::setup_async_iterator_prototype(*this);
    AsyncGenerator::setup_async_generator_prototype(*this);
    
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

    // Set [[Prototype]] of built-in constructors to Function.prototype so that
    // Object.getPrototypeOf(Array) === Function.prototype etc.
    {
        Value fn_ctor = get_binding("Function");
        if (fn_ctor.is_function()) {
            Value fn_proto = fn_ctor.as_function()->get_property("prototype");
            if (fn_proto.is_object()) {
                Object* fp = fn_proto.as_object();
                static const char* ctors[] = {
                    "Array","Object","String","Number","Boolean","RegExp","Date",
                    "Error","TypeError","RangeError","ReferenceError","SyntaxError",
                    "URIError","EvalError","AggregateError","SuppressedError",
                    "Promise","Map","Set","WeakMap","WeakSet","Symbol","BigInt",
                    "ArrayBuffer","SharedArrayBuffer","DataView","Proxy","Reflect",
                    "Math","JSON","Iterator","Generator",
                    nullptr
                };
                for (int ci = 0; ctors[ci]; ++ci) {
                    Value ctor = get_binding(ctors[ci]);
                    Object* cobj = nullptr;
                    if (ctor.is_function()) cobj = static_cast<Object*>(ctor.as_function());
                    else if (ctor.is_object()) cobj = ctor.as_object();
                    if (cobj) cobj->set_prototype(fp);
                }
            }
        }
    }
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
            auto it = slots_.find(name);
            if (it != slots_.end()) {
                return it->second.value;
            }
        }
    }

    if (outer_environment_) {
        return outer_environment_->get_binding_with_depth(name, depth + 1);
    }
    
    return Value();
}

bool Environment::set_binding(const std::string& name, const Value& value) {
    Collector::write_barrier_env(this);
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->set_property(name, value);
        } else {
            if (is_mutable_binding(name)) {
                slots_[name].value = value;
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

Value Environment::get_binding_direct(const std::string& name, Context* ctx) const {
    if (type_ == Type::Object && binding_object_) {
        // GetBindingValue step 2: its own HasProperty check, independent of (and without re-checking @@unscopables like) the HasBinding call that already resolved this environment.
        // A side effect of that earlier @@unscopables  getter (or other code) may have deleted the property in the meantime
        // step 3a: strict mode throws ReferenceError, otherwise return undefined.
        if (!binding_object_->has_property(name)) {
            if (ctx && ctx->is_strict_mode()) {
                ctx->throw_reference_error("'" + name + "' is not defined");
            }
            return Value();
        }
        return binding_object_->get_property(name);
    }
    auto it = slots_.find(name);
    if (it != slots_.end()) return it->second.value;
    return Value();
}

Value* Environment::stable_binding_slot(const std::string& name) {
    if (type_ == Type::Object || is_with_environment_) return nullptr;
    auto it = slots_.find(name);
    if (it == slots_.end()) return nullptr;
    if (it->second.deletable) return nullptr;
    if (!it->second.mutable_flag) return nullptr;
    if (!it->second.initialized) return nullptr;
    return &it->second.value;
}

bool Environment::set_binding_direct(const std::string& name, const Value& value, Context* ctx) {
    Collector::write_barrier_env(this);
    if (type_ == Type::Object && binding_object_) {
        // SetMutableBinding step 2-3: strict mode throws if the binding vanished (e.g. a `with` getter deleted it mid-update).
        bool still_exists = binding_object_->has_property(name);
        if (!still_exists && ctx && ctx->is_strict_mode()) {
            ctx->throw_reference_error("'" + name + "' is not defined");
            return false;
        }
        return binding_object_->set_property(name, value);
    }
    if (is_mutable_binding(name)) {
        slots_[name].value = value;
        return true;
    }
    return false;
}

void Environment::force_set_binding(const std::string& name, const Value& value) {
    Collector::write_barrier_env(this);
    if (type_ == Type::Object && binding_object_) {
        binding_object_->set_property(name, value);
    } else {
        auto& slot = slots_[name];
        slot.value = value;
        slot.mutable_flag = true;
        slot.initialized = true;
    }
}

void Environment::create_uninitialized_binding(const std::string& name, bool is_mutable) {
    Collector::write_barrier_env(this);
    if (has_own_binding(name)) return;
    slots_[name] = BindingSlot{Value(), is_mutable, false, false};
}

void Environment::create_global_function_binding(const std::string& name, const Value& value, bool configurable) {
    Collector::write_barrier_env(this);
    if (type_ == Type::Object && binding_object_) {
        PropertyDescriptor existing = binding_object_->get_property_descriptor(name);
        PropertyDescriptor desc;
        if (!binding_object_->has_own_property(name) || existing.is_configurable()) {
            int attrs = PropertyAttributes::Writable | PropertyAttributes::Enumerable;
            if (configurable) attrs |= PropertyAttributes::Configurable;
            desc = PropertyDescriptor(value, static_cast<PropertyAttributes>(attrs));
        } else {
            desc = PropertyDescriptor(value, existing.get_attributes());
        }
        binding_object_->set_property_descriptor(name, desc);
    } else {
        slots_[name] = BindingSlot{value, true, true, true};
    }
}

bool Environment::create_binding(const std::string& name, const Value& value, bool mutable_binding, bool deletable, bool enumerable) {
    Collector::write_barrier_env(this);
    if (has_own_binding(name)) {
        return false;
    }

    if (type_ == Type::Object && binding_object_) {
        int attrs_value = 0;
        if (enumerable) attrs_value |= PropertyAttributes::Enumerable;
        if (mutable_binding) attrs_value |= PropertyAttributes::Writable;
        if (deletable) attrs_value |= PropertyAttributes::Configurable;
        PropertyAttributes attrs = static_cast<PropertyAttributes>(attrs_value);
        PropertyDescriptor desc(value, attrs);
        return binding_object_->set_property_descriptor(name, desc);
    } else {
        slots_[name] = BindingSlot{value, mutable_binding, true, deletable};
        return true;
    }
}

bool Environment::delete_binding(const std::string& name) {
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->delete_property(name);
        } else {
            // ES1: Check if binding is deletable (DontDelete attribute)
            auto it = slots_.find(name);
            bool deletable = (it != slots_.end()) ? it->second.deletable : false;

            if (!deletable) {
                return false;
            }

            slots_.erase(it);
            return true;
        }
    }

    return false;
}

bool Environment::is_mutable_binding(const std::string& name) const {
    auto it = slots_.find(name);
    return (it != slots_.end()) ? it->second.mutable_flag : true;
}

bool Environment::has_mutable_flag(const std::string& name) const {
    return slots_.find(name) != slots_.end();
}

bool Environment::is_initialized_binding(const std::string& name) const {
    auto it = slots_.find(name);
    return (it != slots_.end()) ? it->second.initialized : false;
}

void Environment::initialize_binding(const std::string& name, const Value& value) {
    Collector::write_barrier_env(this);
    auto& slot = slots_[name];
    slot.value = value;
    slot.initialized = true;
}

std::vector<std::string> Environment::get_binding_names() const {
    std::vector<std::string> names;
    
    if (type_ == Type::Object && binding_object_) {
        auto keys = binding_object_->get_own_property_keys();
        names.insert(names.end(), keys.begin(), keys.end());
    } else {
        for (const auto& pair : slots_) {
            names.push_back(pair.first);
        }
    }

    return names;
}

std::string Environment::debug_string() const {
    std::ostringstream oss;
    oss << "Environment(type=" << static_cast<int>(type_)
        << ", bindings=" << slots_.size() << ")";
    return oss.str();
}

// Internal engine bookkeeping bindings (this, __super__, __eval_caller_this__, ...) are never realized as actual object-environment-record properties per spec
static bool is_internal_binding_name(const std::string& name) {
    if (name == "this") return true;
    return name.size() > 4 && name[0] == '_' && name[1] == '_' &&
           name[name.size() - 1] == '_' && name[name.size() - 2] == '_';
}

bool Environment::has_own_binding(const std::string& name) const {
    if (type_ == Type::Object && binding_object_) {
        if (is_with_environment_ && is_internal_binding_name(name)) return false;
        if (!binding_object_->has_property(name)) return false;
        // ES6 8.1.1.2.1 HasBinding: @@unscopables is only consulted when the
        // withEnvironment flag is set -- i.e. for `with` statement object
        // environments, not ordinary object environments (global object, etc).
        if (!is_with_environment_) return true;
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
        return slots_.find(name) != slots_.end();
    }
}


namespace ContextFactory {

std::unique_ptr<Context> create_global_context(Engine* engine) {
    return std::make_unique<Context>(engine, Context::Type::Global);
}

std::unique_ptr<Context> create_function_context(Engine* engine, Context* parent, Function* function) {
    auto context = std::make_unique<Context>(engine, parent, Context::Type::Function);

    Environment* outer_env;
    if (function && function->get_closure_environment()) {
        // Prefer the lexical environment captured at function-creation time:
        // closure_context_'s lexical_environment_ is a mutable pointer that the
        // context reuses and reassigns as it moves through blocks/statements, so
        // resolving it now (at call time, possibly long after creation) would
        // yield whatever scope the context happens to be in *now* -- not the
        // scope that was active when this closure was created.
        outer_env = function->get_closure_environment();
    } else if (function && function->get_closure_context()) {
        outer_env = function->get_closure_context()->get_lexical_environment();
    } else {
        outer_env = parent->get_lexical_environment();
    }
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

    // The new context's env chain hangs off outer_env and can outlive the
    // current block (fibers, survivor contexts) -- pin the chain.
    if (outer_env) outer_env->mark_escaped();
    auto func_env = std::make_unique<Environment>(Environment::Type::Function, outer_env);
    context->set_lexical_environment(func_env.release());
    context->set_variable_environment(context->get_lexical_environment());
    context->set_owned_env(context->get_lexical_environment());

    return context;
}

std::unique_ptr<Context> create_eval_context(Engine* engine, Context* parent) {
    auto context = std::make_unique<Context>(engine, parent, Context::Type::Eval);

    if (parent->get_lexical_environment()) parent->get_lexical_environment()->mark_escaped();
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
        Environment* popped = lexical_environment_;
        lexical_environment_ = popped->get_outer();
        // Captured chains were pinned via mark_escaped(); an unescaped env is
        // dead the moment it's popped.
        if (!popped->is_escaped()) {
            Collector::release_env(popped);
        }
    }
}

void Context::push_dispose_scope() {
    dispose_scope_stack_.push_back({});
}

void Context::add_disposable_resource(const Value& resource, const Value& method, bool is_async_dispose) {
    if (!dispose_scope_stack_.empty()) {
        dispose_scope_stack_.back().push_back({resource, method, is_async_dispose});
    }
}

bool await_value(Context& ctx, const Value& value, Value& out_result) {
    AsyncGenerator* async_gen = AsyncGenerator::get_current();
    AsyncExecutor* exec = AsyncExecutor::get_current();
    bool in_gen_fiber = async_gen && async_gen->fiber_stack_ != nullptr;
    bool in_exec_fiber = !in_gen_fiber && exec && exec->fiber_stack_ != nullptr;

    Context* gctx = nullptr;
    if (in_gen_fiber) {
        gctx = async_gen->get_outer_context() ? async_gen->get_outer_context() : async_gen->get_generator_context();
    } else if (in_exec_fiber) {
        gctx = exec->engine_ ? exec->engine_->get_current_context() : exec->exec_context_;
    }

    Promise* awaited_promise = nullptr;
    Value settled_val;
    bool settled_throw = false;
    bool is_pending = false;
    Value wrapped_keepalive;

    if (AsyncUtils::is_promise(value)) {
        // Await -> PromiseResolve(%Promise%, value) reads value.constructor;
        // a throwing getter makes the whole Await complete abruptly.
        Context* prev_cc = Object::current_context_;
        Object::current_context_ = &ctx;
        value.as_object()->get_property("constructor");
        Object::current_context_ = prev_cc;
        if (ctx.has_exception()) {
            out_result = ctx.get_exception();
            ctx.clear_exception();
            return true;
        }
        awaited_promise = static_cast<Promise*>(value.as_object());
    } else if (AsyncUtils::is_thenable(value)) {
        auto wrapped_obj = ObjectFactory::create_promise(gctx);
        Promise* wrapped_raw = static_cast<Promise*>(wrapped_obj.get());
        auto res_fn = ObjectFactory::create_native_function("",
            [wrapped_raw](Context&, const std::vector<Value>& args) -> Value {
                wrapped_raw->fulfill(args.empty() ? Value() : args[0]); return Value();
            }, 1);
        auto rej_fn = ObjectFactory::create_native_function("",
            [wrapped_raw](Context&, const std::vector<Value>& args) -> Value {
                wrapped_raw->reject(args.empty() ? Value() : args[0]); return Value();
            }, 1);
        wrapped_raw->set_property("__tr_", Value(res_fn.release()));
        wrapped_raw->set_property("__tj_", Value(rej_fn.release()));
        Object* thenable_obj = value.as_object();
        Value then_val = thenable_obj->get_property("then");
        if (then_val.is_function()) {
            Value r = wrapped_raw->get_property("__tr_");
            Value j = wrapped_raw->get_property("__tj_");
            AsyncUtils::call_thenable_job(gctx ? gctx : &ctx, then_val.as_function(), value, r, j, wrapped_raw);
        }
        awaited_promise = wrapped_raw;
        wrapped_keepalive = Value(wrapped_obj.release());
    }

    if (awaited_promise) {
        if (awaited_promise->get_state() == PromiseState::FULFILLED) {
            settled_val = awaited_promise->get_value();
        } else if (awaited_promise->get_state() == PromiseState::REJECTED) {
            settled_val = awaited_promise->get_value();
            settled_throw = true;
        } else {
            is_pending = true;
        }
    } else {
        settled_val = value;
    }

    if (in_gen_fiber) {
        if (is_pending) {
            auto self = async_gen;
            auto on_f = ObjectFactory::create_native_function("",
                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                    Value val = args.empty() ? Value() : args[0];
                    self->resume_from_await(val, false);
                    return Value();
                });
            auto on_r = ObjectFactory::create_native_function("",
                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                    Value reason = args.empty() ? Value() : args[0];
                    self->resume_from_await(reason, true);
                    return Value();
                });
            awaited_promise->then(on_f.release(), on_r.release());
        } else {
            auto self = async_gen;
            Value val = settled_val; bool thr = settled_throw;
            if (gctx) gctx->queue_microtask([self, val, thr]() mutable { self->resume_from_await(val, thr); }, {Value(self), val});
        }
        async_gen->await_result_ = wrapped_keepalive.is_undefined() ? value : wrapped_keepalive;
        async_gen->suspend_reason_ = AsyncGenerator::SuspendReason::Await;
        swapcontext(&async_gen->fiber_->fiber_ctx, &async_gen->fiber_->caller_ctx);
        if (async_gen->await_is_throw_) {
            out_result = async_gen->await_result_;
            async_gen->await_is_throw_ = false;
            async_gen->await_result_ = Value();
            return true;
        }
        out_result = async_gen->await_result_;
        async_gen->await_result_ = Value();
        return false;
    }

    if (in_exec_fiber) {
        if (is_pending) {
            auto self = exec->shared_from_this();
            auto on_f = ObjectFactory::create_native_function("",
                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                    Value val = args.empty() ? Value() : args[0];
                    self->resume(val, false);
                    return Value();
                });
            auto on_r = ObjectFactory::create_native_function("",
                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                    Value reason = args.empty() ? Value() : args[0];
                    self->resume(reason, true);
                    return Value();
                });
            awaited_promise->then(on_f.release(), on_r.release());
        } else {
            auto self = exec->shared_from_this();
            Value val = settled_val; bool thr = settled_throw;
            if (gctx) gctx->queue_microtask([self, val, thr]() mutable { self->resume(val, thr); }, {val});
        }
        exec->await_result_ = wrapped_keepalive.is_undefined() ? value : wrapped_keepalive;
        swapcontext(&exec->fiber_->fiber_ctx, &exec->fiber_->caller_ctx);
        if (exec->await_is_throw_) {
            out_result = exec->await_result_;
            exec->await_is_throw_ = false;
            exec->await_result_ = Value();
            return true;
        }
        out_result = exec->await_result_;
        exec->await_result_ = Value();
        return false;
    }

    // No active fiber (e.g. top-level await context): drain microtasks synchronously, bounded.
    if (awaited_promise && awaited_promise->get_state() == PromiseState::PENDING) {
        Context* top_gctx = ctx.get_engine() ? ctx.get_engine()->get_global_context() : &ctx;
        int spins = 0;
        while (awaited_promise->get_state() == PromiseState::PENDING && spins < 100000) {
            if (!top_gctx || !top_gctx->has_pending_microtasks()) break;
            top_gctx->drain_microtasks();
            spins++;
        }
        if (awaited_promise->get_state() == PromiseState::FULFILLED) {
            settled_val = awaited_promise->get_value();
        } else if (awaited_promise->get_state() == PromiseState::REJECTED) {
            settled_val = awaited_promise->get_value();
            settled_throw = true;
        }
    }
    out_result = settled_val;
    return settled_throw;
}

void Context::run_dispose_resources() {
    if (dispose_scope_stack_.empty()) return;

    auto resources = std::move(dispose_scope_stack_.back());
    dispose_scope_stack_.pop_back();

    // Capture any existing exception
    bool had_exception = has_exception_;
    Value saved_exception = current_exception_;
    if (had_exception) clear_exception();

    // current_error tracks the "pending" throw completion (starts as the saved body exception)
    Value current_error = had_exception ? saved_exception : Value();
    bool has_current_error = had_exception;

    // Dispose in reverse order (spec: reverse list order)
    for (auto it = resources.rbegin(); it != resources.rend(); ++it) {
        // No method: `await using` still owes its Await(undefined) tick (Dispose step 3); plain `using` skips entirely.
        if (!it->dispose_method.is_function() && !it->is_async_dispose) continue;

        Value call_result;
        if (it->dispose_method.is_function()) {
            Function* fn = it->dispose_method.as_function();
            call_result = fn->call(*this, {}, it->resource_value);
        }

        bool threw = has_exception_;
        Value new_error;
        if (threw) {
            new_error = current_exception_;
            clear_exception();
        } else if (it->is_async_dispose) {
            // `await using` always Awaits the call's result, even a sync Symbol.dispose fallback.
            Value awaited;
            if (await_value(*this, call_result, awaited)) {
                threw = true;
                new_error = awaited;
            }
        }

        if (threw) {
            if (has_current_error) {
                // Spec: create SuppressedError(newError, existingError)
                Value suppressed_ctor = get_global_object()
                    ? get_global_object()->get_property("SuppressedError") : Value();
                if (suppressed_ctor.is_function()) {
                    std::vector<Value> se_args = {new_error, current_error};
                    Value se = suppressed_ctor.as_function()->call(*this, se_args, Value());
                    if (!has_exception_) {
                        current_error = se;
                        has_current_error = true;
                        continue;
                    }
                    clear_exception();
                }
                // Fallback: just use new_error
                current_error = new_error;
            } else {
                current_error = new_error;
            }
            has_current_error = true;
        }
    }

    if (has_current_error) {
        throw_exception(current_error, true);
    }
}

void Context::push_with_scope(Object* obj) {
    // Create object environment for with statement
    auto new_env = std::make_unique<Environment>(obj, lexical_environment_);
    new_env->set_with_environment(true);
    lexical_environment_ = new_env.release();
}

void Context::pop_with_scope() {
    if (lexical_environment_ && lexical_environment_->get_outer()) {
        // Do not delete the with-environment: a closure created inside the
        // with body may have captured this Environment* directly (see
        // Function::closure_environment_), and deleting it here would leave
        // that closure holding a dangling pointer. Block scopes are leaked
        // for the same reason (see BlockStatement::evaluate) and environments
        // are already swept by Context::~Context(), so leaking here is
        // consistent.
        lexical_environment_ = lexical_environment_->get_outer();
    }
}

void Context::register_typed_array_constructors() {
    register_typed_array_builtins(*this);
}

void Context::load_bootstrap() {
    // Harness injection removed - kangax-es6 tests are self-contained
}

}

