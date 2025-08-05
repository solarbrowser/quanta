#include "../include/Promise.h"
#include "../include/Context.h"
#include "../include/Async.h"
#include "../../parser/include/AST.h"
#include <iostream>

namespace Quanta {

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
    auto* new_promise = new Promise(context_);
    
    if (state_ == PromiseState::FULFILLED) {
        if (on_fulfilled) {
            // Schedule callback execution as microtask
            EventLoop::instance().schedule_microtask([this, on_fulfilled, new_promise]() {
                try {
                    if (context_) {
                        std::vector<Value> args = {value_};
                        Value result = on_fulfilled->call(*context_, args);
                        
                        // Handle result - for now just fulfill with the result
                        // TODO: Handle promise chaining properly
                        new_promise->fulfill(result);
                    } else {
                        new_promise->reject(Value("No execution context for callback"));
                    }
                } catch (...) {
                    new_promise->reject(Value("Handler execution failed"));
                }
            });
        } else {
            new_promise->fulfill(value_);
        }
    } else if (state_ == PromiseState::REJECTED) {
        if (on_rejected) {
            // Schedule rejection handler as microtask
            EventLoop::instance().schedule_microtask([this, on_rejected, new_promise]() {
                try {
                    if (context_) {
                        std::vector<Value> args = {value_};
                        Value result = on_rejected->call(*context_, args);
                        new_promise->fulfill(result);  // Rejection handler fulfills the new promise
                    } else {
                        new_promise->reject(Value("No execution context for callback"));
                    }
                } catch (...) {
                    new_promise->reject(value_);  // Re-reject with original reason
                }
            });
        } else {
            new_promise->reject(value_);
        }
    } else {
        // Promise is pending, store handlers
        if (on_fulfilled) {
            fulfillment_handlers_.push_back(on_fulfilled);
        }
        if (on_rejected) {
            rejection_handlers_.push_back(on_rejected);
        }
    }
    
    return new_promise;
}

Promise* Promise::catch_method(Function* on_rejected) {
    return then(nullptr, on_rejected);
}

Promise* Promise::finally_method(Function* on_finally) {
    // Simplified implementation - just execute the finally handler
    return then(on_finally, on_finally);
}

Promise* Promise::resolve(const Value& value) {
    auto* promise = new Promise(nullptr);  // Will be updated when bound to Context
    promise->fulfill(value);
    return promise;
}

Promise* Promise::reject_static(const Value& reason) {
    auto* promise = new Promise(nullptr);  // Will be updated when bound to Context
    promise->reject(reason);
    return promise;
}

Promise* Promise::all(const std::vector<Promise*>& promises) {
    auto* result_promise = new Promise(nullptr);
    
    if (promises.empty()) {
        result_promise->fulfill(Value("empty_array"));
        return result_promise;
    }
    
    // Simplified implementation - just fulfill with "all_resolved"
    result_promise->fulfill(Value("all_resolved"));
    return result_promise;
}

Promise* Promise::race(const std::vector<Promise*>& promises) {
    auto* result_promise = new Promise(nullptr);
    
    if (promises.empty()) {
        // Never resolves if empty
        return result_promise;
    }
    
    // Simplified implementation - just fulfill with "race_winner"
    result_promise->fulfill(Value("race_winner"));
    return result_promise;
}

void Promise::execute_handlers() {
    if (state_ == PromiseState::FULFILLED) {
        // Execute fulfillment handlers using EventLoop
        auto handlers = std::move(fulfillment_handlers_);
        for (Function* handler : handlers) {
            EventLoop::instance().schedule_microtask([this, handler]() {
                try {
                    if (context_) {
                        std::vector<Value> args = {value_};
                        handler->call(*context_, args);
                    }
                } catch (...) {
                    // Handler failed, but don't affect this promise
                }
            });
        }
        rejection_handlers_.clear();
    } else if (state_ == PromiseState::REJECTED) {
        // Execute rejection handlers using EventLoop  
        auto handlers = std::move(rejection_handlers_);
        for (Function* handler : handlers) {
            EventLoop::instance().schedule_microtask([this, handler]() {
                try {
                    if (context_) {
                        std::vector<Value> args = {value_};
                        handler->call(*context_, args);
                    }
                } catch (...) {
                    // Handler failed, but don't affect this promise
                }
            });
        }
        fulfillment_handlers_.clear();
    }
}

// ES2025: Promise.withResolvers()
Value Promise::withResolvers(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args; // Suppress unused parameter warnings
    
    auto* promise = new Promise(nullptr);
    auto result_obj = ObjectFactory::create_object();
    
    // Create resolve function
    auto resolve_fn = ObjectFactory::create_native_function("resolve",
        [promise](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            Value resolve_value = args.empty() ? Value() : args[0];
            promise->fulfill(resolve_value);
            return Value();
        });
    
    // Create reject function  
    auto reject_fn = ObjectFactory::create_native_function("reject",
        [promise](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            Value reject_value = args.empty() ? Value("Promise rejected") : args[0];
            promise->reject(reject_value);
            return Value();
        });
    
    // Return object with promise, resolve, reject
    result_obj->set_property("promise", Value(promise));
    result_obj->set_property("resolve", Value(resolve_fn.release()));
    result_obj->set_property("reject", Value(reject_fn.release()));
    
    return Value(result_obj.release());
}

// ES2025: Promise.try()
Value Promise::try_method(Context& ctx, const std::vector<Value>& args) {
    if (args.empty() || !args[0].is_function()) {
        ctx.throw_exception(Value("Promise.try requires a function argument"));
        return Value();
    }
    
    Function* callback = args[0].as_function();
    auto* promise = new Promise(nullptr);
    
    try {
        // Execute the callback immediately
        std::vector<Value> callback_args;
        Value result = callback->call(ctx, callback_args);
        
        // If result is a Promise, chain it
        if (result.is_object() && result.as_object()->get_type() == ObjectType::Promise) {
            Promise* result_promise = static_cast<Promise*>(result.as_object());
            if (result_promise->state_ == PromiseState::FULFILLED) {
                promise->fulfill(result_promise->value_);
            } else if (result_promise->state_ == PromiseState::REJECTED) {
                promise->reject(result_promise->value_);
            }
        } else {
            // Fulfill with the direct result
            promise->fulfill(result);
        }
    } catch (const std::exception& e) {
        promise->reject(Value(std::string("Promise.try caught exception: ") + e.what()));
    }
    
    return Value(promise);
}

} // namespace Quanta