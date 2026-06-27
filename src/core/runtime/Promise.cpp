/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Promise.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/parser/AST.h"
#include <iostream>

namespace Quanta {

Promise::Promise(Context* ctx)
    : Object(ObjectType::Promise), state_(PromiseState::PENDING), context_(ctx), engine_(nullptr) {
    if (ctx) {
        engine_ = ctx->get_engine();
        // Retain context_: a pending Promise may invoke it to run .then() reactions an arbitrary time later (e.g. resolved by a real setTimeout elsewhere).
        EventLoop::instance().retain_context(ctx);
    }
}

Promise::~Promise() {
    if (context_) EventLoop::instance().release_context(context_);
    then_records_.clear();
    context_ = nullptr;
    engine_ = nullptr;
}

// Helper: get the global context via engine (safe for async microtasks)
static Context* get_exec_ctx(Engine* engine, Context* fallback) {
    if (engine) {
        Context* global = engine->get_global_context();
        if (global) return global;
    }
    return fallback;
}

void Promise::fulfill(const Value& value) {
    if (state_ != PromiseState::PENDING) return;

    // Self-resolution (SameValue(resolution, promise)) applies regardless of type.
    Object* value_obj_for_self_check = value.is_function() ? static_cast<Object*>(value.as_function())
                                       : (value.is_object() ? value.as_object() : nullptr);
    if (value_obj_for_self_check == this) {
        state_ = PromiseState::REJECTED;
        Context* err_ctx = get_exec_ctx(engine_, context_);
        if (err_ctx) {
            err_ctx->throw_type_error("Chaining cycle detected for promise");
            value_ = err_ctx->get_exception();
            err_ctx->clear_exception();
        } else {
            value_ = Value(std::string("TypeError: Promise resolved with itself"));
        }
        execute_handlers();
        return;
    }

    bool use_promise_fast_path = false;
    if (value.is_object() && value.as_object()->get_type() == ObjectType::Promise) {
        use_promise_fast_path = true;
        Context* check_ctx = get_exec_ctx(engine_, context_);
        if (check_ctx) {
            Value promise_ctor = check_ctx->get_binding("Promise");
            if (promise_ctor.is_function()) {
                Value proto = static_cast<Object*>(promise_ctor.as_function())->get_property("prototype");
                if (proto.is_object()) {
                    Value std_then = proto.as_object()->get_property("then");
                    Value inner_then = value.as_object()->get_property("then");
                    if (!(std_then.is_function() && inner_then.is_function() &&
                          std_then.as_function() == inner_then.as_function())) {
                        use_promise_fast_path = false;
                    }
                    Value inner_ctor = value.as_object()->get_property("constructor");
                    if (!(inner_ctor.is_function() && inner_ctor.as_function() == promise_ctor.as_function())) {
                        use_promise_fast_path = false;
                    }
                }
            }
        }
    }

    if (use_promise_fast_path) {
        Promise* inner = static_cast<Promise*>(value.as_object());
        if (inner->get_state() == PromiseState::FULFILLED) {
            state_ = PromiseState::FULFILLED;
            value_ = inner->get_value();
            execute_handlers();
        } else if (inner->get_state() == PromiseState::REJECTED) {
            state_ = PromiseState::REJECTED;
            value_ = inner->get_value();
            execute_handlers();
        } else {
            // Inner promise is pending -- chain callbacks
            Promise* self = this;
            auto res_fn = ObjectFactory::create_native_function("",
                [self](Context&, const std::vector<Value>& args) -> Value {
                    self->fulfill(args.empty() ? Value() : args[0]);
                    return Value();
                });
            auto rej_fn = ObjectFactory::create_native_function("",
                [self](Context&, const std::vector<Value>& args) -> Value {
                    self->reject(args.empty() ? Value() : args[0]);
                    return Value();
                });
            Function* rf = res_fn.get();
            Function* rj = rej_fn.get();
            // Store on inner to keep callbacks alive
            inner->set_property("__prp_res__", Value(res_fn.release()));
            inner->set_property("__prp_rej__", Value(rej_fn.release()));
            inner->then(rf, rj);
        }
        return;
    }

    // Promise Resolution Procedure: if value is a thenable (plain object with callable .then),
    // schedule a PromiseResolveThenableJob rather than settling immediately.
    if ((value.is_object() || value.is_function()) && Object::current_context_) {
        Object* obj = value.is_function() ? static_cast<Object*>(value.as_function()) : value.as_object();
        Value then_val = obj->get_property("then");
        if (Object::current_context_->has_exception()) {
            Value exc = Object::current_context_->get_exception();
            Object::current_context_->clear_exception();
            state_ = PromiseState::REJECTED;
            value_ = exc;
            execute_handlers();
            return;
        } else if (then_val.is_function()) {
            // Enqueue as a microtask so it doesn't run synchronously inside fulfill().
            Promise* self = this;
            Function* then_fn = then_val.as_function();
            Value thenable = value;
            set_property("__trp_then__", Value(then_fn));
            set_property("__trp_val__", thenable);
            Context* queue_ctx = get_exec_ctx(engine_, context_);
            if (queue_ctx) {
                queue_ctx->queue_microtask([self, then_fn, thenable]() mutable {
                    self->delete_property("__trp_then__");
                    self->delete_property("__trp_val__");
                    // Same AlreadyResolved rationale as the constructor's resolve/reject pair.
                    auto already_called = std::make_shared<bool>(false);
                    auto res_fn = ObjectFactory::create_native_function("",
                        [self, already_called](Context&, const std::vector<Value>& args) -> Value {
                            if (*already_called) return Value();
                            *already_called = true;
                            self->fulfill(args.empty() ? Value() : args[0]);
                            return Value();
                        });
                    auto rej_fn = ObjectFactory::create_native_function("",
                        [self, already_called](Context&, const std::vector<Value>& args) -> Value {
                            if (*already_called) return Value();
                            *already_called = true;
                            self->reject(args.empty() ? Value() : args[0]);
                            return Value();
                        });
                    Function* rf = res_fn.get();
                    Function* rj = rej_fn.get();
                    self->set_property("__trp_res__", Value(res_fn.release()));
                    self->set_property("__trp_rej__", Value(rej_fn.release()));
                    if (Object::current_context_) {
                        then_fn->call(*Object::current_context_, {Value(rf), Value(rj)}, thenable);
                        if (Object::current_context_->has_exception()) {
                            Value exc = Object::current_context_->get_exception();
                            Object::current_context_->clear_exception();
                            self->delete_property("__trp_res__");
                            self->delete_property("__trp_rej__");
                            if (!*already_called) {
                                *already_called = true;
                                self->reject(exc);
                            }
                        }
                    }
                });
                return;
            }
        }
    }

    state_ = PromiseState::FULFILLED;
    value_ = value;
    execute_handlers();
}

void Promise::reject(const Value& reason) {
    if (state_ != PromiseState::PENDING) return;

    state_ = PromiseState::REJECTED;
    value_ = reason;
    execute_handlers();
}

Promise* Promise::then(Function* on_fulfilled, Function* on_rejected) {
    Context* exec_ctx = get_exec_ctx(engine_, context_);
    // Invoke handlers on the promise's creation context (closures need the right
    // defining scope) but always schedule on the global queue -- the only one
    // drain_microtasks() drains; queuing elsewhere silently drops the job.
    Context* call_ctx = context_ ? context_ : exec_ctx;
    Context* queue_ctx = exec_ctx;

    // Create child promise — inherit parent's context so closure propagation works
    // correctly when promises are created inside nested function scopes.
    auto child_obj = ObjectFactory::create_promise(call_ctx);
    Promise* child = static_cast<Promise*>(child_obj.release());

    if (state_ == PromiseState::PENDING) {
        // Also store callbacks as own properties so GC (which traces properties but
        // not C++ then_records_ directly) keeps them alive during suspension.
        std::string pin = "__then_" + std::to_string(then_records_.size());
        if (on_fulfilled) set_property(pin + "f", Value(on_fulfilled));
        if (on_rejected)  set_property(pin + "r", Value(on_rejected));
        if (child)        set_property(pin + "c", Value(child));
        then_records_.push_back({on_fulfilled, on_rejected, child});
    } else if (state_ == PromiseState::FULFILLED) {
        // ES2015 25.4.5.3: PromiseReactionJob is always enqueued, never run
        // synchronously -- test262's ordering tests assert .then runs strictly
        // after the current synchronous job, in FIFO microtask order.
        Value val = value_;
        Promise* self = this;
        Function* cb = on_fulfilled;
        Promise* ch = child;
        if (queue_ctx && (cb || ch)) {
            static thread_local size_t then_pin_counter = 0;
            std::string pin = "__thenp_" + std::to_string(then_pin_counter++);
            if (cb) self->set_property(pin + "f", Value(cb));
            if (ch) self->set_property(pin + "c", Value(ch));
            queue_ctx->queue_microtask([self, pin, cb, ch, val, call_ctx]() mutable {
                self->delete_property(pin + "f");
                self->delete_property(pin + "c");
                if (cb) {
                    std::vector<Value> args = {val};
                    Value result = cb->call(*call_ctx, args);
                    if (call_ctx->has_exception()) {
                        Value exc = call_ctx->get_exception();
                        call_ctx->clear_exception();
                        if (ch) ch->reject(exc);
                    } else {
                        if (ch) ch->fulfill(result);
                    }
                } else {
                    if (ch) ch->fulfill(val);
                }
            });
        } else if (ch) {
            ch->fulfill(val);
        }
    } else { // REJECTED
        Value val = value_;
        Promise* self = this;
        Function* cb = on_rejected;
        Promise* ch = child;
        if (queue_ctx && (cb || ch)) {
            static thread_local size_t then_pin_counter = 0;
            std::string pin = "__thenp_" + std::to_string(then_pin_counter++);
            if (cb) self->set_property(pin + "f", Value(cb));
            if (ch) self->set_property(pin + "c", Value(ch));
            queue_ctx->queue_microtask([self, pin, cb, ch, val, call_ctx]() mutable {
                self->delete_property(pin + "f");
                self->delete_property(pin + "c");
                if (cb) {
                    std::vector<Value> args = {val};
                    Value result = cb->call(*call_ctx, args);
                    if (call_ctx->has_exception()) {
                        Value exc = call_ctx->get_exception();
                        call_ctx->clear_exception();
                        if (ch) ch->reject(exc);
                    } else {
                        if (ch) ch->fulfill(result);
                    }
                } else {
                    if (ch) ch->reject(val);
                }
            });
        } else if (ch) {
            ch->reject(val);
        }
    }

    return child;
}

Promise* Promise::catch_method(Function* on_rejected) {
    return then(nullptr, on_rejected);
}

Promise* Promise::finally_method(Function* on_finally) {
    return then(on_finally, on_finally);
}

Promise* Promise::resolve(const Value& value) {
    // Use the live execution context so execute_handlers() can queue microtasks;
    // nullptr produces a null queue_ctx, silently dropping all .then() callbacks.
    auto promise_obj = ObjectFactory::create_promise(Object::current_context_);
    auto* promise = static_cast<Promise*>(promise_obj.release());
    promise->fulfill(value);
    return promise;
}

Promise* Promise::reject_static(const Value& reason) {
    auto promise_obj = ObjectFactory::create_promise(Object::current_context_);
    auto* promise = static_cast<Promise*>(promise_obj.release());
    promise->reject(reason);
    return promise;
}

Promise* Promise::all(const std::vector<Promise*>& promises) {
    auto promise_obj = ObjectFactory::create_promise(Object::current_context_);
    auto* result_promise = static_cast<Promise*>(promise_obj.release());

    if (promises.empty()) {
        result_promise->fulfill(Value(std::string("empty_array")));
        return result_promise;
    }

    result_promise->fulfill(Value(std::string("all_resolved")));
    return result_promise;
}

Promise* Promise::race(const std::vector<Promise*>& promises) {
    auto promise_obj = ObjectFactory::create_promise(Object::current_context_);
    auto* result_promise = static_cast<Promise*>(promise_obj.release());

    if (promises.empty()) {
        return result_promise;
    }

    result_promise->fulfill(Value(std::string("race_winner")));
    return result_promise;
}

void Promise::execute_handlers() {
    // Use context_ (the promise's creation context, kept alive in Engine's survivor pool)
    // so that Function::call's parent_context includes the defining scope, enabling
    // live closure variable access (shared mutable state across async boundaries).
    auto records = std::move(then_records_);
    then_records_.clear();

    PromiseState settled_state = state_;
    Value settled_value = value_;
    // call_ctx (promise's creation context) invokes handlers so closures see live
    // state; queue_ctx (always global -- the only queue drain_microtasks() drains)
    // schedules the job, since queuing elsewhere silently drops it.
    Context* call_ctx = context_;
    Context* queue_ctx = get_exec_ctx(engine_, context_);
    Promise* self = this;

    // ES2015 25.4.1.3.2/25.4.1.8: PromiseReactionJob always runs as a queued job,
    // never inside fulfill()/reject(), so .then ordering matches what test262's
    // interleaving tests expect relative to other microtasks.
    for (size_t i = 0; i < records.size(); i++) {
        std::string pin = "__then_" + std::to_string(i);
        Function* on_fulfilled = records[i].on_fulfilled;
        Function* on_rejected = records[i].on_rejected;
        Promise* child = records[i].child;

        if (!call_ctx || !queue_ctx) {
            // No context to queue on (shouldn't happen) -- run synchronously as a
            // last resort, after dropping the GC pins.
            delete_property(pin + "f");
            delete_property(pin + "r");
            delete_property(pin + "c");
            if (settled_state == PromiseState::FULFILLED) {
                if (child) child->fulfill(settled_value);
            } else {
                if (child) child->reject(settled_value);
            }
            continue;
        }

        // Keep __then_ pins alive (GC root via self's properties) until the job runs.
        Context* ctx = call_ctx;
        queue_ctx->queue_microtask([self, pin, ctx, on_fulfilled, on_rejected, child,
                              settled_state, settled_value]() mutable {
            self->delete_property(pin + "f");
            self->delete_property(pin + "r");
            self->delete_property(pin + "c");

            if (settled_state == PromiseState::FULFILLED) {
                if (on_fulfilled) {
                    std::vector<Value> args = {settled_value};
                    Value result = on_fulfilled->call(*ctx, args);
                    if (ctx->has_exception()) {
                        Value exc = ctx->get_exception();
                        ctx->clear_exception();
                        if (child) child->reject(exc);
                    } else {
                        if (child) child->fulfill(result);
                    }
                } else {
                    if (child) child->fulfill(settled_value);
                }
            } else { // REJECTED
                if (on_rejected) {
                    std::vector<Value> args = {settled_value};
                    Value result = on_rejected->call(*ctx, args);
                    if (ctx->has_exception()) {
                        Value exc = ctx->get_exception();
                        ctx->clear_exception();
                        if (child) child->reject(exc);
                    } else {
                        if (child) child->fulfill(result);
                    }
                } else {
                    if (child) child->reject(settled_value);
                }
            }
        });
    }
}

Value Promise::withResolvers(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;

    auto promise_obj = ObjectFactory::create_promise(Object::current_context_);
    auto* promise = static_cast<Promise*>(promise_obj.release());
    auto result_obj = ObjectFactory::create_object();

    auto resolve_fn = ObjectFactory::create_native_function("resolve",
        [promise](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            Value resolve_value = args.empty() ? Value() : args[0];
            promise->fulfill(resolve_value);
            return Value();
        });

    auto reject_fn = ObjectFactory::create_native_function("reject",
        [promise](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            Value reject_value = args.empty() ? Value(std::string("Promise rejected")) : args[0];
            promise->reject(reject_value);
            return Value();
        });

    result_obj->set_property("promise", Value(promise));
    result_obj->set_property("resolve", Value(resolve_fn.release()));
    result_obj->set_property("reject", Value(reject_fn.release()));

    return Value(result_obj.release());
}

Value Promise::try_method(Context& ctx, const std::vector<Value>& args) {
    if (args.empty() || !args[0].is_function()) {
        ctx.throw_exception(Value(std::string("Promise.try requires a function argument")));
        return Value();
    }

    Function* callback = args[0].as_function();
    auto promise_obj = ObjectFactory::create_promise(Object::current_context_);
    auto* promise = static_cast<Promise*>(promise_obj.release());

    try {
        std::vector<Value> callback_args;
        Value result = callback->call(ctx, callback_args);

        if (result.is_object() && result.as_object()->get_type() == ObjectType::Promise) {
            Promise* result_promise = static_cast<Promise*>(result.as_object());
            if (result_promise->state_ == PromiseState::FULFILLED) {
                promise->fulfill(result_promise->value_);
            } else if (result_promise->state_ == PromiseState::REJECTED) {
                promise->reject(result_promise->value_);
            }
        } else {
            promise->fulfill(result);
        }
    } catch (const std::exception& e) {
        promise->reject(Value(std::string("Promise.try caught exception: ") + e.what()));
    }

    return Value(promise);
}

void Promise::setup_promise_methods(Promise* promise) {
    if (!promise) return;

    auto then_method = ObjectFactory::create_native_function("then",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* new_promise = static_cast<Promise*>(promise_obj.release());
            if (args.size() > 0 && args[0].is_function()) {
                Function* callback = args[0].as_function();
                try {
                    std::vector<Value> callback_args = {Value(std::string("resolved"))};
                    Value result = callback->call(ctx, callback_args);
                    new_promise->fulfill(result);
                } catch (...) {
                    new_promise->reject(Value(std::string("Callback execution failed")));
                }
            } else {
                new_promise->fulfill(Value(std::string("resolved")));
            }
            return Value(new_promise);
        });
    promise->set_property("then", Value(then_method.release()));

    auto catch_method = ObjectFactory::create_native_function("catch",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* new_promise = static_cast<Promise*>(promise_obj.release());
            new_promise->fulfill(Value(std::string("catch_resolved")));
            return Value(new_promise);
        });
    promise->set_property("catch", Value(catch_method.release()));

    auto finally_method = ObjectFactory::create_native_function("finally",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            auto promise_obj = ObjectFactory::create_promise(&ctx);
            Promise* new_promise = static_cast<Promise*>(promise_obj.release());
            new_promise->fulfill(Value(std::string("finally_resolved")));
            return Value(new_promise);
        });
    promise->set_property("finally", Value(finally_method.release()));
}

}
