/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/PromiseBuiltin.h"
#include "quanta/core/engine/builtins/ObjectBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/ProxyReflect.h"

namespace Quanta {

struct PromiseCapabilityResult {
    Value promise;
    Function* resolve = nullptr;
    Function* reject = nullptr;
};

// 25.4.1.5 NewPromiseCapability ( C )
static bool new_promise_capability(Context& ctx, Value C_val, PromiseCapabilityResult& out) {
    if (!C_val.is_function()) {
        ctx.throw_type_error("PromiseCapability: not a constructor");
        return false;
    }
    Function* C = C_val.as_function();

    struct CapState { Value resolve_val; Value reject_val; Function* resolve_fn = nullptr; Function* reject_fn = nullptr; };
    auto state = std::make_shared<CapState>();

    auto executor = ObjectFactory::create_native_function("",
        [state](Context& ctx, const std::vector<Value>& args) -> Value {
            Value res_arg = args.empty() ? Value() : args[0];
            Value rej_arg = args.size() > 1 ? args[1] : Value();
            if (!state->resolve_val.is_undefined()) {
                ctx.throw_type_error("Promise resolve function already set");
                return Value();
            }
            if (!state->reject_val.is_undefined()) {
                ctx.throw_type_error("Promise reject function already set");
                return Value();
            }
            state->resolve_val = res_arg;
            state->reject_val = rej_arg;
            if (res_arg.is_function()) state->resolve_fn = res_arg.as_function();
            if (rej_arg.is_function()) state->reject_fn = rej_arg.as_function();
            return Value();
        }, 2);

    Value executor_val(executor.release());
    std::vector<Value> ctor_args = { executor_val };
    // construct() only resets new.target when it was undefined (correct for super() chaining);
    // calling it directly here, unlike a literal `new C()`, must reset it itself or it leaks
    // an unrelated outer constructor's new.target (e.g. .then() called from inside one).
    Value old_new_target = ctx.get_new_target();
    ctx.set_new_target(C_val);
    Value promise_val = C->construct(ctx, ctor_args);
    ctx.set_new_target(old_new_target);
    if (ctx.has_exception()) return false;

    if (!state->resolve_fn || !state->reject_fn) {
        ctx.throw_type_error("Promise capability functions not callable");
        return false;
    }

    out.promise = promise_val;
    out.resolve = state->resolve_fn;
    out.reject = state->reject_fn;
    return true;
}

// Reject a promise capability with the current pending exception (ctx must have one set).
static void reject_capability_with_exception(Context& ctx, PromiseCapabilityResult& cap) {
    Value exc = ctx.get_exception();
    ctx.clear_exception();
    std::vector<Value> a = { exc };
    cap.reject->call(ctx, a);
}

static void reject_capability_type_error(Context& ctx, PromiseCapabilityResult& cap, const std::string& msg) {
    ctx.throw_type_error(msg);
    reject_capability_with_exception(ctx, cap);
}

static Object* as_object_or_function(const Value& v) {
    if (v.is_function()) return static_cast<Object*>(v.as_function());
    if (v.is_object()) return v.as_object();
    return nullptr;
}

// [[OwnPropertyKeys]] / [[GetOwnProperty]] that respect a Proxy's traps (the plain Object
// virtuals don't dispatch to Proxy traps at all -- see ownKeys/getOwnPropertyDescriptor tests).
static std::vector<std::string> own_keys_for(Object* obj) {
    Proxy* px = dynamic_cast<Proxy*>(obj);
    return px ? px->own_keys_trap() : obj->get_own_property_keys();
}
static PropertyDescriptor own_property_descriptor_for(Object* obj, const std::string& key) {
    Proxy* px = dynamic_cast<Proxy*>(obj);
    return px ? px->get_own_property_descriptor_trap(Value(key)) : obj->get_property_descriptor(key);
}

// 7.3.22 SpeciesConstructor ( O, defaultConstructor )
static Function* species_constructor(Context& ctx, Object* O, Function* default_ctor) {
    Value c = O->get_property("constructor");
    if (ctx.has_exception()) return nullptr;
    if (c.is_undefined()) return default_ctor;
    Object* c_obj = as_object_or_function(c);
    if (!c_obj) { ctx.throw_type_error("constructor is not an object"); return nullptr; }
    Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
    Value s = species_sym ? c_obj->get_property(species_sym->to_property_key()) : Value();
    if (ctx.has_exception()) return nullptr;
    if (s.is_null() || s.is_undefined()) return default_ctor;
    if (s.is_function() && s.as_function()->is_constructor()) return s.as_function();
    ctx.throw_type_error("Species constructor is not a constructor");
    return nullptr;
}

// An iterator must be consumed lazily (one IteratorStep at a time, interleaved with per-element
// processing) rather than drained upfront: a `then`/`resolve` failure partway through must close
// the iterator instead of continuing to pull from it, and a never-`done` iterator must not hang
// the whole engine collecting an unbounded array before the caller gets a chance to bail out.
struct IteratorRecord {
    Object* iterator = nullptr;
    Function* next_fn = nullptr;
    bool done = false;
};

static bool get_iterator_record(Context& ctx, const Value& iterable_val, IteratorRecord& rec) {
    // GetIterator has no "is iterable an object" precheck: a string, number, etc. is boxed
    // for the @@iterator property lookup (e.g. '' has Symbol.iterator via String.prototype).
    // Only null/undefined fail (ToObject would throw). No array fast path either: an array's
    // own/inherited @@iterator can be overridden or poisoned, and skipping the lookup would
    // mask that.
    Object* iterable = as_object_or_function(iterable_val);
    if (!iterable) {
        if (iterable_val.is_null() || iterable_val.is_undefined()) {
            ctx.throw_type_error("Cannot convert undefined or null to object");
            return false;
        }
        iterable = to_object_or_throw(ctx, iterable_val);
        if (!iterable) return false;
    }
    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
    if (!iter_sym) { ctx.throw_type_error("Symbol.iterator unavailable"); return false; }
    Value iter_method = iterable->get_property(iter_sym->to_property_key());
    if (ctx.has_exception()) return false;
    if (!iter_method.is_function()) { ctx.throw_type_error("value is not iterable"); return false; }
    Value iter_obj = iter_method.as_function()->call(ctx, {}, iterable_val);
    if (ctx.has_exception()) return false;
    if (!iter_obj.is_object()) { ctx.throw_type_error("iterator result is not an object"); return false; }
    Value next_fn = iter_obj.as_object()->get_property("next");
    if (ctx.has_exception()) return false;
    if (!next_fn.is_function()) { ctx.throw_type_error("iterator.next is not a function"); return false; }
    rec.iterator = iter_obj.as_object();
    rec.next_fn = next_fn.as_function();
    return true;
}

enum class IterStepStatus { DONE, VALUE, ABRUPT };

// On ABRUPT, ctx is left with the pending exception and rec.done is set (so the caller must
// NOT call iterator_close -- IteratorClose only applies while [[Done]] is still false).
static IterStepStatus iterator_step(Context& ctx, IteratorRecord& rec, Value& out_value) {
    Value res = rec.next_fn->call(ctx, {}, Value(rec.iterator));
    if (ctx.has_exception()) { rec.done = true; return IterStepStatus::ABRUPT; }
    if (!res.is_object()) { ctx.throw_type_error("iterator result is not an object"); rec.done = true; return IterStepStatus::ABRUPT; }
    Value done_val = res.as_object()->get_property("done");
    if (ctx.has_exception()) { rec.done = true; return IterStepStatus::ABRUPT; }
    if (done_val.to_boolean()) { rec.done = true; return IterStepStatus::DONE; }
    out_value = res.as_object()->get_property("value");
    if (ctx.has_exception()) { rec.done = true; return IterStepStatus::ABRUPT; }
    return IterStepStatus::VALUE;
}

// Calls iterator.return() (swallowing any error from it -- the abrupt completion already in
// flight takes precedence) unless the iterator already reported [[Done]].
static void iterator_close(Context& ctx, IteratorRecord& rec) {
    if (rec.done || !rec.iterator) return;
    rec.done = true;
    // Callers always invoke this with an abrupt completion already pending (the reason for
    // closing in the first place) -- stash it so probing/calling .return() can't clobber it,
    // and restore it once the close attempt (whose own errors are spec-discarded) is done.
    bool had_exception = ctx.has_exception();
    Value saved_exc;
    if (had_exception) { saved_exc = ctx.get_exception(); ctx.clear_exception(); }
    Value return_fn = rec.iterator->get_property("return");
    if (!ctx.has_exception() && return_fn.is_function()) {
        return_fn.as_function()->call(ctx, {}, Value(rec.iterator));
    }
    ctx.clear_exception();
    if (had_exception) ctx.throw_exception(saved_exc, true);
}

void register_promise_builtins(Context& ctx) {
    auto promise_constructor = ObjectFactory::create_native_constructor("Promise",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (!ctx.is_in_constructor_call()) {
                ctx.throw_type_error("Promise constructor cannot be invoked without 'new'");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Promise executor must be a function");
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
                if (ctx.has_exception()) return Value();
                if (!proto.is_object()) {
                    Value promise_ctor = ctx.get_binding("Promise");
                    if (promise_ctor.is_function())
                        proto = static_cast<Object*>(promise_ctor.as_function())->get_property("prototype");
                }
                if (proto.is_object()) promise->set_prototype(proto.as_object());
            }

            Function* executor = args[0].as_function();

            // AlreadyResolved flag, shared by resolve_fn/reject_fn: a resolve() call that
            // chains through a thenable leaves the promise pending, so a later reject()
            // must still be a no-op even though promise->is_pending() would say otherwise.
            auto already_called = std::make_shared<bool>(false);

            auto resolve_fn = ObjectFactory::create_native_function("",
                [promise_ptr = promise.get(), already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (*already_called) return Value();
                    *already_called = true;
                    Value value = args.empty() ? Value() : args[0];
                    promise_ptr->fulfill(value);
                    return Value();
                }, 1);

            auto reject_fn = ObjectFactory::create_native_function("",
                [promise_ptr = promise.get(), already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (*already_called) return Value();
                    *already_called = true;
                    Value reason = args.empty() ? Value() : args[0];
                    promise_ptr->reject(reason);
                    return Value();
                }, 1);
            
            Function* reject_fn_raw = reject_fn.get();
            std::vector<Value> executor_args = {
                Value(resolve_fn.release()),
                Value(reject_fn.release())
            };

            executor->call(ctx, executor_args);
            if (ctx.has_exception()) {
                Value err = ctx.get_exception();
                ctx.clear_exception();
                std::vector<Value> rej_args = { err };
                reject_fn_raw->call(ctx, rej_args);
                ctx.clear_exception();
            }

            return Value(promise.release());
        });
    
    auto promise_try = ObjectFactory::create_native_function("try",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value c_val = ctx.get_binding("this");
            if (!c_val.is_object() && !c_val.is_function()) {
                ctx.throw_type_error("Promise.try called on non-object");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("Promise.try requires a function");
                return Value();
            }
            Function* fn = args[0].as_function();
            std::vector<Value> fwd_args(args.begin() + 1, args.end());

            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, c_val, cap)) return Value();

            Value result = fn->call(ctx, fwd_args);
            if (ctx.has_exception()) {
                Value err = ctx.get_exception();
                ctx.clear_exception();
                std::vector<Value> rej_args = { err };
                cap.reject->call(ctx, rej_args);
                return cap.promise;
            }
            std::vector<Value> res_args = { result };
            cap.resolve->call(ctx, res_args);
            if (ctx.has_exception()) return Value();
            return cap.promise;
        }, 1);
    promise_constructor->set_property("try", Value(promise_try.release()), PropertyAttributes::BuiltinFunction);
    
    auto promise_withResolvers = ObjectFactory::create_native_function("withResolvers",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Value c_val = ctx.get_binding("this");
            if (!c_val.is_object() && !c_val.is_function()) {
                ctx.throw_type_error("Promise.withResolvers called on non-object");
                return Value();
            }
            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, c_val, cap)) return Value();

            auto result_obj = ObjectFactory::create_object();
            result_obj->set_property("promise", cap.promise);
            result_obj->set_property("resolve", Value(cap.resolve));
            result_obj->set_property("reject", Value(cap.reject));

            return Value(result_obj.release());
        });
    promise_constructor->set_property("withResolvers", Value(promise_withResolvers.release()), PropertyAttributes::BuiltinFunction);
    
    auto promise_prototype = ObjectFactory::create_object();
    
    auto promise_then = ObjectFactory::create_native_function("then",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* this_obj = ctx.get_this_binding();
            if (!this_obj) {
                ctx.throw_type_error("Promise.prototype.then called on non-object");
                return Value();
            }

            Promise* promise = dynamic_cast<Promise*>(this_obj);
            if (!promise) {
                ctx.throw_type_error("Promise.prototype.then called on non-Promise");
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

            // Spec: C = SpeciesConstructor(promise, %Promise%); resultCapability =
            // NewPromiseCapability(C); PerformPromiseThen(...). The capability's promise --
            // genuinely constructed via `new C(executor)` -- is what gets returned, not a
            // bare native Promise with its prototype patched after the fact; that's required
            // for subclasses to see their own constructor actually invoked, and for a
            // constructor that returns a substitute object to work at all.
            Value default_ctor_val = ctx.get_binding("Promise");
            if (!default_ctor_val.is_function()) { ctx.throw_type_error("Promise unavailable"); return Value(); }
            Function* species_ctor = species_constructor(ctx, this_obj, default_ctor_val.as_function());
            if (ctx.has_exception()) return Value();

            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, Value(species_ctor), cap)) return Value();
            Function* cap_resolve = cap.resolve;
            Function* cap_reject = cap.reject;

            // PerformPromiseThen's "Identity"/"Thrower" defaults: with no real handler,
            // forward the value/reason to the capability unchanged rather than skipping it.
            auto ful_wrapper = ObjectFactory::create_native_function("",
                [on_fulfilled, cap_resolve, cap_reject](Context& ctx, const std::vector<Value>& args) -> Value {
                    Value val = args.empty() ? Value() : args[0];
                    if (!on_fulfilled) {
                        std::vector<Value> ra = { val };
                        cap_resolve->call(ctx, ra);
                        return Value();
                    }
                    Value result = on_fulfilled->call(ctx, { val });
                    if (ctx.has_exception()) {
                        Value exc = ctx.get_exception(); ctx.clear_exception();
                        std::vector<Value> rja = { exc };
                        cap_reject->call(ctx, rja);
                    } else {
                        std::vector<Value> ra = { result };
                        cap_resolve->call(ctx, ra);
                    }
                    return Value();
                }, 1);
            auto rej_wrapper = ObjectFactory::create_native_function("",
                [on_rejected, cap_resolve, cap_reject](Context& ctx, const std::vector<Value>& args) -> Value {
                    Value reason = args.empty() ? Value() : args[0];
                    if (!on_rejected) {
                        std::vector<Value> rja = { reason };
                        cap_reject->call(ctx, rja);
                        return Value();
                    }
                    Value result = on_rejected->call(ctx, { reason });
                    if (ctx.has_exception()) {
                        Value exc = ctx.get_exception(); ctx.clear_exception();
                        std::vector<Value> rja = { exc };
                        cap_reject->call(ctx, rja);
                    } else {
                        std::vector<Value> ra = { result };
                        cap_resolve->call(ctx, ra);
                    }
                    return Value();
                }, 1);

            // The native child this creates is intentionally discarded -- cap.promise (built
            // via the species constructor above) is the promise actually returned to JS;
            // Promise::then's own pinning of these wrapper functions onto `promise` keeps
            // them alive until they fire, same as any other .then() call.
            promise->then(ful_wrapper.release(), rej_wrapper.release());

            return cap.promise;
        }, 2);
    promise_prototype->set_property("then", Value(promise_then.release()), PropertyAttributes::BuiltinFunction);

    auto promise_catch = ObjectFactory::create_native_function("catch",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Spec: Return ? Invoke(promise, "then", «undefined, onRejected»). Duck-typed: works
            // on any object-coercible `this`, not just real Promise instances.
            Value this_val = ctx.get_binding("this");
            Object* this_obj = to_object_or_throw(ctx, this_val);
            if (!this_obj) return Value();

            Value then_val = this_obj->get_property("then");
            if (ctx.has_exception()) return Value();
            if (!then_val.is_function()) {
                ctx.throw_type_error("Promise.prototype.catch: then is not a function");
                return Value();
            }
            Value on_rejected = args.empty() ? Value() : args[0];
            std::vector<Value> then_args = { Value(), on_rejected };
            Value receiver = this_val.is_object() || this_val.is_function() ? this_val : Value(this_obj);
            return then_val.as_function()->call(ctx, then_args, receiver);
        }, 1);
    promise_prototype->set_property("catch", Value(promise_catch.release()), PropertyAttributes::BuiltinFunction);
    
    auto promise_finally = ObjectFactory::create_native_function("finally",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value this_val = ctx.get_binding("this");
            if (!this_val.is_object() && !this_val.is_function()) {
                ctx.throw_type_error("Promise.prototype.finally called on non-object");
                return Value();
            }
            Object* this_obj = this_val.is_function() ? static_cast<Object*>(this_val.as_function()) : this_val.as_object();

            Function* on_finally = nullptr;
            if (!args.empty() && args[0].is_function()) {
                on_finally = args[0].as_function();
            }

            Value then_val = this_obj->get_property("then");
            if (ctx.has_exception()) return Value();
            if (!then_val.is_function()) {
                ctx.throw_type_error("Promise.prototype.finally: then is not a function");
                return Value();
            }

            if (!on_finally) {
                // Spec: when onFinally isn't callable, thenFinally/catchFinally are onFinally itself.
                Value non_callable = args.empty() ? Value() : args[0];
                std::vector<Value> then_args = { non_callable, non_callable };
                return then_val.as_function()->call(ctx, then_args, this_val);
            }
            // Spec: C = SpeciesConstructor(promise, %Promise%); result = onFinally();
            // promise = PromiseResolve(C, result); return Invoke(promise, "then", «continuation»).
            // Must observably call .then on the wrapped promise (not just inspect its internal
            // state) so an overridden .then on `result` itself still runs, and must use the
            // species constructor (not bare global Promise) so PromiseResolve's "already the
            // right type" fast path matches subclass instances.
            Value default_ctor_val = ctx.get_binding("Promise");
            if (!default_ctor_val.is_function()) { ctx.throw_type_error("Promise unavailable"); return Value(); }
            Function* species_ctor = species_constructor(ctx, this_obj, default_ctor_val.as_function());
            if (ctx.has_exception()) return Value();
            auto resolve_then_continue = [species_ctor](Context& ctx, Value result, Function* continuation) -> Value {
                Value resolve_method = species_ctor->get_property("resolve");
                if (ctx.has_exception()) return Value();
                if (!resolve_method.is_function()) { ctx.throw_type_error("Promise.resolve is not callable"); return Value(); }
                Value wrapped = resolve_method.as_function()->call(ctx, {result}, Value(species_ctor));
                if (ctx.has_exception()) return Value();
                Object* wrapped_obj = as_object_or_function(wrapped);
                if (!wrapped_obj) { ctx.throw_type_error("PromiseResolve did not return an object"); return Value(); }
                Value then_m = wrapped_obj->get_property("then");
                if (ctx.has_exception()) return Value();
                if (!then_m.is_function()) { ctx.throw_type_error("then is not a function"); return Value(); }
                std::vector<Value> ta = { Value(continuation) };
                return then_m.as_function()->call(ctx, ta, wrapped);
            };
            auto then_wrapper = ObjectFactory::create_native_function("",
                [on_finally, resolve_then_continue](Context& ctx, const std::vector<Value>& args) -> Value {
                    Value original_val = args.empty() ? Value() : args[0];
                    Value result = on_finally->call(ctx, {});
                    if (ctx.has_exception()) return Value();
                    auto value_thunk = ObjectFactory::create_native_function("",
                        [original_val](Context&, const std::vector<Value>&) -> Value { return original_val; }, 0);
                    return resolve_then_continue(ctx, result, value_thunk.release());
                }, 1);
            auto catch_wrapper = ObjectFactory::create_native_function("",
                [on_finally, resolve_then_continue](Context& ctx, const std::vector<Value>& args) -> Value {
                    Value original_reason = args.empty() ? Value() : args[0];
                    Value result = on_finally->call(ctx, {});
                    if (ctx.has_exception()) return Value();
                    auto thrower = ObjectFactory::create_native_function("",
                        [original_reason](Context& ctx, const std::vector<Value>&) -> Value {
                            ctx.throw_exception(original_reason, true);
                            return Value();
                        }, 0);
                    return resolve_then_continue(ctx, result, thrower.release());
                }, 1);
            std::vector<Value> then_args = { Value(then_wrapper.release()), Value(catch_wrapper.release()) };
            return then_val.as_function()->call(ctx, then_args, this_val);
        }, 1);
    promise_prototype->set_property("finally", Value(promise_finally.release()), PropertyAttributes::BuiltinFunction);

    PropertyDescriptor promise_tag_desc(Value(std::string("Promise")), PropertyAttributes::Configurable);
    promise_prototype->set_property_descriptor("Symbol.toStringTag", promise_tag_desc);

    // ES6: Promise.prototype.constructor = Promise
    promise_prototype->set_property("constructor", Value(promise_constructor.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    promise_constructor->set_property("prototype", Value(promise_prototype.release()));
    
    auto promise_resolve_static = ObjectFactory::create_native_function("resolve",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value value = args.empty() ? Value() : args[0];
            Value c_val = ctx.get_binding("this");
            if (!c_val.is_object() && !c_val.is_function()) {
                ctx.throw_type_error("Promise.resolve called on non-object");
                return Value();
            }
            if (value.is_object()) {
                Promise* p = dynamic_cast<Promise*>(value.as_object());
                if (p) {
                    Value xctor = value.as_object()->get_property("constructor");
                    if (xctor.is_function() && c_val.is_function() && xctor.as_function() == c_val.as_function()) {
                        return value;
                    }
                }
            }
            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, c_val, cap)) return Value();
            std::vector<Value> resolve_args = { value };
            cap.resolve->call(ctx, resolve_args);
            if (ctx.has_exception()) return Value();
            return cap.promise;
        }, 1);
    promise_constructor->set_property("resolve", Value(promise_resolve_static.release()), PropertyAttributes::BuiltinFunction);

    auto promise_reject_static = ObjectFactory::create_native_function("reject",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value reason = args.empty() ? Value() : args[0];
            Value c_val = ctx.get_binding("this");
            if (!c_val.is_object() && !c_val.is_function()) {
                ctx.throw_type_error("Promise.reject called on non-object");
                return Value();
            }
            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, c_val, cap)) return Value();
            std::vector<Value> reject_args = { reason };
            cap.reject->call(ctx, reject_args);
            if (ctx.has_exception()) return Value();
            return cap.promise;
        }, 1);
    promise_constructor->set_property("reject", Value(promise_reject_static.release()), PropertyAttributes::BuiltinFunction);

    auto promise_all_static = ObjectFactory::create_native_function("all",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value raw_this = ctx.get_binding("this");
            if (!raw_this.is_object() && !raw_this.is_function()) { ctx.throw_type_error("Promise.all called on non-object"); return Value(); }

            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, raw_this, cap)) return Value();
            Function* this_ctor = raw_this.as_function();

            // Spec: GetPromiseResolve(C) happens before GetIterator(iterable).
            Value resolve_method = this_ctor->get_property("resolve");
            if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            if (!resolve_method.is_function()) {
                reject_capability_type_error(ctx, cap, "Promise.resolve is not callable");
                return cap.promise;
            }
            Function* resolve_fn = resolve_method.as_function();

            IteratorRecord rec;
            if (!get_iterator_record(ctx, args.empty() ? Value() : args[0], rec)) {
                reject_capability_with_exception(ctx, cap);
                return cap.promise;
            }

            struct AllState {
                std::vector<Value> results;
                uint32_t remaining;
            };
            auto state = std::make_shared<AllState>();
            state->remaining = 1; // spec trick: avoids resolving before the loop finishes if elements settle synchronously
            Function* cap_resolve = cap.resolve;
            Function* cap_reject = cap.reject;

            Object* pin_target = as_object_or_function(cap.promise);
            if (!pin_target) pin_target = ctx.get_global_object();

            uint32_t idx = 0;
            while (true) {
                Value element;
                IterStepStatus step = iterator_step(ctx, rec, element);
                if (step == IterStepStatus::ABRUPT) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (step == IterStepStatus::DONE) {
                    if (--state->remaining == 0) {
                        auto arr = ObjectFactory::create_array(static_cast<uint32_t>(state->results.size()));
                        for (size_t j = 0; j < state->results.size(); j++) arr->set_element(static_cast<uint32_t>(j), state->results[j]);
                        std::vector<Value> ra = { Value(arr.release()) };
                        cap_resolve->call(ctx, ra);
                        if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); }
                    }
                    return cap.promise;
                }

                std::vector<Value> resolve_args = { element };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    iterator_close(ctx, rec);
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                state->results.push_back(Value());
                state->remaining++;
                uint32_t this_idx = idx;
                auto already_called = std::make_shared<bool>(false);
                auto on_ful = ObjectFactory::create_native_function("",
                    [this_idx, state, cap_resolve, cap_reject, already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        Value val = args.empty() ? Value() : args[0];
                        state->results[this_idx] = val;
                        if (--state->remaining == 0) {
                            auto arr = ObjectFactory::create_array(static_cast<uint32_t>(state->results.size()));
                            for (size_t j = 0; j < state->results.size(); j++) arr->set_element(static_cast<uint32_t>(j), state->results[j]);
                            std::vector<Value> ra = { Value(arr.release()) };
                            cap_resolve->call(ctx, ra);
                            if (ctx.has_exception()) {
                                Value exc = ctx.get_exception(); ctx.clear_exception();
                                std::vector<Value> rja = { exc };
                                cap_reject->call(ctx, rja);
                            }
                        }
                        return Value();
                    }, 1);

                Function* ful_fn = on_ful.release();
                // Keep handler alive (next_promise may be a non-Promise thenable with no GC pin of its own).
                pin_target->set_property("__all_ful_" + std::to_string(idx) + "__", Value(ful_fn));

                Value then_method = next_promise_obj->get_property("then");
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!then_method.is_function()) {
                    iterator_close(ctx, rec);
                    reject_capability_type_error(ctx, cap, "then is not a function");
                    return cap.promise;
                }
                std::vector<Value> then_args = { Value(ful_fn), Value(cap.reject) };
                then_method.as_function()->call(ctx, then_args, next_promise);
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }

                idx++;
            }
        }, 1);
    promise_constructor->set_property("all", Value(promise_all_static.release()), PropertyAttributes::BuiltinFunction);

    auto promise_race_static = ObjectFactory::create_native_function("race",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value raw_this = ctx.get_binding("this");
            if (!raw_this.is_object() && !raw_this.is_function()) { ctx.throw_type_error("Promise.race called on non-object"); return Value(); }

            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, raw_this, cap)) return Value();
            Function* this_ctor = raw_this.as_function();

            Value resolve_method = this_ctor->get_property("resolve");
            if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            if (!resolve_method.is_function()) {
                reject_capability_type_error(ctx, cap, "Promise.resolve is not callable");
                return cap.promise;
            }
            Function* resolve_fn = resolve_method.as_function();

            IteratorRecord rec;
            if (!get_iterator_record(ctx, args.empty() ? Value() : args[0], rec)) {
                reject_capability_with_exception(ctx, cap);
                return cap.promise;
            }

            // First settled promise wins (cap.resolve/cap.reject already enforce "AlreadyResolved"
            // semantics, so the same pair is safely reused for every element).
            while (true) {
                Value element;
                IterStepStatus step = iterator_step(ctx, rec, element);
                if (step == IterStepStatus::ABRUPT) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (step == IterStepStatus::DONE) return cap.promise;

                std::vector<Value> resolve_args = { element };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    iterator_close(ctx, rec);
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                Value then_method = next_promise_obj->get_property("then");
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!then_method.is_function()) {
                    iterator_close(ctx, rec);
                    reject_capability_type_error(ctx, cap, "then is not a function");
                    return cap.promise;
                }
                std::vector<Value> then_args = { Value(cap.resolve), Value(cap.reject) };
                then_method.as_function()->call(ctx, then_args, next_promise);
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }
            }
        }, 1);
    promise_constructor->set_property("race", Value(promise_race_static.release()), PropertyAttributes::BuiltinFunction);

    auto promise_allSettled_static = ObjectFactory::create_native_function("allSettled",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value raw_this = ctx.get_binding("this");
            if (!raw_this.is_object() && !raw_this.is_function()) { ctx.throw_type_error("Promise.allSettled called on non-object"); return Value(); }

            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, raw_this, cap)) return Value();
            Function* this_ctor = raw_this.as_function();

            Value resolve_method = this_ctor->get_property("resolve");
            if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            if (!resolve_method.is_function()) {
                reject_capability_type_error(ctx, cap, "Promise.resolve is not callable");
                return cap.promise;
            }
            Function* resolve_fn = resolve_method.as_function();

            IteratorRecord rec;
            if (!get_iterator_record(ctx, args.empty() ? Value() : args[0], rec)) {
                reject_capability_with_exception(ctx, cap);
                return cap.promise;
            }

            struct SettledState {
                std::vector<Value> results;
                uint32_t remaining;
            };
            auto state = std::make_shared<SettledState>();
            state->remaining = 1; // spec trick: avoids resolving before the loop finishes if elements settle synchronously
            Function* cap_resolve = cap.resolve;
            Function* cap_reject = cap.reject;

            Object* pin_target = as_object_or_function(cap.promise);
            if (!pin_target) pin_target = ctx.get_global_object();

            uint32_t idx = 0;
            while (true) {
                Value element;
                IterStepStatus step = iterator_step(ctx, rec, element);
                if (step == IterStepStatus::ABRUPT) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (step == IterStepStatus::DONE) {
                    if (--state->remaining == 0) {
                        auto arr = ObjectFactory::create_array(static_cast<uint32_t>(state->results.size()));
                        for (size_t j = 0; j < state->results.size(); j++) arr->set_element(static_cast<uint32_t>(j), state->results[j]);
                        std::vector<Value> ra = { Value(arr.release()) };
                        cap_resolve->call(ctx, ra);
                        if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); }
                    }
                    return cap.promise;
                }

                std::vector<Value> resolve_args = { element };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    iterator_close(ctx, rec);
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                state->results.push_back(Value());
                state->remaining++;
                uint32_t this_idx = idx;
                // Spec: resolveElement and rejectElement for the same index share one AlreadyCalled flag.
                auto already_called = std::make_shared<bool>(false);

                auto on_ful = ObjectFactory::create_native_function("",
                    [this_idx, state, cap_resolve, cap_reject, already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        Value val = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("fulfilled")));
                        settled->set_property("value", val);
                        state->results[this_idx] = Value(settled.release());
                        if (--state->remaining == 0) {
                            auto arr = ObjectFactory::create_array(static_cast<uint32_t>(state->results.size()));
                            for (size_t j = 0; j < state->results.size(); j++) arr->set_element(static_cast<uint32_t>(j), state->results[j]);
                            std::vector<Value> ra = { Value(arr.release()) };
                            cap_resolve->call(ctx, ra);
                            if (ctx.has_exception()) {
                                Value exc = ctx.get_exception(); ctx.clear_exception();
                                std::vector<Value> rja = { exc };
                                cap_reject->call(ctx, rja);
                            }
                        }
                        return Value();
                    }, 1);

                auto on_rej = ObjectFactory::create_native_function("",
                    [this_idx, state, cap_resolve, cap_reject, already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        Value reason = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("rejected")));
                        settled->set_property("reason", reason);
                        state->results[this_idx] = Value(settled.release());
                        if (--state->remaining == 0) {
                            auto arr = ObjectFactory::create_array(static_cast<uint32_t>(state->results.size()));
                            for (size_t j = 0; j < state->results.size(); j++) arr->set_element(static_cast<uint32_t>(j), state->results[j]);
                            std::vector<Value> ra = { Value(arr.release()) };
                            cap_resolve->call(ctx, ra);
                            if (ctx.has_exception()) {
                                Value exc = ctx.get_exception(); ctx.clear_exception();
                                std::vector<Value> rja = { exc };
                                cap_reject->call(ctx, rja);
                            }
                        }
                        return Value();
                    }, 1);

                Function* ful_fn = on_ful.release();
                Function* rej_fn = on_rej.release();
                pin_target->set_property("__settled_ful_" + std::to_string(idx) + "__", Value(ful_fn));
                pin_target->set_property("__settled_rej_" + std::to_string(idx) + "__", Value(rej_fn));

                Value then_method = next_promise_obj->get_property("then");
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!then_method.is_function()) {
                    iterator_close(ctx, rec);
                    reject_capability_type_error(ctx, cap, "then is not a function");
                    return cap.promise;
                }
                std::vector<Value> then_args = { Value(ful_fn), Value(rej_fn) };
                then_method.as_function()->call(ctx, then_args, next_promise);
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }

                idx++;
            }
        }, 1);
    promise_constructor->set_property("allSettled", Value(promise_allSettled_static.release()), PropertyAttributes::BuiltinFunction);

    auto promise_allKeyed_static = ObjectFactory::create_native_function("allKeyed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value raw_this = ctx.get_binding("this");
            if (!raw_this.is_object() && !raw_this.is_function()) { ctx.throw_type_error("Promise.allKeyed called on non-object"); return Value(); }

            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, raw_this, cap)) return Value();
            Function* this_ctor = raw_this.as_function();

            Value resolve_method = this_ctor->get_property("resolve");
            if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            if (!resolve_method.is_function()) {
                reject_capability_type_error(ctx, cap, "Promise.resolve is not callable");
                return cap.promise;
            }
            Function* resolve_fn = resolve_method.as_function();

            if (args.empty() || (!args[0].is_object() && !args[0].is_function())) {
                reject_capability_type_error(ctx, cap, "Promise.allKeyed argument must be an object");
                return cap.promise;
            }
            Object* dict = as_object_or_function(args[0]);

            std::vector<std::string> all_keys = own_keys_for(dict);
            if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }

            std::vector<std::string> keys;
            for (auto& key : all_keys) {
                PropertyDescriptor desc = own_property_descriptor_for(dict, key);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if ((desc.is_data_descriptor() || desc.is_accessor_descriptor()) && desc.is_enumerable()) {
                    keys.push_back(key);
                }
            }

            auto results_obj = ObjectFactory::create_object();
            results_obj->set_prototype(nullptr);
            // Pre-populate every key (in dict's own enumeration order) before any element
            // settles asynchronously, so the result's key order matches the source object's
            // -- not arbitrary, settlement-dependent order.
            for (const auto& k : keys) results_obj->set_property(k, Value());
            Object* results_raw = results_obj.release();

            Object* pin_target = as_object_or_function(cap.promise);
            if (!pin_target) pin_target = ctx.get_global_object();
            pin_target->set_property("__allkeyed_results__", Value(results_raw));

            if (keys.empty()) {
                std::vector<Value> ra = { Value(results_raw) };
                cap.resolve->call(ctx, ra);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); }
                return cap.promise;
            }

            struct KeyedAllState { Object* results; uint32_t remaining; };
            auto state = std::make_shared<KeyedAllState>();
            state->results = results_raw;
            state->remaining = static_cast<uint32_t>(keys.size());
            Function* cap_resolve = cap.resolve;
            Function* cap_reject = cap.reject;

            for (size_t i = 0; i < keys.size(); i++) {
                std::string key = keys[i];
                Value value = dict->get_property(key);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }

                std::vector<Value> resolve_args = { value };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                auto already_called = std::make_shared<bool>(false);
                auto on_ful = ObjectFactory::create_native_function("",
                    [key, state, cap_resolve, cap_reject, already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        Value val = args.empty() ? Value() : args[0];
                        state->results->set_property(key, val);
                        if (--state->remaining == 0) {
                            std::vector<Value> ra = { Value(state->results) };
                            cap_resolve->call(ctx, ra);
                            if (ctx.has_exception()) {
                                Value exc = ctx.get_exception(); ctx.clear_exception();
                                std::vector<Value> rja = { exc };
                                cap_reject->call(ctx, rja);
                            }
                        }
                        return Value();
                    }, 1);

                Function* ful_fn = on_ful.release();
                pin_target->set_property("__allkeyed_ful_" + std::to_string(i) + "__", Value(ful_fn));

                Value then_method = next_promise_obj->get_property("then");
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!then_method.is_function()) {
                    reject_capability_type_error(ctx, cap, "then is not a function");
                    return cap.promise;
                }
                std::vector<Value> then_args = { Value(ful_fn), Value(cap.reject) };
                then_method.as_function()->call(ctx, then_args, next_promise);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            }

            return cap.promise;
        }, 1);
    promise_constructor->set_property("allKeyed", Value(promise_allKeyed_static.release()), PropertyAttributes::BuiltinFunction);

    auto promise_allSettledKeyed_static = ObjectFactory::create_native_function("allSettledKeyed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value raw_this = ctx.get_binding("this");
            if (!raw_this.is_object() && !raw_this.is_function()) { ctx.throw_type_error("Promise.allSettledKeyed called on non-object"); return Value(); }

            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, raw_this, cap)) return Value();
            Function* this_ctor = raw_this.as_function();

            Value resolve_method = this_ctor->get_property("resolve");
            if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            if (!resolve_method.is_function()) {
                reject_capability_type_error(ctx, cap, "Promise.resolve is not callable");
                return cap.promise;
            }
            Function* resolve_fn = resolve_method.as_function();

            if (args.empty() || (!args[0].is_object() && !args[0].is_function())) {
                reject_capability_type_error(ctx, cap, "Promise.allSettledKeyed argument must be an object");
                return cap.promise;
            }
            Object* dict = as_object_or_function(args[0]);

            std::vector<std::string> all_keys = own_keys_for(dict);
            if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }

            std::vector<std::string> keys;
            for (auto& key : all_keys) {
                PropertyDescriptor desc = own_property_descriptor_for(dict, key);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if ((desc.is_data_descriptor() || desc.is_accessor_descriptor()) && desc.is_enumerable()) {
                    keys.push_back(key);
                }
            }

            auto results_obj = ObjectFactory::create_object();
            results_obj->set_prototype(nullptr);
            for (const auto& k : keys) results_obj->set_property(k, Value());
            Object* results_raw = results_obj.release();

            Object* pin_target = as_object_or_function(cap.promise);
            if (!pin_target) pin_target = ctx.get_global_object();
            pin_target->set_property("__settledkeyed_results__", Value(results_raw));

            if (keys.empty()) {
                std::vector<Value> ra = { Value(results_raw) };
                cap.resolve->call(ctx, ra);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); }
                return cap.promise;
            }

            struct KeyedSettledState { Object* results; uint32_t remaining; };
            auto state = std::make_shared<KeyedSettledState>();
            state->results = results_raw;
            state->remaining = static_cast<uint32_t>(keys.size());
            Function* cap_resolve = cap.resolve;
            Function* cap_reject = cap.reject;

            for (size_t i = 0; i < keys.size(); i++) {
                std::string key = keys[i];
                Value value = dict->get_property(key);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }

                std::vector<Value> resolve_args = { value };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                // Spec: resolveElement and rejectElement for the same index share one AlreadyCalled flag.
                auto already_called = std::make_shared<bool>(false);
                auto on_ful = ObjectFactory::create_native_function("",
                    [key, state, cap_resolve, cap_reject, already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        Value val = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("fulfilled")));
                        settled->set_property("value", val);
                        state->results->set_property(key, Value(settled.release()));
                        if (--state->remaining == 0) {
                            std::vector<Value> ra = { Value(state->results) };
                            cap_resolve->call(ctx, ra);
                            if (ctx.has_exception()) {
                                Value exc = ctx.get_exception(); ctx.clear_exception();
                                std::vector<Value> rja = { exc };
                                cap_reject->call(ctx, rja);
                            }
                        }
                        return Value();
                    }, 1);

                auto on_rej = ObjectFactory::create_native_function("",
                    [key, state, cap_resolve, cap_reject, already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        Value reason = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("rejected")));
                        settled->set_property("reason", reason);
                        state->results->set_property(key, Value(settled.release()));
                        if (--state->remaining == 0) {
                            std::vector<Value> ra = { Value(state->results) };
                            cap_resolve->call(ctx, ra);
                            if (ctx.has_exception()) {
                                Value exc = ctx.get_exception(); ctx.clear_exception();
                                std::vector<Value> rja = { exc };
                                cap_reject->call(ctx, rja);
                            }
                        }
                        return Value();
                    }, 1);

                Function* ful_fn = on_ful.release();
                Function* rej_fn = on_rej.release();
                pin_target->set_property("__settledkeyed_ful_" + std::to_string(i) + "__", Value(ful_fn));
                pin_target->set_property("__settledkeyed_rej_" + std::to_string(i) + "__", Value(rej_fn));

                Value then_method = next_promise_obj->get_property("then");
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!then_method.is_function()) {
                    reject_capability_type_error(ctx, cap, "then is not a function");
                    return cap.promise;
                }
                std::vector<Value> then_args = { Value(ful_fn), Value(rej_fn) };
                then_method.as_function()->call(ctx, then_args, next_promise);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            }

            return cap.promise;
        }, 1);
    promise_constructor->set_property("allSettledKeyed", Value(promise_allSettledKeyed_static.release()), PropertyAttributes::BuiltinFunction);

    auto promise_any_static = ObjectFactory::create_native_function("any",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value raw_this = ctx.get_binding("this");
            if (!raw_this.is_object() && !raw_this.is_function()) { ctx.throw_type_error("Promise.any called on non-object"); return Value(); }

            PromiseCapabilityResult cap;
            if (!new_promise_capability(ctx, raw_this, cap)) return Value();
            Function* this_ctor = raw_this.as_function();

            Value resolve_method = this_ctor->get_property("resolve");
            if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            if (!resolve_method.is_function()) {
                reject_capability_type_error(ctx, cap, "Promise.resolve is not callable");
                return cap.promise;
            }
            Function* resolve_fn = resolve_method.as_function();

            IteratorRecord rec;
            if (!get_iterator_record(ctx, args.empty() ? Value() : args[0], rec)) {
                reject_capability_with_exception(ctx, cap);
                return cap.promise;
            }

            struct AnyState {
                Function* cap_reject;
                std::vector<Value> errors;
                uint32_t remaining;
            };
            auto state = std::make_shared<AnyState>();
            state->cap_reject = cap.reject;
            state->remaining = 1; // spec trick: avoids rejecting before the loop finishes if elements settle synchronously
            Function* cap_resolve = cap.resolve;

            Object* pin_target = as_object_or_function(cap.promise);
            if (!pin_target) pin_target = ctx.get_global_object();

            auto finalize_aggregate_reject = [](Context& c, AnyState* st) {
                auto errors_arr = ObjectFactory::create_array();
                for (size_t j = 0; j < st->errors.size(); j++) errors_arr->set_element(static_cast<uint32_t>(j), st->errors[j]);
                Value errors_val(errors_arr.release());
                Object* agg_ctor = c.get_built_in_object("AggregateError");
                Value reason;
                if (agg_ctor && agg_ctor->is_function()) {
                    reason = static_cast<Function*>(agg_ctor)->call(c, {errors_val, Value(std::string("All promises were rejected"))});
                } else {
                    reason = Value(std::string("AggregateError: All promises were rejected"));
                }
                std::vector<Value> ra = { reason };
                st->cap_reject->call(c, ra);
            };

            uint32_t idx = 0;
            while (true) {
                Value elem;
                IterStepStatus step = iterator_step(ctx, rec, elem);
                if (step == IterStepStatus::ABRUPT) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (step == IterStepStatus::DONE) {
                    if (--state->remaining == 0) finalize_aggregate_reject(ctx, state.get());
                    return cap.promise;
                }

                std::vector<Value> resolve_args = { elem };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    iterator_close(ctx, rec);
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                state->errors.push_back(Value());
                state->remaining++;
                uint32_t this_idx = idx;

                // Spec: onFulfilled is the shared resultCapability.[[Resolve]]; onRejected is per-index.
                auto already_called = std::make_shared<bool>(false);
                auto on_reject = ObjectFactory::create_native_function("",
                    [state, this_idx, already_called, finalize_aggregate_reject](Context& c, const std::vector<Value>& a) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        state->errors[this_idx] = a.empty() ? Value() : a[0];
                        if (--state->remaining == 0) finalize_aggregate_reject(c, state.get());
                        return Value();
                    }, 1);

                Function* reject_raw = on_reject.release();
                pin_target->set_property("__any_r__" + std::to_string(idx), Value(reject_raw));

                Value then_method = next_promise_obj->get_property("then");
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!then_method.is_function()) {
                    iterator_close(ctx, rec);
                    reject_capability_type_error(ctx, cap, "then is not a function");
                    return cap.promise;
                }
                std::vector<Value> then_args = { Value(cap_resolve), Value(reject_raw) };
                then_method.as_function()->call(ctx, then_args, next_promise);
                if (ctx.has_exception()) { iterator_close(ctx, rec); reject_capability_with_exception(ctx, cap); return cap.promise; }

                idx++;
            }
        }, 1);
    promise_constructor->set_property("any", Value(promise_any_static.release()), PropertyAttributes::BuiltinFunction);

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

    ctx.register_built_in_object("Promise", promise_constructor.release());

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
    ctx.register_built_in_object("WeakRef", weakref_constructor.release());

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
    fr_proto_ptr->set_property("constructor", Value(finalizationregistry_constructor.get()), PropertyAttributes::BuiltinFunction);
    ctx.register_built_in_object("FinalizationRegistry", finalizationregistry_constructor.release());
}

} // namespace Quanta
