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

void Function::setup_mapped_arguments(Context& fn_ctx, const std::vector<Value>& args, Object* arguments_obj) {
    // ES5 10.6 / ES6 9.4.4: mapped arguments only for simple, non-strict parameter lists.
    bool is_simple_params = true;
    for (const auto& p : parameter_objects_) {
        if (p->has_default() || p->is_rest() || p->has_destructuring()) {
            is_simple_params = false; break;
        }
    }
    if (fn_ctx.is_strict_mode() || parameter_objects_.empty() || !is_simple_params) return;

    size_t map_count = std::min(args.size(), parameter_objects_.size());
    for (size_t mi = 0; mi < map_count; mi++) {
        const std::string& pname = parameter_objects_[mi]->get_name()->get_name();
        if (pname.empty() || pname[0] == '_') continue;
        auto name = std::make_shared<std::string>(pname);
        // fn_ctx outlives the accessors, so a raw pointer is safe to capture.
        Context* fc_ptr = &fn_ctx;
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

Value Function::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    // Consumed immediately so a nested call triggered from inside this invocation
    // (e.g. a native function calling another function) doesn't inherit it.
    bool is_construct_invocation = ctx.consume_pending_construct_call();
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
        bool was_primitive = !was_nullish && !this_value.is_object() && !this_value.is_function();
        if (was_nullish) {
            ctx.set_this_binding(nullptr);
        }
        bool prev_nullish = ctx.original_this_was_nullish();
        bool prev_primitive = ctx.original_this_was_primitive();
        ctx.set_original_this_nullish(was_nullish);
        ctx.set_original_this_primitive(was_primitive);

        // A plain call (not this construct invocation) must see new.target == undefined --
        // ctx is shared with the caller here since native calls don't get their own Context.
        Value saved_new_target = ctx.get_new_target();
        if (!is_construct_invocation) ctx.set_new_target(Value());

        Context* prev_context = Object::current_context_;
        Object::current_context_ = &ctx;
        Value result = native_fn_(ctx, args);
        Object::current_context_ = prev_context;
        ctx.set_original_this_nullish(prev_nullish);
        ctx.set_original_this_primitive(prev_primitive);

        if (!is_construct_invocation) ctx.set_new_target(saved_new_target);

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
        // Binding already exists -- force update
        function_context.set_binding("this", actual_this);
    }

    auto prop_keys = this->get_internal_property_keys();

    // A named class's own name is bound as an immutable self-reference inside its
    // methods (__closure_const_<name>) since the class's name binding doesn't exist
    // in scope yet when its methods are created -- see ClassDeclaration::evaluate.
    // Everything else resolves through closure_environment_, no materialization needed.
    for (const auto& key : prop_keys) {
        if (key.length() > 10 && key.substr(0, 10) == "__closure_" && key.substr(0, 16) != "__closure_const_") {
            std::string var_name = key.substr(10);
            if (this->has_property("__closure_const_" + var_name)) {
                Value closure_value = this->get_property(key);
                Environment* fn_lex = function_context.get_lexical_environment();
                if (!fn_lex || !fn_lex->has_own_binding(var_name)) {
                    function_context.create_lexical_binding(var_name, closure_value, false);
                }
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

                Value rest_val(rest_array.release());
                if (param->has_destructuring()) {
                    auto* destr = dynamic_cast<DestructuringAssignment*>(param->get_destructuring_pattern());
                    if (destr) {
                        destr->evaluate_with_value(function_context, rest_val);
                        if (function_context.has_exception()) {
                            function_context.set_in_param_eval(false);
                            ctx.throw_exception(function_context.get_exception(), true);
                            return Value();
                        }
                    }
                } else {
                    function_context.create_binding(param->get_name()->get_name(), rest_val, false);
                }
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
    } else {
        for (size_t i = 0; i < parameters_.size(); ++i) {
            Value arg_value = (i < args.size()) ? args[i] : Value();
            // ES1: Function parameters are mutable bindings
            function_context.create_binding(parameters_[i], arg_value, true);
        }
    }
    
    // Arrow functions don't have their own arguments object -- they resolve
    // `arguments` lexically through closure_environment_ to the enclosing scope.
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

        setup_mapped_arguments(function_context, args, arguments_obj.get());
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

        if (body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
            scan_for_var_declarations(body_.get(), function_context);
        }

        Context* prev_context = Object::current_context_;
        Object::current_context_ = &function_context;
        Value result = body_->evaluate(function_context);
        Object::current_context_ = prev_context;

        // Propagate super_called flag to parent context
        if (function_context.was_super_called()) {
            ctx.set_super_called(true);
        }

        // Outer-variable writes during body execution went straight through
        // closure_environment_ to the real defining Environment already (live,
        // by construction) -- no post-hoc diff/write-back/sibling-update needed.

        
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
        if (descriptors_ && descriptors_->count("length")) {
            PropertyDescriptor desc = (*descriptors_)["length"];
            if (desc.is_data_descriptor()) return desc.get_value();
            if (desc.is_accessor_descriptor()) {
                Object* getter = desc.get_getter();
                if (getter && current_context_) {
                    Function* gfn = dynamic_cast<Function*>(getter);
                    if (gfn) return gfn->call(*current_context_, {}, Value(const_cast<Function*>(this)));
                }
                return Value();
            }
        }
        if (overflow_properties_ && overflow_properties_->count("length")) {
            return (*overflow_properties_)["length"];
        }
        // After delete f.length, has_own_property is false -- traverse prototype chain instead of
        // falling back to parameters_.size() which would ignore the deletion.
        if (!has_own_property("length") && get_prototype()) {
            return Object::get_property(key);
        }
        return Value(static_cast<double>(parameters_.size()));
    }
    if (key == "prototype") {
        if (prototype_ != nullptr) return Value(prototype_);
        Value base_val = get_own_property(key);
        if (!base_val.is_undefined()) return base_val;
        return Value();
    }

    Value result = get_own_property(key);
    if (!result.is_undefined()) {
        return result;
    }
    // A setter-only own accessor must return undefined here, not fall through to an
    // inherited getter for the same key.
    if (descriptors_) {
        auto it = descriptors_->find(key);
        if (it != descriptors_->end() && it->second.is_accessor_descriptor()) {
            return Value();
        }
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
    if (constructor_prototype.is_object() || constructor_prototype.is_function()) {
        Object* proto_obj = constructor_prototype.is_function()
            ? static_cast<Object*>(constructor_prototype.as_function())
            : constructor_prototype.as_object();
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

    ctx.set_pending_construct_call(true);
    Value result = call(ctx, args, this_value);
    bool super_was_called = ctx.was_super_called();
    ctx.set_in_constructor_call(false);
    ctx.set_new_target(old_new_target);

    // Propagate any exception from the constructor body before checking super state
    if (ctx.has_exception()) return Value();

    bool is_derived = !super_constructor_prop.is_undefined();

    // TypeError for explicit non-object return must come before ReferenceError for missing super (spec 13c)
    if (is_derived && !result.is_undefined() && !result.is_object() && !result.is_function()) {
        ctx.throw_type_error("Derived constructors may only return object or undefined");
        return Value();
    }

    if (!result.is_object() && !result.is_function() &&
        !super_was_called && !super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        ctx.throw_reference_error("Must call super constructor before accessing 'this' in derived class constructor");
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
    // A well-known-symbol-named function's `name` is internally stored as "@@x" (e.g.
    // "@@asyncIterator"); NativeFunction syntax requires the spec's bracketed form instead.
    std::string display_name = name_;
    if (display_name.size() > 2 && display_name[0] == '@' && display_name[1] == '@') {
        display_name = "[Symbol." + display_name.substr(2) + "]";
    }
    if (is_native_) {
        return "function " + display_name + "() { [native code] }";
    }
    if (!source_text_.empty()) {
        // Trim trailing whitespace -- source_text_ may include a trailing newline.
        std::string s = source_text_;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        return s;
    }
    // Non-native function without preserved source text: use NativeFunction format
    // (test262's assertToStringOrNativeFunction accepts "function name() { [native code] }").
    return "function " + display_name + "() { [native code] }";
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
