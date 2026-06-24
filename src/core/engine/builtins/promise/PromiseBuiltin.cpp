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
    Value promise_val = C->construct(ctx, ctor_args);
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

// Collects elements from an iterable (array fast path, else full Symbol.iterator protocol).
// On any abrupt completion (including poisoned `done`/`value`/`next`), leaves ctx in an
// exception state and returns false, for the caller to convert into IfAbruptRejectPromise.
static bool collect_iterable_elements(Context& ctx, const Value& iterable_val, std::vector<Value>& out_elements) {
    Object* iterable = as_object_or_function(iterable_val);
    if (!iterable) { ctx.throw_type_error("not an object"); return false; }
    if (iterable->is_array()) {
        uint32_t length = iterable->get_length();
        for (uint32_t i = 0; i < length; i++) out_elements.push_back(iterable->get_element(i));
        return true;
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
    for (uint32_t ii = 0; ii < 100000; ii++) {
        Value res = next_fn.as_function()->call(ctx, {}, iter_obj);
        if (ctx.has_exception()) return false;
        if (!res.is_object()) { ctx.throw_type_error("iterator result is not an object"); return false; }
        Value done_val = res.as_object()->get_property("done");
        if (ctx.has_exception()) return false;
        if (done_val.to_boolean()) break;
        Value val = res.as_object()->get_property("value");
        if (ctx.has_exception()) return false;
        out_elements.push_back(val);
    }
    return true;
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
                std::vector<Value> then_args = { Value(), Value() };
                return then_val.as_function()->call(ctx, then_args, this_val);
            }
            auto then_wrapper = ObjectFactory::create_native_function("",
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
                }, 1);
            auto catch_wrapper = ObjectFactory::create_native_function("",
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

            if (args.empty() || (!args[0].is_object() && !args[0].is_function())) {
                reject_capability_type_error(ctx, cap, "Promise.all expects an iterable");
                return cap.promise;
            }

            std::vector<Value> elements;
            if (!collect_iterable_elements(ctx, args[0], elements)) {
                reject_capability_with_exception(ctx, cap);
                return cap.promise;
            }
            uint32_t length = static_cast<uint32_t>(elements.size());

            if (length == 0) {
                auto empty_array = ObjectFactory::create_array(0);
                std::vector<Value> resolve_args = { Value(empty_array.release()) };
                cap.resolve->call(ctx, resolve_args);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); }
                return cap.promise;
            }

            struct AllState {
                std::vector<Value> results;
                uint32_t remaining;
            };
            auto state = std::make_shared<AllState>();
            state->results.resize(length);
            state->remaining = length;
            Function* cap_resolve = cap.resolve;
            Function* cap_reject = cap.reject;

            Object* pin_target = as_object_or_function(cap.promise);
            if (!pin_target) pin_target = ctx.get_global_object();

            for (uint32_t i = 0; i < length; i++) {
                Value element = elements[i];

                std::vector<Value> resolve_args = { element };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                uint32_t idx = i;
                auto already_called = std::make_shared<bool>(false);
                auto on_ful = ObjectFactory::create_native_function("",
                    [idx, state, cap_resolve, cap_reject, already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        Value val = args.empty() ? Value() : args[0];
                        state->results[idx] = val;
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
                pin_target->set_property("__all_ful_" + std::to_string(i) + "__", Value(ful_fn));

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

            if (args.empty() || (!args[0].is_object() && !args[0].is_function())) {
                reject_capability_type_error(ctx, cap, "Promise.race requires an iterable argument");
                return cap.promise;
            }

            std::vector<Value> elements;
            if (!collect_iterable_elements(ctx, args[0], elements)) {
                reject_capability_with_exception(ctx, cap);
                return cap.promise;
            }
            uint32_t length = static_cast<uint32_t>(elements.size());

            if (length == 0) {
                return cap.promise;
            }

            // Async Promise.race: first settled promise wins (cap.resolve/cap.reject already
            // enforce "AlreadyResolved" semantics, so the same pair is safely reused for every element).
            for (uint32_t i = 0; i < length; i++) {
                Value element = elements[i];

                std::vector<Value> resolve_args = { element };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                Value then_method = next_promise_obj->get_property("then");
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!then_method.is_function()) {
                    reject_capability_type_error(ctx, cap, "then is not a function");
                    return cap.promise;
                }
                std::vector<Value> then_args = { Value(cap.resolve), Value(cap.reject) };
                then_method.as_function()->call(ctx, then_args, next_promise);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            }

            return cap.promise;
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

            if (args.empty() || (!args[0].is_object() && !args[0].is_function())) {
                reject_capability_type_error(ctx, cap, "Promise.allSettled expects an iterable");
                return cap.promise;
            }

            std::vector<Value> elements;
            if (!collect_iterable_elements(ctx, args[0], elements)) {
                reject_capability_with_exception(ctx, cap);
                return cap.promise;
            }
            uint32_t length = static_cast<uint32_t>(elements.size());

            if (length == 0) {
                auto empty_array = ObjectFactory::create_array(0);
                std::vector<Value> resolve_args = { Value(empty_array.release()) };
                cap.resolve->call(ctx, resolve_args);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); }
                return cap.promise;
            }

            struct SettledState {
                std::vector<Value> results;
                uint32_t remaining;
            };
            auto state = std::make_shared<SettledState>();
            state->results.resize(length);
            state->remaining = length;
            Function* cap_resolve = cap.resolve;
            Function* cap_reject = cap.reject;

            Object* pin_target = as_object_or_function(cap.promise);
            if (!pin_target) pin_target = ctx.get_global_object();

            for (uint32_t i = 0; i < length; i++) {
                Value element = elements[i];

                std::vector<Value> resolve_args = { element };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                uint32_t idx = i;
                // Spec: resolveElement and rejectElement for the same index share one AlreadyCalled flag.
                auto already_called = std::make_shared<bool>(false);

                auto on_ful = ObjectFactory::create_native_function("",
                    [idx, state, cap_resolve, cap_reject, already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        Value val = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("fulfilled")));
                        settled->set_property("value", val);
                        state->results[idx] = Value(settled.release());
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
                    [idx, state, cap_resolve, cap_reject, already_called](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        Value reason = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("rejected")));
                        settled->set_property("reason", reason);
                        state->results[idx] = Value(settled.release());
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
                pin_target->set_property("__settled_ful_" + std::to_string(i) + "__", Value(ful_fn));
                pin_target->set_property("__settled_rej_" + std::to_string(i) + "__", Value(rej_fn));

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
    promise_constructor->set_property("allSettled", Value(promise_allSettled_static.release()), PropertyAttributes::BuiltinFunction);

    auto promise_allKeyed_static = ObjectFactory::create_native_function("allKeyed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_type_error("Promise.allKeyed argument must be an object");
                return Value();
            }
            Object* dict = args[0].as_object();
            std::vector<std::string> keys;
            for (const auto& k : dict->get_enumerable_keys()) {
                if (k.find("@@sym:") != 0 && k.find("Symbol.") != 0) keys.push_back(k);
            }

            auto result_promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* result_promise = static_cast<Promise*>(result_promise_obj.get());

            auto results_obj = ObjectFactory::create_object();
            results_obj->set_prototype(nullptr);
            for (const auto& k : keys) results_obj->set_property(k, Value());
            Object* results_raw = results_obj.release();
            result_promise->set_property("__allkeyed_results__", Value(results_raw));
            result_promise->set_property("__allkeyed_remaining__", Value((double)keys.size()));

            if (keys.empty()) {
                result_promise->fulfill(Value(results_raw));
                return Value(result_promise_obj.release());
            }

            for (size_t i = 0; i < keys.size(); i++) {
                std::string key = keys[i];
                Value element = dict->get_property(key);

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
                    [key, rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value val = args.empty() ? Value() : args[0];
                        Value res_v = rp->get_property("__allkeyed_results__");
                        if (res_v.is_object()) res_v.as_object()->set_property(key, val);
                        Value rem_v = rp->get_property("__allkeyed_remaining__");
                        double remaining = rem_v.to_number() - 1.0;
                        rp->set_property("__allkeyed_remaining__", Value(remaining));
                        if (remaining <= 0.0) rp->fulfill(rp->get_property("__allkeyed_results__"));
                        return Value();
                    });

                auto on_rej = ObjectFactory::create_native_function("",
                    [rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value reason = args.empty() ? Value() : args[0];
                        rp->reject(reason);
                        return Value();
                    });

                std::string k_ful = "__allkeyed_ful_" + std::to_string(i) + "__";
                std::string k_rej = "__allkeyed_rej_" + std::to_string(i) + "__";
                Function* ful_fn = on_ful.get();
                Function* rej_fn = on_rej.get();
                result_promise->set_property(k_ful, Value(on_ful.release()));
                result_promise->set_property(k_rej, Value(on_rej.release()));

                p->then(ful_fn, rej_fn);
            }

            return Value(result_promise_obj.release());
        }, 1);
    promise_constructor->set_property("allKeyed", Value(promise_allKeyed_static.release()));

    auto promise_allSettledKeyed_static = ObjectFactory::create_native_function("allSettledKeyed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].is_object()) {
                ctx.throw_type_error("Promise.allSettledKeyed argument must be an object");
                return Value();
            }
            Object* dict = args[0].as_object();
            std::vector<std::string> keys;
            for (const auto& k : dict->get_enumerable_keys()) {
                if (k.find("@@sym:") != 0 && k.find("Symbol.") != 0) keys.push_back(k);
            }

            auto result_promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* result_promise = static_cast<Promise*>(result_promise_obj.get());

            auto results_obj = ObjectFactory::create_object();
            results_obj->set_prototype(nullptr);
            for (const auto& k : keys) results_obj->set_property(k, Value());
            Object* results_raw = results_obj.release();
            result_promise->set_property("__settledkeyed_results__", Value(results_raw));
            result_promise->set_property("__settledkeyed_remaining__", Value((double)keys.size()));

            if (keys.empty()) {
                result_promise->fulfill(Value(results_raw));
                return Value(result_promise_obj.release());
            }

            for (size_t i = 0; i < keys.size(); i++) {
                std::string key = keys[i];
                Value element = dict->get_property(key);

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
                    [key, rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value val = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("fulfilled")));
                        settled->set_property("value", val);
                        Value res_v = rp->get_property("__settledkeyed_results__");
                        if (res_v.is_object()) res_v.as_object()->set_property(key, Value(settled.release()));
                        Value rem_v = rp->get_property("__settledkeyed_remaining__");
                        double remaining = rem_v.to_number() - 1.0;
                        rp->set_property("__settledkeyed_remaining__", Value(remaining));
                        if (remaining <= 0.0) rp->fulfill(rp->get_property("__settledkeyed_results__"));
                        return Value();
                    });

                auto on_rej = ObjectFactory::create_native_function("",
                    [key, rp](Context& ctx, const std::vector<Value>& args) -> Value {
                        if (!rp->is_pending()) return Value();
                        Value reason = args.empty() ? Value() : args[0];
                        auto settled = ObjectFactory::create_object();
                        settled->set_property("status", Value(std::string("rejected")));
                        settled->set_property("reason", reason);
                        Value res_v = rp->get_property("__settledkeyed_results__");
                        if (res_v.is_object()) res_v.as_object()->set_property(key, Value(settled.release()));
                        Value rem_v = rp->get_property("__settledkeyed_remaining__");
                        double remaining = rem_v.to_number() - 1.0;
                        rp->set_property("__settledkeyed_remaining__", Value(remaining));
                        if (remaining <= 0.0) rp->fulfill(rp->get_property("__settledkeyed_results__"));
                        return Value();
                    });

                std::string k_ful = "__settledkeyed_ful_" + std::to_string(i) + "__";
                std::string k_rej = "__settledkeyed_rej_" + std::to_string(i) + "__";
                Function* ful_fn = on_ful.get();
                Function* rej_fn = on_rej.get();
                result_promise->set_property(k_ful, Value(on_ful.release()));
                result_promise->set_property(k_rej, Value(on_rej.release()));

                p->then(ful_fn, rej_fn);
            }

            return Value(result_promise_obj.release());
        }, 1);
    promise_constructor->set_property("allSettledKeyed", Value(promise_allSettledKeyed_static.release()));

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

            if (args.empty() || (!args[0].is_object() && !args[0].is_function())) {
                reject_capability_type_error(ctx, cap, "Promise.any expects an iterable");
                return cap.promise;
            }

            std::vector<Value> elements;
            if (!collect_iterable_elements(ctx, args[0], elements)) {
                reject_capability_with_exception(ctx, cap);
                return cap.promise;
            }
            uint32_t length = static_cast<uint32_t>(elements.size());

            if (length == 0) {
                auto errors_arr = ObjectFactory::create_array();
                Value errors_val(errors_arr.release());
                Object* agg_ctor = ctx.get_built_in_object("AggregateError");
                Value reason;
                if (agg_ctor && agg_ctor->is_function()) {
                    reason = static_cast<Function*>(agg_ctor)->call(ctx, {errors_val, Value(std::string("All promises were rejected"))});
                } else {
                    reason = Value(std::string("AggregateError: All promises were rejected"));
                }
                std::vector<Value> rej_args = { reason };
                cap.reject->call(ctx, rej_args);
                return cap.promise;
            }

            struct AnyState {
                Function* cap_reject;
                std::vector<Value> errors;
                uint32_t total;
                uint32_t rejected_count;
            };
            auto state = std::make_shared<AnyState>();
            state->cap_reject = cap.reject;
            state->errors.resize(length);
            state->total = length;
            state->rejected_count = 0;
            Function* cap_resolve = cap.resolve;

            Object* pin_target = as_object_or_function(cap.promise);
            if (!pin_target) pin_target = ctx.get_global_object();

            for (uint32_t i = 0; i < length; i++) {
                Value elem = elements[i];

                std::vector<Value> resolve_args = { elem };
                Value next_promise = resolve_fn->call(ctx, resolve_args, Value(this_ctor));
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!next_promise.is_object() && !next_promise.is_function()) {
                    reject_capability_type_error(ctx, cap, "Promise.resolve did not return an object");
                    return cap.promise;
                }
                Object* next_promise_obj = as_object_or_function(next_promise);

                // Spec: onFulfilled is the shared resultCapability.[[Resolve]]; onRejected is per-index.
                auto already_called = std::make_shared<bool>(false);
                auto on_reject = ObjectFactory::create_native_function("",
                    [state, i, already_called](Context& c, const std::vector<Value>& a) -> Value {
                        if (*already_called) return Value();
                        *already_called = true;
                        state->errors[i] = a.empty() ? Value() : a[0];
                        state->rejected_count++;
                        if (state->rejected_count == state->total) {
                            auto errors_arr = ObjectFactory::create_array();
                            for (uint32_t j = 0; j < state->total; j++)
                                errors_arr->set_element(j, state->errors[j]);
                            Value errors_val(errors_arr.release());
                            Object* agg_ctor = c.get_built_in_object("AggregateError");
                            Value reason;
                            if (agg_ctor && agg_ctor->is_function()) {
                                reason = static_cast<Function*>(agg_ctor)->call(c, {errors_val, Value(std::string("All promises were rejected"))});
                            } else {
                                reason = Value(std::string("AggregateError: All promises were rejected"));
                            }
                            std::vector<Value> ra = { reason };
                            state->cap_reject->call(c, ra);
                        }
                        return Value();
                    }, 1);

                Function* reject_raw = on_reject.release();
                pin_target->set_property("__any_r__" + std::to_string(i), Value(reject_raw));

                Value then_method = next_promise_obj->get_property("then");
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
                if (!then_method.is_function()) {
                    reject_capability_type_error(ctx, cap, "then is not a function");
                    return cap.promise;
                }
                std::vector<Value> then_args = { Value(cap_resolve), Value(reject_raw) };
                then_method.as_function()->call(ctx, then_args, next_promise);
                if (ctx.has_exception()) { reject_capability_with_exception(ctx, cap); return cap.promise; }
            }

            return cap.promise;
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
