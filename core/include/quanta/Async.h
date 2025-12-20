/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Value.h"
#include "quanta/Object.h"
#include "quanta/Promise.h"
#include <memory>
#include <functional>
#include <future>
#include <thread>

namespace Quanta {

class Context;
class ASTNode;
class Function;


class AsyncFunction : public Function {
private:
    std::unique_ptr<ASTNode> body_;
    
public:
    AsyncFunction(const std::string& name, 
                  const std::vector<std::string>& params,
                  std::unique_ptr<ASTNode> body,
                  Context* closure_context);
    
    Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value()) override;
    
    std::unique_ptr<Promise> execute_async(Context& ctx, const std::vector<Value>& args);
    
private:
    void execute_async_body(Context& ctx, Promise* promise);
};


class AsyncAwaitExpression {
private:
    std::unique_ptr<ASTNode> expression_;
    
public:
    AsyncAwaitExpression(std::unique_ptr<ASTNode> expression);
    
    Value evaluate(Context& ctx);
    
    static bool is_awaitable(const Value& value);
    
    static std::unique_ptr<Promise> to_promise(const Value& value, Context& ctx);
};


class AsyncGenerator : public Object {
public:
    enum class State {
        SuspendedStart,
        SuspendedYield,
        Completed
    };
    
    struct AsyncGeneratorResult {
        std::unique_ptr<Promise> promise;
        
        AsyncGeneratorResult(std::unique_ptr<Promise> p) : promise(std::move(p)) {}
    };

private:
    AsyncFunction* generator_function_;
    Context* generator_context_;
    std::unique_ptr<ASTNode> body_;
    State state_;
    
public:
    AsyncGenerator(AsyncFunction* gen_func, Context* ctx, std::unique_ptr<ASTNode> body);
    virtual ~AsyncGenerator() = default;
    
    AsyncGeneratorResult next(const Value& value = Value());
    AsyncGeneratorResult return_value(const Value& value);
    AsyncGeneratorResult throw_exception(const Value& exception);
    
    Value get_async_iterator();
    
    State get_state() const { return state_; }
    bool is_done() const { return state_ == State::Completed; }
    
    static Value async_generator_next(Context& ctx, const std::vector<Value>& args);
    static Value async_generator_return(Context& ctx, const std::vector<Value>& args);
    static Value async_generator_throw(Context& ctx, const std::vector<Value>& args);
    
    static void setup_async_generator_prototype(Context& ctx);
};


class AsyncIterator : public Object {
public:
    using AsyncNextFunction = std::function<std::unique_ptr<Promise>()>;
    
private:
    AsyncNextFunction next_fn_;
    bool done_;
    
public:
    AsyncIterator(AsyncNextFunction next_fn);
    virtual ~AsyncIterator() = default;
    
    std::unique_ptr<Promise> next();
    std::unique_ptr<Promise> return_value(const Value& value);
    std::unique_ptr<Promise> throw_exception(const Value& exception);
    
    static Value async_iterator_next(Context& ctx, const std::vector<Value>& args);
    static Value async_iterator_return(Context& ctx, const std::vector<Value>& args);
    static Value async_iterator_throw(Context& ctx, const std::vector<Value>& args);
    
    static void setup_async_iterator_prototype(Context& ctx);
};


namespace AsyncUtils {
    bool is_promise(const Value& value);
    bool is_thenable(const Value& value);
    
    std::unique_ptr<Promise> to_promise(const Value& value, Context& ctx);
    std::unique_ptr<Promise> promise_all(const std::vector<Value>& promises, Context& ctx);
    std::unique_ptr<Promise> promise_race(const std::vector<Value>& promises, Context& ctx);
    std::unique_ptr<Promise> promise_all_settled(const std::vector<Value>& promises, Context& ctx);
    std::unique_ptr<Promise> promise_resolve(const Value& value, Context& ctx);
    std::unique_ptr<Promise> promise_reject(const Value& reason, Context& ctx);
    
    std::unique_ptr<Promise> promise_with_resolvers(Context& ctx);
    std::unique_ptr<Promise> promise_try(std::function<Value()> fn, Context& ctx);
    
    void for_await_of_loop(const Value& async_iterable, 
                          std::function<std::unique_ptr<Promise>(const Value&)> callback, 
                          Context& ctx);
    
    void setup_async_functions(Context& ctx);
}


class EventLoop {
private:
    std::vector<std::function<void()>> microtasks_;
    std::vector<std::function<void()>> macrotasks_;
    bool running_;
    
public:
    EventLoop();
    ~EventLoop() = default;
    
    void schedule_microtask(std::function<void()> task);
    void schedule_macrotask(std::function<void()> task);
    
    void run();
    void stop();
    bool is_running() const { return running_; }
    
    void process_microtasks();
    void process_macrotasks();
    
    static EventLoop& instance();
};

}
