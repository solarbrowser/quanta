#include "../include/Promise.h"
#include "../include/Context.h"
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
    auto* new_promise = new Promise();
    
    if (state_ == PromiseState::FULFILLED) {
        if (on_fulfilled) {
            // Execute fulfillment handler immediately
            // For now, just fulfill the new promise with the same value
            new_promise->fulfill(value_);
        } else {
            new_promise->fulfill(value_);
        }
    } else if (state_ == PromiseState::REJECTED) {
        if (on_rejected) {
            // Execute rejection handler immediately
            // For now, just fulfill the new promise (simulating handled rejection)
            new_promise->fulfill(Value("handled_rejection"));
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
    auto* promise = new Promise();
    promise->fulfill(value);
    return promise;
}

Promise* Promise::reject_static(const Value& reason) {
    auto* promise = new Promise();
    promise->reject(reason);
    return promise;
}

Promise* Promise::all(const std::vector<Promise*>& promises) {
    auto* result_promise = new Promise();
    
    if (promises.empty()) {
        result_promise->fulfill(Value("empty_array"));
        return result_promise;
    }
    
    // Simplified implementation - just fulfill with "all_resolved"
    result_promise->fulfill(Value("all_resolved"));
    return result_promise;
}

Promise* Promise::race(const std::vector<Promise*>& promises) {
    auto* result_promise = new Promise();
    
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
        // Execute fulfillment handlers
        fulfillment_handlers_.clear();
        rejection_handlers_.clear();
    } else if (state_ == PromiseState::REJECTED) {
        // Execute rejection handlers
        fulfillment_handlers_.clear();
        rejection_handlers_.clear();
    }
}

} // namespace Quanta