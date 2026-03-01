/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Async.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"
#include "quanta/lexer/Lexer.h"
#include "quanta/parser/Parser.h"
#include <iostream>
#include <chrono>

namespace Quanta {

// AsyncExecutor

thread_local AsyncExecutor* AsyncExecutor::current_ = nullptr;

AsyncExecutor::AsyncExecutor(std::unique_ptr<ASTNode> body,
                              std::unique_ptr<Context> exec_ctx,
                              Promise* outer_promise,
                              Engine* engine)
    : next_await_index_(0), target_await_index_(0),
      outer_promise_(outer_promise),
      exec_context_owned_(std::move(exec_ctx)),
      exec_context_(exec_context_owned_.get()),
      engine_(engine),
      initial_lex_env_(exec_context_ ? exec_context_->get_lexical_environment() : nullptr),
      body_(std::move(body)) {}

AsyncExecutor::~AsyncExecutor() = default;

void AsyncExecutor::run() {
    auto* prev = current_;
    current_ = this;
    next_await_index_ = 0;

    // Restore the lexical env to the initial state (as it was after parameter binding)
    // to prevent block scope accumulation across replayed runs.
    if (initial_lex_env_ && exec_context_) {
        exec_context_->set_lexical_environment(initial_lex_env_);
    }

    // Clear state from previous execution pass
    exec_context_->clear_exception();
    exec_context_->clear_return_value();

    try {
        Value result;
        if (body_) {
            result = body_->evaluate(*exec_context_);
        }
        current_ = prev;

        // Body ran to completion
        if (exec_context_->has_exception()) {
            Value exc = exec_context_->get_exception();
            exec_context_->clear_exception();
            outer_promise_->reject(exc);
        } else if (exec_context_->has_return_value()) {
            Value ret = exec_context_->get_return_value();
            exec_context_->clear_return_value();
            if (AsyncUtils::is_promise(ret)) {
                Promise* p = static_cast<Promise*>(ret.as_object());
                if (p->get_state() == PromiseState::FULFILLED) {
                    outer_promise_->fulfill(p->get_value());
                } else if (p->get_state() == PromiseState::REJECTED) {
                    outer_promise_->reject(p->get_value());
                } else {
                    outer_promise_->fulfill(ret);
                }
            } else {
                outer_promise_->fulfill(ret);
            }
        } else {
            outer_promise_->fulfill(result);
        }
    } catch (const AwaitSuspendException&) {
        // Suspended at an await — callbacks capture shared_ptr keeping us alive
        current_ = prev;
    } catch (...) {
        current_ = prev;
        outer_promise_->reject(Value(std::string("Async execution error")));
    }
}

// AsyncFunction

AsyncFunction::AsyncFunction(const std::string& name,
                           const std::vector<std::string>& params,
                           std::unique_ptr<ASTNode> body,
                           Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {

}

Value AsyncFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    // Create the outer promise returned to caller
    auto promise_obj = ObjectFactory::create_promise(&ctx);
    Promise* promise_raw = static_cast<Promise*>(promise_obj.get());
    Value promise_value(promise_obj.release());

    // Create a persistent function-level context for this execution
    auto exec_ctx = ContextFactory::create_function_context(ctx.get_engine(), &ctx, this);

    // Bind 'this' (use __arrow_this__ if arrow function)
    Value bound_this = this_value;
    Value arrow_this_val = get_property("__arrow_this__");
    if (!arrow_this_val.is_undefined()) {
        bound_this = arrow_this_val;
    }
    exec_ctx->create_binding("this", bound_this, true);

    // Bind parameters
    const auto& params = get_parameters();
    for (size_t i = 0; i < params.size(); ++i) {
        Value arg = i < args.size() ? args[i] : Value();
        exec_ctx->create_binding(params[i], arg);
    }

    // Clone body and start executor
    std::unique_ptr<ASTNode> body_clone = body_ ? body_->clone() : nullptr;
    auto executor = std::make_shared<AsyncExecutor>(
        std::move(body_clone), std::move(exec_ctx), promise_raw, ctx.get_engine());
    executor->run();

    return promise_value;
}

std::unique_ptr<Promise> AsyncFunction::execute_async(Context& ctx, const std::vector<Value>& args) {

    // Use create_promise so the returned promise has Promise.prototype in its chain,
    // which is required for `p instanceof Promise` to return true.
    auto promise_obj = ObjectFactory::create_promise(&ctx);
    std::unique_ptr<Promise> promise(static_cast<Promise*>(promise_obj.release()));

    const auto& params = get_parameters();
    std::vector<std::pair<std::string, Value>> old_bindings;

    for (size_t i = 0; i < params.size(); ++i) {
        Value arg = i < args.size() ? args[i] : Value();

        try {
            Value old_value = ctx.get_binding(params[i]);
            old_bindings.push_back({params[i], old_value});
        } catch (...) {
            old_bindings.push_back({params[i], Value()});
        }

        ctx.create_binding(params[i], arg);
    }

    try {
        if (body_) {
            Value result = body_->evaluate(ctx);
            // Capture and clear return value to prevent it from bleeding into the
            // caller's context and causing the caller to exit early.
            if (ctx.has_return_value()) {
                result = ctx.get_return_value();
                ctx.clear_return_value();
            }
            if (ctx.has_exception()) {
                Value exc = ctx.get_exception();
                ctx.clear_exception();
                promise->reject(exc);
            } else {
                promise->fulfill(result);
            }
        } else {
            promise->fulfill(Value());
        }
    } catch (const std::exception& e) {
        ctx.clear_return_value();
        promise->reject(Value(e.what()));
    }

    for (const auto& binding : old_bindings) {
        if (!binding.second.is_undefined()) {
            ctx.create_binding(binding.first, binding.second);
        }
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


AsyncAwaitExpression::AsyncAwaitExpression(std::unique_ptr<ASTNode> expression)
    : expression_(std::move(expression)) {
}

Value AsyncAwaitExpression::evaluate(Context& ctx) {
    AsyncExecutor* exec = AsyncExecutor::get_current();

    if (exec) {
        // Replay-based path 
        size_t await_index = exec->next_await_index_++;

        if (await_index < exec->target_await_index_) {
            // REPLAY: return stored result without re-evaluating the expression
            if (await_index < exec->await_is_throw_.size() && exec->await_is_throw_[await_index]) {
                ctx.throw_exception(exec->await_results_[await_index]);
                return Value();
            }
            if (await_index < exec->await_results_.size()) {
                return exec->await_results_[await_index];
            }
            return Value();
        }

        // NEW AWAIT: evaluate the expression for the first time
        if (!expression_) {
            exec->await_results_.push_back(Value());
            exec->await_is_throw_.push_back(false);
            exec->target_await_index_++;
            auto self = exec->shared_from_this();
            Context* gctx = exec->engine_ ? exec->engine_->get_current_context() : exec->exec_context_;
            if (gctx) gctx->queue_microtask([self]() mutable { self->run(); });
            throw AwaitSuspendException{};
        }

        Value expr_val = expression_->evaluate(ctx);
        if (ctx.has_exception()) return Value();

        // Get global context for the microtask queue
        Context* gctx = exec->engine_ ? exec->engine_->get_current_context() : exec->exec_context_;

        // Determine the state of the awaited value
        Promise* awaited_promise = nullptr;
        Value resolved_value;
        bool is_throw = false;
        bool is_pending = false;

        if (AsyncUtils::is_promise(expr_val)) {
            awaited_promise = static_cast<Promise*>(expr_val.as_object());
            if (awaited_promise->get_state() == PromiseState::FULFILLED) {
                resolved_value = awaited_promise->get_value();
            } else if (awaited_promise->get_state() == PromiseState::REJECTED) {
                resolved_value = awaited_promise->get_value();
                is_throw = true;
            } else {
                is_pending = true;
            }
        } else {
            resolved_value = expr_val;
        }

        if (is_pending) {
            // Register callbacks on the pending Promise to resume when it settles
            auto self = exec->shared_from_this();
            size_t target_idx = exec->target_await_index_;

            auto on_fulfill_fn = ObjectFactory::create_native_function("",
                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                    Value val = args.empty() ? Value() : args[0];
                    self->await_results_.push_back(val);
                    self->await_is_throw_.push_back(false);
                    self->target_await_index_++;
                    if (gctx) gctx->queue_microtask([self]() mutable { self->run(); });
                    return Value();
                });

            auto on_reject_fn = ObjectFactory::create_native_function("",
                [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                    Value reason = args.empty() ? Value() : args[0];
                    self->await_results_.push_back(reason);
                    self->await_is_throw_.push_back(true);
                    self->target_await_index_++;
                    if (gctx) gctx->queue_microtask([self]() mutable { self->run(); });
                    return Value();
                });

            Function* fulfill_raw = on_fulfill_fn.get();
            Function* reject_raw = on_reject_fn.get();

            // Store on the awaited Promise to keep native functions alive
            std::string suffix = std::to_string(target_idx);
            awaited_promise->set_property("__af__" + suffix, Value(on_fulfill_fn.release()));
            awaited_promise->set_property("__ar__" + suffix, Value(on_reject_fn.release()));

            awaited_promise->then(fulfill_raw, reject_raw);

            throw AwaitSuspendException{};
        }

        // Already settled: store result and queue re-run as microtask
        exec->await_results_.push_back(resolved_value);
        exec->await_is_throw_.push_back(is_throw);
        exec->target_await_index_++;
        auto self = exec->shared_from_this();
        if (gctx) gctx->queue_microtask([self]() mutable { self->run(); });
        throw AwaitSuspendException{};
    }

    //  Fallback path (no executor active) 
    if (!expression_) return Value();
    Value awaited_value = expression_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    if (!is_awaitable(awaited_value)) return awaited_value;

    auto promise = to_promise(awaited_value, ctx);
    if (!promise) return awaited_value;

    if (promise->get_state() == PromiseState::FULFILLED) {
        return promise->get_value();
    } else if (promise->get_state() == PromiseState::REJECTED) {
        ctx.throw_exception(promise->get_value());
        return Value();
    }
    return Value();
}

bool AsyncAwaitExpression::is_awaitable(const Value& value) {
    return AsyncUtils::is_promise(value) || AsyncUtils::is_thenable(value);
}

std::unique_ptr<Promise> AsyncAwaitExpression::to_promise(const Value& value, Context& ctx) {
    return AsyncUtils::to_promise(value, ctx);
}


AsyncGenerator::AsyncGenerator(AsyncFunction* gen_func, Context* ctx, std::unique_ptr<ASTNode> body)
    : Object(ObjectType::Custom), generator_function_(gen_func), generator_context_(ctx),
      body_(std::move(body)), state_(State::SuspendedStart) {
}

AsyncGenerator::AsyncGeneratorResult AsyncGenerator::next(const Value& value) {
    (void)value;
    
    if (state_ == State::Completed) {
        auto promise = std::make_unique<Promise>(generator_context_);
        
        auto result_obj = ObjectFactory::create_object();
        result_obj->set_property("value", Value());
        result_obj->set_property("done", Value(true));
        
        promise->fulfill(Value(result_obj.release()));
        return AsyncGeneratorResult(std::move(promise));
    }
    
    auto promise = std::make_unique<Promise>(generator_context_);
    
    EventLoop::instance().schedule_microtask([this, promise_ptr = promise.get()]() {
        try {
            if (body_) {
                Value result = body_->evaluate(*generator_context_);
                
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
    auto async_gen_prototype = ObjectFactory::create_object();
    
    auto next_fn = ObjectFactory::create_native_function("next", async_generator_next);
    async_gen_prototype->set_property("next", Value(next_fn.release()));
    
    auto return_fn = ObjectFactory::create_native_function("return", async_generator_return);
    async_gen_prototype->set_property("return", Value(return_fn.release()));
    
    auto throw_fn = ObjectFactory::create_native_function("throw", async_generator_throw);
    async_gen_prototype->set_property("throw", Value(throw_fn.release()));
    
    Symbol* async_iterator_symbol = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
    if (async_iterator_symbol) {
        auto async_iterator_fn = ObjectFactory::create_native_function("@@asyncIterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                return ctx.get_binding("this");
            });
        async_gen_prototype->set_property(async_iterator_symbol->to_string(), Value(async_iterator_fn.release()));
    }
    
    ctx.create_binding("AsyncGeneratorPrototype", Value(async_gen_prototype.release()));
}

Value AsyncGenerator::async_generator_next(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncGenerator.next called on non-object")));
        return Value();
    }
    
    AsyncGenerator* async_gen = dynamic_cast<AsyncGenerator*>(this_value.as_object());
    if (!async_gen) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncGenerator.next called on wrong type")));
        return Value();
    }
    
    Value value = args.empty() ? Value() : args[0];
    auto result = async_gen->next(value);
    
    return Value(result.promise.release());
}

Value AsyncGenerator::async_generator_return(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncGenerator.return called on non-object")));
        return Value();
    }
    
    AsyncGenerator* async_gen = dynamic_cast<AsyncGenerator*>(this_value.as_object());
    if (!async_gen) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncGenerator.return called on wrong type")));
        return Value();
    }
    
    Value value = args.empty() ? Value() : args[0];
    auto result = async_gen->return_value(value);
    
    return Value(result.promise.release());
}

Value AsyncGenerator::async_generator_throw(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncGenerator.throw called on non-object")));
        return Value();
    }
    
    AsyncGenerator* async_gen = dynamic_cast<AsyncGenerator*>(this_value.as_object());
    if (!async_gen) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncGenerator.throw called on wrong type")));
        return Value();
    }
    
    Value exception = args.empty() ? Value() : args[0];
    auto result = async_gen->throw_exception(exception);
    
    return Value(result.promise.release());
}


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
    auto async_iterator_prototype = ObjectFactory::create_object();
    
    auto next_fn = ObjectFactory::create_native_function("next", async_iterator_next);
    async_iterator_prototype->set_property("next", Value(next_fn.release()));
    
    auto return_fn = ObjectFactory::create_native_function("return", async_iterator_return);
    async_iterator_prototype->set_property("return", Value(return_fn.release()));
    
    auto throw_fn = ObjectFactory::create_native_function("throw", async_iterator_throw);
    async_iterator_prototype->set_property("throw", Value(throw_fn.release()));
    
    Symbol* async_iterator_symbol = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
    if (async_iterator_symbol) {
        auto self_async_iterator_fn = ObjectFactory::create_native_function("@@asyncIterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                return ctx.get_binding("this");
            });
        async_iterator_prototype->set_property(async_iterator_symbol->to_string(), Value(self_async_iterator_fn.release()));
    }
    
    ctx.create_binding("AsyncIteratorPrototype", Value(async_iterator_prototype.release()));
}

Value AsyncIterator::async_iterator_next(Context& ctx, const std::vector<Value>& /* args */) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncIterator.next called on non-object")));
        return Value();
    }
    
    AsyncIterator* async_iter = dynamic_cast<AsyncIterator*>(this_value.as_object());
    if (!async_iter) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncIterator.next called on wrong type")));
        return Value();
    }
    
    auto promise = async_iter->next();
    
    return Value(promise.release());
}

Value AsyncIterator::async_iterator_return(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncIterator.return called on non-object")));
        return Value();
    }
    
    AsyncIterator* async_iter = dynamic_cast<AsyncIterator*>(this_value.as_object());
    if (!async_iter) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncIterator.return called on wrong type")));
        return Value();
    }
    
    Value value = args.empty() ? Value() : args[0];
    auto promise = async_iter->return_value(value);
    
    return Value(promise.release());
}

Value AsyncIterator::async_iterator_throw(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncIterator.throw called on non-object")));
        return Value();
    }
    
    AsyncIterator* async_iter = dynamic_cast<AsyncIterator*>(this_value.as_object());
    if (!async_iter) {
        ctx.throw_exception(Value(std::string("TypeError: AsyncIterator.throw called on wrong type")));
        return Value();
    }
    
    Value exception = args.empty() ? Value() : args[0];
    auto promise = async_iter->throw_exception(exception);
    
    return Value(promise.release());
}


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
        Object* promise_obj = value.as_object();
        Promise* existing_promise = static_cast<Promise*>(promise_obj);
        
        auto new_promise = std::make_unique<Promise>(&ctx);
        
        if (existing_promise->get_state() == PromiseState::FULFILLED) {
            new_promise->fulfill(existing_promise->get_value());
        } else if (existing_promise->get_state() == PromiseState::REJECTED) {
            new_promise->reject(existing_promise->get_value());
        }
        
        return new_promise;
    }
    
    if (is_thenable(value)) {
        auto promise = std::make_unique<Promise>(&ctx);
        
        Object* thenable = value.as_object();
        Value then_method = thenable->get_property("then");
        
        if (then_method.is_function()) {
            Function* then_fn = then_method.as_function();
            
            auto resolve_fn = ObjectFactory::create_native_function("resolve", 
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value resolve_value = args.empty() ? Value() : args[0];
                    promise_ptr->fulfill(resolve_value);
                    return Value();
                });
                
            auto reject_fn = ObjectFactory::create_native_function("reject", 
                [promise_ptr = promise.get()](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    Value reject_reason = args.empty() ? Value() : args[0];
                    promise_ptr->reject(reject_reason);
                    return Value();
                });
            
            std::vector<Value> then_args = {
                Value(resolve_fn.release()),
                Value(reject_fn.release())
            };
            
            then_fn->call(ctx, then_args, value);
        }
        
        return promise;
    }
    
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
    Value promise_constructor = ctx.get_binding("Promise");
    if (promise_constructor.is_function()) {
        Function* promise_fn = promise_constructor.as_function();
        
        auto resolve_fn = ObjectFactory::create_native_function("resolve", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Value value = args.empty() ? Value() : args[0];
                auto promise = promise_resolve(value, ctx);
                return Value(promise.release());
            });
        
        auto reject_fn = ObjectFactory::create_native_function("reject", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                Value reason = args.empty() ? Value() : args[0];
                auto promise = promise_reject(reason, ctx);
                return Value(promise.release());
            });
        
        promise_fn->set_property("resolve", Value(resolve_fn.release()));
        promise_fn->set_property("reject", Value(reject_fn.release()));
    }

    auto async_function_constructor = ObjectFactory::create_native_function("AsyncFunction",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string params_str = "";
            std::string body_str = "";

            if (args.size() > 1) {
                body_str = args.back().to_string();
                for (size_t i = 0; i < args.size() - 1; ++i) {
                    if (i > 0) params_str += ", ";
                    params_str += args[i].to_string();
                }
            } else if (args.size() == 1) {
                body_str = args[0].to_string();
            }

            // Parse and compile the async function body
            std::string func_code = "(async function(" + params_str + ") { " + body_str + " })";
            try {
                Lexer lexer(func_code);
                TokenSequence tokens = lexer.tokenize();
                Parser::ParseOptions opts;
                Parser parser(tokens, opts);
                auto expr = parser.parse_expression();
                if (parser.has_errors() || !expr) {
                    ctx.throw_syntax_error("Invalid async function body");
                    return Value();
                }
                return expr->evaluate(ctx);
            } catch (...) {
                ctx.throw_syntax_error("Invalid async function body in AsyncFunction constructor");
                return Value();
            }
        });

    async_function_constructor->set_property("name", Value(std::string("AsyncFunction")));

    // Build AsyncFunction.prototype with correct prototype chain:
    // AsyncFunction.prototype[[Prototype]] = Function.prototype
    auto async_fn_proto = ObjectFactory::create_object();
    Object* fn_proto = ObjectFactory::get_function_prototype();
    if (fn_proto) {
        async_fn_proto->set_prototype(fn_proto);
    }
    // Symbol.toStringTag = "AsyncFunction"
    Symbol* to_string_tag = Symbol::get_well_known(Symbol::TO_STRING_TAG);
    if (to_string_tag) {
        async_fn_proto->set_property(to_string_tag->to_property_key(),
            Value(std::string("AsyncFunction")), PropertyAttributes::Configurable);
    }

    // AsyncFunction.prototype.constructor = AsyncFunction (must be set before releasing proto)
    async_fn_proto->set_property("constructor", Value(async_function_constructor.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    Object* async_fn_proto_ptr = async_fn_proto.get();
    async_function_constructor->set_property("prototype", Value(async_fn_proto.release()), PropertyAttributes::None);

    // Store raw pointer on constructor so AsyncFunctionExpression can find it
    async_function_constructor->set_property("__asyncProtoPtr__",
        Value(async_fn_proto_ptr), PropertyAttributes::None);

    ctx.create_binding("AsyncFunction", Value(async_function_constructor.release()));

}

}


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
        process_microtasks();
        
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
        auto task = std::move(microtasks_.front());
        microtasks_.erase(microtasks_.begin());
        
        try {
            if (task) {
                task();
            }
        } catch (...) {
        }
    }
}

void EventLoop::process_macrotasks() {
    if (!macrotasks_.empty()) {
        auto task = std::move(macrotasks_.front());
        macrotasks_.erase(macrotasks_.begin());
        
        try {
            if (task) {
                task();
            }
        } catch (...) {
        }
    }
}

EventLoop& EventLoop::instance() {
    static EventLoop instance;
    return instance;
}

}
