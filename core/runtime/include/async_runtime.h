/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include "../../include/Object.h"
#include <memory>
#include <future>
#include <functional>

namespace Quanta {

class Context;

/**
 * Async runtime for JavaScript async/await and Promise handling
 */
class AsyncRuntime {
public:
    enum class PromiseState {
        Pending,
        Fulfilled,
        Rejected
    };

    struct PromiseData {
        PromiseState state;
        Value value;
        std::vector<std::function<void(const Value&)>> fulfillment_handlers;
        std::vector<std::function<void(const Value&)>> rejection_handlers;

        PromiseData() : state(PromiseState::Pending) {}
    };

private:
    std::vector<std::unique_ptr<PromiseData>> active_promises_;
    std::queue<std::function<void()>> microtask_queue_;
    std::queue<std::function<void()>> task_queue_;
    std::mutex runtime_mutex_;

public:
    AsyncRuntime();
    ~AsyncRuntime();

    // Promise operations
    Value create_promise(Context& ctx);
    void resolve_promise(PromiseData* promise, const Value& value);
    void reject_promise(PromiseData* promise, const Value& reason);
    void add_fulfillment_handler(PromiseData* promise, std::function<void(const Value&)> handler);
    void add_rejection_handler(PromiseData* promise, std::function<void(const Value&)> handler);

    // Microtask queue
    void queue_microtask(std::function<void()> task);
    void drain_microtask_queue();

    // Task queue
    void queue_task(std::function<void()> task);
    void process_tasks();

    // Event loop operations
    void run_event_loop();
    void stop_event_loop();
    bool has_pending_tasks() const;

    // Async/await support
    Value create_async_function(Context& ctx, void* native_func);
    Value await_promise(Context& ctx, const Value& promise);

    // Static methods for JavaScript binding
    static Value promise_constructor(Context& ctx, const std::vector<Value>& args);
    static Value promise_resolve(Context& ctx, const std::vector<Value>& args);
    static Value promise_reject(Context& ctx, const std::vector<Value>& args);
    static Value promise_then(Context& ctx, const std::vector<Value>& args);
    static Value promise_catch(Context& ctx, const std::vector<Value>& args);

    // Setup
    static void setup_async_runtime(Context& ctx);

private:
    void process_promise_queue();
    PromiseData* get_promise_data(Object* promise_obj);
};

} // namespace Quanta