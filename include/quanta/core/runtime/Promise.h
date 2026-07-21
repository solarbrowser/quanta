/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PROMISE_H
#define QUANTA_PROMISE_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include <memory>
#include <vector>

namespace Quanta {

class Context;
class Function;

/**
 * Promise states according to JavaScript Promise specification
 */
enum class PromiseState {
    PENDING,
    FULFILLED,
    REJECTED
};

/**
 * JavaScript Promise implementation
 */
class Promise : public Object {
private:
    PromiseState state_;
    Value value_;
    std::vector<Function*> fulfillment_handlers_;
    std::vector<Function*> rejection_handlers_;
    Context* context_;

public:
    Promise(Context* ctx = nullptr) : Object(ObjectType::Promise), state_(PromiseState::PENDING), context_(ctx) {}
    
    virtual ~Promise() {
        fulfillment_handlers_.clear();
        rejection_handlers_.clear();
        context_ = nullptr;
    }
    
    void fulfill(const Value& value);
    void reject(const Value& reason);
    
    Promise* then(Function* on_fulfilled, Function* on_rejected = nullptr);
    Promise* catch_method(Function* on_rejected);
    Promise* finally_method(Function* on_finally);
    
    static Promise* resolve(const Value& value);
    static Promise* reject_static(const Value& reason);
    static Promise* all(const std::vector<Promise*>& promises);
    static Promise* race(const std::vector<Promise*>& promises);
    
    static Value withResolvers(Context& ctx, const std::vector<Value>& args);
    static Value try_method(Context& ctx, const std::vector<Value>& args);
    
    PromiseState get_state() const { return state_; }
    const Value& get_value() const { return value_; }
    bool is_pending() const { return state_ == PromiseState::PENDING; }
    bool is_fulfilled() const { return state_ == PromiseState::FULFILLED; }
    bool is_rejected() const { return state_ == PromiseState::REJECTED; }
    
    static void setup_promise_methods(Promise* promise);

private:
    void execute_handlers();
};

}

#endif
