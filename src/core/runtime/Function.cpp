/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/StackFloor.h"
#include <span>
#include "quanta/core/runtime/Object.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/CallStack.h"
#include "quanta/core/vm/BytecodeCompiler.h"
#include "quanta/core/vm/Interpreter.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/parser/AST.h"
#include "quanta/parser/ScriptUnit.h"
#include <optional>
#include <sstream>
#include <iostream>
#include <chrono>
#include <unordered_set>

#define UNLIKELY_NATIVE(x) (__builtin_expect(!!(x), 0))

#ifdef _MSC_VER
#include <xmmintrin.h>
#endif

namespace Quanta {

// QUANTA_VM is read once at startup and never changes, but VM::enabled() lives
// in another translation unit, so each of the three call sites below paid a
// real call plus a static-init guard on every invocation -- one of them on the
// register-mode gate, i.e. every JS call.
static const bool g_vm_enabled = VM::enabled();

// IsAnonymousFunctionDefinition, for the NamedEvaluation a default parameter
// performs on its initializer.
static bool is_anon_func_def(const ASTNode* node) {
    if (!node) return false;
    auto t = node->get_type();
    return t == ASTNode::Type::FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ARROW_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION ||
           t == ASTNode::Type::CLASS_DECLARATION;
}
    class Engine;
    class JITCompiler;
}

namespace Quanta {

constinit thread_local Object* Function::s_throw_type_error_ = nullptr;

// closure_context_ is stored for the Function's whole life and read by the
// tracer (Function::trace_default) and by every arrow's `this`/`super` lookup,
// so the Context has to outlive the call that created the closure. Saying so
// here, at the one place the pointer is taken, is what makes it unconditional:
// the survivor guard reads this off the environment instead, and the
// environment's own signal is deliberately deferred by mark_escaped_now=false
// and simply never arrives for generator/async class methods -- which left a
// live closure pointing at a freed Context.
static Context* capture_closure_context(Context* closure_context) {
    if (closure_context) closure_context->mark_exposed_to_escape();
    return closure_context;
}

// A closure pins its captured environment chain: pop_block_scope deletes
// unescaped block envs, so the capture must mark the chain first -- unless
// the caller has already proven (closure_needs_outer_environment) that
// nothing inside this specific closure can ever observe it, in which case
// mark_escaped_now=false defers that decision to an explicit, later
// Function::mark_closure_environment_escaped() call (or none at all).
static Environment* capture_closure_environment(Context* closure_context, bool mark_escaped_now = true) {
    if (!closure_context) return nullptr;
    Environment* env = closure_context->get_lexical_environment();
    if (!env) return nullptr;
    // Either way the pointer is stored in closure_environment_ and read by the
    // tracer, so the chain stops being provably unreachable. mark_escaped is
    // the stronger claim (never free it on scope exit); when the caller has
    // proven this closure cannot observe the environment, only the weaker one
    // is made and the collector still gets to adjudicate.
    env->mark_referenced();
    if (mark_escaped_now) env->mark_escaped();
    return env;
}

// Duplicate parameter names share one binding; only the last occurrence is live-mapped.
// Shared by setup_mapped_arguments() and the pre-pass seeding raw element values.
static bool param_gets_mapped_accessor(const std::vector<std::unique_ptr<Parameter>>& params, size_t mi) {
    const std::string& pname = params[mi]->get_name()->get_name();
    if (pname.empty() || pname[0] == '_') return false;
    for (size_t later = mi + 1; later < params.size(); later++) {
        if (params[later]->get_name()->get_name() == pname) return false;
    }
    return true;
}

Function::Function(const std::string& name,
                   const std::vector<std::string>& params,
                   std::unique_ptr<ASTNode> body,
                   Context* closure_context,
                   bool create_prototype)
    : Object(ObjectType::Function),
      closure_context_(capture_closure_context(closure_context)),
      closure_environment_(capture_closure_environment(closure_context, /*mark_escaped_now=*/false)),
      prototype_(nullptr), is_native_(false), is_constructor_(create_prototype), is_arrow_(false), is_class_constructor_(false), is_strict_(false), is_param_default_(false) {
    auto exe = make_executable_ref();
    exe->name = name;  // fresh executable, guaranteed empty -- no compare needed
    exe->parameters = params;
    exe->adopt_body(std::move(body));

    // Deferred: the object, and the 304-byte descriptor block its own
    // "constructor" entry needs, are built the first time anything asks for
    // them. Most functions are never used as constructors and never have
    // .prototype read, so most never pay for either. See ensure_prototype().
    prototype_pending_ = create_prototype;

    // "name"/"length" are lazy -- see the class-header comment on
    // name_deleted_/length_deleted_. exe->declared_length mirrors this
    // constructor's own former eager-descriptor value exactly.
    exe->declared_length = params.size();
    executable_ = std::move(exe);

    // [[Prototype]] (not the .prototype property above): direct construction sites that bypass
    // ObjectFactory::create_js_function would otherwise leave this null.
    // This function is still being constructed, so no site can have cached a
    // lookup through its chain -- see initialize_prototype. Going through
    // set_prototype here retired every prototype cache entry in the engine
    // once per function created.
    if (Object* func_proto = ObjectFactory::get_function_prototype()) {
        initialize_prototype(func_proto);
    }
}

Function::Function(const std::string& name,
                   std::vector<std::unique_ptr<Parameter>> params,
                   std::unique_ptr<ASTNode> body,
                   Context* closure_context,
                   bool create_prototype)
    : Object(ObjectType::Function),
      closure_context_(capture_closure_context(closure_context)),
      closure_environment_(capture_closure_environment(closure_context, /*mark_escaped_now=*/false)),
      prototype_(nullptr), is_native_(false), is_constructor_(create_prototype), is_arrow_(false), is_class_constructor_(false), is_strict_(false), is_param_default_(false) {
    auto exe = make_executable_ref();
    exe->name = name;  // fresh executable, guaranteed empty -- no compare needed
    for (const auto& param : params) {
        exe->parameters.push_back(param->get_name()->get_name());
    }
    exe->parameter_objects = std::move(params);
    exe->adopt_body(std::move(body));

    // Deferred: the object, and the 304-byte descriptor block its own
    // "constructor" entry needs, are built the first time anything asks for
    // them. Most functions are never used as constructors and never have
    // .prototype read, so most never pay for either. See ensure_prototype().
    prototype_pending_ = create_prototype;

    // "name"/"length" are lazy -- see the class-header comment. ES6: length =
    // number of params before first rest or default.
    size_t formal_length = 0;
    for (const auto& param : exe->parameter_objects) {
        if (param->is_rest() || param->has_default()) break;
        formal_length++;
    }
    exe->declared_length = formal_length;
    executable_ = std::move(exe);

    // [[Prototype]] (not the .prototype property above): direct construction sites that bypass
    // ObjectFactory::create_js_function would otherwise leave this null.
    // This function is still being constructed, so no site can have cached a
    // lookup through its chain -- see initialize_prototype. Going through
    // set_prototype here retired every prototype cache entry in the engine
    // once per function created.
    if (Object* func_proto = ObjectFactory::get_function_prototype()) {
        initialize_prototype(func_proto);
    }
}

void Function::borrow_body_from(const ExecutableRef<ScriptUnit>& unit, ASTNode* body) {
    // Only ever called right after construction, on an executable this
    // function is still the sole owner of -- a decl site that shares its
    // executable with siblings already has its body installed.
    if (!executable_) return;
    const_cast<FunctionExecutable*>(executable_.get())->borrow_body(unit, body);
}

Function::Function(const std::string& name,
                   ExecutableRef<const FunctionExecutable> executable,
                   Context* closure_context,
                   bool create_prototype)
    : Object(ObjectType::Function), executable_(std::move(executable)),
      closure_context_(capture_closure_context(closure_context)),
      closure_environment_(capture_closure_environment(closure_context, /*mark_escaped_now=*/false)),
      prototype_(nullptr), is_native_(false), is_constructor_(create_prototype), is_arrow_(false), is_class_constructor_(false), is_strict_(false), is_param_default_(false) {
    // executable_ may already be shared with sibling instances from the same
    // decl site -- populate its name once, or fall back to a per-instance
    // override if a sibling already claimed a different one (see
    // assign_decl_site_name's own doc comment).
    assign_decl_site_name(name);
    // Deferred: the object, and the 304-byte descriptor block its own
    // "constructor" entry needs, are built the first time anything asks for
    // them. Most functions are never used as constructors and never have
    // .prototype read, so most never pay for either. See ensure_prototype().
    prototype_pending_ = create_prototype;

    // Spec length is set explicitly by the caller via set_declared_length()
    // (writes through to executable_->declared_length) -- it depends on
    // which parameter came from the shared executable's cached spec length,
    // not derivable generically here.

    // This function is still being constructed, so no site can have cached a
    // lookup through its chain -- see initialize_prototype. Going through
    // set_prototype here retired every prototype cache entry in the engine
    // once per function created.
    if (Object* func_proto = ObjectFactory::get_function_prototype()) {
        initialize_prototype(func_proto);
    }
}

Function::Function(const std::string& name,
                   std::function<Value(Context&, std::span<const Value>, Value)> native_fn,
                   bool create_prototype)
    : Object(ObjectType::Function), closure_context_(nullptr), closure_environment_(nullptr),
      prototype_(nullptr), is_native_(true), is_constructor_(create_prototype), is_arrow_(false),
      is_class_constructor_(false), is_strict_(false), is_param_default_(false),
      instance_data_(new NativeFunctionData{std::move(native_fn), 0, name})
 {
    if (create_prototype) {
        auto proto = ObjectFactory::create_object();
        prototype_ = proto.release();
        PropertyDescriptor prototype_desc(Value(prototype_), PropertyAttributes::None);
        this->set_property_descriptor("prototype", prototype_desc);
    }

    // "name"/"length" are lazy -- see the class-header comment.
    // native_data()->declared_length defaults to 0 (already set above).
}

Function::Function(const std::string& name,
                   std::function<Value(Context&, std::span<const Value>, Value)> native_fn,
                   uint32_t arity,
                   bool create_prototype)
    : Object(ObjectType::Function), closure_context_(nullptr), closure_environment_(nullptr),
      prototype_(nullptr), is_native_(true), is_constructor_(create_prototype), is_arrow_(false),
      is_class_constructor_(false), is_strict_(false), is_param_default_(false),
      instance_data_(new NativeFunctionData{std::move(native_fn), arity, name})
 {
    if (create_prototype) {
        auto proto = ObjectFactory::create_object();
        prototype_ = proto.release();
        PropertyDescriptor prototype_desc(Value(prototype_), PropertyAttributes::None);
        this->set_property_descriptor("prototype", prototype_desc);
    }

    // "name"/"length" are lazy -- see the class-header comment.
}

Function::~Function() {
    if (!instance_data_) return;
    if (is_native_) delete static_cast<NativeFunctionData*>(instance_data_);
    else delete static_cast<NonNativeInstanceData*>(instance_data_);
}

void Function::setup_mapped_arguments(Context& fn_ctx, std::span<const Value> args, Object* arguments_obj) {
    const auto& parameter_objects_ = get_parameter_objects();
    // ES5 10.6 / ES6 9.4.4: mapped arguments only for simple, non-strict parameter lists.
    bool is_simple_params = true;
    for (const auto& p : parameter_objects_) {
        if (p->has_default() || p->is_rest() || p->has_destructuring()) {
            is_simple_params = false; break;
        }
    }
    if (fn_ctx.is_strict_mode() || parameter_objects_.empty() || !is_simple_params) return;

    size_t map_count = std::min(args.size(), parameter_objects_.size());
    // fn_ctx itself does NOT outlive the accessors once the arguments object
    // escapes this call (e.g. `args = arguments;` then read later): fn_ctx is
    // popped from the exec-context stack when this call returns and becomes
    // GC-invisible, but a raw Context* capture doesn't know that. Capture the
    // Environment instead -- the same object Context::get/set_binding just
    // delegate to -- and pin it via the standard closure_environment_
    // mechanism (Function::trace already visits it), exactly like a real JS
    // closure capturing its defining scope.
    Environment* env_ptr = capture_closure_environment(&fn_ctx);
    for (size_t mi = 0; mi < map_count; mi++) {
        if (!param_gets_mapped_accessor(parameter_objects_, mi)) continue;
        auto name = std::make_shared<std::string>(parameter_objects_[mi]->get_name()->get_name());
        auto getter_fn = ObjectFactory::create_native_function("get",
            [env_ptr, name](Context& ctx, std::span<const Value>, Value) -> Value {
                (void)ctx;
                return env_ptr ? env_ptr->get_binding(*name) : Value();
            });
        getter_fn->set_closure_environment(env_ptr);
        // Mark getter so get_property_descriptor can synthesize a DATA descriptor.
        // A C++-only private-field write, not a JS-visible property: a plain
        // property here would let any script forge trust onto its own
        // accessor (or, via Function.prototype, onto every function at once).
        getter_fn->is_mapped_arguments_accessor_ = true;
        auto setter_fn = ObjectFactory::create_native_function("set",
            [env_ptr, name](Context& ctx, std::span<const Value> a, Value this_value) -> Value {
                (void)ctx;
                if (!a.empty() && env_ptr) env_ptr->set_binding(*name, a[0]);
                return Value();
            });
        setter_fn->set_closure_environment(env_ptr);
        PropertyDescriptor map_desc;
        map_desc.set_getter(getter_fn.get());
        map_desc.set_setter(setter_fn.get());
        map_desc.set_enumerable(true);
        map_desc.set_configurable(true);
        getter_fn.release(); setter_fn.release();
        arguments_obj->set_property_descriptor(std::to_string(mi), map_desc);
    }
}

void Function::create_arguments_object(Context& fn_ctx, std::span<const Value> args) {
    const auto& parameter_objects_ = get_parameter_objects();
    auto arguments_obj = ObjectFactory::create_array(args.size());
    // Elements for non-mapped indices; mapped ones get accessor descriptors below.
    // Only skip elements for simple param lists (no defaults/rest/destructuring).
    bool pre_simple = !fn_ctx.is_strict_mode() && !parameter_objects_.empty();
    if (pre_simple) {
        for (const auto& p : parameter_objects_) {
            if (p->has_default() || p->is_rest() || p->has_destructuring()) { pre_simple = false; break; }
        }
    }
    size_t map_count_pre = pre_simple ? std::min(args.size(), parameter_objects_.size()) : 0;
    for (size_t i = 0; i < args.size(); i++) {
        if (i < map_count_pre && param_gets_mapped_accessor(parameter_objects_, i)) continue; // will be set via accessor
        arguments_obj->set_element(i, args[i]);
    }
    {
        // Stop being an Array first: an array's length is virtual and reads as
        // non-configurable, so installing the configurable one Arguments needs
        // would be rejected as an attempt to relax it.
        arguments_obj->set_type(Object::ObjectType::Arguments);
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
        Object* global = fn_ctx.get_global_object();
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
    // 'callee' is a poison-pill accessor using the shared %ThrowTypeError% intrinsic.
    if (fn_ctx.is_strict_mode()) {
        if (!Function::s_throw_type_error_) {
            auto thrower = ObjectFactory::create_native_function("ThrowTypeError",
                [](Context& ctx, std::span<const Value> args, Value this_value) -> Value {
                    (void)args;
                    ctx.throw_type_error("'callee' may not be accessed on strict mode arguments");
                    return Value();
                });
            // %ThrowTypeError% must be non-extensible with non-configurable, non-writable properties
            PropertyDescriptor len_desc(Value(0.0), PropertyAttributes::None);
            len_desc.set_configurable(false); len_desc.set_writable(false); len_desc.set_enumerable(false);
            thrower->set_property_descriptor("length", len_desc);
            PropertyDescriptor name_desc(Value(std::string("")), PropertyAttributes::None);
            name_desc.set_configurable(false); name_desc.set_writable(false); name_desc.set_enumerable(false);
            thrower->set_property_descriptor("name", name_desc);
            thrower->prevent_extensions();
            Function::s_throw_type_error_ = thrower.release();
        }

        PropertyDescriptor callee_desc;
        callee_desc.set_getter(Function::s_throw_type_error_);
        callee_desc.set_setter(Function::s_throw_type_error_);
        callee_desc.set_configurable(false);
        callee_desc.set_enumerable(false);
        arguments_obj->set_property_descriptor("callee", callee_desc);
        // 'caller' is NOT added as own property in strict mode (ES2017 spec)
    } else {
        // ES5 10.6 step 13.a: callee is {writable:true, enumerable:false, configurable:true}
        PropertyDescriptor callee_desc(Value(this), PropertyAttributes::BuiltinFunction);
        arguments_obj->set_property_descriptor("callee", callee_desc);
    }

    setup_mapped_arguments(fn_ctx, args, arguments_obj.get());
    fn_ctx.create_binding("arguments", Value(arguments_obj.release()), true, false);
}

bool Function::has_closure_props() const {
    return has_closure_props_hint_;
}

const std::vector<std::unique_ptr<Parameter>>& Function::get_parameter_objects() const {
    static const std::vector<std::unique_ptr<Parameter>> empty;
    return executable_ ? executable_->parameter_objects : empty;
}

Value Function::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    switch (get_function_kind()) {
        case FunctionKind::Async: return static_cast<AsyncFunction*>(this)->call(ctx, args, this_value);
        case FunctionKind::Generator: return static_cast<GeneratorFunction*>(this)->call(ctx, args, this_value);
        case FunctionKind::AsyncGenerator: return static_cast<AsyncGeneratorFunction*>(this)->call(ctx, args, this_value);
        default:
            if (is_native_) return call_native_rooted(ctx, args, this_value);
            return call_default_impl(ctx, args, this_value, &args);
    }
}

Value Function::call_default(Context& ctx, const std::vector<Value>& args, Value this_value) {
    if (is_native_) return call_native_rooted(ctx, args, this_value);
    return call_default_impl(ctx, args, this_value, &args);
}

Value Function::call_default_impl(Context& ctx, std::span<const Value> args, Value this_value,
                                  const std::vector<Value>* args_vec) {
    // A vector's storage is malloc'd and invisible to the stack scan, so it
    // has to be rooted for the whole call. Register-resident arguments are
    // already covered by the caller's own frame and need nothing.
    std::optional<ValueVectorRoot> args_root;
    if (args_vec) args_root.emplace(args_vec);
    // Consumed immediately so a nested call triggered from inside this invocation
    // (e.g. a native function calling another function) doesn't inherit it.
    bool is_construct_invocation = ctx.consume_pending_construct_call();
    CallStack& stack = CallStack::instance();
    // Runaway recursion has to become a catchable error before it becomes a
    // segfault: past this depth the C++ stack under the interpreter is nearly
    // spent, and nothing below here would get the chance to report it.
    if (stack.depth() >= CallStack::MAX_STACK_DEPTH) {
        ctx.throw_range_error("Maximum call stack size exceeded");
        return Value();
    }
    // A frame count is only a stand-in for how much stack is left, and it is
    // calibrated for the thread's. A generator or async function runs on a
    // fiber whose stack is a fraction of that, so the count runs out long
    // after the stack does and the process dies instead of reporting
    // anything. Where the stack's own end is known, ask it.
    if (const char* floor = current_stack_floor()) {
        const char probe = 0;
        if (&probe < floor) {
            ctx.throw_range_error("Maximum call stack size exceeded");
            return Value();
        }
    }
    CallStackFrameGuard frame_guard(stack, &ctx.get_current_filename(), this);

    // Class constructors must be called with new
    // Only a class declaration's constructor carries this, and that is always
    // compiled function code, so the native entry has no reason to ask.
    if (is_class_constructor_ && !ctx.is_in_constructor_call()) {
        ctx.throw_exception(Value("TypeError: Class constructor " + get_name() + " cannot be invoked without 'new'"));
        return Value();
    }

    
    // Resolve the fast-path gating states as early as possible (before any
    // Context exists) so a Function called for the first time -- e.g. a
    // closure recreated fresh on every call of its enclosing function, then
    // invoked exactly once -- can still qualify below, instead of being
    // permanently confined to the slow path that only ever resolves these on
    // a "next" call that never comes. Only possible once bytecode_chunk_ is
    // already attached (true for shared/pre-attached chunks); the slow-path
    // resolutions further down remain as the fallback for a chunk that
    // compiles for the first time later in this same call.
    //
    // A set fast_gate already means all three resolved, so a warm call skips
    // the block outright instead of re-asking three questions it settled on
    // its first trip through.
    if (executable_ && !executable_->fast_gate && executable_->bytecode_chunk) {
        if (executable_->strict_directive_state < 0) {
            // A concise arrow body is an expression, which cannot carry a
            // directive prologue -- resolved, not skipped, or the gate below
            // never opens for one. Read off the executable rather than the
            // tree: body_has_use_strict was taken when the body was attached.
            executable_->strict_directive_state =
                (!is_strict_ && executable_->body_has_use_strict) ? 1 : 0;
            executable_->recompute_fast_gate();
        }
        if (executable_->closure_props_state < 0) {
            executable_->closure_props_state = has_closure_props() ? 1 : 0;
            executable_->recompute_fast_gate();
        }
        if (executable_->self_name_state < 0) {
            executable_->self_name_state = 0;
            const std::string& self_name = get_name();
            if (!self_name.empty() && self_name != "<anonymous>") {
                for (const auto& n : executable_->bytecode_chunk->names) {
                    if (*n == self_name) { executable_->self_name_state = 1; break; }
                }
            }
            executable_->recompute_fast_gate();
        }
    }

    // Register-mode fast call: a compiled chunk with no env-resident names
    // needs no per-call Environment (every binding insert is already skipped)
    // and no heap Context / survivor-pool bookkeeping -- a stack Context
    // pointing straight at the captured chain suffices.
    // A class constructor is allowed in as long as it is a base class: a
    // derived one needs the this-TDZ and super bookkeeping the full path sets
    // up. The [[HomeObject]]/private-brand bindings the full path also creates
    // cannot be missed here, because `super`, a private name and a direct eval
    // each force env_mode, which this same condition already excludes.
    bool ctor_ok = !is_class_constructor_ || !is_derived_ctor();
    if (g_vm_enabled && executable_ && executable_->fast_gate && ctor_ok &&
        !(is_arrow_ && closure_context_ && closure_context_->this_needs_super())) {
        // The context is heap-allocated and survivor-managed like the full
        // path: native code (promise reactions, job queues) can capture the
        // active context and run after this call returns, so a stack context
        // would dangle. It is taken from the call pool rather than built,
        // since consecutive calls differ in only the fields reset_for_call
        // writes. The saving beyond that is everything else: no per-call
        // Environment, no binding inserts, `this` as a run() param.
        Engine* fast_engine = ctx.get_engine();
        Context& fast_ctx = *CallContextPool::acquire(fast_engine, &ctx);
        struct PoolRelease {
            Context* c; Engine* e;
            ~PoolRelease() { CallContextPool::release(c, e); }
        } fast_release{&fast_ctx, fast_engine};
        Environment* outer_env = get_closure_environment();
        if (!outer_env && closure_context_) outer_env = closure_context_->get_lexical_environment();
        if (!outer_env) outer_env = ctx.get_lexical_environment();
        // The chain can outlive this call for the same reason the context can.
        if (outer_env) outer_env->mark_escaped();
        fast_ctx.set_lexical_environment(outer_env);
        fast_ctx.set_variable_environment(outer_env);
        fast_ctx.set_arrow_function_context(is_arrow_);
        if (is_strict_ || executable_->fast_strict) fast_ctx.set_strict_mode(true);

        Value fast_this = this_value;
        // Skipped entirely when the body cannot observe `this`: Op::LdaThis is
        // the only reader, and a native called from here is handed its own
        // receiver rather than reading one off this context.
        if (executable_->fast_uses_this) {
            if (is_arrow_) {
                // Own, not inherited: ArrowFunctionExpression::evaluate stamps
                // these markers on the arrow itself, so asking has_property here
                // only bought a walk up to Function.prototype and Object.prototype
                // on every call.
                if (has_arrow_this_) fast_this = arrow_this_;
            } else if (!fast_ctx.is_strict_mode()) {
                if (this_value.is_undefined() || this_value.is_null()) {
                    Object* global = fast_ctx.get_global_object();
                    if (global) fast_this = Value(global);
                } else if (!this_value.is_object() && !this_value.is_function()) {
                    // box_primitive_this_sloppy's own first check is exactly this --
                    // skip the cross-TU call for the common already-object `this`
                    // (every ordinary method call), not just primitives.
                    fast_this = ObjectFactory::box_primitive_this_sloppy(fast_ctx, this_value);
                }
            }
            if (fast_this.is_object() || fast_this.is_function()) {
                fast_ctx.set_this_binding(fast_this.is_object() ? fast_this.as_object()
                                                                : fast_this.as_function());
            }
        }

        ExecContextScope gc_frame(&fast_ctx);
        Context* prev_context = Object::current_context_;
        Object::current_context_ = &fast_ctx;
        Value vm_result = VM::run(*executable_->bytecode_chunk, fast_ctx, args, &fast_this, this);
        Object::current_context_ = prev_context;
        if (fast_ctx.has_exception()) {
            ctx.throw_exception(fast_ctx.get_exception(), true);
            return Value();
        }
        return vm_result;
    }

    // Environment-mode functions take the general path today only because the
    // register-mode gate excludes them, not because they need any of what that
    // path does: the context still has to be built and own an Environment, but
    // the prologue around it -- parameter objects, the strict directive scan,
    // new.target and arrow bookkeeping, the closure-property sweep, the class
    // slots, the self-name and arguments questions -- is either decl-site
    // constant or does not apply. Everything that is not constant is a term of
    // this gate, so entering here means the general path would have done
    // exactly what follows.
    if (g_vm_enabled && executable_ && executable_->fast_env_gate && !is_arrow_ &&
        !is_class_constructor_ &&
        !(ctx.is_in_constructor_call() && !ctx.get_new_target().is_undefined())) {
        const ClassSlots& slots = class_slots();
        if (!slots.home_object && !slots.super_ctor && !slots.super_is_null && !slots.private_brands) {
            Engine* env_engine = ctx.get_engine();
            Context& env_ctx = *CallContextPool::acquire(env_engine, &ctx);
            struct PoolRelease {
                Context* c; Engine* e;
                ~PoolRelease() { CallContextPool::release(c, e); }
            } env_release{&env_ctx, env_engine};
            // What create_function_context builds around the context: the
            // scope the closure was created in, and a fresh Environment of
            // this call's own hanging off it.
            Environment* outer_env = get_closure_environment();
            if (!outer_env && closure_context_) outer_env = closure_context_->get_lexical_environment();
            if (!outer_env) outer_env = ctx.get_lexical_environment();
            if (is_param_default()) {
                Environment* walk = outer_env;
                while (walk && walk->get_type() == Environment::Type::Declarative) {
                    if (!walk->get_outer()) break;
                    walk = walk->get_outer();
                }
                if (walk && walk->get_type() != Environment::Type::Declarative) outer_env = walk;
            }
            // The chain can outlive this call: a closure made inside it keeps
            // pointing here after the call returns.
            if (outer_env) outer_env->mark_escaped();
            Environment* call_env = new Environment(Environment::Type::Function, outer_env);
            env_ctx.set_lexical_environment(call_env);
            env_ctx.set_variable_environment(call_env);
            env_ctx.set_owned_env(call_env);
            ExecContextScope gc_frame(&env_ctx);
            env_ctx.set_arrow_function_context(false);
            if (is_strict_ || executable_->fast_strict) env_ctx.set_strict_mode(true);

            Value actual_this = this_value;
            if (!env_ctx.is_strict_mode()) {
                if (this_value.is_undefined() || this_value.is_null()) {
                    if (Object* global = env_ctx.get_global_object()) actual_this = Value(global);
                } else if (!this_value.is_object() && !this_value.is_function()) {
                    actual_this = ObjectFactory::box_primitive_this_sloppy(env_ctx, this_value);
                }
            }
            if (actual_this.is_object() || actual_this.is_function()) {
                env_ctx.set_this_binding(actual_this.is_object() ? actual_this.as_object()
                                                                 : actual_this.as_function());
            }
            env_ctx.create_binding("this", actual_this, true);

            Context* prev_context = Object::current_context_;
            Object::current_context_ = &env_ctx;
            Value vm_result = VM::run(*executable_->bytecode_chunk, env_ctx, args, nullptr, this);
            Object::current_context_ = prev_context;

            if (env_ctx.was_super_called()) {
                ctx.set_super_called(true);
                if (env_ctx.last_super_override()) {
                    ctx.set_last_super_override(env_ctx.last_super_override());
                }
            }
            if (env_ctx.has_exception()) {
                ctx.throw_exception(env_ctx.get_exception(), true);
                return Value();
            }
            return vm_result;
        }
    }

    return call_tree_walker(ctx, args, this_value);
}

// Out of line for the same reason as call_tree_walker below.
namespace {

// Only an eval native takes these, and each one builds a std::string and can
// throw. Left inline they hand every builtin call the stack layout and the
// unwind edges that work needs, for a branch it does not take.
[[gnu::noinline, gnu::cold]] bool save_eval_caller_this(Context& ctx, const Value& caller_this) {
    if (ctx.has_binding("__eval_caller_this__")) return false;
    ctx.create_binding("__eval_caller_this__", caller_this, true);
    return true;
}

[[gnu::noinline, gnu::cold]] Value throw_call_stack_exceeded(Context& ctx) {
    ctx.throw_range_error("Maximum call stack size exceeded");
    return Value();
}

[[gnu::noinline, gnu::cold]] Value throw_class_ctor_without_new(Context& ctx, const std::string& name) {
    ctx.throw_exception(Value("TypeError: Class constructor " + name +
                              " cannot be invoked without 'new'"));
    return Value();
}

[[gnu::noinline, gnu::cold]] void drop_eval_caller_this(Context& ctx) {
    try { ctx.delete_binding("__eval_caller_this__"); } catch (...) {}
}

}

// Entered directly rather than through call_default_impl. That function is
// built for a JS body -- a closure environment, the bytecode gates, the
// argument binding -- and a native reaches none of it, yet paid its prologue
// and its frame to walk past all of it. What a native does need from there is
// short enough to stand here: the two recursion guards, a stack frame, and the
// pending-construct flag, in the order that function had them.
// A vector's storage is malloc'd and invisible to the stack scan, so it has to
// be rooted for the whole call. Only a caller that arrived with one needs this,
// which a register-mode call never does -- its arguments are covered by its own
// frame -- so the root lives out here rather than as a branch inside every call.
Value Function::call_native_rooted(Context& ctx, const std::vector<Value>& args_vec,
                                   Value this_value) {
    ValueVectorRoot args_root(&args_vec);
    return call_native(ctx, args_vec, this_value);
}

[[gnu::noinline]] Value Function::call_native(Context& ctx, std::span<const Value> args,
                                              Value this_value) {
    // Consumed immediately so a nested call triggered from inside this
    // invocation doesn't inherit it.
    const bool is_construct_invocation = ctx.consume_pending_construct_call();
    CallStack& stack = CallStack::instance();
    if (stack.depth() >= CallStack::MAX_STACK_DEPTH) return throw_call_stack_exceeded(ctx);
    // A frame count is only a stand-in for how much stack is left, and it is
    // calibrated for the thread's. Where the stack's own end is known, ask it.
    if (const char* floor = current_stack_floor()) {
        const char probe = 0;
        if (&probe < floor) return throw_call_stack_exceeded(ctx);
    }
    CheckedDepthFrameGuard frame_guard(stack, &ctx.get_current_filename(), this);

    // A native is handed its receiver, so the context's `this` is left alone:
    // it still holds the caller's, which is what a direct eval inside a native
    // has to inherit.
    bool saved_caller_this = false;
    if (UNLIKELY_NATIVE(is_eval_native_ && !ctx.get_this_value().is_undefined())) {
        saved_caller_this = save_eval_caller_this(ctx, ctx.get_this_value());
    }

    // ctx is reused as-is (no fresh Context for natives) -- if native_data()->fn
    // stashes current_context_ somewhere long-lived (Promise's own ctor,
    // setTimeout), it's THIS context that would leak. ContextSurvivorGuard
    // consults this instead of registering unconditionally.
    if (native_captures_ctx_) ctx.mark_exposed_to_escape();
    Context* prev_context = Object::current_context_;
    Object::current_context_ = &ctx;
    // A native takes a view of the arguments wherever they already are.
    //
    // A plain call has to see new.target as undefined, and the context is
    // shared with the caller, so a native invoked from inside a constructor
    // body would otherwise inherit that constructor's. Only a construct
    // invocation ever puts anything there, so an ordinary call finds it
    // already undefined and has nothing to clear -- and nothing that has to
    // survive the call in order to be put back.
    Value result;
    if (UNLIKELY_NATIVE(!is_construct_invocation && !ctx.get_new_target().is_undefined())) {
        const Value caller_new_target = ctx.get_new_target();
        ctx.set_new_target(Value());
        result = native_data()->fn(ctx, args, this_value);
        ctx.set_new_target(caller_new_target);
    } else {
        result = native_data()->fn(ctx, args, this_value);
    }
    Object::current_context_ = prev_context;



    if (UNLIKELY_NATIVE(saved_caller_this)) drop_eval_caller_this(ctx);

    return result;
}

// A register-mode call never reaches this, so it is not part of that call's
// function. Safe to split because it reads nothing the caller's prologue
// produced: the frame guard and the argument root stay live there across it.
[[gnu::noinline]] Value Function::call_tree_walker(Context& ctx, std::span<const Value> args,
                                                  Value this_value) {
    // Only the tree-walker below reads these. They used to be computed in the
    // prologue, on the wrong side of the fast-path gate: get_parameter_objects
    // carries a static-init guard and body_ is dead there.
    // The names shadow the old members so the slow path reads unchanged --
    // both are decl-site data living on the shared executable_.
    const auto& parameter_objects_ = get_parameter_objects();
    ASTNode* ast = ast_body();
    // A body that is still deferred after being asked for did not rebuild.
    // The one way that happens is running out of stack partway through it,
    // which is an error the caller has to see: without this the function is
    // indistinguishable from one with an empty body and quietly answers
    // undefined.
    if (!ast && executable_ && executable_->body_is_deferred()) {
        ctx.throw_syntax_error("Maximum expression nesting exceeded");
        return Value();
    }
    ASTNode* body_ = ast;

    Context* parent_context = &ctx;
    auto function_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), parent_context, this);
    Context& function_context = *function_context_ptr;
    ExecContextScope gc_frame(&function_context);

    // Transfer to Engine's survivor pool on return instead of destroying it --
    // see ContextSurvivorGuard's doc comment for why an abrupt exit path still
    // needs this.
    Engine* fn_engine = ctx.get_engine();
    ContextSurvivorGuard survivor_guard(function_context_ptr, fn_engine);

    // Propagate new.target into function scope
    if (ctx.is_in_constructor_call() && !ctx.get_new_target().is_undefined()) {
        function_context.set_new_target(ctx.get_new_target());
    }

    // Arrow functions capture new.target from enclosing scope
    if (is_arrow_ && this->has_own_property("__arrow_new_target__")) {
        function_context.set_new_target(this->get_property("__arrow_new_target__"));
    }
    function_context.set_arrow_function_context(is_arrow_);

    // Arrows share the enclosing derived constructor's this-TDZ state: `this`
    // (and a second super()) inside an arrow must throw while the creating
    // constructor hasn't finished super() yet. Copied back after the body runs.
    if (is_arrow_ && closure_context_) {
        function_context.set_this_needs_super(closure_context_->this_needs_super());
        function_context.set_super_called(closure_context_->was_super_called());
    }
    // Field-initializer arrows re-arm the direct-eval `arguments` ban (see
    // ArrowFunctionExpression::evaluate); nested arrows re-mark themselves
    // from this flag at their own creation.
    if (is_arrow_ && this->has_own_property("__in_cfi__")) {
        function_context.set_in_class_field_init(true);
    }

    // Check for strict mode BEFORE setting up 'this' binding
    if (is_strict_) {
        function_context.set_strict_mode(true);
    }
    if (ast && ast->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        if (executable_->strict_directive_state < 0) {
            bool was_strict = function_context.is_strict_mode();
            BlockStatement* block = static_cast<BlockStatement*>(ast);
            block->check_use_strict_directive(function_context);
            executable_->strict_directive_state =
                (!was_strict && function_context.is_strict_mode()) ? 1 : 0;
            executable_->recompute_fast_gate();
        } else if (executable_->strict_directive_state == 1) {
            function_context.set_strict_mode(true);
        }
    }

    Value actual_this = this_value;

    // Arrow functions use their lexical this, ignoring the passed this_value
    if (is_arrow_ && has_arrow_this_) {
        actual_this = arrow_this_;
    }

    bool is_strict_now = function_context.is_strict_mode();
    bool this_is_nullish = this_value.is_undefined() || this_value.is_null();
    if (!is_arrow_ && !is_strict_now) {
        if (this_is_nullish) {
            Object* global = function_context.get_global_object();
            if (global) {
                actual_this = Value(global);
            }
        } else if (!this_value.is_object() && !this_value.is_function()) {
            // Same cross-TU-call skip as the register-fast path above: an
            // already-object `this` (the common case) needs no boxing.
            actual_this = ObjectFactory::box_primitive_this_sloppy(function_context, this_value);
        }
    }

    if (actual_this.is_object() || actual_this.is_function()) {
        Object* this_obj = actual_this.is_object() ? actual_this.as_object() : actual_this.as_function();
        function_context.set_this_binding(this_obj);
    }

    // Track uninitialized this for derived constructors (for super[expr] check).
    // `extends null` is still ConstructorKind "derived": this stays uninitialized
    // (its super() always throws TypeError, so it can never become initialized).
    if (is_class_constructor_) {
        if (!is_default_ctor() && is_derived_ctor()) {
            function_context.set_this_needs_super(true);
            function_context.set_super_called(false);
        }
    }

    // Register-mode VM frames take `this` as a run() parameter (Op::LdaThis),
    // so the per-call binding insert -- and the hash-map growth it forces in
    // the fresh Environment -- is skipped. First call (chunk not compiled
    // yet) and every non-VM path still bind normally.
    bool vm_register_fast = g_vm_enabled && !executable_->vm_incompatible && executable_->bytecode_chunk &&
                            !executable_->bytecode_chunk->env_mode && !function_context.this_needs_super();
    if (!vm_register_fast) {
        if (!function_context.create_binding("this", actual_this, true)) {
            // Binding already exists -- force update
            function_context.set_binding("this", actual_this);
        }
    }

    // A named class's own name is bound as an immutable self-reference inside its
    // methods (__closure_const_<name>) since the class's name binding doesn't exist
    // in scope yet when its methods are created -- see ClassDeclaration::evaluate.
    // Everything else resolves through closure_environment_, no materialization needed.
    if (executable_->closure_props_state != 0) {
        auto prop_keys = this->get_internal_property_keys();
        bool found_any = false;
        for (const auto& key : prop_keys) {
            if (key.length() > 10 && key.substr(0, 10) == "__closure_" && key.substr(0, 16) != "__closure_const_") {
                found_any = true;
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
        if (executable_->closure_props_state < 0) {
            executable_->closure_props_state = found_any ? 1 : 0;
            executable_->recompute_fast_gate();
        }
    }

    // Super/private-brand bindings must exist before the VM branch too --
    // super.x and #private access delegate to the tree-walker's own evaluate()
    // (BytecodeCompiler::emit_treewalker_delegate) and resolve these via
    // normal environment lookup, same as the tree-walker path below.
    const ClassSlots& slots = class_slots();
    if (slots.home_object) {
        function_context.create_binding("__home_object__", Value(slots.home_object), false);
    }
    if (slots.super_ctor) {
        function_context.create_binding("__super__", Value(slots.super_ctor), false);
        // member.cpp's super lookup needs to know if this is a static method (resolves on the parent constructor itself) or an instance method (resolves on its .prototype).
        if (slots.is_static_method) {
            function_context.create_binding("__super_is_static__", Value(true), false);
        }
    }
    if (slots.super_is_null) {
        function_context.create_binding("__super_is_null__", Value(true), false);
    }
    if (!vm_register_fast && slots.private_brands) {
        function_context.create_binding("__eval_private_names__", Value(slots.private_brands), false);
    }

    // VM execution branches off BEFORE parameter/arguments materialization:
    // compiled functions read parameters from registers and reject any use of
    // `arguments`/`this`/`eval`, so the whole binding ceremony below is dead
    // weight for them (it dominated call-heavy benchmarks, e.g. fib). Derived
    // constructors ARE compiled -- Op::LdaThis carries its own this-TDZ check.
    // A concise arrow body is an expression, and the compiler now takes one
    // (as an implicit return), so the shape of the body no longer decides
    // whether a function is compiled at all.
    if (g_vm_enabled && !executable_->vm_incompatible && ast) {
        if (!executable_->bytecode_chunk) {
            // A `with` environment in the captured scope chain makes write-
            // reference resolution order observable: the tree-walker resolves
            // (and SetMutableBinding HasProperty-checks) the target BEFORE the
            // RHS runs, while Op::StaLookup resolves at write time. The chain
            // is fixed at closure creation, so one check decides for good.
            for (Environment* e = function_context.get_lexical_environment(); e; e = e->get_outer()) {
                if (e->is_with_environment()) { executable_->vm_incompatible = true; executable_->recompute_fast_gate(); break; }
            }
        }
        if (!executable_->bytecode_chunk && !executable_->vm_incompatible) {
            executable_->bytecode_chunk = BytecodeCompiler::compile(ast, parameter_objects_);
            executable_->recompute_fast_gate();
            if (executable_->bytecode_chunk) {
                // The chunk's constants (new, unmarked cells) are only reachable
                // through this Function's trace(). If this Function already
                // survived an earlier GC cycle (sticky mark bit = "old"), a
                // minor collection won't re-trace it without this barrier,
                // leaving the constants permanently unmarked -- a real
                // dangling-pointer bug once sweep runs, not just a diagnostic.
                Collector::write_barrier(this);
                static const bool disasm = [] {
                    const char* env = std::getenv("QUANTA_VM_DISASM");
                    return env && env[0] == '1';
                }();
                if (disasm) {
                    std::fprintf(stderr, "%s", disassemble_chunk(*executable_->bytecode_chunk, get_name()).c_str());
                }
            } else {
                executable_->vm_incompatible = true;
                executable_->recompute_fast_gate();
            }
        }
        if (executable_->bytecode_chunk) {
            // Named function expressions still need their self-reference
            // binding for recursion through LdaLookup -- but only if the
            // compiled body mentions the name at all (checked once).
            if (executable_->self_name_state < 0) {
                executable_->self_name_state = 0;
                const std::string& self_name = get_name();
                if (!self_name.empty() && self_name != "<anonymous>") {
                    for (const auto& n : executable_->bytecode_chunk->names) {
                        if (*n == self_name) { executable_->self_name_state = 1; break; }
                    }
                }
                executable_->recompute_fast_gate();
            }
            if (executable_->self_name_state == 1) {
                if (!function_context.has_binding(get_name())) {
                    function_context.create_binding(get_name(), Value(this), false);
                } else {
                    // The captured chain already provides the name (function
                    // declarations): fast calls don't need the self-binding.
                    executable_->self_name_state = 2;
                    executable_->recompute_fast_gate();
                }
            }
            // Arrows resolve `arguments` lexically -- only a real function
            // materializes its own. The mapped accessors read the parameter
            // bindings lazily, so creating this before run() binds them is fine.
            if (executable_->bytecode_chunk->needs_arguments && !is_arrow_) {
                create_arguments_object(function_context, args);
            }
            Context* prev_context = Object::current_context_;
            Object::current_context_ = &function_context;
            Value vm_result = VM::run(*executable_->bytecode_chunk, function_context, args,
                                      executable_->bytecode_chunk->env_mode ? nullptr : &actual_this, this);
            Object::current_context_ = prev_context;

            // Propagate super_called flag to parent context (mirrors the
            // tree-walker path below) -- Function::construct() checks this
            // on its own ctx after call() returns.
            if (function_context.was_super_called()) {
                ctx.set_super_called(true);
                if (function_context.last_super_override()) {
                    ctx.set_last_super_override(function_context.last_super_override());
                }
                if (is_arrow_ && closure_context_) {
                    closure_context_->set_super_called(true);
                    closure_context_->set_this_needs_super(false);
                }
            }

            if (function_context.has_exception()) {
                ctx.throw_exception(function_context.get_exception(), true);
                return Value();
            }

            // For class constructors: if super() updated this to a new object,
            // return it so Function::construct() can use the correct object
            // (mirrors the tree-walker path below). Only an undefined completion
            // (implicit fallthrough, or explicit `return;`/`return undefined;`)
            // falls back to this -- any other explicit return (including a bare
            // primitive like `return 0;`) must reach Function::construct() as-is
            // so its own "derived constructors may only return object or
            // undefined" TypeError check still fires.
            if (is_class_constructor_ && vm_result.is_undefined()) {
                Object* final_this = function_context.get_this_binding();
                if (final_this && actual_this.is_object() && final_this != actual_this.as_object()) {
                    return Value(final_this);
                }
            }
            return vm_result;
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
            // Retype before touching length: an array's is non-configurable,
            // and Arguments needs a configurable one.
            early_args->set_type(Object::ObjectType::Arguments);
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
                    // NamedEvaluation: an anonymous default takes the parameter's
                    // name. A class has to learn it before evaluating, since its
                    // static initializers can read this.name; everything else is
                    // named once it exists.
                    ASTNode* def = param->get_default_value();
                    const bool infer = !pname.empty() && !param->has_destructuring();
                    if (infer && def->get_type() == ASTNode::Type::CLASS_DECLARATION) {
                        auto* cd = static_cast<ClassDeclaration*>(def);
                        if (cd->is_expression() && cd->get_id() && cd->get_id()->get_name().empty()) {
                            cd->set_inferred_name(pname);
                        }
                    }
                    arg_value = def->evaluate(function_context);
                    if (function_context.has_exception()) {
                        function_context.set_in_param_eval(false);
                        ctx.throw_exception(function_context.get_exception(), true);
                        return Value();
                    }
                    if (infer && arg_value.is_function() && is_anon_func_def(def)) {
                        Function* fn = arg_value.as_function();
                        if (fn->get_name().empty() || fn->get_name() == "<arrow>") fn->set_name(pname);
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
        const std::vector<std::string>& params = get_parameters();
        for (size_t i = 0; i < params.size(); ++i) {
            Value arg_value = (i < args.size()) ? args[i] : Value();
            // ES1: Function parameters are mutable bindings
            function_context.create_binding(params[i], arg_value, true);
        }
    }
    
    // Arrow functions don't have their own arguments object -- they resolve
    // `arguments` lexically through closure_environment_ to the enclosing scope.
    // Everything else only gets one if the body (or a parameter initializer)
    // can actually name it, mirroring the register path's needs_arguments gate.
    // A direct eval keeps it: eval can name `arguments` at runtime, which no
    // static walk of this body can see.
    if (!is_arrow_) {
        if (executable_->needs_arguments_state < 0) {
            bool needs = true;
            if (body_) {
                needs = BytecodeCompiler::references_arguments(body_);
                if (!needs) {
                    for (const auto& p : parameter_objects_) {
                        if ((p->has_default() &&
                             BytecodeCompiler::references_arguments(p->get_default_value())) ||
                            (p->has_destructuring() &&
                             BytecodeCompiler::references_arguments(p->get_destructuring_pattern()))) {
                            needs = true;
                            break;
                        }
                    }
                }
                if (!needs && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT &&
                    static_cast<BlockStatement*>(body_)->has_direct_eval_cached()) {
                    needs = true;
                }
            }
            executable_->needs_arguments_state = needs ? 1 : 0;
        }
        if (executable_->needs_arguments_state == 1) {
            create_arguments_object(function_context, args);
        }
    }

    // Use actual_this which respects strict mode (can be undefined in strict mode)
    function_context.create_binding("this", actual_this, false);

    if (body_) {
        // ES5: Named function expressions have their name as an immutable binding
        const std::string& self_name = get_name();
        if (!self_name.empty() && self_name != "<anonymous>" && !function_context.has_binding(self_name)) {
            function_context.create_binding(self_name, Value(this), false);
        }

        // The scope the parameters were bound in, kept only when the body gets
        // a variable environment of its own below -- that is the one case where
        // a var can repeat a parameter name and not find it.
        Environment* param_env = nullptr;
        {
            bool has_complex_params = false;
            for (const auto& p : parameter_objects_) {
                if (p->has_default() || p->has_destructuring()) {
                    has_complex_params = true;
                    break;
                }
            }
            if (has_complex_params) {
                param_env = function_context.get_lexical_environment();
                function_context.push_block_scope();
                function_context.set_variable_environment(function_context.get_lexical_environment());
            }
        }

        if (body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
            scan_for_var_declarations(body_, function_context, param_env);
        }

        Context* prev_context = Object::current_context_;
        Object::current_context_ = &function_context;
        Value result = body_->evaluate(function_context);
        Object::current_context_ = prev_context;

        // Propagate super_called flag to parent context
        if (function_context.was_super_called()) {
            ctx.set_super_called(true);
            if (function_context.last_super_override()) {
                ctx.set_last_super_override(function_context.last_super_override());
            }
            if (is_arrow_ && closure_context_) {
                closure_context_->set_super_called(true);
                closure_context_->set_this_needs_super(false);
            }
        }

        // Outer-variable writes during body execution went straight through
        // closure_environment_ to the real defining Environment already (live,
        // by construction) -- no post-hoc diff/write-back/sibling-update needed.

        
        if (function_context.has_return_value()) {
            Value rv = function_context.get_return_value();
            // A derived ctor's bare `return;` still resolves to the current
            // `this` -- which super() may have swapped to an override object.
            if (!(is_class_constructor_ && rv.is_undefined())) {
                return rv;
            }
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

// "name"/"length" are lazy (see the class-header comment on
// name_deleted_/length_deleted_, and get_declared_length()): synthesizes the
// spec-correct descriptor {value, writable:false, enumerable:false,
// configurable:true} on demand, without ever touching descriptors_/shape --
// mirrors get_property's own fallback so introspection (getOwnPropertyDescriptor,
// hasOwnProperty via the caller, defineProperty's merge logic) sees a
// descriptor consistent with what a plain read would return, at zero
// allocation cost for the common (never introspected) case.
PropertyDescriptor Function::get_property_descriptor(const std::string& key) const {
    auto* d = descriptors();
    if (key == "name" && !name_deleted_ && !(d && d->count("name"))) {
        const std::string& n = get_name();
        return PropertyDescriptor(Value(n == "<arrow>" ? std::string("") : n), PropertyAttributes::Configurable);
    }
    if (key == "length" && !length_deleted_ && !(d && d->count("length")) && !has_shape_slot("length")) {
        return PropertyDescriptor(Value(static_cast<double>(get_declared_length())), PropertyAttributes::Configurable);
    }
    // Same treatment for "prototype", whose object the constructors keep in
    // prototype_ without an entry of its own: {writable, not enumerable, not
    // configurable} per ES5 13.2. A native constructor's is non-writable and
    // does get a real entry, which the check above hands back instead.
    if (key == "prototype" && (prototype_ || prototype_pending_) &&
        !(d && d->count("prototype")) && !has_shape_slot("prototype")) {
        PropertyDescriptor pd(Value(ensure_prototype()), PropertyAttributes::Writable);
        pd.set_enumerable(false);
        pd.set_configurable(false);
        return pd;
    }
    return Object::get_property_descriptor_default(key);
}

// Configurable:true, so `delete` always succeeds. Two cases: never
// materialized (nothing really stored, just flip the flag), or already
// materialized (e.g. a builtin whose "length" was overridden post-construction
// via set_property_descriptor, see that override below) -- there IS a real
// descriptors_/shape entry to erase via Object::delete_property, but the
// flag must ALSO be set afterward, or has_own_property/get_property_descriptor
// would see "no real entry" post-erase and incorrectly treat the property as
// virtually-present-again instead of genuinely, permanently gone.
bool Function::delete_property(const std::string& key) {
    auto* d = descriptors();
    if (key == "name") {
        if (!name_deleted_ && !(d && d->count("name"))) {
            name_deleted_ = true;
            return true;
        }
        bool ok = Object::delete_property_default(key);
        if (ok) name_deleted_ = true;
        return ok;
    }
    if (key == "length") {
        if (!length_deleted_ && !(d && d->count("length")) && !has_shape_slot("length")) {
            length_deleted_ = true;
            return true;
        }
        bool ok = Object::delete_property_default(key);
        if (ok) length_deleted_ = true;
        return ok;
    }
    return Object::delete_property_default(key);
}

// The one case that must NOT stay purely synthesized: any real definition
// request (Object.defineProperty, or the ~56 call sites across the builtins
// that construct a native function then immediately override its "length")
// needs persistent state to redefine against. Materializes first (running
// the exact same install Function's constructors used to run eagerly, shape
// slot included) so the second call below finds an existing property and
// takes Object::set_property_descriptor's normal "redefine" path -- callers
// need no changes.
bool Function::set_property_descriptor(const std::string& key, const PropertyDescriptor& desc) {
    auto* d = descriptors();
    if (key == "name" && !name_deleted_ && !(d && d->count("name"))) {
        const std::string& n = get_name();
        PropertyDescriptor mat(Value(n == "<arrow>" ? std::string("") : n), PropertyAttributes::Configurable);
        Object::set_property_descriptor_default("name", mat);
    } else if (key == "length" && !length_deleted_ && !(d && d->count("length")) && !has_shape_slot("length")) {
        PropertyDescriptor mat(Value(static_cast<double>(get_declared_length())), PropertyAttributes::Configurable);
        Object::set_property_descriptor_default("length", mat);
    } else if (key == "prototype" && (prototype_ || prototype_pending_) &&
               !(d && d->count("prototype")) && !has_shape_slot("prototype")) {
        PropertyDescriptor mat(Value(ensure_prototype()), PropertyAttributes::Writable);
        mat.set_enumerable(false);
        mat.set_configurable(false);
        Object::set_property_descriptor_default("prototype", mat);
    }
    return Object::set_property_descriptor_default(key, desc);
}

Value Function::get_property(const std::string& key) const {
    // Strict functions poison-pill their own `arguments`/`caller` properties
    // (spec: %ThrowTypeError% accessors); sloppy functions keep returning
    // undefined for compatibility.
    if ((key == "arguments" || key == "caller") &&
        is_strict_ && !is_native_ && !has_own_property(key)) {
        if (current_context_) {
            current_context_->throw_type_error(
                "'caller' and 'arguments' may not be accessed on strict mode functions");
        }
        return Value();
    }
    if (key == "name") {
        if (auto* d = descriptors()) {
            auto* it = d->find("name");
            if (it) {
                if (it->is_data_descriptor()) {
                    Value v = it->get_value();
                    if (v.is_string() && v.to_string() == "<arrow>") return Value(std::string(""));
                    return v;
                }
                if (it->is_accessor_descriptor()) {
                    Object* getter = it->get_getter();
                    if (getter && current_context_) {
                        Function* gfn = as_function(getter);
                        if (gfn) return gfn->call(*current_context_, {}, Value(const_cast<Function*>(this)));
                    }
                    return Value();
                }
            }
        }
        const std::string& n = get_name();
        return Value(n == "<arrow>" ? std::string("") : n);
    }
    if (key == "length") {
        auto* d = descriptors();
        if (d && d->count("length")) {
            PropertyDescriptor desc = (*d)["length"];
            if (desc.is_data_descriptor()) return desc.get_value();
            if (desc.is_accessor_descriptor()) {
                Object* getter = desc.get_getter();
                if (getter && current_context_) {
                    Function* gfn = as_function(getter);
                    if (gfn) return gfn->call(*current_context_, {}, Value(const_cast<Function*>(this)));
                }
                return Value();
            }
        }
        if (const Value* slot = find_shape_slot("length")) {
            return *slot;
        }
        if (length_deleted_) return Value(0.0);
        return Value(static_cast<double>(get_declared_length()));
    }
    if (key == "prototype") {
        if (prototype_ || prototype_pending_) return Value(ensure_prototype());
        Value base_val = get_own_property(key);
        if (!base_val.is_undefined()) return base_val;
        return Value();
    }

    Value result = get_own_property(key);
    if (!result.is_undefined() || has_own_property(key)) {
        return result;
    }
    // A setter-only own accessor must return undefined here, not fall through to an
    // inherited getter for the same key.
    if (auto* d = descriptors()) {
        auto* it = d->find(key);
        if (it && it->is_accessor_descriptor()) {
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
        if (auto* d = current->descriptors()) {
            auto* desc_it = d->find(key);
            if (desc_it) {
                const PropertyDescriptor& desc = *desc_it;
                if (desc.is_accessor_descriptor() && desc.has_getter()) {
                    Function* getter_fn = as_function(desc.get_getter());
                    if (getter_fn && current_context_) {
                        return getter_fn->call(*current_context_, {}, Value(const_cast<Function*>(this)));
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

Object* Function::ensure_prototype() const {
    if (prototype_) return prototype_;
    if (!prototype_pending_) return nullptr;
    prototype_pending_ = false;
    Function* self = const_cast<Function*>(this);
    auto proto = ObjectFactory::create_object();
    Collector::write_barrier(self);
    self->prototype_ = proto.release();
    // ES5 13.2: .prototype.constructor is {writable:true, enumerable:false, configurable:true}
    PropertyDescriptor ctor_desc(Value(self), static_cast<PropertyAttributes>(
        PropertyAttributes::Writable | PropertyAttributes::Configurable));
    self->prototype_->set_property_descriptor("constructor", ctor_desc);
    return prototype_;
}

void Function::set_function_prototype(Object* proto) {
    Collector::write_barrier(this);
    prototype_pending_ = false;
    prototype_ = proto;
    if (!proto) remove_own_property("prototype");
}

void Function::set_closure_environment(Environment* env) {
    Collector::write_barrier(this);
    if (env) env->mark_escaped();
    closure_environment_ = env;
}

void Function::mark_closure_environment_escaped() const {
    if (closure_environment_) closure_environment_->mark_escaped();
}

void Function::set_name(const std::string& name) {
    if (executable_) assign_decl_site_name(name);
    else if (auto* nd = native_data()) nd->name = name;
    // Force-update the name in descriptors (bypasses writable check)
    // But don't overwrite if the descriptor was explicitly set to a function (e.g. static name())
    if (auto* d = descriptors()) {
        auto* it = d->find("name");
        if (it && it->is_data_descriptor()) {
            if (!it->get_value().is_function()) {
                *it = PropertyDescriptor(Value(get_name()), it->get_attributes());
            }
        }
    }
}

std::vector<std::string> Function::get_internal_property_keys() const {
    return Object::get_own_property_keys_default();
}

std::vector<std::string> Function::get_own_property_keys() const {
    // Object::get_own_property_keys_default() already sorts array-index keys first
    // (ascending), then strings in creation order, then symbols -- that ordering must
    // be preserved even when the function also has integer-named static members (e.g.
    // `static [1](){}`). Only the STRING portion gets length/name/prototype pulled to
    // its front here.
    auto all = Object::get_own_property_keys_default();
    // "length"/"name" won't be in the base result when virtually present
    // (lazy, not yet materialized) -- inject them so the priority loop below
    // still surfaces them (Object.getOwnPropertyNames/Reflect.ownKeys must
    // see them; they're non-enumerable so Object.keys/for-in still won't).
    auto* d = descriptors();
    if (!length_deleted_ && !(d && d->count("length")) && !has_shape_slot("length")) {
        all.push_back("length");
    }
    if (!name_deleted_ && !(d && d->count("name"))) {
        all.push_back("name");
    }
    if ((prototype_ || prototype_pending_) && !(d && d->count("prototype")) &&
        !has_shape_slot("prototype")) {
        all.push_back("prototype");
    }
    std::vector<std::string> result;
    result.reserve(all.size());

    size_t i = 0;
    for (; i < all.size(); i++) {
        uint32_t idx;
        if (!is_array_index(all[i], &idx)) break;
        result.push_back(all[i]);
    }

    static const char* const kPriority[] = { "length", "name", "prototype" };
    for (const char* pkey : kPriority) {
        for (size_t j = i; j < all.size(); j++) {
            if (all[j] == pkey) { result.push_back(all[j]); break; }
        }
    }
    for (; i < all.size(); i++) {
        const auto& k = all[i];
        if (k == "length" || k == "name" || k == "prototype") continue;
        result.push_back(k);
    }
    return result;
}

bool Function::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    Collector::write_barrier(this);
    if (key == "prototype") {
        if (attrs == PropertyAttributes::Default) {
            if (auto* d = descriptors()) {
                auto* it = d->find("prototype");
                if (it && it->is_data_descriptor() && !it->is_writable()) {
                    return false;
                }
            }
        }
        if (value.is_object()) {
            prototype_pending_ = false;
            prototype_ = value.as_object();
            Object::delete_property_default(key);
            return true;
        }
        if (value.is_function()) {
            prototype_pending_ = false;
            prototype_ = value.as_function();
            Object::delete_property_default(key);
            return true;
        }
        // A non-object value clears prototype_, but the property is still this
        // function's own: without an entry of its own the write would walk the
        // chain and hand itself to an accessor inherited from
        // Function.prototype, which ES5 13.2 step 18 forbids.
        prototype_pending_ = false;
        prototype_ = nullptr;
        if (attrs == PropertyAttributes::Default && !(descriptors() && descriptors()->count(key)) &&
            !has_shape_slot(key)) {
            PropertyDescriptor own(value, PropertyAttributes::Writable);
            own.set_enumerable(false);
            own.set_configurable(false);
            Object::set_property_descriptor_default(key, own);
            return true;
        }
        return Object::set_property_default(key, value, attrs);
    }

    bool ok = Object::set_property_default(key, value, attrs);
    if (ok && key.size() > 10 && key.compare(0, 10, "__closure_") == 0 &&
        key.compare(0, 16, "__closure_const_") != 0) {
        has_closure_props_hint_ = true;
    }
    return ok;
}

Value Function::construct(Context& ctx, const std::vector<Value>& args) {
    ValueVectorRoot args_root(&args);
    return construct(ctx, std::span<const Value>(args));
}

Value Function::construct(Context& ctx, std::span<const Value> args) {
    // Check if this function is a constructor
    if (!is_constructor_) {
        ctx.throw_exception(Value("TypeError: " + get_name() + " is not a constructor"));
        return Value();
    }

    // The hint is what the last object this constructor built ended up
    // needing, so the cell can be asked for room to hold it and the object
    // never reaches for a butterfly block of its own. Zero on the first
    // construction, which is where the hint is learned.
    uint32_t construct_slot_hint = get_construct_slot_hint();
    std::unique_ptr<Object> new_object;
    if (construct_slot_hint <= 4) {
        new_object = ObjectFactory::create_object_with_slots(4);
    } else {
        new_object = ObjectFactory::create_object();
        new_object->reserve_property_slots(construct_slot_hint);
    }
    Value this_value(new_object.get());

    Value constructor_prototype = this->constructor_prototype();
    // GetPrototypeFromConstructor: initial prototype comes from new.target, which may already differ from `this`.
    Value initial_proto = constructor_prototype;
    Value existing_new_target = ctx.get_new_target();
    if (!existing_new_target.is_undefined()) {
        Object* nt_obj = existing_new_target.is_function() ? static_cast<Object*>(existing_new_target.as_function())
                        : existing_new_target.is_object() ? existing_new_target.as_object() : nullptr;
        if (nt_obj && nt_obj != static_cast<Object*>(this)) {
            Value nt_proto = nt_obj->get_property("prototype");
            if (nt_proto.is_object() || nt_proto.is_function()) initial_proto = nt_proto;
        }
    }
    if (initial_proto.is_object() || initial_proto.is_function()) {
        Object* proto_obj = initial_proto.is_function()
            ? static_cast<Object*>(initial_proto.as_function())
            : initial_proto.as_object();
        // Allocated a few lines up, constructor body not run yet.
        new_object->initialize_prototype(proto_obj);
    }
    
    Function* super_constructor_fn = super_constructor();
    bool default_ctor = is_default_ctor();

    ctx.set_in_constructor_call(true);
    ctx.set_super_called(false);
    // Preserve new.target across the whole super-chain instead of stomping it with `this`.
    Value old_new_target = ctx.get_new_target();
    if (old_new_target.is_undefined()) {
        ctx.set_new_target(Value(static_cast<Object*>(this)));
    }

    // A synthesized default derived constructor is spec'd as `constructor(...args) { super(...args); }`, so auto-super must run before the constructor body (which here only contains field initializers) -- otherwise a super-chain override (e.g. a base constructor returning `new Proxy(this, ...)`) takes effect too late and fields get written to the object that's about to be discarded.
    if (default_ctor && super_constructor_fn) {
        Function* super_constructor = super_constructor_fn;
        Value super_result;
        if (super_constructor->is_native() || super_constructor->is_default_ctor()) {
            // Native built-ins need construct semantics; so does a default-ctor JS parent,
            // whose own implicit super(...args) only runs inside construct().
            super_result = super_constructor->construct(ctx, args);
        } else {
            super_result = super_constructor->call_register_args(ctx, args, this_value);
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
        const std::string& pm_slot = pm_brand_slot();
        if (!pm_slot.empty()) {
            Object* pm_this = this_value.is_object() ? this_value.as_object() : nullptr;
            if (pm_this) pm_this->add_private_field(pm_slot);
        }
        // Nested super construct() clears/overwrites these shared flags -- restore them.
        ctx.set_in_constructor_call(true);
        ctx.set_new_target(old_new_target);
    }

    ctx.set_last_super_override(nullptr);
    ctx.set_pending_construct_call(true);
    Value result = call_register_args(ctx, args, this_value);
    bool super_was_called = ctx.was_super_called();
    ctx.set_in_constructor_call(false);
    ctx.set_new_target(old_new_target);

    // Propagate any exception from the constructor body before checking super state
    if (ctx.has_exception()) return Value();

    // `extends null` is also derived: super() can never succeed there, so a
    // completed constructor still has an uninitialized this -> ReferenceError below.
    bool is_derived = is_derived_ctor();

    // TypeError for explicit non-object return must come before ReferenceError for missing super (spec 13c)
    if (is_derived && !result.is_undefined() && !result.is_object() && !result.is_function()) {
        ctx.throw_type_error("Derived constructors may only return object or undefined");
        return Value();
    }

    if (!result.is_object() && !result.is_function() && !super_was_called && is_derived) {
        ctx.throw_reference_error("Must call super constructor before accessing 'this' in derived class constructor");
        return Value();
    }

    // An explicit return from the constructor body wins; otherwise fall back to this_value, which the auto-super override above may have replaced.
    bool explicit_return = result.is_object() || result.is_function();
    Value final_result = explicit_return ? result : this_value;

    Object* super_override_obj = ctx.last_super_override();
    ctx.set_last_super_override(nullptr);

    // If construction resolved to an object or function other than the pre-allocated this, use that
    if ((final_result.is_object() || final_result.is_function()) && final_result.as_object() != new_object.get()) {
        Object* ret_obj = final_result.as_object();
        // A super-swapped `this` (auto-super this_value replacement, or the
        // identity recorded by call.cpp's super() handling) gets the subclass
        // prototype so `new Derived() instanceof Derived` works even when super
        // is a built-in that ignored new.target. An explicit `return obj` from
        // the constructor body is returned untouched (spec: NormalCompletion of
        // the returned object as-is).
        bool from_super_swap = !explicit_return || ret_obj == super_override_obj;
        if (from_super_swap && is_derived && constructor_prototype.is_object() &&
            ret_obj->get_type() != Object::ObjectType::Proxy) {
            ret_obj->set_prototype(constructor_prototype.as_object());
        }
        if (!ret_obj->get_prototype_raw() && constructor_prototype.is_object()) {
            ret_obj->set_prototype(constructor_prototype.as_object());
        }
        // A base-class constructor may have captured a raw pointer to new_object before returning this override (e.g. `new Proxy(this, ...)`), so release rather than let the unique_ptr delete it out from under them.
        new_object.release();
        return final_result;
    } else {
        learn_construct_slot_hint(new_object.get());
        return Value(new_object.release());
    }
}

// What this constructor's object ended up holding, so the next one it builds
// can be given a cell that already has room. Only ever raised: a constructor
// with a conditional property would otherwise flip the hint back and forth.
void Function::learn_construct_slot_hint(const Object* built) {
    if (!built) return;
    Shape* shape = built->get_shape();
    if (!shape) return;
    const uint32_t slots = shape->slot_count();
    if (slots > get_construct_slot_hint()) set_construct_slot_hint(slots);
}

std::string Function::to_string() const {
    // A well-known-symbol-named function's `name` is internally stored as "@@x" (e.g.
    // "@@asyncIterator"); NativeFunction syntax requires the spec's bracketed form instead.
    std::string display_name = get_name();
    if (display_name.size() > 2 && display_name[0] == '@' && display_name[1] == '@') {
        display_name = "[Symbol." + display_name.substr(2) + "]";
    }
    if (is_native_) {
        // A bound function's NativeFunction text carries no name. The spec
        // leaves the string implementation-defined, but real code compares it
        // against what every other engine prints, and ours was leaking the
        // "bound <target>" name that bind() puts on the function.
        if (has_internal_slot("__bound_target__")) {
            return "function () { [native code] }";
        }
        return "function " + display_name + "() { [native code] }";
    }
    const std::string& src = get_source_text();
    if (!src.empty()) {
        // Trim trailing whitespace -- source_text_ may include a trailing newline.
        std::string s = src;
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
                                             Context* closure_context,
                                             bool create_prototype) {
    auto func = std::make_unique<Function>(name, params, std::move(body), closure_context, create_prototype);
    // This factory's callers (class methods, top-level function
    // declarations, the native Function constructor) aren't analyzed for
    // closure_needs_outer_environment -- preserve the old unconditional
    // pin. FunctionExpression::evaluate's own hot path calls the Function
    // constructors directly instead of through here, so it can decide.
    func->mark_closure_environment_escaped();
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        // Freshly made here and not handed to JS yet.
        func->initialize_prototype(func_proto);
    } else {
        // If function_prototype not set yet, delay prototype assignment
        // It will be set when the function is accessed
    }
    return func;
}

std::unique_ptr<Function> create_js_function(const std::string& name,
                                             std::vector<std::unique_ptr<Parameter>> params,
                                             std::unique_ptr<ASTNode> body,
                                             Context* closure_context,
                                             bool create_prototype) {
    auto func = std::make_unique<Function>(name, std::move(params), std::move(body), closure_context, create_prototype);
    // Same as the other overload above: this factory's callers aren't
    // analyzed, preserve the old unconditional pin.
    func->mark_closure_environment_escaped();
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        // Freshly made here and not handed to JS yet.
        func->initialize_prototype(func_proto);
    }
    return func;
}

std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, std::span<const Value>, Value)> fn) {
    auto func = std::make_unique<Function>(name, fn, false);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        // Freshly made here and not handed to JS yet.
        func->initialize_prototype(func_proto);
    }
    return func;
}

std::unique_ptr<Function> create_native_function(const std::string& name,
                                                 std::function<Value(Context&, std::span<const Value>, Value)> fn,
                                                 uint32_t arity) {
    auto func = std::make_unique<Function>(name, fn, arity, false);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        // Freshly made here and not handed to JS yet.
        func->initialize_prototype(func_proto);
    }
    return func;
}

std::unique_ptr<Function> create_native_constructor(const std::string& name,
                                                    std::function<Value(Context&, std::span<const Value>, Value)> fn,
                                                    uint32_t arity) {
    auto func = std::make_unique<Function>(name, fn, arity, true);
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        // Freshly made here and not handed to JS yet.
        func->initialize_prototype(func_proto);
    }
    return func;
}

}

void Function::scan_for_var_declarations(ASTNode* node, Context& ctx, Environment* param_env) {
    if (!node) return;

    if (node->get_type() == ASTNode::Type::VARIABLE_DECLARATION) {
        VariableDeclaration* var_decl = static_cast<VariableDeclaration*>(node);

        for (const auto& declarator : var_decl->get_declarations()) {
            if (declarator->get_kind() == VariableDeclarator::Kind::VAR) {
                const std::string& name = declarator->get_id()->get_name();

                auto* var_env = ctx.get_variable_environment();
                if (!var_env || !var_env->has_own_binding(name)) {
                    // Non-simple parameters put the body's vars in an
                    // environment of their own, so a var repeating a parameter
                    // name no longer finds it and used to start at undefined,
                    // dropping the argument. Spec seeds it from the parameter
                    // instead. Only the parameter scope is consulted: asking
                    // the whole chain would pick up an outer variable that has
                    // nothing to do with this function.
                    Value initial;
                    if (param_env && param_env->has_own_binding(name)) {
                        initial = param_env->get_binding_direct(name, &ctx);
                    }
                    ctx.create_var_binding(name, initial, true);
                }
            }
        }
    }

    if (node->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        BlockStatement* block = static_cast<BlockStatement*>(node);
        for (const auto& stmt : block->get_statements()) {
            scan_for_var_declarations(stmt.get(), ctx, param_env);
        }
    }
    else if (node->get_type() == ASTNode::Type::IF_STATEMENT) {
        IfStatement* if_stmt = static_cast<IfStatement*>(node);
        scan_for_var_declarations(if_stmt->get_consequent(), ctx, param_env);
        if (if_stmt->get_alternate()) {
            scan_for_var_declarations(if_stmt->get_alternate(), ctx, param_env);
        }
    }
    else if (node->get_type() == ASTNode::Type::FOR_STATEMENT) {
        ForStatement* for_stmt = static_cast<ForStatement*>(node);
        if (for_stmt->get_init()) {
            scan_for_var_declarations(for_stmt->get_init(), ctx, param_env);
        }
        scan_for_var_declarations(for_stmt->get_body(), ctx, param_env);
    }
    else if (node->get_type() == ASTNode::Type::WHILE_STATEMENT) {
        WhileStatement* while_stmt = static_cast<WhileStatement*>(node);
        scan_for_var_declarations(while_stmt->get_body(), ctx, param_env);
    }
    else if (node->get_type() == ASTNode::Type::DO_WHILE_STATEMENT) {
        DoWhileStatement* do_stmt = static_cast<DoWhileStatement*>(node);
        scan_for_var_declarations(do_stmt->get_body(), ctx, param_env);
    }
    else if (node->get_type() == ASTNode::Type::WITH_STATEMENT) {
        WithStatement* with_stmt = static_cast<WithStatement*>(node);
        scan_for_var_declarations(with_stmt->get_body(), ctx, param_env);
    }
    else if (node->get_type() == ASTNode::Type::TRY_STATEMENT) {
        TryStatement* try_stmt = static_cast<TryStatement*>(node);
        scan_for_var_declarations(try_stmt->get_try_block(), ctx, param_env);
        if (try_stmt->get_catch_clause()) scan_for_var_declarations(try_stmt->get_catch_clause(), ctx, param_env);
        if (try_stmt->get_finally_block()) scan_for_var_declarations(try_stmt->get_finally_block(), ctx, param_env);
    }
    else if (node->get_type() == ASTNode::Type::SWITCH_STATEMENT) {
        SwitchStatement* sw = static_cast<SwitchStatement*>(node);
        for (const auto& c : sw->get_cases()) {
            for (const auto& s : static_cast<CaseClause*>(c.get())->get_consequent()) {
                scan_for_var_declarations(s.get(), ctx, param_env);
            }
        }
    }
    else if (node->get_type() == ASTNode::Type::LABELED_STATEMENT) {
        LabeledStatement* lbl = static_cast<LabeledStatement*>(node);
        scan_for_var_declarations(lbl->get_statement(), ctx, param_env);
    }
    else if (node->get_type() == ASTNode::Type::FOR_IN_STATEMENT) {
        ForInStatement* forin = static_cast<ForInStatement*>(node);
        if (forin->get_left()) scan_for_var_declarations(forin->get_left(), ctx, param_env);
        scan_for_var_declarations(forin->get_body(), ctx, param_env);
    }
    else if (node->get_type() == ASTNode::Type::FOR_OF_STATEMENT) {
        ForOfStatement* forof = static_cast<ForOfStatement*>(node);
        if (forof->get_left()) scan_for_var_declarations(forof->get_left(), ctx, param_env);
        scan_for_var_declarations(forof->get_body(), ctx, param_env);
    }
}

}
