/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "quanta/core/engine/builtins/PromiseBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Symbol.h"

namespace Quanta {

void register_promise_builtins(Context& ctx) {
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
    fr_proto_ptr->set_property("constructor", Value(finalizationregistry_constructor.get()));
    ctx.register_built_in_object("FinalizationRegistry", finalizationregistry_constructor.release());
}

} // namespace Quanta
