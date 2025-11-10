/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/async_runtime.h"
#include "../../include/Context.h"
#include <iostream>
#include <future>

namespace Quanta {

AsyncRuntime::AsyncRuntime() : running_(false), event_loop_thread_() {
}

AsyncRuntime::~AsyncRuntime() {
    shutdown();
}

void AsyncRuntime::initialize() {
    if (running_) return;

    running_ = true;
    event_loop_thread_ = std::thread(&AsyncRuntime::event_loop, this);
}

void AsyncRuntime::shutdown() {
    if (!running_) return;

    running_ = false;

    // Wake up event loop
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push([]() { /* dummy task to wake up loop */ });
    }
    queue_condition_.notify_one();

    if (event_loop_thread_.joinable()) {
        event_loop_thread_.join();
    }

    // Clear remaining tasks
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!task_queue_.empty()) {
        task_queue_.pop();
    }
}

Promise* AsyncRuntime::create_promise() {
    return new Promise();
}

void AsyncRuntime::schedule_task(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push(std::move(task));
    }
    queue_condition_.notify_one();
}

void AsyncRuntime::schedule_microtask(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(microtask_mutex_);
        microtask_queue_.push(std::move(task));
    }
}

void AsyncRuntime::run_microtasks() {
    std::lock_guard<std::mutex> lock(microtask_mutex_);

    while (!microtask_queue_.empty()) {
        auto task = microtask_queue_.front();
        microtask_queue_.pop();

        try {
            task();
        } catch (const std::exception& e) {
            std::cerr << "Microtask error: " << e.what() << std::endl;
        }
    }
}

void AsyncRuntime::set_timeout(std::function<void()> callback, int32_t delay_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);

    Timer timer;
    timer.callback = std::move(callback);
    timer.deadline = deadline;
    timer.interval_ms = 0; // One-shot timer
    timer.active = true;

    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        timers_.push_back(timer);
    }
}

void AsyncRuntime::set_interval(std::function<void()> callback, int32_t interval_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms);

    Timer timer;
    timer.callback = std::move(callback);
    timer.deadline = deadline;
    timer.interval_ms = interval_ms;
    timer.active = true;

    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        timers_.push_back(timer);
    }
}

void AsyncRuntime::process_timers() {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    auto now = std::chrono::steady_clock::now();

    for (auto it = timers_.begin(); it != timers_.end();) {
        if (!it->active) {
            it = timers_.erase(it);
            continue;
        }

        if (now >= it->deadline) {
            // Timer expired, execute callback
            auto callback = it->callback;

            if (it->interval_ms > 0) {
                // Recurring timer - reschedule
                it->deadline = now + std::chrono::milliseconds(it->interval_ms);
                ++it;
            } else {
                // One-shot timer - remove
                it = timers_.erase(it);
            }

            // Execute callback outside of lock
            {
                std::lock_guard<std::mutex> unlock(timer_mutex_);
                try {
                    callback();
                } catch (const std::exception& e) {
                    std::cerr << "Timer callback error: " << e.what() << std::endl;
                }
            }
        } else {
            ++it;
        }
    }
}

void AsyncRuntime::event_loop() {
    while (running_) {
        // Process microtasks first
        run_microtasks();

        // Process timers
        process_timers();

        // Process main task queue
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // Wait for tasks or timeout
            queue_condition_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return !task_queue_.empty() || !running_;
            });

            if (!running_) break;

            if (!task_queue_.empty()) {
                task = task_queue_.front();
                task_queue_.pop();
            }
        }

        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "Task error: " << e.what() << std::endl;
            }
        }
    }
}

// Promise Implementation
Promise::Promise() : state_(PromiseState::PENDING), has_value_(false) {
}

Promise::~Promise() {
}

void Promise::resolve(const Value& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != PromiseState::PENDING) return;

    state_ = PromiseState::FULFILLED;
    value_ = value;
    has_value_ = true;

    // Execute fulfillment handlers
    for (auto& handler : fulfillment_handlers_) {
        try {
            handler(value);
        } catch (const std::exception& e) {
            std::cerr << "Promise fulfillment handler error: " << e.what() << std::endl;
        }
    }

    fulfillment_handlers_.clear();
    rejection_handlers_.clear();
}

void Promise::reject(const Value& reason) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != PromiseState::PENDING) return;

    state_ = PromiseState::REJECTED;
    value_ = reason;
    has_value_ = true;

    // Execute rejection handlers
    for (auto& handler : rejection_handlers_) {
        try {
            handler(reason);
        } catch (const std::exception& e) {
            std::cerr << "Promise rejection handler error: " << e.what() << std::endl;
        }
    }

    fulfillment_handlers_.clear();
    rejection_handlers_.clear();
}

Promise* Promise::then(std::function<Value(const Value&)> on_fulfilled,
                      std::function<Value(const Value&)> on_rejected) {
    std::lock_guard<std::mutex> lock(mutex_);

    Promise* new_promise = new Promise();

    auto fulfillment_wrapper = [new_promise, on_fulfilled](const Value& value) {
        try {
            if (on_fulfilled) {
                Value result = on_fulfilled(value);
                new_promise->resolve(result);
            } else {
                new_promise->resolve(value);
            }
        } catch (...) {
            new_promise->reject(Value("Promise fulfillment error"));
        }
    };

    auto rejection_wrapper = [new_promise, on_rejected](const Value& reason) {
        try {
            if (on_rejected) {
                Value result = on_rejected(reason);
                new_promise->resolve(result);
            } else {
                new_promise->reject(reason);
            }
        } catch (...) {
            new_promise->reject(Value("Promise rejection error"));
        }
    };

    if (state_ == PromiseState::FULFILLED) {
        fulfillment_wrapper(value_);
    } else if (state_ == PromiseState::REJECTED) {
        rejection_wrapper(value_);
    } else {
        fulfillment_handlers_.push_back(fulfillment_wrapper);
        rejection_handlers_.push_back(rejection_wrapper);
    }

    return new_promise;
}

Promise* Promise::catch_error(std::function<Value(const Value&)> on_rejected) {
    return then(nullptr, on_rejected);
}

bool Promise::is_fulfilled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == PromiseState::FULFILLED;
}

bool Promise::is_rejected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == PromiseState::REJECTED;
}

bool Promise::is_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == PromiseState::PENDING;
}

Value Promise::get_value() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_value_ ? value_ : Value();
}

// Static factory methods for JavaScript binding
Value AsyncRuntime::create_promise(Context& ctx, const std::vector<Value>& args) {
    auto promise = new Promise();
    return Value(static_cast<Object*>(promise));
}

Value AsyncRuntime::promise_resolve(Context& ctx, const std::vector<Value>& args) {
    auto promise = new Promise();
    Value value = args.empty() ? Value() : args[0];
    promise->resolve(value);
    return Value(static_cast<Object*>(promise));
}

Value AsyncRuntime::promise_reject(Context& ctx, const std::vector<Value>& args) {
    auto promise = new Promise();
    Value reason = args.empty() ? Value("Promise rejected") : args[0];
    promise->reject(reason);
    return Value(static_cast<Object*>(promise));
}

void AsyncRuntime::setup_promise_object(Context& ctx) {
    // Set up Promise constructor and static methods
    // This would be called during engine initialization
}

} // namespace Quanta