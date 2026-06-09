/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Promise.h"
#include <memory>
#include <functional>
#include <vector>
#include <deque>
#include <ucontext.h>

namespace Quanta {

class Context;
class ASTNode;
class Function;
class Engine;
class Environment;

// Fiber-based async executor (ucontext_t). The body runs exactly once on a
// dedicated stack. `await expr` suspends via swapcontext; promise callbacks
// call resume() to swap back in.
// When `await expr` is hit, the fiber suspends via swapcontext.
// When the awaited Promise settles, resume() swaps back into the fiber.
class AsyncExecutor : public std::enable_shared_from_this<AsyncExecutor> {
public:
    AsyncExecutor(std::unique_ptr<ASTNode> body,
                  std::unique_ptr<Context> exec_ctx,
                  Promise* outer_promise,
                  Engine* engine);
    ~AsyncExecutor();

    void run();                                       // start or resume fiber (from microtask)
    void resume(Value result, bool is_throw = false); // called by promise callbacks

    static AsyncExecutor* get_current() { return current_; }

    // Await-suspension state (written before suspend, read after resume)
    Value await_result_;
    bool  await_is_throw_ = false;

    Promise* outer_promise_;
    std::unique_ptr<Context> exec_context_owned_;
    Context* exec_context_;
    Engine*  engine_;

    // Fiber infrastructure
    static constexpr size_t STACK_SIZE = 2 * 1024 * 1024;
    ucontext_t fiber_ctx_;
    ucontext_t caller_ctx_;
    std::vector<char> fiber_stack_;

private:
    std::unique_ptr<ASTNode> body_;
    static thread_local AsyncExecutor* current_;
    static void fiber_entry(uint32_t lo, uint32_t hi);
};


class Parameter;

class AsyncFunction : public Function {
private:
    std::unique_ptr<ASTNode> body_;
    std::vector<std::unique_ptr<Parameter>> ast_params_;

public:
    AsyncFunction(const std::string& name,
                  const std::vector<std::string>& params,
                  std::unique_ptr<ASTNode> body,
                  Context* closure_context);
    AsyncFunction(const std::string& name,
                  std::vector<std::unique_ptr<Parameter>> params,
                  std::unique_ptr<ASTNode> body,
                  Context* closure_context);

    Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value()) override;

private:
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

    // Two reasons the fiber can suspend:
    // Yield  → body yielded a value; fulfill the pending promise
    // Await  → body hit an `await`; wait for the awaited promise to settle
    enum class SuspendReason { Yield, Await, Done };

private:
    std::unique_ptr<Context> context_owned_;
    Context* generator_context_;
    Context* outer_context_;

public:
    Context* get_generator_context() const { return generator_context_; }
    Context* get_outer_context() const { return outer_context_; }
    std::unique_ptr<ASTNode> body_;
    State state_;

    // Fiber infrastructure
    static constexpr size_t STACK_SIZE = 2 * 1024 * 1024;
    ucontext_t fiber_ctx_;
    ucontext_t caller_ctx_;
    std::vector<char> fiber_stack_;

    // Yield protocol (written by fiber, read by caller after suspend)
    SuspendReason suspend_reason_ = SuspendReason::Done;
    Value yield_value_;        // value from `yield expr`
    Value return_value_;       // final return value
    bool  has_exception_ = false;
    Value exception_value_;

    // next()/throw()/return() input (written by caller, read by fiber after resume)
    Value sent_value_;
    bool  throwing_    = false;  // throw the sent_value_ into generator
    bool  returning_   = false;  // return(sent_value_) — close generator
    Value return_arg_;

    // Await protocol (internal — fiber suspends/resumes transparently)
    Value await_result_;
    bool  await_is_throw_ = false;

    // The promise belonging to the currently-in-flight next()/return()/throw() call
    Promise* pending_promise_ = nullptr;

    // ES2018 27.6.3.x: AsyncGenerator request queue. next()/return()/throw()
    // calls made while a request is already in flight must queue and run
    // strictly in order, one at a time, as the generator suspends.
    struct Request {
        enum class Type { Next, Return, Throw } type;
        Value value;
        Promise* promise;
        std::string pin_key;  // GC-keepalive property name on `this`
    };
    std::deque<Request> request_queue_;
    uint32_t request_pin_counter_ = 0;

public:
    AsyncGenerator(std::unique_ptr<Context> ctx, std::unique_ptr<ASTNode> body, Context* outer_ctx = nullptr);
    virtual ~AsyncGenerator() = default;

    AsyncGeneratorResult next(const Value& value = Value());
    AsyncGeneratorResult return_value(const Value& value);
    AsyncGeneratorResult throw_exception(const Value& exception);

    // Resume fiber after an `await` inside the generator settled
    void resume_from_await(Value result, bool is_throw = false);

    Value get_async_iterator();

    State get_state() const { return state_; }
    bool is_done() const { return state_ == State::Completed; }

    static Value async_generator_next(Context& ctx, const std::vector<Value>& args);
    static Value async_generator_return(Context& ctx, const std::vector<Value>& args);
    static Value async_generator_throw(Context& ctx, const std::vector<Value>& args);

    static void setup_async_generator_prototype(Context& ctx);
    static Object* s_async_generator_prototype_;

    static AsyncGenerator* get_current() { return current_; }
    static void set_current(AsyncGenerator* g) { current_ = g; }

private:
    // Enter/re-enter the fiber; after swapcontext returns, handle the suspend reason.
    void enter_fiber();
    // Called after fiber suspends; fulfills/rejects pending_promise_ when appropriate.
    void handle_suspension();

    // Enqueue a next()/return()/throw() request and start processing it if idle.
    AsyncGeneratorResult enqueue_request(Request::Type type, const Value& value, std::unique_ptr<Promise> promise);
    // Pop the settled front request and kick off the next queued one, if any.
    void advance_queue();
    // Start processing request_queue_.front() -- assumes pending_promise_ == nullptr.
    void process_next_request();

    static thread_local AsyncGenerator* current_;
    static void fiber_entry(uint32_t lo, uint32_t hi);
};


class AsyncGeneratorFunction : public Function {
    std::unique_ptr<ASTNode> body_;
public:
    AsyncGeneratorFunction(const std::string& name,
                           const std::vector<std::string>& params,
                           std::unique_ptr<ASTNode> body,
                           Context* closure_context);
    AsyncGeneratorFunction(const std::string& name,
                           std::vector<std::unique_ptr<class Parameter>> params,
                           std::unique_ptr<ASTNode> body,
                           Context* closure_context);
    Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value()) override;
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
