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
    if (ctx) engine_ = ctx->get_engine();
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

    // Create child promise
    auto child_obj = ObjectFactory::create_promise(exec_ctx);
    Promise* child = static_cast<Promise*>(child_obj.release());

    if (state_ == PromiseState::PENDING) {
        // Store for later when promise resolves
        then_records_.push_back({on_fulfilled, on_rejected, child});
    } else if (state_ == PromiseState::FULFILLED) {
        if (on_fulfilled && exec_ctx) {
            Value val = value_;
            Function* cb = on_fulfilled;
            Promise* ch = child;
            exec_ctx->queue_microtask([cb, ch, val, exec_ctx]() mutable {
                std::vector<Value> args = {val};
                Value result = cb->call(*exec_ctx, args);
                if (exec_ctx->has_exception()) {
                    Value exc = exec_ctx->get_exception();
                    exec_ctx->clear_exception();
                    if (ch) ch->reject(exc);
                } else {
                    if (ch) ch->fulfill(result);
                }
            });
        } else {
            child->fulfill(value_);
        }
    } else { // REJECTED
        if (on_rejected && exec_ctx) {
            Value val = value_;
            Function* cb = on_rejected;
            Promise* ch = child;
            exec_ctx->queue_microtask([cb, ch, val, exec_ctx]() mutable {
                std::vector<Value> args = {val};
                Value result = cb->call(*exec_ctx, args);
                if (exec_ctx->has_exception()) {
                    Value exc = exec_ctx->get_exception();
                    exec_ctx->clear_exception();
                    if (ch) ch->reject(exc);
                } else {
                    if (ch) ch->fulfill(result);
                }
            });
        } else {
            child->reject(value_);
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
    auto promise_obj = ObjectFactory::create_promise(nullptr);
    auto* promise = static_cast<Promise*>(promise_obj.release());
    promise->fulfill(value);
    return promise;
}

Promise* Promise::reject_static(const Value& reason) {
    auto promise_obj = ObjectFactory::create_promise(nullptr);
    auto* promise = static_cast<Promise*>(promise_obj.release());
    promise->reject(reason);
    return promise;
}

Promise* Promise::all(const std::vector<Promise*>& promises) {
    auto promise_obj = ObjectFactory::create_promise(nullptr);
    auto* result_promise = static_cast<Promise*>(promise_obj.release());

    if (promises.empty()) {
        result_promise->fulfill(Value(std::string("empty_array")));
        return result_promise;
    }

    result_promise->fulfill(Value(std::string("all_resolved")));
    return result_promise;
}

Promise* Promise::race(const std::vector<Promise*>& promises) {
    auto promise_obj = ObjectFactory::create_promise(nullptr);
    auto* result_promise = static_cast<Promise*>(promise_obj.release());

    if (promises.empty()) {
        return result_promise;
    }

    result_promise->fulfill(Value(std::string("race_winner")));
    return result_promise;
}

void Promise::execute_handlers() {
    Context* exec_ctx = get_exec_ctx(engine_, context_);

    auto records = std::move(then_records_);
    then_records_.clear();

    for (auto& rec : records) {
        if (state_ == PromiseState::FULFILLED) {
            if (rec.on_fulfilled && exec_ctx) {
                Value val = value_;
                Function* cb = rec.on_fulfilled;
                Promise* ch = rec.child;
                exec_ctx->queue_microtask([cb, ch, val, exec_ctx]() mutable {
                    std::vector<Value> args = {val};
                    Value result = cb->call(*exec_ctx, args);
                    if (exec_ctx->has_exception()) {
                        Value exc = exec_ctx->get_exception();
                        exec_ctx->clear_exception();
                        if (ch) ch->reject(exc);
                    } else {
                        if (ch) ch->fulfill(result);
                    }
                });
            } else {
                if (rec.child) rec.child->fulfill(value_);
            }
        } else { // REJECTED
            if (rec.on_rejected && exec_ctx) {
                Value val = value_;
                Function* cb = rec.on_rejected;
                Promise* ch = rec.child;
                exec_ctx->queue_microtask([cb, ch, val, exec_ctx]() mutable {
                    std::vector<Value> args = {val};
                    Value result = cb->call(*exec_ctx, args);
                    if (exec_ctx->has_exception()) {
                        Value exc = exec_ctx->get_exception();
                        exec_ctx->clear_exception();
                        if (ch) ch->reject(exc);
                    } else {
                        if (ch) ch->fulfill(result);
                    }
                });
            } else {
                if (rec.child) rec.child->reject(value_);
            }
        }
    }
}

Value Promise::withResolvers(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args;

    auto promise_obj = ObjectFactory::create_promise(nullptr);
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
    auto promise_obj = ObjectFactory::create_promise(nullptr);
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
