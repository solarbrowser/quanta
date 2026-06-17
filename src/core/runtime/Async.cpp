/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Async.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/parser/AST.h"
#include "quanta/lexer/Lexer.h"
#include "quanta/parser/Parser.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <cmath>

namespace Quanta {

thread_local AsyncExecutor* AsyncExecutor::current_ = nullptr;

AsyncExecutor::AsyncExecutor(std::unique_ptr<ASTNode> body,
                              std::unique_ptr<Context> exec_ctx,
                              Promise* outer_promise,
                              Engine* engine)
    : outer_promise_(outer_promise),
      exec_context_owned_(std::move(exec_ctx)),
      exec_context_(exec_context_owned_.get()),
      engine_(engine),
      fiber_stack_(STACK_SIZE),
      body_(std::move(body)) {
    getcontext(&fiber_ctx_);
    fiber_ctx_.uc_stack.ss_sp   = fiber_stack_.data();
    fiber_ctx_.uc_stack.ss_size = STACK_SIZE;
    fiber_ctx_.uc_link = nullptr;
    uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
    makecontext(&fiber_ctx_, (void(*)())fiber_entry, 2,
                (uint32_t)(ptr & 0xFFFFFFFFu), (uint32_t)(ptr >> 32));
    // Keep outer_promise alive as a GC root for the lifetime of this executor.
    // Without this, GC may collect the promise (and the awaited inner Promises
    // stored on it as pins) while the fiber is suspended.
    if (engine_ && engine_->get_garbage_collector()) {
        engine_->get_garbage_collector()->add_root_object(outer_promise_);
    }
}

AsyncExecutor::~AsyncExecutor() {
    if (engine_ && engine_->get_garbage_collector()) {
        engine_->get_garbage_collector()->remove_root_object(outer_promise_);
    }
}

void AsyncExecutor::fiber_entry(uint32_t lo, uint32_t hi) {
    uintptr_t ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    auto* self = reinterpret_cast<AsyncExecutor*>(ptr);

    Context* ctx = self->exec_context_;
    Value result;
    try {
        if (self->body_) {
            result = self->body_->evaluate(*ctx);
        }
    } catch (const std::exception& e) {
        if (!ctx->has_exception()) {
            ctx->throw_exception(Value(std::string(e.what())));
        }
    } catch (...) {
        if (!ctx->has_exception()) {
            ctx->throw_exception(Value(std::string("Unknown error in async function")));
        }
    }

    if (ctx->has_exception()) {
        Value exc = ctx->get_exception();
        ctx->clear_exception();
        self->outer_promise_->reject(exc);
    } else if (ctx->has_return_value()) {
        Value ret = ctx->get_return_value();
        ctx->clear_return_value();
        if (AsyncUtils::is_promise(ret)) {
            Promise* p = static_cast<Promise*>(ret.as_object());
            if (p->get_state() == PromiseState::FULFILLED) {
                self->outer_promise_->fulfill(p->get_value());
            } else if (p->get_state() == PromiseState::REJECTED) {
                self->outer_promise_->reject(p->get_value());
            } else {
                self->outer_promise_->fulfill(ret);
            }
        } else {
            self->outer_promise_->fulfill(ret);
        }
    } else {
        self->outer_promise_->fulfill(result);
    }

    // Function is fully done and won't be resumed again -- release the retain taken in AsyncFunction::call.
    EventLoop::instance().release_context(ctx);

    // Return control to whoever called run()/resume()
    swapcontext(&self->fiber_ctx_, &self->caller_ctx_);
}

void AsyncExecutor::run() {
    auto* prev = current_;
    current_ = this;
    swapcontext(&caller_ctx_, &fiber_ctx_);  // enter or re-enter fiber
    current_ = prev;
}

void AsyncExecutor::resume(Value result, bool is_throw) {
    await_result_   = result;
    await_is_throw_ = is_throw;
    // Pin the result on outer_promise_ so GC doesn't collect it while the
    // fiber reads it (fiber stack is not a GC root).
    if (outer_promise_) outer_promise_->set_property("__rv_", result);
    run();
    if (outer_promise_) outer_promise_->delete_property("__rv_");
}

// AsyncFunction

AsyncFunction::AsyncFunction(const std::string& name,
                           const std::vector<std::string>& params,
                           std::unique_ptr<ASTNode> body,
                           Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {
    set_is_constructor(false);
    set_function_prototype(nullptr);
}

AsyncFunction::AsyncFunction(const std::string& name,
                           std::vector<std::unique_ptr<Parameter>> params,
                           std::unique_ptr<ASTNode> body,
                           Context* closure_context)
    : Function(name, std::move(params), nullptr, closure_context), body_(std::move(body)) {
    set_is_constructor(false);
    set_function_prototype(nullptr);
}

Value AsyncFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    auto promise_obj = ObjectFactory::create_promise(&ctx);
    Promise* promise_raw = static_cast<Promise*>(promise_obj.get());
    Value promise_value(promise_obj.release());

    // Create a persistent function-level context for this execution
    auto exec_ctx = ContextFactory::create_function_context(ctx.get_engine(), &ctx, this);

    // Propagate strict mode from the function definition
    if (is_strict()) exec_ctx->set_strict_mode(true);

    // Bind 'this' (arrow uses captured __arrow_this__; sloppy-mode undefined/null -> global)
    Value bound_this = this_value;
    Value arrow_this_val = get_property("__arrow_this__");
    if (!arrow_this_val.is_undefined()) {
        bound_this = arrow_this_val;
    } else if (!exec_ctx->is_strict_mode() && (bound_this.is_undefined() || bound_this.is_null())) {
        Object* global = ctx.get_global_object();
        if (global) bound_this = Value(global);
    }
    exec_ctx->create_binding("this", bound_this, true);

    // Set up __super__ for class methods
    Value super_ctor = get_property("__super_constructor__");
    if (super_ctor.is_function()) {
        exec_ctx->create_binding("__super__", super_ctor, false);
    }

    // Restore captured closure variables
    for (const auto& key : get_internal_property_keys()) {
        if (key.size() <= 10 || key.substr(0, 10) != "__closure_") continue;
        std::string var_name = key.substr(10);
        if (var_name == "this") continue;
        bool is_const = has_property("__closure_const_" + var_name);
        exec_ctx->create_binding(var_name, get_property(key), !is_const);
    }

    // Spec 15.8.4 NamedEvaluation / FunctionDeclarationInstantiation: a named
    // AsyncFunctionExpression binds its own name as an immutable self-reference
    // inside its body (assignment is silently ignored in sloppy mode, throws in strict).
    const std::string& fn_name = get_name();
    if (!fn_name.empty() && fn_name != "<anonymous>" && !exec_ctx->has_binding(fn_name)) {
        exec_ctx->create_binding(fn_name, Value(this), false);
    }

    // Bind parameters with full AST support (defaults, destructuring, rest).
    // Errors during param evaluation must reject the promise, not propagate synchronously.
    const auto& param_objs = get_parameter_objects();
    bool param_named_arguments = false;
    if (!param_objs.empty()) {
        size_t regular_count = 0;
        for (const auto& p : param_objs) { if (!p->is_rest()) regular_count++; }
        {
            std::unordered_set<std::string> pnames;
            for (const auto& p : param_objs) {
                if (p->get_name() && !p->get_name()->get_name().empty())
                    pnames.insert(p->get_name()->get_name());
            }
            exec_ctx->set_eval_param_names(std::move(pnames));
        }
        exec_ctx->set_in_param_eval(true);
        for (size_t i = 0; i < param_objs.size(); ++i) {
            const auto& param = param_objs[i];
            if (param->is_rest()) {
                auto rest_arr = ObjectFactory::create_array(0);
                for (size_t j = regular_count; j < args.size(); ++j) rest_arr->push(args[j]);
                exec_ctx->create_binding(param->get_name()->get_name(), Value(rest_arr.release()), false);
            } else {
                std::string pname = param->get_name() ? param->get_name()->get_name() : "";
                if (pname == "arguments") param_named_arguments = true;
                // Create TDZ binding first so self-referential defaults (x = x) throw ReferenceError
                if (!pname.empty() && pname != "arguments" && !param->has_destructuring()) {
                    if (exec_ctx->get_lexical_environment())
                        exec_ctx->get_lexical_environment()->create_uninitialized_binding(pname);
                }
                Value arg_val;
                if (i < args.size() && !args[i].is_undefined()) {
                    arg_val = args[i];
                } else if (param->has_default()) {
                    arg_val = param->get_default_value()->evaluate(*exec_ctx);
                    if (exec_ctx->has_exception()) {
                        exec_ctx->set_in_param_eval(false);
                        promise_raw->reject(exec_ctx->get_exception());
                        return promise_value;
                    }
                }
                if (param->has_destructuring()) {
                    auto* destr = dynamic_cast<DestructuringAssignment*>(param->get_destructuring_pattern());
                    if (destr) {
                        destr->evaluate_with_value(*exec_ctx, arg_val);
                        if (exec_ctx->has_exception()) {
                            exec_ctx->set_in_param_eval(false);
                            promise_raw->reject(exec_ctx->get_exception());
                            return promise_value;
                        }
                    }
                } else if (!pname.empty()) {
                    // Initialize the binding (was in TDZ during default evaluation)
                    if (exec_ctx->get_lexical_environment())
                        exec_ctx->get_lexical_environment()->initialize_binding(pname, arg_val);
                    else
                        exec_ctx->create_binding(pname, arg_val, true);
                }
            }
        }
        exec_ctx->set_in_param_eval(false);
    } else {
        const auto& params = get_parameters();
        for (size_t i = 0; i < params.size(); ++i) {
            if (params[i] == "arguments") param_named_arguments = true;
            Value arg = i < args.size() ? args[i] : Value();
            exec_ctx->create_binding(params[i], arg);
        }
    }

    // Arrow functions inherit arguments from the enclosing scope; regular async functions get their own.
    // Skip if a parameter was already named "arguments" (it takes precedence).
    if (arrow_this_val.is_undefined() && !param_named_arguments) {
        auto arguments_obj = ObjectFactory::create_array(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            arguments_obj->set_element(static_cast<uint32_t>(i), args[i]);
        }
        arguments_obj->set_property("length", Value(static_cast<double>(args.size())));
        arguments_obj->set_type(Object::ObjectType::Arguments);
        exec_ctx->create_binding("arguments", Value(arguments_obj.release()), false);
    }

    // FunctionDeclarationInstantiation: hoist `var` declarations to the top of
    // the function body, creating bindings initialized to `undefined` before
    // the body executes (so e.g. `with` blocks resolve the local shadowing
    // binding rather than falling through to an outer scope).
    if (body_ && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        scan_for_var_declarations(body_.get(), *exec_ctx);
    }

    // Clone body and start executor
    std::unique_ptr<ASTNode> body_clone = body_ ? body_->clone() : nullptr;
    auto executor = std::make_shared<AsyncExecutor>(
        std::move(body_clone), std::move(exec_ctx), promise_raw, ctx.get_engine());
    executor->run();

    // Transfer the exec context to the engine's survivor pool so closures created inside the body can still look up bindings later. Mirrors ContextSurvivorGuard in sync Function::call().
    // Retain first: the fiber is suspended at its first await and will resume later still using this exact Context.
    // Without the retain, the very next clear_survivor_contexts() would delete it before the fiber ever resumes.
    // Released in fiber_entry once the function fully completes.
    if (ctx.get_engine() && executor->exec_context_owned_) {
        EventLoop::instance().retain_context(executor->exec_context_owned_.get());
        ctx.get_engine()->add_survivor_context(executor->exec_context_owned_.release());
    }

    return promise_value;
}



AsyncAwaitExpression::AsyncAwaitExpression(std::unique_ptr<ASTNode> expression)
    : expression_(std::move(expression)) {
}

Value AsyncAwaitExpression::evaluate(Context& ctx) {
    AsyncExecutor* exec = AsyncExecutor::get_current();

    if (exec && !exec->fiber_stack_.empty()) {
        // Fiber-based path: no replay, just suspend/resume

        Value expr_val = expression_ ? expression_->evaluate(ctx) : Value();
        if (ctx.has_exception()) return Value();

        Context* gctx = exec->engine_ ? exec->engine_->get_current_context() : exec->exec_context_;

        // Pin expr_val on outer_promise_ IMMEDIATELY (before any allocations that
        // could trigger GC and collect the awaited Promise while it's only on the
        // fiber's stack -- which GC does not scan). Fixed key, not a counter: only one
        // await is ever in flight on a given outer_promise_ at a time (set here, always
        // deleted below before the next await's pin), so reusing the same key keeps
        // property_insertion_order_ bounded instead of growing once per await.
        const std::string pin_key = "__ap_";
        exec->outer_promise_->set_property(pin_key, expr_val);

        // Now resolve awaited value / register pending callbacks
        Value settled_val;
        bool settled_throw = false;
        bool is_pending = false;

        if (AsyncUtils::is_promise(expr_val)) {
            Promise* p = static_cast<Promise*>(expr_val.as_object());
            if (p->get_state() == PromiseState::FULFILLED) {
                settled_val = p->get_value();
            } else if (p->get_state() == PromiseState::REJECTED) {
                settled_val = p->get_value();
                settled_throw = true;
            } else {
                is_pending = true;
                auto self = exec->shared_from_this();

                auto on_fulfill = ObjectFactory::create_native_function("",
                    [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                        Value val = args.empty() ? Value() : args[0];
                        if (gctx) gctx->queue_microtask([self, val]() mutable {
                            self->resume(val, false);
                        });
                        return Value();
                    });
                auto on_reject = ObjectFactory::create_native_function("",
                    [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                        Value reason = args.empty() ? Value() : args[0];
                        if (gctx) gctx->queue_microtask([self, reason]() mutable {
                            self->resume(reason, true);
                        });
                        return Value();
                    });

                std::string key = std::to_string(reinterpret_cast<uintptr_t>(exec));
                Function* ff_tmp = on_fulfill.get(); Function* fr_tmp = on_reject.get();
                p->set_property("__af_" + key, Value(on_fulfill.release()));
                p->set_property("__ar_" + key, Value(on_reject.release()));
                p->then(ff_tmp, fr_tmp);
            }
        } else {
            settled_val = expr_val;
        }

        if (!is_pending) {
            // Also pin the settled value (e.g. module namespace) on outer_promise
            // so GC can't collect it while it's only in the microtask lambda capture.
            std::string sv_key = pin_key + "_v";
            exec->outer_promise_->set_property(sv_key, settled_val);
            auto self = exec->shared_from_this();
            Value val = settled_val;
            bool thr = settled_throw;
            if (gctx) gctx->queue_microtask([self, val, thr]() mutable {
                self->resume(val, thr);
            });
        }

        // Suspend fiber — return control to the microtask runner
        swapcontext(&exec->fiber_ctx_, &exec->caller_ctx_);

        // Resumed by resume() — await_result_ holds the settled value
        exec->outer_promise_->delete_property(pin_key);
        exec->outer_promise_->delete_property(pin_key + "_v");

        if (exec->await_is_throw_) {
            ctx.throw_exception(exec->await_result_, true);
            exec->await_is_throw_ = false;
            exec->await_result_ = Value();
            return Value();
        }
        Value result = exec->await_result_;
        exec->await_result_ = Value();
        return result;
    }

    // Fallback: no executor (e.g., top-level await outside async fn)
    if (!expression_) return Value();
    Value awaited_value = expression_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    if (!is_awaitable(awaited_value)) return awaited_value;

    auto promise = to_promise(awaited_value, ctx);
    if (!promise) return awaited_value;

    if (promise->get_state() == PromiseState::FULFILLED) {
        return promise->get_value();
    } else if (promise->get_state() == PromiseState::REJECTED) {
        ctx.throw_exception(promise->get_value(), true);
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


Object* AsyncGenerator::s_async_generator_prototype_ = nullptr;
thread_local AsyncGenerator* AsyncGenerator::current_ = nullptr;

AsyncGenerator::AsyncGenerator(std::unique_ptr<Context> ctx, std::unique_ptr<ASTNode> body, Context* outer_ctx)
    : Object(ObjectType::Custom), context_owned_(std::move(ctx)),
      generator_context_(context_owned_.get()),
      outer_context_(outer_ctx),
      body_(std::move(body)), state_(State::SuspendedStart),
      fiber_stack_(STACK_SIZE) {
    getcontext(&fiber_ctx_);
    fiber_ctx_.uc_stack.ss_sp   = fiber_stack_.data();
    fiber_ctx_.uc_stack.ss_size = STACK_SIZE;
    fiber_ctx_.uc_link = nullptr;
    uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
    makecontext(&fiber_ctx_, (void(*)())fiber_entry, 2,
                (uint32_t)(ptr & 0xFFFFFFFFu), (uint32_t)(ptr >> 32));
    if (s_async_generator_prototype_) {
        set_prototype(s_async_generator_prototype_);
    }
}

void AsyncGenerator::fiber_entry(uint32_t lo, uint32_t hi) {
    uintptr_t ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    auto* self = reinterpret_cast<AsyncGenerator*>(ptr);

    Context* ctx = self->generator_context_;
    Value result;
    try {
        if (self->body_) {
            result = self->body_->evaluate(*ctx);
        }
    } catch (const GeneratorReturnException& ret_ex) {
        if (!ctx->has_return_value()) {
            ctx->set_return_value(ret_ex.return_value);
        }
    } catch (const std::exception& e) {
        if (!ctx->has_exception()) {
            ctx->throw_exception(Value(std::string(e.what())));
        }
    } catch (...) {
        if (!ctx->has_exception()) {
            ctx->throw_exception(Value(std::string("Unknown error in async generator")));
        }
    }

    // Body ran to completion (or threw via ctx exception)
    self->state_ = State::Completed;
    self->suspend_reason_ = SuspendReason::Done;

    if (ctx->has_exception()) {
        self->has_exception_ = true;
        self->exception_value_ = ctx->get_exception();
        ctx->clear_exception();
    } else if (ctx->has_return_value()) {
        self->return_value_ = ctx->get_return_value();
        ctx->clear_return_value();
    } else {
        self->return_value_ = result;
    }

    swapcontext(&self->fiber_ctx_, &self->caller_ctx_);
}

void AsyncGenerator::enter_fiber() {
    auto* prev = current_;
    current_ = this;
    swapcontext(&caller_ctx_, &fiber_ctx_);
    current_ = prev;
    handle_suspension();
}

void AsyncGenerator::handle_suspension() {
    if (!pending_promise_) return;

    switch (suspend_reason_) {
        case SuspendReason::Yield: {
            state_ = State::SuspendedYield;
            auto result_obj = ObjectFactory::create_object();
            result_obj->set_property("value", yield_value_);
            result_obj->set_property("done", Value(false));
            Promise* fulfilled = pending_promise_;
            fulfilled->fulfill(Value(result_obj.release()));
            // .then reactions run as queued microtasks now, so a later one could
            // re-enter and replace pending_promise_ before we resume -- advance only ours.
            if (pending_promise_ == fulfilled) {
                advance_queue();
            }
            break;
        }
        case SuspendReason::Await:
            // Internal suspension — pending_promise_ stays; fiber will resume via resume_from_await()
            break;
        case SuspendReason::Done: {
            Promise* settled = pending_promise_;
            if (has_exception_) {
                has_exception_ = false;
                settled->reject(exception_value_);
            } else {
                auto result_obj = ObjectFactory::create_object();
                result_obj->set_property("value", return_value_);
                result_obj->set_property("done", Value(true));
                settled->fulfill(Value(result_obj.release()));
            }
            if (pending_promise_ == settled) {
                advance_queue();
            }
            break;
        }
    }
}

void AsyncGenerator::advance_queue() {
    if (!request_queue_.empty()) {
        Request front = std::move(request_queue_.front());
        request_queue_.pop_front();
        if (!front.pin_key.empty()) delete_property(front.pin_key);
    }
    pending_promise_ = nullptr;
    delete_property("__pending_promise__");
    process_next_request();
}

void AsyncGenerator::process_next_request() {
    if (request_queue_.empty()) return;
    Request& front = request_queue_.front();

    // Generator already finished (or never runs, see suspendedStart case below):
    // settle each queued request directly, in order, without entering the fiber.
    bool finishes_without_running =
        state_ == State::Completed ||
        (state_ == State::SuspendedStart && front.type != Request::Type::Next);

    if (finishes_without_running) {
        state_ = State::Completed;
        Promise* p = front.promise;
        switch (front.type) {
            case Request::Type::Throw:
                p->reject(front.value);
                break;
            case Request::Type::Return: {
                auto result_obj = ObjectFactory::create_object();
                result_obj->set_property("value", front.value);
                result_obj->set_property("done", Value(true));
                p->fulfill(Value(result_obj.release()));
                break;
            }
            case Request::Type::Next: {
                auto result_obj = ObjectFactory::create_object();
                result_obj->set_property("value", Value());
                result_obj->set_property("done", Value(true));
                p->fulfill(Value(result_obj.release()));
                break;
            }
        }
        if (!front.pin_key.empty()) delete_property(front.pin_key);
        request_queue_.pop_front();
        process_next_request();
        return;
    }

    pending_promise_ = front.promise;
    set_property("__pending_promise__", Value(pending_promise_));
    sent_value_  = front.value;
    return_arg_  = front.value;
    throwing_    = (front.type == Request::Type::Throw);
    returning_   = (front.type == Request::Type::Return);

    if (state_ == State::SuspendedStart) {
        // Per spec: the first next() call must run the generator body synchronously
        // up to the first yield/await so that side effects (e.g. callCount++) are
        // visible immediately after next() returns.
        enter_fiber();
    } else {
        // generator_context_ is always set (constructed in the AsyncGenerator constructor).
        Context* queue_ctx = outer_context_ ? outer_context_ : generator_context_;
        auto self = this;
        queue_ctx->queue_microtask([self]() { self->enter_fiber(); });
    }
}

AsyncGenerator::AsyncGeneratorResult AsyncGenerator::enqueue_request(Request::Type type, const Value& value, std::unique_ptr<Promise> promise) {
    Promise* raw = promise.get();
    std::string pin_key = "__agq_" + std::to_string(request_pin_counter_++) + "__";
    // Pin the promise on 'this' (GC-traced) so it survives until the request settles.
    set_property(pin_key, Value(raw));
    request_queue_.push_back({type, value, raw, pin_key});
    if (!pending_promise_) {
        process_next_request();
    }
    return AsyncGeneratorResult(std::move(promise));
}

void AsyncGenerator::resume_from_await(Value result, bool is_throw) {
    await_result_   = result;
    await_is_throw_ = is_throw;
    // Pin result on 'this' (a GC-managed Object) to prevent collection.
    set_property("__rv_", result);
    enter_fiber();
    delete_property("__rv_");
}

AsyncGenerator::AsyncGeneratorResult AsyncGenerator::next(const Value& value) {
    Context* promise_ctx = outer_context_ ? outer_context_ : generator_context_;
    auto promise_obj = ObjectFactory::create_promise(promise_ctx);
    auto promise = std::unique_ptr<Promise>(static_cast<Promise*>(promise_obj.release()));
    return enqueue_request(Request::Type::Next, value, std::move(promise));
}

AsyncGenerator::AsyncGeneratorResult AsyncGenerator::return_value(const Value& value) {
    Context* promise_ctx = outer_context_ ? outer_context_ : generator_context_;
    auto promise_obj = ObjectFactory::create_promise(promise_ctx);
    auto promise = std::unique_ptr<Promise>(static_cast<Promise*>(promise_obj.release()));
    return enqueue_request(Request::Type::Return, value, std::move(promise));
}

AsyncGenerator::AsyncGeneratorResult AsyncGenerator::throw_exception(const Value& exception) {
    Context* promise_ctx = outer_context_ ? outer_context_ : generator_context_;
    auto promise_obj = ObjectFactory::create_promise(promise_ctx);
    auto promise = std::unique_ptr<Promise>(static_cast<Promise*>(promise_obj.release()));
    return enqueue_request(Request::Type::Throw, exception, std::move(promise));
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
        async_gen_prototype->set_property(async_iterator_symbol->to_property_key(), Value(async_iterator_fn.release()));
    }

    s_async_generator_prototype_ = async_gen_prototype.get();
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
        async_iterator_prototype->set_property(async_iterator_symbol->to_property_key(), Value(self_async_iterator_fn.release()));
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


EventLoop::EventLoop() : next_timer_id_(1) {
}

void EventLoop::retain_context(Context* ctx) {
    if (ctx) context_use_count_[ctx]++;
}

void EventLoop::release_context(Context* ctx) {
    if (!ctx) return;
    auto it = context_use_count_.find(ctx);
    if (it == context_use_count_.end()) return;
    if (--it->second <= 0) context_use_count_.erase(it);
}

int64_t EventLoop::schedule_timer(Context& ctx, Function* callback,
                                   std::vector<Value> args,
                                   double delay_ms, bool repeating) {
    int64_t id = next_timer_id_++;

    // GC-root the callback/args on the global object until the timer fires or is cleared.
    Object* global = ctx.get_global_object();
    if (global) {
        global->set_property("__timer_" + std::to_string(id) + "_cb", Value(callback));
        for (size_t i = 0; i < args.size(); i++) {
            global->set_property("__timer_" + std::to_string(id) + "_arg" + std::to_string(i), args[i]);
        }
    }

    TimerEntry entry;
    entry.id = id;
    entry.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int64_t>(delay_ms));
    entry.interval_ms = repeating ? static_cast<int64_t>(delay_ms) : -1;
    entry.callback = callback;
    entry.bound_args = std::move(args);
    entry.call_ctx = &ctx;
    retain_context(&ctx);
    timers_.push(std::move(entry));
    return id;
}

void EventLoop::clear_timer(int64_t id) {
    cancelled_ids_.insert(id);
}

bool EventLoop::run_pending_timers(Context& ctx) {
    auto start = std::chrono::steady_clock::now();
    const auto wall_cap = std::chrono::seconds(8);
    const int64_t iteration_cap = 100000;
    int64_t iterations = 0;

    while (!timers_.empty()) {
        if (std::chrono::steady_clock::now() - start > wall_cap) return false;
        if (++iterations > iteration_cap) return false;

        TimerEntry entry = timers_.top();
        timers_.pop();

        if (cancelled_ids_.count(entry.id)) {
            cancelled_ids_.erase(entry.id);
            release_context(entry.call_ctx);
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (entry.deadline > now) {
            std::this_thread::sleep_until(entry.deadline);
        }

        Value result = entry.callback->call(*entry.call_ctx, entry.bound_args);
        (void)result;
        if (entry.call_ctx->has_exception()) {
            Value exc = entry.call_ctx->get_exception();
            entry.call_ctx->clear_exception();
            std::cerr << "Uncaught (in timer) " << exc.to_string() << std::endl;
        }

        bool still_active = entry.interval_ms >= 0 && !cancelled_ids_.count(entry.id);
        Object* global = entry.call_ctx->get_global_object();
        if (still_active) {
            TimerEntry next = entry;
            next.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(entry.interval_ms);
            timers_.push(std::move(next));
        } else {
            cancelled_ids_.erase(entry.id);
            release_context(entry.call_ctx);
            if (global) {
                global->delete_property("__timer_" + std::to_string(entry.id) + "_cb");
                for (size_t i = 0; i < entry.bound_args.size(); i++) {
                    global->delete_property("__timer_" + std::to_string(entry.id) + "_arg" + std::to_string(i));
                }
            }
        }

        // Promise/queueMicrotask jobs queue onto the engine's global context (Promise.cpp's get_exec_ctx), not entry.call_ctx.
        // Drain the global context here so jobs queued during this callback run before the next timer fires.
        Engine* engine = entry.call_ctx->get_engine();
        Context* drain_ctx = (engine && engine->get_global_context()) ? engine->get_global_context() : entry.call_ctx;
        drain_ctx->drain_microtasks();
        if (engine) engine->clear_survivor_contexts();
    }
    return true;
}

EventLoop& EventLoop::instance() {
    static EventLoop instance;
    return instance;
}


AsyncGeneratorFunction::AsyncGeneratorFunction(const std::string& name,
                                               const std::vector<std::string>& params,
                                               std::unique_ptr<ASTNode> body,
                                               Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {}

AsyncGeneratorFunction::AsyncGeneratorFunction(const std::string& name,
                                               std::vector<std::unique_ptr<Parameter>> params,
                                               std::unique_ptr<ASTNode> body,
                                               Context* closure_context)
    : Function(name, std::move(params), nullptr, closure_context), body_(std::move(body)) {}

Value AsyncGeneratorFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    auto gen_ctx = ContextFactory::create_function_context(ctx.get_engine(), &ctx, this);

    if (is_strict()) gen_ctx->set_strict_mode(true);

    Value bound_this = this_value;
    Value arrow_this_ag = get_property("__arrow_this__");
    if (!arrow_this_ag.is_undefined()) {
        bound_this = arrow_this_ag;
    } else if (!gen_ctx->is_strict_mode() && (bound_this.is_undefined() || bound_this.is_null())) {
        Object* global = ctx.get_global_object();
        if (global) bound_this = Value(global);
    }
    try {
        gen_ctx->create_binding("this", bound_this, true);
    } catch (...) {
        gen_ctx->set_binding("this", bound_this);
    }

    // Apply closures
    auto prop_keys = get_internal_property_keys();
    for (const auto& key : prop_keys) {
        if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
            std::string var_name = key.substr(10);
            Value closure_value = get_property(key);
            if (var_name != "arguments" && var_name != "this") {
                if (!ctx.has_binding(var_name)) {
                    gen_ctx->create_binding(var_name, closure_value, true);
                }
            }
        }
    }

    // Spec 15.8.4 NamedEvaluation / FunctionDeclarationInstantiation: a named
    // AsyncGeneratorExpression binds its own name as an immutable self-reference
    // inside its body (assignment is silently ignored in sloppy mode, throws in strict).
    {
        const std::string& fn_name = get_name();
        if (!fn_name.empty() && fn_name != "<anonymous>" && !gen_ctx->has_binding(fn_name)) {
            gen_ctx->create_binding(fn_name, Value(this), false);
        }
    }

    // Bind parameters
    const auto& param_objs = get_parameter_objects();
    if (!param_objs.empty()) {
        size_t regular_count = 0;
        for (const auto& p : param_objs) { if (!p->is_rest()) regular_count++; }
        gen_ctx->set_eval_arguments_conflict(true);
        {
            std::unordered_set<std::string> pnames;
            for (const auto& p : param_objs) {
                if (p->get_name() && !p->get_name()->get_name().empty())
                    pnames.insert(p->get_name()->get_name());
            }
            gen_ctx->set_eval_param_names(std::move(pnames));
        }
        gen_ctx->set_in_param_eval(true);
        for (size_t i = 0; i < param_objs.size(); ++i) {
            const auto& param = param_objs[i];
            if (param->is_rest()) {
                auto rest_arr = ObjectFactory::create_array(0);
                for (size_t j = regular_count; j < args.size(); ++j) rest_arr->push(args[j]);
                gen_ctx->create_binding(param->get_name()->get_name(), Value(rest_arr.release()), false);
            } else {
                const std::string& pname = param->get_name() ? param->get_name()->get_name() : std::string();
                // Create TDZ binding first so self-referential defaults (x = x) throw ReferenceError
                if (!pname.empty() && !param->has_destructuring()) {
                    if (gen_ctx->get_lexical_environment())
                        gen_ctx->get_lexical_environment()->create_uninitialized_binding(pname);
                }
                Value arg_val;
                if (i < args.size() && !args[i].is_undefined()) {
                    arg_val = args[i];
                } else if (param->has_default()) {
                    arg_val = param->get_default_value()->evaluate(*gen_ctx);
                    if (gen_ctx->has_exception()) {
                        gen_ctx->set_in_param_eval(false);
                        ctx.throw_exception(gen_ctx->get_exception(), true);
                        return Value();
                    }
                }
                if (param->has_destructuring()) {
                    auto* pat = param->get_destructuring_pattern();
                    auto* destr = dynamic_cast<DestructuringAssignment*>(pat);
                    if (destr) {
                        destr->evaluate_with_value(*gen_ctx, arg_val);
                        if (gen_ctx->has_exception()) {
                            gen_ctx->set_in_param_eval(false);
                            ctx.throw_exception(gen_ctx->get_exception(), true);
                            return Value();
                        }
                    }
                } else if (!pname.empty()) {
                    // Initialize the binding (was in TDZ during default evaluation)
                    if (gen_ctx->get_lexical_environment())
                        gen_ctx->get_lexical_environment()->initialize_binding(pname, arg_val);
                    else
                        gen_ctx->create_binding(pname, arg_val, true);
                }
            }
        }
        gen_ctx->set_in_param_eval(false);
    } else {
        const auto& params = get_parameters();
        for (size_t i = 0; i < params.size(); ++i) {
            Value arg = i < args.size() ? args[i] : Value();
            gen_ctx->create_binding(params[i], arg);
        }
    }

    // arguments object
    auto arguments_obj = ObjectFactory::create_array(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        arguments_obj->set_element(static_cast<uint32_t>(i), args[i]);
    }
    arguments_obj->set_property("length", Value(static_cast<double>(args.size())));
    arguments_obj->set_type(Object::ObjectType::Arguments);
    gen_ctx->create_binding("arguments", Value(arguments_obj.release()), false);

    // FunctionDeclarationInstantiation: hoist `var` declarations to the top of
    // the function body before it executes (see AsyncFunction::call for rationale).
    if (body_ && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        scan_for_var_declarations(body_.get(), *gen_ctx);
    }

    Context* outer_ctx = ctx.get_engine() ? ctx.get_engine()->get_global_context() : &ctx;
    std::unique_ptr<ASTNode> body_clone = body_ ? body_->clone() : nullptr;
    auto async_gen = std::make_unique<AsyncGenerator>(std::move(gen_ctx), std::move(body_clone), outer_ctx);
    return Value(async_gen.release());
}

}
