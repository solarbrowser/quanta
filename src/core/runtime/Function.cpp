/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Object.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/parser/AST.h"
#include <sstream>
#include <iostream>
#include <chrono>
#include <unordered_set>

#ifdef _MSC_VER
#include <xmmintrin.h>
#endif

namespace Quanta {
    class Engine;
    class JITCompiler;
}

namespace Quanta {

Function::Function(const std::string& name,
                   const std::vector<std::string>& params,
                   std::unique_ptr<ASTNode> body,
                   Context* closure_context)
    : Object(ObjectType::Function), name_(name), parameters_(params),
      body_(std::move(body)), closure_context_(closure_context),
      closure_environment_(closure_context ? closure_context->get_lexical_environment() : nullptr),
      prototype_(nullptr), is_native_(false), is_constructor_(true), is_arrow_(false), is_class_constructor_(false), is_strict_(false), is_param_default_(false), execution_count_(0), is_hot_(false) {
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();

    // ES5 13.2: function.prototype is {writable:true, enumerable:false, configurable:false}
    {
        PropertyDescriptor proto_desc(Value(prototype_), PropertyAttributes::Writable);
        proto_desc.set_enumerable(false);
        proto_desc.set_configurable(false);
        this->set_property_descriptor("prototype", proto_desc);
    }
    // ES5 13.2: .prototype.constructor is {writable:true, enumerable:false, configurable:true}
    PropertyDescriptor ctor_desc(Value(this), static_cast<PropertyAttributes>(
        PropertyAttributes::Writable | PropertyAttributes::Configurable));
    prototype_->set_property_descriptor("constructor", ctor_desc);

    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);
    PropertyDescriptor length_desc(Value(static_cast<double>(parameters_.size())), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);

    // [[Prototype]] (not the .prototype property above): direct construction sites that bypass
    // ObjectFactory::create_js_function would otherwise leave this null.
    if (Object* func_proto = ObjectFactory::get_function_prototype()) {
        set_prototype(func_proto);
    }
}

Function::Function(const std::string& name,
                   std::vector<std::unique_ptr<Parameter>> params,
                   std::unique_ptr<ASTNode> body,
                   Context* closure_context)
    : Object(ObjectType::Function), name_(name), parameter_objects_(std::move(params)),
      body_(std::move(body)), closure_context_(closure_context),
      closure_environment_(closure_context ? closure_context->get_lexical_environment() : nullptr),
      prototype_(nullptr), is_native_(false), is_constructor_(true), is_arrow_(false), is_class_constructor_(false), is_strict_(false), is_param_default_(false), execution_count_(0), is_hot_(false) {
    for (const auto& param : parameter_objects_) {
        parameters_.push_back(param->get_name()->get_name());
    }
    
    auto proto = ObjectFactory::create_object();
    prototype_ = proto.release();

    {
        PropertyDescriptor proto_desc2(Value(prototype_), PropertyAttributes::Writable);
        proto_desc2.set_enumerable(false);
        proto_desc2.set_configurable(false);
        this->set_property_descriptor("prototype", proto_desc2);
    }
    {
        PropertyDescriptor ctor_desc2(Value(this), static_cast<PropertyAttributes>(
            PropertyAttributes::Writable | PropertyAttributes::Configurable));
        prototype_->set_property_descriptor("constructor", ctor_desc2);
    }

    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);
    // ES6: length = number of params before first rest or default
    size_t formal_length = 0;
    for (const auto& param : parameter_objects_) {
        if (param->is_rest() || param->has_default()) break;
        formal_length++;
    }
    PropertyDescriptor length_desc(Value(static_cast<double>(formal_length)), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);

    // [[Prototype]] (not the .prototype property above): direct construction sites that bypass
    // ObjectFactory::create_js_function would otherwise leave this null.
    if (Object* func_proto = ObjectFactory::get_function_prototype()) {
        set_prototype(func_proto);
    }
}

Function::Function(const std::string& name,
                   std::function<Value(Context&, const std::vector<Value>&)> native_fn,
                   bool create_prototype)
    : Object(ObjectType::Function), name_(name), closure_context_(nullptr), closure_environment_(nullptr),
      prototype_(nullptr), is_native_(true), is_constructor_(create_prototype), is_arrow_(false),
      is_class_constructor_(false), is_strict_(false), is_param_default_(false), native_fn_(native_fn), execution_count_(0), is_hot_(false) {
    if (create_prototype) {
        auto proto = ObjectFactory::create_object();
        prototype_ = proto.release();
        PropertyDescriptor prototype_desc(Value(prototype_), PropertyAttributes::None);
        this->set_property_descriptor("prototype", prototype_desc);
    }

    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);

    PropertyDescriptor length_desc(Value(static_cast<double>(0)), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);

}

Function::Function(const std::string& name,
                   std::function<Value(Context&, const std::vector<Value>&)> native_fn,
                   uint32_t arity,
                   bool create_prototype)
    : Object(ObjectType::Function), name_(name), closure_context_(nullptr), closure_environment_(nullptr),
      prototype_(nullptr), is_native_(true), is_constructor_(create_prototype), is_arrow_(false),
      is_class_constructor_(false), is_strict_(false), is_param_default_(false), native_fn_(native_fn), execution_count_(0), is_hot_(false) {
    if (create_prototype) {
        auto proto = ObjectFactory::create_object();
        prototype_ = proto.release();
        PropertyDescriptor prototype_desc(Value(prototype_), PropertyAttributes::None);
        this->set_property_descriptor("prototype", prototype_desc);
    }

    PropertyDescriptor name_desc(Value(name_), PropertyAttributes::Configurable);
    this->set_property_descriptor("name", name_desc);

    PropertyDescriptor length_desc(Value(static_cast<double>(arity)), PropertyAttributes::Configurable);
    this->set_property_descriptor("length", length_desc);

}

Value Function::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    CallStack& stack = CallStack::instance();
    Position call_position = body_ ? body_->get_start() : Position(1, 1, 0);
    CallStackFrameGuard frame_guard(stack, get_name(), ctx.get_current_filename(), call_position, this);

    execution_count_++;
    last_call_time_ = std::chrono::high_resolution_clock::now();

    if (execution_count_ >= 3) {
        #ifdef __GNUC__
        __builtin_prefetch(this, 0, 3);
        __builtin_prefetch(body_.get(), 0, 3);
        __builtin_prefetch(&args, 0, 2);
        __builtin_prefetch(&ctx, 0, 2);
        #elif defined(_MSC_VER)
        _mm_prefetch((const char*)this, _MM_HINT_T0);
        _mm_prefetch((const char*)body_.get(), _MM_HINT_T0);
        _mm_prefetch((const char*)&args, _MM_HINT_T0);
        _mm_prefetch((const char*)&ctx, _MM_HINT_T0);
        #endif
    }
    
    if (execution_count_ >= 2 && !is_hot_) {
        is_hot_ = true;
    }

    // Class constructors must be called with new
    if (is_class_constructor_ && !ctx.is_in_constructor_call()) {
        ctx.throw_exception(Value("TypeError: Class constructor " + name_ + " cannot be invoked without 'new'"));
        return Value();
    }

    if (is_native_) {
        if (!ctx.check_execution_depth()) {
            ctx.throw_exception(Value(std::string("call stack size exceeded")));
            return Value();
        }
        
        Object* old_this = ctx.get_this_binding();
        if (this_value.is_object() || this_value.is_function()) {
            Object* this_obj = this_value.is_object() ? this_value.as_object() : this_value.as_function();
            ctx.set_this_binding(this_obj);
        }

        Value old_this_value = Value();
        bool had_this_binding = false;
        try {
            old_this_value = ctx.get_binding("this");
            had_this_binding = true;
        } catch (...) {
        }


        // Annex B's sloppy-mode null/undefined-this-becomes-global substitution only
        // applies to ECMAScript function code, never to native functions -- they must see
        // the real this_value (e.g. Object.prototype.toString branches on it).
        Value actual_this = this_value;

        if (actual_this.is_object() || actual_this.is_function()) {
            Object* this_obj = actual_this.is_object() ? actual_this.as_object() : actual_this.as_function();
            ctx.set_this_binding(this_obj);
        }

        // Preserve caller's "this" for direct eval inside native functions.
        // Native function call sets "this" to the native's receiver, but eval must
        // inherit the calling function's "this" (the value that existed before this overwrite).
        bool saved_caller_this = false;
        if (had_this_binding && !old_this_value.is_undefined() && !ctx.has_binding("__eval_caller_this__")) {
            ctx.create_binding("__eval_caller_this__", old_this_value, true);
            saved_caller_this = true;
        }

        ctx.set_binding("this", actual_this);

        // Track whether current call's this is primitive (always reset to avoid stale values).
        if (actual_this.is_number() || actual_this.is_string() || actual_this.is_boolean() ||
            actual_this.is_null() || actual_this.is_undefined() || actual_this.is_symbol() ||
            actual_this.is_bigint()) {
            if (!ctx.has_binding("__primitive_this__")) {
                ctx.create_binding("__primitive_this__", actual_this, true);
            } else {
                ctx.set_binding("__primitive_this__", actual_this);
            }
        } else {
            // Object this: clear so array helpers can detect primitive vs object.
            if (ctx.has_binding("__primitive_this__")) {
                ctx.set_binding("__primitive_this__", Value());
            }
        }

        // Native functions need to see null/undefined this as nullptr (per spec: ToObject throws).
        bool was_nullish = this_value.is_null() || this_value.is_undefined();
        if (was_nullish) {
            ctx.set_this_binding(nullptr);
        }
        bool prev_nullish = ctx.original_this_was_nullish();
        ctx.set_original_this_nullish(was_nullish);

        Context* prev_context = Object::current_context_;
        Object::current_context_ = &ctx;
        Value result = native_fn_(ctx, args);
        Object::current_context_ = prev_context;
        ctx.set_original_this_nullish(prev_nullish);

        ctx.set_this_binding(old_this);

        if (had_this_binding) {
            ctx.set_binding("this", old_this_value);
        } else {
            try {
                ctx.delete_binding("this");
            } catch (...) {
            }
        }

        if (saved_caller_this) {
            try { ctx.delete_binding("__eval_caller_this__"); } catch (...) {}
        }

        return result;
    }
    
    Context* parent_context = &ctx;
    auto function_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), parent_context, this);
    Context& function_context = *function_context_ptr;

    // RAII guard: transfer function context to Engine's survivor pool on return
    // instead of destroying it. Keeps the context alive for Promise async callbacks
    // that need context_ to point to the defining scope for closure variable lookups.
    Engine* fn_engine = ctx.get_engine();
    struct ContextSurvivorGuard {
        std::unique_ptr<Context>& ptr;
        Engine* eng;
        ContextSurvivorGuard(std::unique_ptr<Context>& p, Engine* e) : ptr(p), eng(e) {}
        ~ContextSurvivorGuard() {
            if (eng && ptr) eng->add_survivor_context(ptr.release());
        }
    } survivor_guard(function_context_ptr, fn_engine);

    // Propagate new.target into function scope
    if (ctx.is_in_constructor_call() && !ctx.get_new_target().is_undefined()) {
        function_context.set_new_target(ctx.get_new_target());
    }

    // Arrow functions capture new.target from enclosing scope
    if (is_arrow_ && this->has_property("__arrow_new_target__")) {
        function_context.set_new_target(this->get_property("__arrow_new_target__"));
    }
    function_context.set_arrow_function_context(is_arrow_);

    // Check for strict mode BEFORE setting up 'this' binding
    if (is_strict_) {
        function_context.set_strict_mode(true);
    }
    if (body_ && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(body_.get());
        block->check_use_strict_directive(function_context);
    }

    Value actual_this = this_value;

    // Arrow functions use their lexical this, ignoring the passed this_value
    if (is_arrow_ && this->has_property("__arrow_this__")) {
        actual_this = this->get_property("__arrow_this__");
    }

    bool is_strict_now = function_context.is_strict_mode();
    bool this_is_nullish = this_value.is_undefined() || this_value.is_null();
    if (!is_arrow_ && !is_strict_now) {
        if (this_is_nullish) {
            Object* global = function_context.get_global_object();
            if (global) {
                actual_this = Value(global);
            }
        } else if (this_value.is_number() || this_value.is_string() || this_value.is_boolean()) {
            // ES5 10.4.3: box primitive this to wrapper object in non-strict mode
            Object* global = function_context.get_global_object();
            const char* ctor_name = this_value.is_number() ? "Number"
                                  : this_value.is_string() ? "String" : "Boolean";
            Object::ObjectType obj_type = this_value.is_number() ? Object::ObjectType::Number
                                        : this_value.is_string() ? Object::ObjectType::String
                                        : Object::ObjectType::Boolean;
            auto wrapper = std::make_unique<Object>(obj_type);
            wrapper->set_property("[[PrimitiveValue]]", this_value);
            if (global) {
                Value ctor_val = global->get_property(ctor_name);
                if (ctor_val.is_function()) {
                    Value proto = ctor_val.as_function()->get_property("prototype");
                    if (proto.is_object()) wrapper->set_prototype(proto.as_object());
                }
            }
            actual_this = Value(wrapper.release());
        }
    }

    if (actual_this.is_object() || actual_this.is_function()) {
        Object* this_obj = actual_this.is_object() ? actual_this.as_object() : actual_this.as_function();
        function_context.set_this_binding(this_obj);
    }

    // Track uninitialized this for derived constructors (for super[expr] check)
    {
        Value scp = get_property("__super_constructor__");
        if (is_class_constructor_ && scp.is_function() && !has_own_property("__default_ctor__")) {
            function_context.set_this_needs_super(true);
            function_context.set_super_called(false);
        }
    }

    if (!function_context.create_binding("this", actual_this, true)) {
        // Binding already exists (e.g. set from __closure_this) -- force update
        function_context.set_binding("this", actual_this);
    }

    auto prop_keys = this->get_internal_property_keys();

    // Pre-pull: refresh own closure values from sibling closure functions before loading.
    // This handles async/microtask scenarios where a sibling ran before us (e.g., in a
    // promise chain) and updated its own __closure_* values which we need as our starting
    // point. Priority: parent_context > sibling pull > own stale value.
    {
        std::unordered_map<std::string, Value> sibling_updates;
        for (const auto& pk : prop_keys) {
            if (pk.length() <= 10 || pk.substr(0, 10) != "__closure_") continue;
            Value pval = this->get_property(pk);
            if (!pval.is_function()) continue;
            Function* pfn = pval.as_function();
            if (pfn == this) continue;
            auto sib_keys = pfn->get_internal_property_keys();
            for (const auto& sk : sib_keys) {
                if (sk.length() <= 10 || sk.substr(0, 10) != "__closure_") continue;
                Value sv = pfn->get_property(sk);
                if (sv.is_undefined() || sv.is_function()) continue;
                if (!this->has_property(sk)) continue;
                Value our_val = this->get_property(sk);
                if (!sv.strict_equals(our_val) && !our_val.is_function()) {
                    sibling_updates[sk] = sv;
                }
            }
        }
        for (auto& [uk, uv] : sibling_updates) {
            this->set_property(uk, uv);
        }
    }

    auto* parent_var_env = parent_context->get_variable_environment();
    std::unordered_set<std::string> parent_var_names;
    if (parent_var_env) {
        auto names = parent_var_env->get_binding_names();
        parent_var_names.insert(names.begin(), names.end());
    }
    for (const auto& key : prop_keys) {
        if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
            std::string var_name = key.substr(10);
            Value closure_value = this->get_property(key);
            bool is_const_closure = this->has_property("__closure_const_" + var_name);
            if (var_name != "arguments" && var_name != "this" && !is_const_closure) {
                // Use the caller's current value only when it matches the captured closure
                // (same binding, just mutated). If the values differ, the closure captured
                // a shadowing inner binding -- preserve the captured value.
                if (parent_var_names.count(var_name)) {
                    Value parent_val = parent_var_env->get_binding(var_name);
                    if (!parent_val.is_undefined() && !parent_val.is_function() &&
                            parent_val.strict_equals(closure_value)) {
                        closure_value = parent_val;
                    }
                }
                // Skip materializing only if the variable lives in the global Object env
                // AND the closure captured the same value (not a shadowing inner binding).
                bool skip_materialize = false;
                Environment* check_env = parent_context->get_lexical_environment();
                while (check_env) {
                    if (check_env->has_own_binding(var_name)) {
                        if (check_env->get_type() == Environment::Type::Object &&
                                check_env->get_outer() == nullptr) {
                            Value global_val = check_env->get_binding(var_name);
                            skip_materialize = global_val.strict_equals(closure_value);
                        }
                        break;
                    }
                    check_env = check_env->get_outer();
                }
                if (skip_materialize) continue;
            }
            if (is_const_closure) {
                // Class name binding: always materialize as const, shadowing any outer binding
                Environment* fn_lex = function_context.get_lexical_environment();
                if (!fn_lex || !fn_lex->has_own_binding(var_name)) {
                    function_context.create_lexical_binding(var_name, closure_value, false);
                }
            } else {
                function_context.create_binding(var_name, closure_value, true);
            }
        }
    }


    // For non-simple params (defaults/rest/destructuring), create arguments early
    // so default expressions can reference it (spec: unmapped arguments for non-simple).
    if (!is_arrow_ && !parameter_objects_.empty()) {
        bool has_complex = false;
        for (const auto& p : parameter_objects_) {
            if (p->has_default() || p->is_rest() || p->has_destructuring()) { has_complex = true; break; }
        }
        if (has_complex && !function_context.has_binding("arguments")) {
            auto early_args = ObjectFactory::create_array(args.size());
            for (size_t i = 0; i < args.size(); ++i) early_args->set_element(i, args[i]);
            {
                PropertyDescriptor ld(Value(static_cast<double>(args.size())),
                    static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
                early_args->set_property_descriptor("length", ld);
            }
            early_args->set_type(Object::ObjectType::Arguments);
            function_context.create_binding("arguments", Value(early_args.release()), true, false);
        }
    }

    if (!parameter_objects_.empty()) {
        size_t regular_param_count = 0;

        for (const auto& param : parameter_objects_) {
            if (!param->is_rest()) {
                regular_param_count++;
            }
        }

        {
            bool args_conflict = !is_arrow_;
            if (!args_conflict) {
                for (const auto& p : parameter_objects_) {
                    if (!p->is_rest() && !p->has_destructuring() && p->get_name() && p->get_name()->get_name() == "arguments") {
                        args_conflict = true;
                        break;
                    }
                }
            }
            function_context.set_eval_arguments_conflict(args_conflict);
        }
        {
            std::unordered_set<std::string> pnames;
            for (const auto& p : parameter_objects_) {
                if (p->get_name() && !p->get_name()->get_name().empty())
                    pnames.insert(p->get_name()->get_name());
            }
            function_context.set_eval_param_names(std::move(pnames));
        }
        function_context.set_in_param_eval(true);
        for (size_t i = 0; i < parameter_objects_.size(); ++i) {
            const auto& param = parameter_objects_[i];

            if (param->is_rest()) {
                auto rest_array = ObjectFactory::create_array(0);

                for (size_t j = regular_param_count; j < args.size(); ++j) {
                    rest_array->push(args[j]);
                }

                function_context.create_binding(param->get_name()->get_name(), Value(rest_array.release()), false);
            } else {
                const std::string& pname = param->get_name() ? param->get_name()->get_name() : std::string();
                // Create TDZ binding first so self-referential defaults (x = x) throw ReferenceError
                if (!pname.empty() && !param->has_destructuring()) {
                    if (function_context.get_lexical_environment())
                        function_context.get_lexical_environment()->create_uninitialized_binding(pname);
                }
                Value arg_value;

                if (i < args.size() && !args[i].is_undefined()) {
                    arg_value = args[i];
                } else if (param->has_default()) {
                    arg_value = param->get_default_value()->evaluate(function_context);
                    if (function_context.has_exception()) {
                        function_context.set_in_param_eval(false);
                        ctx.throw_exception(function_context.get_exception(), true);
                        return Value();
                    }
                } else {
                    arg_value = Value();
                }

                if (param->has_destructuring()) {
                    auto* pattern = param->get_destructuring_pattern();
                    auto* destructuring = dynamic_cast<DestructuringAssignment*>(pattern);
                    if (destructuring) {
                        destructuring->evaluate_with_value(function_context, arg_value);
                        if (function_context.has_exception()) {
                            function_context.set_in_param_eval(false);
                            ctx.throw_exception(function_context.get_exception(), true);
                            return Value();
                        }
                    }
                } else if (!pname.empty()) {
                    // Initialize the binding (was in TDZ during default evaluation)
                    if (function_context.get_lexical_environment())
                        function_context.get_lexical_environment()->initialize_binding(pname, arg_value);
                    else
                        function_context.create_binding(pname, arg_value, true);
                }
            }
        }
        function_context.set_in_param_eval(false);

        // After parameter binding (which may have mutated outer variables via generators),
        // refresh closure variables from the parent scope -- but only if the variable
        // was NOT already updated in this context (e.g. by iterator close callbacks).
        if (parent_var_env) {
            for (const auto& key : prop_keys) {
                if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
                    std::string var_name = key.substr(10);
                    if (var_name == "this" || var_name == "arguments") continue;
                    if (parent_var_names.count(var_name)) {
                        Value fresh_val = parent_var_env->get_binding(var_name);
                        Value captured_val = this->get_property(key); // original captured value
                        Value current_val = function_context.get_binding(var_name);
                        // Only refresh if the function context still has the old captured value
                        // (i.e., inner code hasn't already updated it via write-back)
                        if (!fresh_val.is_undefined() && !fresh_val.is_function() &&
                            current_val.strict_equals(captured_val)) {
                            function_context.set_binding(var_name, fresh_val);
                        }
                    }
                }
            }
        }
    } else {
        for (size_t i = 0; i < parameters_.size(); ++i) {
            Value arg_value = (i < args.size()) ? args[i] : Value();
            // ES1: Function parameters are mutable bindings
            function_context.create_binding(parameters_[i], arg_value, true);
        }
    }
    
    // Arrow functions don't have their own arguments object - they use the
    // lexical arguments captured from the enclosing scope via __closure_arguments
    if (!is_arrow_) {
        auto arguments_obj = ObjectFactory::create_array(args.size());
        // Elements for non-mapped indices; mapped ones get accessor descriptors below.
        // Only skip elements for simple param lists (no defaults/rest/destructuring).
        bool pre_simple = !function_context.is_strict_mode() && !parameter_objects_.empty();
        if (pre_simple) {
            for (const auto& p : parameter_objects_) {
                if (p->has_default() || p->is_rest() || p->has_destructuring()) { pre_simple = false; break; }
            }
        }
        size_t map_count_pre = pre_simple ? std::min(args.size(), parameter_objects_.size()) : 0;
        for (size_t i = 0; i < args.size(); i++) {
            if (i < map_count_pre) continue; // will be set via accessor
            arguments_obj->set_element(i, args[i]);
        }
        {
            // create_array's set_length() already established "length" as non-configurable
            // (real Array semantics); Arguments needs it configurable, so clear that internal
            // bookkeeping entry first instead of letting the non-configurable guard see a
            // (spurious, construction-time-only) attempt to relax it.
            arguments_obj->remove_own_property("length");
            PropertyDescriptor len_desc(Value(static_cast<double>(args.size())),
                static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
            arguments_obj->set_property_descriptor("length", len_desc);
        }
        // ES5: Arguments object [[Class]] is "Arguments"
        arguments_obj->set_type(Object::ObjectType::Arguments);
        // Arguments should inherit from Object.prototype, not Array.prototype
        Object* obj_proto = ObjectFactory::get_object_prototype();
        if (obj_proto) {
            arguments_obj->set_prototype(obj_proto);
        }

        // ES6 9.4.4.6/9.4.4.7: arguments[Symbol.iterator] must be %ArrayPrototype%.values.
        // get_element now routes through descriptors_ for Arguments so the aliasing works.
        {
            Value arr_iter_fn;
            Object* global = function_context.get_global_object();
            if (global) {
                Value arr_val = global->get_property("Array");
                if (arr_val.is_function()) {
                    Value arr_proto = arr_val.as_function()->get_property("prototype");
                    if (arr_proto.is_object()) {
                        arr_iter_fn = arr_proto.as_object()->get_property("Symbol.iterator");
                    }
                }
            }
            PropertyDescriptor iter_desc(arr_iter_fn,
                static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
            iter_desc.set_enumerable(false);
            arguments_obj->set_property_descriptor("Symbol.iterator", iter_desc);
        }

        // In strict mode, arguments has no 'caller' own property (ES2017+).
        // 'callee' is a poison-pill accessor that throws TypeError.
        if (function_context.is_strict_mode()) {
            auto thrower = ObjectFactory::create_native_function("ThrowTypeError",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    ctx.throw_type_error("'callee' may not be accessed on strict mode arguments");
                    return Value();
                });
            // %ThrowTypeError% must be non-extensible with non-configurable, non-writable properties
            {
                PropertyDescriptor len_desc(Value(0.0), PropertyAttributes::None);
                len_desc.set_configurable(false); len_desc.set_writable(false); len_desc.set_enumerable(false);
                thrower->set_property_descriptor("length", len_desc);
                PropertyDescriptor name_desc(Value(std::string("")), PropertyAttributes::None);
                name_desc.set_configurable(false); name_desc.set_writable(false); name_desc.set_enumerable(false);
                thrower->set_property_descriptor("name", name_desc);
                thrower->prevent_extensions();
            }

            PropertyDescriptor callee_desc;
            callee_desc.set_getter(thrower.get());
            callee_desc.set_setter(thrower.get());
            callee_desc.set_configurable(false);
            callee_desc.set_enumerable(false);
            arguments_obj->set_property_descriptor("callee", callee_desc);
            thrower.release();
            // 'caller' is NOT added as own property in strict mode (ES2017 spec)
        } else {
            // ES5 10.6 step 13.a: callee is {writable:true, enumerable:false, configurable:true}
            PropertyDescriptor callee_desc(Value(this), PropertyAttributes::BuiltinFunction);
            arguments_obj->set_property_descriptor("callee", callee_desc);
        }

        // ES5 10.6 / ES6 9.4.4: mapped arguments only for simple parameter lists
        // (no defaults, no rest, no destructuring). Non-strict only.
        bool is_simple_params = true;
        for (const auto& p : parameter_objects_) {
            if (p->has_default() || p->is_rest() || p->has_destructuring()) {
                is_simple_params = false; break;
            }
        }
        if (!function_context.is_strict_mode() && !parameter_objects_.empty() && is_simple_params) {
            size_t map_count = std::min(args.size(), parameter_objects_.size());
            for (size_t mi = 0; mi < map_count; mi++) {
                const std::string& pname = parameter_objects_[mi]->get_name()->get_name();
                if (pname.empty() || pname[0] == '_') continue;
                // Shared index and param name captured by getter/setter
                auto idx = std::make_shared<size_t>(mi);
                auto name = std::make_shared<std::string>(pname);
                // We need a stable pointer to function_context for the accessors.
                // Since function_context outlives the argument evaluation, store a raw pointer.
                Context* fc_ptr = &function_context;
                auto getter_fn = ObjectFactory::create_native_function("get",
                    [fc_ptr, name](Context& ctx, const std::vector<Value>&) -> Value {
                        (void)ctx;
                        return fc_ptr->get_binding(*name);
                    });
                // Mark getter so get_property_descriptor can synthesize a DATA descriptor
                getter_fn->set_property("__param_map__", Value(true));
                auto setter_fn = ObjectFactory::create_native_function("set",
                    [fc_ptr, name](Context& ctx, const std::vector<Value>& a) -> Value {
                        (void)ctx;
                        if (!a.empty()) fc_ptr->set_binding(*name, a[0]);
                        return Value();
                    });
                PropertyDescriptor map_desc;
                map_desc.set_getter(getter_fn.get());
                map_desc.set_setter(setter_fn.get());
                map_desc.set_enumerable(true);
                map_desc.set_configurable(true);
                getter_fn.release(); setter_fn.release();
                arguments_obj->set_property_descriptor(std::to_string(mi), map_desc);
            }
        }
        function_context.create_binding("arguments", Value(arguments_obj.release()), true, false);
    }

    // Use actual_this which respects strict mode (can be undefined in strict mode)
    function_context.create_binding("this", actual_this, false);
    
    if (this->has_property("__super_constructor__")) {
        Value super_constructor = this->get_property("__super_constructor__");
        if (!super_constructor.is_undefined() && !super_constructor.is_null()) {
            function_context.create_binding("__super__", super_constructor, false);
            // member.cpp's super lookup needs to know if this is a static method (resolves on the parent constructor itself) or an instance method (resolves on its .prototype).
            if (this->has_property("__is_static_method__")) {
                function_context.create_binding("__super_is_static__", Value(true), false);
            }
        }
    }
    if (this->has_property("__super_is_null__")) {
        function_context.create_binding("__super_is_null__", Value(true), false);
    }

    if (this->has_property("__private_brands__")) {
        Value brands = this->get_property("__private_brands__");
        if (!brands.is_undefined() && !brands.is_null()) {
            function_context.create_binding("__eval_private_names__", brands, false);
        }
    }

    if (body_) {
        // ES5: Named function expressions have their name as an immutable binding
        if (!name_.empty() && name_ != "<anonymous>" && !function_context.has_binding(name_)) {
            function_context.create_binding(name_, Value(this), false);
        }

        {
            bool has_complex_params = false;
            for (const auto& p : parameter_objects_) {
                if (p->has_default() || p->has_destructuring()) {
                    has_complex_params = true;
                    break;
                }
            }
            if (has_complex_params) {
                function_context.push_block_scope();
                function_context.set_variable_environment(function_context.get_lexical_environment());
            }
        }

        std::unordered_set<std::string> pre_scan_names;
        {
            auto* ve = function_context.get_variable_environment();
            if (ve) {
                auto ns = ve->get_binding_names();
                pre_scan_names.insert(ns.begin(), ns.end());
            }
        }

        if (body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
            scan_for_var_declarations(body_.get(), function_context);
        }

        std::unordered_set<std::string> scan_created_names;
        {
            auto* ve = function_context.get_variable_environment();
            if (ve) {
                for (const auto& n : ve->get_binding_names()) {
                    if (!pre_scan_names.count(n)) scan_created_names.insert(n);
                }
            }
        }

        Context* prev_context = Object::current_context_;
        Object::current_context_ = &function_context;
        Value result = body_->evaluate(function_context);
        Object::current_context_ = prev_context;

        // Propagate super_called flag to parent context
        if (function_context.was_super_called()) {
            ctx.set_super_called(true);
        }

        // let/const declared at the function's top level are created while the
        // body runs (after the pre-scan above), so a function declaration hoisted
        // before them captured a stale/undefined __closure_ snapshot. Walk the
        // post-execution variable environment and lexical chain (up to the
        // variable environment) to also pick those names up for re-capture.
        {
            auto* post_ve = function_context.get_variable_environment();
            if (post_ve) {
                for (const auto& n : post_ve->get_binding_names()) {
                    if (!pre_scan_names.count(n)) scan_created_names.insert(n);
                }
            }
            Environment* lex = function_context.get_lexical_environment();
            while (lex && lex != post_ve) {
                for (const auto& n : lex->get_binding_names()) {
                    if (!pre_scan_names.count(n)) scan_created_names.insert(n);
                }
                lex = lex->get_outer();
            }
        }

        // Re-capture closure variables on function objects in this scope.
        // This handles the case where function declarations are hoisted before
        // var initializations, so their __closure_ properties had stale values.
        {
            auto var_env = function_context.get_variable_environment();
            if (var_env && !scan_created_names.empty()) {
                std::vector<std::pair<std::string, Value>> var_values;
                std::vector<std::pair<std::string, Value>> func_values;
                std::vector<Function*> func_objects;

                // Build set of parameter names to exclude from sibling closure propagation
                std::unordered_set<std::string> param_name_set(parameters_.begin(), parameters_.end());
                if (parameter_objects_.empty() == false) {
                    for (const auto& p : parameter_objects_) {
                        param_name_set.insert(p->get_name()->get_name());
                    }
                }

                for (const auto& bname : scan_created_names) {
                    if (bname == "this" || bname == "arguments") continue;
                    if (param_name_set.count(bname)) continue;
                    Value val = function_context.get_binding(bname);
                    if (val.is_function()) {
                        Function* fn = val.as_function();
                        if (fn != this) {
                            func_objects.push_back(fn);
                            func_values.push_back({bname, val});
                        }
                    } else {
                        var_values.push_back({bname, val});
                    }
                }

                for (auto* func : func_objects) {
                    for (auto& [vname, vval] : var_values) {
                        func->set_property("__closure_" + vname, vval);
                    }
                    for (auto& [fname, fval] : func_values) {
                        if (fval.as_function() != func) {
                            func->set_property("__closure_" + fname, fval);
                        }
                    }
                }

                // Also update the return value if it's a function not already in scope
                if (function_context.has_return_value()) {
                    Value ret_val = function_context.get_return_value();
                    if (ret_val.is_function()) {
                        Function* ret_func = ret_val.as_function();
                        bool already_updated = false;
                        for (auto* func : func_objects) {
                            if (func == ret_func) {
                                already_updated = true;
                                break;
                            }
                        }
                        if (!already_updated) {
                            for (auto& [vname, vval] : var_values) {
                                ret_func->set_property("__closure_" + vname, vval);
                            }
                            for (auto& [fname, fval] : func_values) {
                                if (fval.as_function() != ret_func) {
                                    ret_func->set_property("__closure_" + fname, fval);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Write back modified closure variables to this function object, propagate to parent context, and update sibling closures.
        // __contains_eval__ functions never had any __closure_* properties captured to begin with (see language.cpp), so this is just a no-op guard, not the primary mechanism.
        std::vector<std::pair<std::string, Value>> modified_closures;
        auto prop_keys2 = this->has_property("__contains_eval__") ? std::vector<std::string>() : this->get_internal_property_keys();
        for (const auto& key : prop_keys2) {
            if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
                std::string var_name = key.substr(10);

                if (function_context.has_binding(var_name)) {
                    Value current_value = function_context.get_binding(var_name);
                    Value old_value = this->get_property(key);
                    this->set_property(key, current_value);

                    if (!current_value.strict_equals(old_value)) {
                        if (parent_var_names.count(var_name)) {
                            // Variable is in the direct caller's scope -- write there.
                            parent_var_env->set_binding(var_name, current_value);
                        } else if (closure_context_) {
                            auto* cve = closure_context_->get_variable_environment();
                            if (cve && cve->has_own_binding(var_name)) {
                                cve->set_binding(var_name, current_value);
                                // For non-global envs (Function/Declarative), the own
                                // binding is a closure-loaded copy.  Propagate the write
                                // up the outer chain so callers at higher scopes (e.g.
                                // global) also see the updated value.
                                if (cve->get_type() != Environment::Type::Object &&
                                    cve->get_type() != Environment::Type::Global) {
                                    Environment* outer = cve->get_outer();
                                    while (outer) {
                                        if (outer->has_own_binding(var_name)) {
                                            outer->set_binding(var_name, current_value);
                                            break;
                                        }
                                        outer = outer->get_outer();
                                    }
                                }
                            }
                        }
                    }

                    if (!current_value.strict_equals(old_value)) {
                        modified_closures.push_back({var_name, current_value});
                    }
                }
            }
        }

        if (!modified_closures.empty()) {
            auto* var_env = parent_context->get_variable_environment();
            if (var_env) {
                auto sibling_names = var_env->get_binding_names();
                for (const auto& sname : sibling_names) {
                    Value sval = parent_var_env->get_binding(sname);
                    if (sval.is_function() && sval.as_function() != this) {
                        Function* sibling = sval.as_function();
                        for (auto& [vname, vval] : modified_closures) {
                            if (sibling->has_property("__closure_" + vname)) {
                                sibling->set_property("__closure_" + vname, vval);
                            }
                        }
                    }
                }
            }
            auto own_keys = this->get_internal_property_keys();
            for (const auto& okey : own_keys) {
                if (okey.length() > 10 && okey.substr(0, 10) == "__closure_") {
                    Value sval = this->get_property(okey);
                    if (sval.is_function() && sval.as_function() != this) {
                        Function* sibling = sval.as_function();
                        for (auto& [vname, vval] : modified_closures) {
                            if (sibling->has_property("__closure_" + vname)) {
                                sibling->set_property("__closure_" + vname, vval);
                            }
                        }
                    }
                }
            }
        }

        
        if (function_context.has_return_value()) {
            return function_context.get_return_value();
        }

        if (function_context.has_exception()) {
            ctx.throw_exception(function_context.get_exception(), true);
            return Value();
        }

        // For class constructors: if super() updated this to a new object, return it
        // so Function::construct() can use the correct object
        if (is_class_constructor_) {
            Object* final_this = function_context.get_this_binding();
            if (final_this && actual_this.is_object() && final_this != actual_this.as_object()) {
                return Value(final_this);
            }
        }

        // Concise arrow functions (`() => expr`) have non-block bodies -- return the expression result.
        // Functions with block bodies return undefined unless they have explicit `return`.
        if (body_ && body_->get_type() != ASTNode::Type::BLOCK_STATEMENT) {
            return result;  // concise arrow body
        }
        return Value();  // block body without explicit return
    }
    
    return Value();
}

Value Function::get_property(const std::string& key) const {
    if (key == "name") {
        if (descriptors_) {
            auto it = descriptors_->find("name");
            if (it != descriptors_->end()) {
                if (it->second.is_data_descriptor()) {
                    Value v = it->second.get_value();
                    if (v.is_string() && v.to_string() == "<arrow>") return Value(std::string(""));
                    return v;
                }
                if (it->second.is_accessor_descriptor()) {
                    Object* getter = it->second.get_getter();
                    if (getter && current_context_) {
                        Function* gfn = dynamic_cast<Function*>(getter);
                        if (gfn) return gfn->call(*current_context_, {}, Value(const_cast<Function*>(this)));
                    }
                    return Value();
                }
            }
        }
        return Value(name_ == "<arrow>" ? std::string("") : name_);
    }
    if (key == "length") {
        PropertyDescriptor desc = get_property_descriptor(key);
        if (desc.has_value() && desc.is_data_descriptor()) {
            return desc.get_value();
        }
        return Value(static_cast<double>(parameters_.size()));
    }
    if (key == "prototype") {
        if (prototype_ != nullptr) return Value(prototype_);
        // Check base property table (e.g. prototype set to a non-object like a number)
        Value base_val = get_own_property(key);
        if (!base_val.is_undefined()) return base_val;
        return Value();
    }

    Value result = get_own_property(key);
    if (!result.is_undefined()) {
        return result;
    }

    // Lazy initialization: if our internal prototype is not set yet,
    // try to get Function.prototype (may be available now even if it wasn't during construction)
    Object* current = get_prototype();
    if (!current) {
        Object* func_proto = ObjectFactory::get_function_prototype();
        if (func_proto) {
            const_cast<Function*>(this)->set_prototype(func_proto);
            current = func_proto;
        }
    }

    while (current) {
        if (current->descriptors_) {
            auto desc_it = current->descriptors_->find(key);
            if (desc_it != current->descriptors_->end()) {
                const PropertyDescriptor& desc = desc_it->second;
                if (desc.is_accessor_descriptor() && desc.has_getter()) {
                    Function* getter_fn = dynamic_cast<Function*>(desc.get_getter());
                    if (getter_fn && getter_fn->get_name().find("get [Symbol.") == 0) {
                        return Value(const_cast<Function*>(this));
                    }
                }
            }
        }
        Value result = current->get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
        current = current->get_prototype();
    }

    return Value();
}

void Function::set_name(const std::string& name) {
    name_ = name;
    // Force-update the name in descriptors (bypasses writable check)
    // But don't overwrite if the descriptor was explicitly set to a function (e.g. static name())
    if (descriptors_) {
        auto it = descriptors_->find("name");
        if (it != descriptors_->end() && it->second.is_data_descriptor()) {
            if (!it->second.get_value().is_function()) {
                it->second = PropertyDescriptor(Value(name_), it->second.get_attributes());
            }
        }
    }
}

std::vector<std::string> Function::get_internal_property_keys() const {
    return Object::get_own_property_keys();
}

std::vector<std::string> Function::get_own_property_keys() const {
    auto all = Object::get_own_property_keys();
    std::vector<std::string> result;
    result.reserve(all.size());

    // Spec-mandated order for function own properties: length, name, prototype, then others.
    static const char* const kPriority[] = { "length", "name", "prototype" };
    for (const char* pkey : kPriority) {
        for (const auto& k : all) {
            if (k == pkey) { result.push_back(k); break; }
        }
    }
    for (const auto& k : all) {
        if (k.size() >= 2 && k[0] == '_' && k[1] == '_') continue;
        if (k == "length" || k == "name" || k == "prototype") continue;
        result.push_back(k);
    }
    return result;
}

bool Function::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    if (key == "prototype") {
        if (value.is_object()) {
            prototype_ = value.as_object();
            // Remove from base property table if present
            Object::delete_property(key);
            return true;
        }
        if (value.is_function()) {
            prototype_ = value.as_function();
            Object::delete_property(key);
            return true;
        }
        // Non-object: clear internal pointer, store in base property table so typeof works
        prototype_ = nullptr;
        return Object::set_property(key, value, attrs);
    }

    return Object::set_property(key, value, attrs);
}

Value Function::construct(Context& ctx, const std::vector<Value>& args) {
    // Check if this function is a constructor
    if (!is_constructor_) {
        ctx.throw_exception(Value("TypeError: " + name_ + " is not a constructor"));
        return Value();
    }

    auto new_object = ObjectFactory::create_object();
    Value this_value(new_object.get());
    
    Value constructor_prototype = get_property("prototype");
    if (constructor_prototype.is_object()) {
        Object* proto_obj = constructor_prototype.as_object();
        new_object->set_prototype(proto_obj);
    }
    
    Value super_constructor_prop = get_property("__super_constructor__");
    // Use has_own_property to avoid inheriting __default_ctor__ from parent class
    bool is_default_ctor = has_own_property("__default_ctor__");

    ctx.set_in_constructor_call(true);
    ctx.set_super_called(false);
    // Preserve new.target across the whole super-chain instead of stomping it with `this`.
    Value old_new_target = ctx.get_new_target();
    if (old_new_target.is_undefined()) {
        ctx.set_new_target(Value(static_cast<Object*>(this)));
    }

    // A synthesized default derived constructor is spec'd as `constructor(...args) { super(...args); }`, so auto-super must run before the constructor body (which here only contains field initializers) -- otherwise a super-chain override (e.g. a base constructor returning `new Proxy(this, ...)`) takes effect too late and fields get written to the object that's about to be discarded.
    if (is_default_ctor && super_constructor_prop.is_function()) {
        Function* super_constructor = super_constructor_prop.as_function();
        Value super_result;
        if (super_constructor->is_native()) {
            // Native built-ins (Boolean, Number, String, etc.) need construct semantics
            super_result = super_constructor->construct(ctx, args);
        } else {
            super_result = super_constructor->call(ctx, args, this_value);
        }
        ctx.set_super_called(true);
        if (ctx.has_exception()) {
            ctx.set_in_constructor_call(false);
            ctx.set_new_target(old_new_target);
            return Value();
        }
        if (super_result.is_object() || super_result.is_function()) {
            this_value = super_result;
        }
        // InitializeInstanceElements after auto-super: add per-instance private method brand slot.
        Value pm_slot_val = get_property("__pm_brand_slot__");
        if (pm_slot_val.is_string()) {
            std::string pm_slot = pm_slot_val.to_string();
            Object* pm_this = this_value.is_object() ? this_value.as_object() : nullptr;
            if (pm_this) pm_this->add_private_field(pm_slot);
        }
        // Nested super construct() clears/overwrites these shared flags -- restore them.
        ctx.set_in_constructor_call(true);
        ctx.set_new_target(old_new_target);
    }

    Value result = call(ctx, args, this_value);
    bool super_was_called = ctx.was_super_called();
    ctx.set_in_constructor_call(false);
    ctx.set_new_target(old_new_target);

    // Propagate any exception from the constructor body before checking super state
    if (ctx.has_exception()) return Value();

    // Explicit constructor that never called super() -- ReferenceError
    if (!super_was_called && !super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        ctx.throw_reference_error("Must call super constructor before accessing 'this' in derived class constructor");
        return Value();
    }

    // In derived class constructors, returning a primitive throws TypeError
    bool is_derived = !super_constructor_prop.is_undefined();
    if (is_derived && !result.is_undefined() && !result.is_object() && !result.is_function()) {
        ctx.throw_type_error("Derived constructors may only return object or undefined");
        return Value();
    }

    // An explicit return from the constructor body wins; otherwise fall back to this_value, which the auto-super override above may have replaced.
    Value final_result = (result.is_object() || result.is_function()) ? result : this_value;

    // If construction resolved to an object or function other than the pre-allocated this, use that
    if ((final_result.is_object() || final_result.is_function()) && final_result.as_object() != new_object.get()) {
        Object* ret_obj = final_result.as_object();
        // For derived classes: always set prototype to this class's prototype
        // so `new Derived() instanceof Derived` works even when super is a built-in
        if (is_derived && constructor_prototype.is_object()) {
            ret_obj->set_prototype(constructor_prototype.as_object());
        } else if (!ret_obj->get_prototype_raw() && constructor_prototype.is_object()) {
            ret_obj->set_prototype(constructor_prototype.as_object());
        }
        // A base-class constructor may have captured a raw pointer to new_object before returning this override (e.g. `new Proxy(this, ...)`), so release rather than let the unique_ptr delete it out from under them.
        new_object.release();
        return final_result;
    } else {
        return Value(new_object.release());
    }
}

std::string Function::to_string() const {
    if (is_native_) {
        return "function " + name_ + "() { [native code] }";
    }
    if (!source_text_.empty()) {
        return source_text_;
    }
    // Non-native function without preserved source text: use NativeFunction format
    // (test262's assertToStringOrNativeFunction accepts "function name() { [native code] }").
    return "function " + name_ + "() { [native code] }";
}


namespace ObjectFactory {

std::unique_ptr<Function> create_js_function(const std::string& name,
                                             const std::vector<std::string>& params,
                                             std::unique_ptr<ASTNode> body,
                                             Context* closure_context) {
    auto func = std::make_unique<Function>(name, params, std::move(body), closure_context);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    } else {
        // If function_prototype not set yet, delay prototype assignment
        // It will be set when the function is accessed
    }
    return func;
}

std::unique_ptr<Function> create_js_function(const std::string& name,
                                             std::vector<std::unique_ptr<Parameter>> params,
                                             std::unique_ptr<ASTNode> body,
                                             Context* closure_context) {
    auto func = std::make_unique<Function>(name, std::move(params), std::move(body), closure_context);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    }
    return func;
}

std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, const std::vector<Value>&)> fn) {
    auto func = std::make_unique<Function>(name, fn, false);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    }
    return func;
}

std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, const std::vector<Value>&)> fn,
                                                 uint32_t arity) {
    auto func = std::make_unique<Function>(name, fn, arity, false);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    }
    return func;
}

std::unique_ptr<Function> create_native_constructor(const std::string& name,
                                                    std::function<Value(Context&, const std::vector<Value>&)> fn,
                                                    uint32_t arity) {
    auto func = std::make_unique<Function>(name, fn, arity, true);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        func->set_prototype(func_proto);
    }
    return func;
}

}

void Function::scan_for_var_declarations(ASTNode* node, Context& ctx) {
    if (!node) return;

    if (node->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(node);

        for (const auto& declarator : var_decl->get_declarations()) {
            if (declarator->get_kind() == VariableDeclarator::Kind::VAR) {
                const std::string& name = declarator->get_id()->get_name();

                auto* var_env = ctx.get_variable_environment();
                if (!var_env || !var_env->has_own_binding(name)) {
                    ctx.create_var_binding(name, Value(), true);
                }
            }
        }
    }

    if (node->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(node);
        for (const auto& stmt : block->get_statements()) {
            scan_for_var_declarations(stmt.get(), ctx);
        }
    }
    else if (node->get_type() == ASTNode::Type::IF_STATEMENT) {
        IfStatement* if_stmt = static_cast<IfStatement*>(node);
        scan_for_var_declarations(if_stmt->get_consequent(), ctx);
        if (if_stmt->get_alternate()) {
            scan_for_var_declarations(if_stmt->get_alternate(), ctx);
        }
    }
    else if (node->get_type() == ASTNode::Type::FOR_STATEMENT) {
        ForStatement* for_stmt = static_cast<ForStatement*>(node);
        if (for_stmt->get_init()) {
            scan_for_var_declarations(for_stmt->get_init(), ctx);
        }
        scan_for_var_declarations(for_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::WHILE_STATEMENT) {
        WhileStatement* while_stmt = static_cast<WhileStatement*>(node);
        scan_for_var_declarations(while_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::DO_WHILE_STATEMENT) {
        DoWhileStatement* do_stmt = static_cast<DoWhileStatement*>(node);
        scan_for_var_declarations(do_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::WITH_STATEMENT) {
        WithStatement* with_stmt = static_cast<WithStatement*>(node);
        scan_for_var_declarations(with_stmt->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::TRY_STATEMENT) {
        TryStatement* try_stmt = static_cast<TryStatement*>(node);
        scan_for_var_declarations(try_stmt->get_try_block(), ctx);
        if (try_stmt->get_catch_clause()) scan_for_var_declarations(try_stmt->get_catch_clause(), ctx);
        if (try_stmt->get_finally_block()) scan_for_var_declarations(try_stmt->get_finally_block(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::SWITCH_STATEMENT) {
        SwitchStatement* sw = static_cast<SwitchStatement*>(node);
        for (const auto& c : sw->get_cases()) {
            for (const auto& s : static_cast<CaseClause*>(c.get())->get_consequent()) {
                scan_for_var_declarations(s.get(), ctx);
            }
        }
    }
    else if (node->get_type() == ASTNode::Type::LABELED_STATEMENT) {
        LabeledStatement* lbl = static_cast<LabeledStatement*>(node);
        scan_for_var_declarations(lbl->get_statement(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::FOR_IN_STATEMENT) {
        ForInStatement* forin = static_cast<ForInStatement*>(node);
        if (forin->get_left()) scan_for_var_declarations(forin->get_left(), ctx);
        scan_for_var_declarations(forin->get_body(), ctx);
    }
    else if (node->get_type() == ASTNode::Type::FOR_OF_STATEMENT) {
        ForOfStatement* forof = static_cast<ForOfStatement*>(node);
        if (forof->get_left()) scan_for_var_declarations(forof->get_left(), ctx);
        scan_for_var_declarations(forof->get_body(), ctx);
    }
}

}
