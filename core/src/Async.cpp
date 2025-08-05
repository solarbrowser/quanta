#include "Async.h"
#include "Context.h"
#include "Symbol.h"
#include "../../parser/include/AST.h"
#include <iostream>
#include <chrono>

namespace Quanta {

//=============================================================================
// AsyncFunction Implementation
//=============================================================================

AsyncFunction::AsyncFunction(const std::string& name, 
                           const std::vector<std::string>& params,
                           std::unique_ptr<ASTNode> body,
                           Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {
}

Value AsyncFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    (void)this_value; // Unused parameter
    
    // Create and return a promise
    auto promise = execute_async(ctx, args);
    return Value(promise.release());
}

std::unique_ptr<Promise> AsyncFunction::execute_async(Context& ctx, const std::vector<Value>& args) {
    // Simplified approach: execute in current context with parameter binding
    // This avoids complex Context creation issues
    
    // Create promise for async execution
    auto promise = std::make_unique<Promise>(&ctx);
    
    // Bind parameters to current context temporarily
    const auto& params = get_parameters();
    std::vector<std::pair<std::string, Value>> old_bindings;
    
    // Save old parameter bindings and set new ones
    for (size_t i = 0; i < params.size(); ++i) {
        Value arg = i < args.size() ? args[i] : Value();
        
        // Save old binding if it exists
        try {
            Value old_value = ctx.get_binding(params[i]);
            old_bindings.push_back({params[i], old_value});
        } catch (...) {
            // Parameter didn't exist before, that's fine
            old_bindings.push_back({params[i], Value()}); // Use undefined as sentinel
        }
        
        // Create new binding
        ctx.create_binding(params[i], arg);
    }
    
    // Execute function body
    try {
        if (body_) {
            Value result = body_->evaluate(ctx);
            if (ctx.has_exception()) {
                promise->reject(ctx.get_exception());
                ctx.clear_exception(); // Clear the exception
            } else {
                promise->fulfill(result);
            }
        } else {
            promise->fulfill(Value());
        }
    } catch (const std::exception& e) {
        promise->reject(Value(e.what()));
    }
    
    // Restore old parameter bindings
    for (const auto& binding : old_bindings) {
        if (!binding.second.is_undefined()) {
            ctx.create_binding(binding.first, binding.second);
        }
        // If old value was undefined, we leave the new binding in place
    }
    
    return promise;
}

void AsyncFunction::execute_async_body(Context& ctx, Promise* promise) {
    try {
        if (body_) {
            Value result = body_->evaluate(ctx);
            promise->fulfill(result);
        } else {
            promise->fulfill(Value());
        }
    } catch (const std::exception& e) {
        promise->reject(Value(e.what()));
    }
}

//=============================================================================
// AsyncAwaitExpression Implementation
//=============================================================================

AsyncAwaitExpression::AsyncAwaitExpression(std::unique_ptr<ASTNode> expression)
    : expression_(std::move(expression)) {
}

Value AsyncAwaitExpression::evaluate(Context& ctx) {
    if (!expression_) {
        return Value();
    }
    
    Value awaited_value = expression_->evaluate(ctx);
    
    // If the value is already a resolved value, return it
    if (!is_awaitable(awaited_value)) {
        return awaited_value;
    }
    
    // Convert to promise and wait for resolution
    auto promise = to_promise(awaited_value, ctx);
    if (!promise) {
        return awaited_value;
    }
    
    // In a real implementation, this would suspend the async function
    // For now, we'll simulate synchronous waiting
    while (promise->get_state() == PromiseState::PENDING) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        EventLoop::instance().process_microtasks();
    }
    
    if (promise->get_state() == PromiseState::FULFILLED) {
        return promise->get_value();
    } else {
        ctx.throw_exception(promise->get_value());
        return Value();
    }
}

bool AsyncAwaitExpression::is_awaitable(const Value& value) {
    return AsyncUtils::is_promise(value) || AsyncUtils::is_thenable(value);
}

std::unique_ptr<Promise> AsyncAwaitExpression::to_promise(const Value& value, Context& ctx) {
    return AsyncUtils::to_promise(value, ctx);
}

//=============================================================================
// AsyncGenerator Implementation
//=============================================================================

AsyncGenerator::AsyncGenerator(AsyncFunction* gen_func, Context* ctx, std::unique_ptr<ASTNode> body)
    : Object(ObjectType::Custom), generator_function_(gen_func), generator_context_(ctx),
      body_(std::move(body)), state_(State::SuspendedStart) {
}

AsyncGenerator::AsyncGeneratorResult AsyncGenerator::next(const Value& value) {
    (void)value; // Unused parameter for now
    
    if (state_ == State::Completed) {
        auto promise = std::make_unique<Promise>(generator_context_);
        
        // Create iterator result object
        auto result_obj = ObjectFactory::create_object();
        result_obj->set_property("value", Value());
        result_obj->set_property("done", Value(true));
        
        promise->fulfill(Value(result_obj.release()));
        return AsyncGeneratorResult(std::move(promise));
    }
    
    // Create promise for async generator result
    auto promise = std::make_unique<Promise>(generator_context_);
    
    // Schedule async execution
    EventLoop::instance().schedule_microtask([this, promise_ptr = promise.get()]() {
        try {
            if (body_) {
                Value result = body_->evaluate(*generator_context_);
                
                // Create iterator result object
                auto result_obj = ObjectFactory::create_object();
                result_obj->set_property("value", result);
                result_obj->set_property("done", Value(false));
                
                promise_ptr->fulfill(Value(result_obj.release()));
            } else {
                state_ = State::Completed;
                
                auto result_obj = ObjectFactory::create_object();
                result_obj->set_property("value", Value());
                result_obj->set_property("done", Value(true));
                
                promise_ptr->fulfill(Value(result_obj.release()));
            }
        } catch (const std::exception& e) {
            promise_ptr->reject(Value(e.what()));
        }
    });
    
    return AsyncGeneratorResult(std::move(promise));
}

AsyncGenerator::AsyncGeneratorResult AsyncGenerator::return_value(const Value& value) {
    state_ = State::Completed;
    
    auto promise = std::make_unique<Promise>(generator_context_);
    
    // Create iterator result object
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", value);
    result_obj->set_property("done", Value(true));
    
    promise->fulfill(Value(result_obj.release()));
    return AsyncGeneratorResult(std::move(promise));
}

AsyncGenerator::AsyncGeneratorResult AsyncGenerator::throw_exception(const Value& exception) {
    state_ = State::Completed;
    
    auto promise = std::make_unique<Promise>(generator_context_);
    promise->reject(exception);
    
    return AsyncGeneratorResult(std::move(promise));
}

Value AsyncGenerator::get_async_iterator() {
    return Value(this);
}

void AsyncGenerator::setup_async_generator_prototype(Context& ctx) {
    // Create AsyncGenerator.prototype
    auto async_gen_prototype = ObjectFactory::create_object();
    
    // Add next method
    auto next_fn = ObjectFactory::create_native_function("next", async_generator_next);
    async_gen_prototype->set_property("next", Value(next_fn.release()));
    
    // Add return method
    auto return_fn = ObjectFactory::create_native_function("return", async_generator_return);
    async_gen_prototype->set_property("return", Value(return_fn.release()));
    
    // Add throw method
    auto throw_fn = ObjectFactory::create_native_function("throw", async_generator_throw);
    async_gen_prototype->set_property("throw", Value(throw_fn.release()));
    
    // Add Symbol.asyncIterator method
    Symbol* async_iterator_symbol = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
    if (async_iterator_symbol) {
        auto async_iterator_fn = ObjectFactory::create_native_function("@@asyncIterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; // Unused parameter
                return ctx.get_binding("this");
            });
        async_gen_prototype->set_property(async_iterator_symbol->to_string(), Value(async_iterator_fn.release()));
    }
    
    ctx.create_binding("AsyncGeneratorPrototype", Value(async_gen_prototype.release()));
}

Value AsyncGenerator::async_generator_next(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args; // Unused parameters
    // TODO: Implement async generator next
    return Value();
}

Value AsyncGenerator::async_generator_return(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args; // Unused parameters
    // TODO: Implement async generator return
    return Value();
}

Value AsyncGenerator::async_generator_throw(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args; // Unused parameters
    // TODO: Implement async generator throw
    return Value();
}

//=============================================================================
// AsyncIterator Implementation
//=============================================================================

AsyncIterator::AsyncIterator(AsyncNextFunction next_fn) 
    : Object(ObjectType::Custom), next_fn_(next_fn), done_(false) {
}

std::unique_ptr<Promise> AsyncIterator::next() {
    if (done_) {
        auto promise = std::make_unique<Promise>(nullptr);
        
        auto result_obj = ObjectFactory::create_object();
        result_obj->set_property("value", Value());
        result_obj->set_property("done", Value(true));
        
        promise->fulfill(Value(result_obj.release()));
        return promise;
    }
    
    return next_fn_();
}

std::unique_ptr<Promise> AsyncIterator::return_value(const Value& value) {
    done_ = true;
    
    auto promise = std::make_unique<Promise>(nullptr);
    
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", value);
    result_obj->set_property("done", Value(true));
    
    promise->fulfill(Value(result_obj.release()));
    return promise;
}

std::unique_ptr<Promise> AsyncIterator::throw_exception(const Value& exception) {
    done_ = true;
    
    auto promise = std::make_unique<Promise>(nullptr);
    promise->reject(exception);
    return promise;
}

void AsyncIterator::setup_async_iterator_prototype(Context& ctx) {
    // Create AsyncIterator.prototype
    auto async_iterator_prototype = ObjectFactory::create_object();
    
    // Add next method
    auto next_fn = ObjectFactory::create_native_function("next", async_iterator_next);
    async_iterator_prototype->set_property("next", Value(next_fn.release()));
    
    // Add return method
    auto return_fn = ObjectFactory::create_native_function("return", async_iterator_return);
    async_iterator_prototype->set_property("return", Value(return_fn.release()));
    
    // Add throw method
    auto throw_fn = ObjectFactory::create_native_function("throw", async_iterator_throw);
    async_iterator_prototype->set_property("throw", Value(throw_fn.release()));
    
    // Add Symbol.asyncIterator method
    Symbol* async_iterator_symbol = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
    if (async_iterator_symbol) {
        auto self_async_iterator_fn = ObjectFactory::create_native_function("@@asyncIterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; // Unused parameter
                return ctx.get_binding("this");
            });
        async_iterator_prototype->set_property(async_iterator_symbol->to_string(), Value(self_async_iterator_fn.release()));
    }
    
    ctx.create_binding("AsyncIteratorPrototype", Value(async_iterator_prototype.release()));
}

// AsyncIterator static methods
Value AsyncIterator::async_iterator_next(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args; // Unused parameters
    // TODO: Implement async iterator next
    return Value();
}

Value AsyncIterator::async_iterator_return(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args; // Unused parameters
    // TODO: Implement async iterator return
    return Value();
}

Value AsyncIterator::async_iterator_throw(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; (void)args; // Unused parameters
    // TODO: Implement async iterator throw
    return Value();
}

//=============================================================================
// AsyncUtils Implementation
//=============================================================================

namespace AsyncUtils {

bool is_promise(const Value& value) {
    if (!value.is_object()) {
        return false;
    }
    
    Object* obj = value.as_object();
    return obj->get_type() == Object::ObjectType::Promise;
}

bool is_thenable(const Value& value) {
    if (!value.is_object()) {
        return false;
    }
    
    Object* obj = value.as_object();
    return obj->has_property("then");
}

std::unique_ptr<Promise> to_promise(const Value& value, Context& ctx) {
    if (is_promise(value)) {
        // Clone the existing promise
        Object* promise_obj = value.as_object();
        Promise* existing_promise = static_cast<Promise*>(promise_obj);
        
        auto new_promise = std::make_unique<Promise>(&ctx);
        
        // Copy state if resolved
        if (existing_promise->get_state() == PromiseState::FULFILLED) {
            new_promise->fulfill(existing_promise->get_value());
        } else if (existing_promise->get_state() == PromiseState::REJECTED) {
            new_promise->reject(existing_promise->get_value());
        }
        
        return new_promise;
    }
    
    if (is_thenable(value)) {
        // Create promise and call then method
        auto promise = std::make_unique<Promise>(&ctx);
        
        Object* thenable = value.as_object();
        Value then_method = thenable->get_property("then");
        
        if (then_method.is_function()) {
            Function* then_fn = then_method.as_function();
            
            // Create resolve and reject functions
            auto resolve_fn = ObjectFactory::create_native_function("resolve", 
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; // Unused parameter
                    Value resolve_value = args.empty() ? Value() : args[0];
                    promise_ptr->fulfill(resolve_value);
                    return Value();
                });
                
            auto reject_fn = ObjectFactory::create_native_function("reject", 
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; // Unused parameter
                    Value reject_reason = args.empty() ? Value() : args[0];
                    promise_ptr->reject(reject_reason);
                    return Value();
                });
            
            // Call then with resolve and reject functions
            std::vector<Value> then_args = {
                Value(resolve_fn.release()),
                Value(reject_fn.release())
            };
            
            then_fn->call(ctx, then_args, value);
        }
        
        return promise;
    }
    
    // Create resolved promise
    auto promise = std::make_unique<Promise>(&ctx);
    promise->fulfill(value);
    return promise;
}

std::unique_ptr<Promise> promise_resolve(const Value& value, Context& ctx) {
    return to_promise(value, ctx);
}

std::unique_ptr<Promise> promise_reject(const Value& reason, Context& ctx) {
    auto promise = std::make_unique<Promise>(&ctx);
    promise->reject(reason);
    return promise;
}

void setup_async_functions(Context& ctx) {
    // Setup Promise static methods
    Value promise_constructor = ctx.get_binding("Promise");
    if (promise_constructor.is_function()) {
        Function* promise_fn = promise_constructor.as_function();
        
        // Promise.resolve
        auto resolve_fn = ObjectFactory::create_native_function("resolve", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Value value = args.empty() ? Value() : args[0];
                auto promise = promise_resolve(value, ctx);
                return Value(promise.release());
            });
        
        // Promise.reject
        auto reject_fn = ObjectFactory::create_native_function("reject", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Value reason = args.empty() ? Value() : args[0];
                auto promise = promise_reject(reason, ctx);
                return Value(promise.release());
            });
        
        promise_fn->set_property("resolve", Value(resolve_fn.release()));
        promise_fn->set_property("reject", Value(reject_fn.release()));
    }
}

} // namespace AsyncUtils

//=============================================================================
// EventLoop Implementation
//=============================================================================

EventLoop::EventLoop() : running_(false) {
}

void EventLoop::schedule_microtask(std::function<void()> task) {
    microtasks_.push_back(task);
}

void EventLoop::schedule_macrotask(std::function<void()> task) {
    macrotasks_.push_back(task);
}

void EventLoop::run() {
    running_ = true;
    
    while (running_ && (!microtasks_.empty() || !macrotasks_.empty())) {
        // Process all microtasks first
        process_microtasks();
        
        // Process one macrotask
        if (!macrotasks_.empty()) {
            auto task = macrotasks_.front();
            macrotasks_.erase(macrotasks_.begin());
            task();
        }
    }
}

void EventLoop::stop() {
    running_ = false;
}

void EventLoop::process_microtasks() {
    while (!microtasks_.empty()) {
        auto task = microtasks_.front();
        microtasks_.erase(microtasks_.begin());
        task();
    }
}

void EventLoop::process_macrotasks() {
    if (!macrotasks_.empty()) {
        auto task = macrotasks_.front();
        macrotasks_.erase(macrotasks_.begin());
        task();
    }
}

EventLoop& EventLoop::instance() {
    static EventLoop instance;
    return instance;
}

} // namespace Quanta