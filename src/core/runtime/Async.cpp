/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/FiberStackPool.h"
#include <cstdio>
#include <cstdlib>
#include "quanta/core/gc/Collector.h"
#include "quanta/core/gc/FiberRegistry.h"
#include "quanta/core/vm/Interpreter.h"
#include "quanta/core/vm/BytecodeCompiler.h"
#include "quanta/core/gc/Visitor.h"
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

void AsyncGenerator::trace(Visitor& v) {
    Object::trace(v);
    v.visit_context(generator_context_);
    v.visit_context(outer_context_);
    v.visit(yield_value_);
    v.visit(return_value_);
    v.visit(exception_value_);
    v.visit(sent_value_);
    v.visit(return_arg_);
    v.visit(await_result_);
    v.visit_object(pending_promise_);
    v.visit_object(owner_fn_);
    for (const auto& req : request_queue_) {
        v.visit(req.value);
        v.visit_object(req.promise);
    }
}


thread_local AsyncExecutor* AsyncExecutor::current_ = nullptr;

AsyncExecutor::AsyncExecutor(ASTNode* body,
                              AsyncFunction* owner_fn,
                              std::unique_ptr<Context> exec_ctx,
                              Promise* outer_promise,
                              Engine* engine)
    : outer_promise_(outer_promise),
      exec_context_owned_(std::move(exec_ctx)),
      exec_context_(exec_context_owned_.get()),
      engine_(engine),
      fiber_stack_(FiberStackPool::acquire(STACK_SIZE)),
      body_(body),
      owner_fn_(owner_fn) {
    getcontext(&fiber_->fiber_ctx);
    fiber_->fiber_ctx.uc_stack.ss_sp   = fiber_stack_;
    fiber_->fiber_ctx.uc_stack.ss_size = STACK_SIZE;
    fiber_->fiber_ctx.uc_link = nullptr;
    uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
    makecontext(&fiber_->fiber_ctx, (void(*)())fiber_entry, 2,
                (uint32_t)(ptr & 0xFFFFFFFFu), (uint32_t)(ptr >> 32));
    FiberRegistry::register_fiber(this, fiber_stack_, STACK_SIZE, fiber_.get(), nullptr,
        [this](Visitor& v) {
            v.visit_object(outer_promise_);
            v.visit_context(exec_context_);
            v.visit(await_result_);
            v.visit_object(owner_fn_);
        });

}

AsyncExecutor::~AsyncExecutor() {
    FiberRegistry::unregister_fiber(this);
    if (fiber_stack_) FiberStackPool::release(fiber_stack_, STACK_SIZE);
}

void AsyncExecutor::fiber_entry(uint32_t lo, uint32_t hi) {
    uintptr_t ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    auto* self = reinterpret_cast<AsyncExecutor*>(ptr);

    Context* ctx = self->exec_context_;
    try {
        if (self->body_) {
            // Bindings already live in exec_context_; a delegated await suspends
            // the fiber from inside the VM dispatch loop (see Generator::run_body).
            const BytecodeChunk* chunk = self->owner_fn_ ? self->owner_fn_->get_suspendable_chunk(*ctx) : nullptr;
            if (chunk) {
                Value vm_result = VM::run_suspendable_chunk(*chunk, *ctx, self->owner_fn_);
                if (!vm_result.is_undefined() && !ctx->has_return_value() && !ctx->has_exception()) {
                    ctx->set_return_value(vm_result);
                }
            } else {
                self->body_->evaluate(*ctx);
            }
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
        // Falling off the end without a `return` resolves to undefined, not the last statement's value.
        self->outer_promise_->fulfill(Value());
    }

    // Function is fully done and won't be resumed again -- release the retain taken in AsyncFunction::call.
    EventLoop::instance().release_context(ctx);

    // Return control to whoever called run()/resume()
    swapcontext(&self->fiber_->fiber_ctx, &self->fiber_->caller_ctx);
}

void AsyncExecutor::run() {
    auto* prev = current_;
    current_ = this;
    {
        FiberEnterScope enter_scope;
        swapcontext(&fiber_->caller_ctx, &fiber_->fiber_ctx);  // enter or re-enter fiber
    }
    current_ = prev;
}

void AsyncExecutor::resume(Value result, bool is_throw) {
    await_result_   = result;
    await_is_throw_ = is_throw;
    run();
}

// AsyncFunction

AsyncFunction::AsyncFunction(const std::string& name,
                           const std::vector<std::string>& params,
                           std::unique_ptr<ASTNode> body,
                           Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {
    set_is_constructor(false);
    set_function_prototype(nullptr);
    remove_own_property("prototype"); // async functions must not have .prototype
}

AsyncFunction::AsyncFunction(const std::string& name,
                           std::vector<std::unique_ptr<Parameter>> params,
                           std::unique_ptr<ASTNode> body,
                           Context* closure_context)
    : Function(name, std::move(params), nullptr, closure_context), body_(std::move(body)) {
    set_is_constructor(false);
    set_function_prototype(nullptr);
    remove_own_property("prototype");
}

Value AsyncFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    auto promise_obj = ObjectFactory::create_promise(&ctx);
    Promise* promise_raw = static_cast<Promise*>(promise_obj.get());
    Value promise_value(promise_obj.release());

    // Create a persistent function-level context for this execution
    auto exec_ctx = ContextFactory::create_function_context(ctx.get_engine(), &ctx, this);
    ExecContextScope gc_frame(exec_ctx.get());
    // See ContextSurvivorGuard's doc comment: no-op once exec_ctx is moved
    // into the AsyncExecutor below; catches an abrupt exit before that point.
    ContextSurvivorGuard survivor_guard(exec_ctx, ctx.get_engine());

    // Propagate strict mode from the function definition
    if (is_strict()) exec_ctx->set_strict_mode(true);

    // Bind 'this' (arrow uses captured __arrow_this__; sloppy-mode undefined/null -> global)
    // Use presence (has_property), not is_undefined(), since a captured this can legitimately BE undefined.
    Value bound_this = this_value;
    bool has_arrow_this = is_arrow() && has_property("__arrow_this__");
    Value arrow_this_val = has_arrow_this ? get_property("__arrow_this__") : Value();
    if (has_arrow_this) {
        bound_this = arrow_this_val;
    } else if (!exec_ctx->is_strict_mode()) {
        if (bound_this.is_undefined() || bound_this.is_null()) {
            Object* global = ctx.get_global_object();
            if (global) bound_this = Value(global);
        } else {
            bound_this = ObjectFactory::box_primitive_this_sloppy(ctx, bound_this);
        }
    }
    exec_ctx->create_binding("this", bound_this, true);

    // Set up __super__ for class methods
    Value super_ctor = get_property("__super_constructor__");
    if (super_ctor.is_function()) {
        exec_ctx->create_binding("__super__", super_ctor, false);
    }

    // A named class's own name is bound as an immutable self-reference inside its
    // methods (__closure_const_<name>) -- see ClassDeclaration::evaluate. Everything
    // else resolves through closure_environment_, no materialization needed.
    for (const auto& key : get_internal_property_keys()) {
        if (key.size() <= 10 || key.substr(0, 10) != "__closure_" || key.substr(0, 16) == "__closure_const_") continue;
        std::string var_name = key.substr(10);
        if (has_property("__closure_const_" + var_name)) {
            exec_ctx->create_binding(var_name, get_property(key), false);
        }
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
    for (const auto& p : param_objs) {
        if (p->get_name() && p->get_name()->get_name() == "arguments") param_named_arguments = true;
    }
    for (const auto& p : get_parameters()) {
        if (p == "arguments") param_named_arguments = true;
    }

    // Compile now so the arguments-object check below can see needs_arguments.
    const BytecodeChunk* susp_chunk = get_suspendable_chunk(*exec_ctx);

    // The chunk always compiles with an empty parameter list (see
    // VM::compile_suspendable), so needs_arguments only reflects the body;
    // check defaults/destructuring for `arguments` separately.
    bool params_need_arguments = false;
    for (const auto& p : param_objs) {
        if ((p->has_default() && BytecodeCompiler::references_arguments(p->get_default_value())) ||
            (p->has_destructuring() && BytecodeCompiler::references_arguments(p->get_destructuring_pattern()))) {
            params_need_arguments = true;
            break;
        }
    }

    // Created BEFORE parameter binding: default expressions may reference it.
    // Arrow functions inherit arguments from the enclosing scope; a parameter
    // literally named "arguments" takes precedence. Skipped when nothing
    // needs it, mirroring Function::call.
    if (!is_arrow() && !param_named_arguments &&
        (!susp_chunk || susp_chunk->needs_arguments || params_need_arguments)) {
        auto arguments_obj = ObjectFactory::create_array(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            arguments_obj->set_element(static_cast<uint32_t>(i), args[i]);
        }
        arguments_obj->set_property("length", Value(static_cast<double>(args.size())));
        arguments_obj->set_type(Object::ObjectType::Arguments);
        setup_mapped_arguments(*exec_ctx, args, arguments_obj.get());
        exec_ctx->create_binding("arguments", Value(arguments_obj.release()), false);
    }

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
                Value rest_val(rest_arr.release());
                if (param->has_destructuring()) {
                    auto* destr = dynamic_cast<DestructuringAssignment*>(param->get_destructuring_pattern());
                    if (destr) {
                        destr->evaluate_with_value(*exec_ctx, rest_val);
                        if (exec_ctx->has_exception()) {
                            exec_ctx->set_in_param_eval(false);
                            promise_raw->reject(exec_ctx->get_exception());
                            return promise_value;
                        }
                    }
                } else {
                    exec_ctx->create_binding(param->get_name()->get_name(), rest_val, false);
                }
            } else {
                std::string pname = param->get_name() ? param->get_name()->get_name() : "";
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
            Value arg = i < args.size() ? args[i] : Value();
            exec_ctx->create_binding(params[i], arg);
        }
    }

    // FDI step 27: param-default closures must not see the body's `var`s, so the body gets its own variable environment.
    {
        bool has_complex_params = false;
        for (const auto& p : param_objs) {
            if (p->has_default() || p->has_destructuring()) { has_complex_params = true; break; }
        }
        if (has_complex_params) {
            exec_ctx->push_block_scope();
            exec_ctx->set_variable_environment(exec_ctx->get_lexical_environment());
        }
    }

    // FunctionDeclarationInstantiation: hoist `var` declarations to the top of
    // the function body, creating bindings initialized to `undefined` before
    // the body executes (so e.g. `with` blocks resolve the local shadowing
    // binding rather than falling through to an outer scope).
    if (body_ && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        scan_for_var_declarations(body_.get(), *exec_ctx);
    }

    // Shared with every call, like an ordinary function's body -- no clone.
    auto executor = std::make_shared<AsyncExecutor>(
        body_.get(), this, std::move(exec_ctx), promise_raw, ctx.get_engine());
    executor->run();

    // Transfer the exec context to the engine's survivor pool so closures created inside the body can still look up bindings later. Mirrors ContextSurvivorGuard in sync Function::call().
    // Retain first: the fiber is suspended at its first await and will resume later still using this exact Context.
    // Without the retain, the collector's reachability-based survivor prune (Collector.cpp) could delete it before the fiber ever resumes.
    // Released in fiber_entry once the function fully completes.
    if (ctx.get_engine() && executor->exec_context_owned_) {
        EventLoop::instance().retain_context(executor->exec_context_owned_.get());
        ctx.get_engine()->add_survivor_context(executor->exec_context_owned_.release());
    }

    return promise_value;
}

const BytecodeChunk* AsyncFunction::get_suspendable_chunk(Context& ctx) {
    if (suspendable_incompatible_) return nullptr;
    if (suspendable_chunk_) return suspendable_chunk_.get();
    // Same `with`-chain check as Function::call; fixed at closure creation.
    for (Environment* e = ctx.get_lexical_environment(); e; e = e->get_outer()) {
        if (e->is_with_environment()) { suspendable_incompatible_ = true; return nullptr; }
    }
    suspendable_chunk_ = VM::compile_suspendable(body_.get());
    if (!suspendable_chunk_) { suspendable_incompatible_ = true; return nullptr; }
    Collector::write_barrier(this);
    return suspendable_chunk_.get();
}

void AsyncFunction::trace(Visitor& v) {
    Function::trace(v);
    if (suspendable_chunk_) suspendable_chunk_->trace(v);
}


AsyncAwaitExpression::AsyncAwaitExpression(std::unique_ptr<ASTNode> expression)
    : expression_(std::move(expression)) {
}

Value AsyncAwaitExpression::evaluate(Context& ctx) {
    AsyncExecutor* exec = AsyncExecutor::get_current();

    if (exec && exec->fiber_stack_) {
        // Fiber-based path: no replay, just suspend/resume

        Value expr_val = expression_ ? expression_->evaluate(ctx) : Value();
        if (ctx.has_exception()) return Value();

        Context* gctx = exec->engine_ ? exec->engine_->get_current_context() : exec->exec_context_;


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
                        self->resume(val, false);
                        return Value();
                    });
                auto on_reject = ObjectFactory::create_native_function("",
                    [self, gctx](Context&, const std::vector<Value>& args) -> Value {
                        Value reason = args.empty() ? Value() : args[0];
                        self->resume(reason, true);
                        return Value();
                    });

                p->then(on_fulfill.release(), on_reject.release());
            }
        } else {
            settled_val = expr_val;
        }

        if (!is_pending) {
            auto self = exec->shared_from_this();
            Value val = settled_val;
            bool thr = settled_throw;
            if (gctx) gctx->queue_microtask([self, val, thr]() mutable { self->resume(val, thr); }, {val});
        }

        // Suspend fiber -- return control to the microtask runner
        swapcontext(&exec->fiber_->fiber_ctx, &exec->fiber_->caller_ctx);

        // Resumed by resume() -- await_result_ holds the settled value
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


thread_local Object* AsyncGenerator::s_async_generator_prototype_ = nullptr;
thread_local Object* AsyncGenerator::s_async_generator_function_prototype_ = nullptr;
thread_local AsyncGenerator* AsyncGenerator::current_ = nullptr;

AsyncGenerator::AsyncGenerator(std::unique_ptr<Context> ctx, ASTNode* body,
                               AsyncGeneratorFunction* owner_fn, Context* outer_ctx)
    : Object(ObjectType::Custom), context_owned_(std::move(ctx)),
      generator_context_(context_owned_.get()),
      outer_context_(outer_ctx),
      body_(body), owner_fn_(owner_fn), state_(State::SuspendedStart),
      fiber_stack_(FiberStackPool::acquire(STACK_SIZE)) {
    getcontext(&fiber_->fiber_ctx);
    fiber_->fiber_ctx.uc_stack.ss_sp   = fiber_stack_;
    fiber_->fiber_ctx.uc_stack.ss_size = STACK_SIZE;
    fiber_->fiber_ctx.uc_link = nullptr;
    uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
    makecontext(&fiber_->fiber_ctx, (void(*)())fiber_entry, 2,
                (uint32_t)(ptr & 0xFFFFFFFFu), (uint32_t)(ptr >> 32));
    FiberRegistry::register_fiber(this, fiber_stack_, STACK_SIZE, fiber_.get(), this);
    if (s_async_generator_prototype_) {
        set_prototype(s_async_generator_prototype_);
    }
}

AsyncGenerator::~AsyncGenerator() {
    FiberRegistry::unregister_fiber(this);
    if (fiber_stack_) FiberStackPool::release(fiber_stack_, STACK_SIZE);
    // Closures born inside the generator body share this context via their
    // closure_context_; a swept generator must not tear it down under them.
    // Contexts stay engine-lifetime until they become traced cells themselves.
    context_owned_.release();
}

void AsyncGenerator::fiber_entry(uint32_t lo, uint32_t hi) {
    uintptr_t ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    auto* self = reinterpret_cast<AsyncGenerator*>(ptr);

    Context* ctx = self->generator_context_;
    try {
        if (self->body_) {
            // Same VM-or-treewalk split as Generator::run_body.
            const BytecodeChunk* chunk = self->owner_fn_ ? self->owner_fn_->get_suspendable_chunk(*ctx) : nullptr;
            if (chunk) {
                Value vm_result = VM::run_suspendable_chunk(*chunk, *ctx, self->owner_fn_);
                if (!vm_result.is_undefined() && !ctx->has_return_value() && !ctx->has_exception()) {
                    ctx->set_return_value(vm_result);
                }
            } else {
                self->body_->evaluate(*ctx);
            }
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
        // Falling off the end without a `return` completes with undefined, not the last statement's value.
        self->return_value_ = Value();
    }

    // Direct-assigned traced fields: re-gray for an open incremental cycle.
    Collector::write_barrier(self);
    swapcontext(&self->fiber_->fiber_ctx, &self->fiber_->caller_ctx);
}

void AsyncGenerator::enter_fiber() {
    auto* prev = current_;
    current_ = this;
    {
        FiberEnterScope enter_scope;
        swapcontext(&fiber_->caller_ctx, &fiber_->fiber_ctx);
    }
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
            // Internal suspension -- pending_promise_ stays; fiber will resume via resume_from_await()
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
        request_queue_.pop_front();
    }
    pending_promise_ = nullptr;
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
                // AsyncGeneratorAwaitReturn: PromiseResolve(%Promise%, value) unwraps a
                // promise-valued completion; its "constructor" lookup can throw.
                if (AsyncUtils::is_promise(front.value)) {
                    Context* cctx = outer_context_ ? outer_context_ : generator_context_;
                    Context* prev_cc = Object::current_context_;
                    Object::current_context_ = cctx;
                    front.value.as_object()->get_property("constructor");
                    Object::current_context_ = prev_cc;
                    if (cctx && cctx->has_exception()) {
                        Value err = cctx->get_exception();
                        cctx->clear_exception();
                        p->reject(err);
                        break;
                    }
                    pending_promise_ = p;
                    Value gen_val(static_cast<Object*>(this));
                    auto on_ok = ObjectFactory::create_native_function("",
                        [gen_val](Context&, const std::vector<Value>& a) -> Value {
                            auto* gen = static_cast<AsyncGenerator*>(gen_val.as_object());
                            Promise* settled = gen->pending_promise_;
                            if (settled) {
                                auto result_obj = ObjectFactory::create_object();
                                result_obj->set_property("value", a.empty() ? Value() : a[0]);
                                result_obj->set_property("done", Value(true));
                                settled->fulfill(Value(result_obj.release()));
                            }
                            gen->advance_queue();
                            return Value();
                        }, 1);
                    auto on_err = ObjectFactory::create_native_function("",
                        [gen_val](Context&, const std::vector<Value>& a) -> Value {
                            auto* gen = static_cast<AsyncGenerator*>(gen_val.as_object());
                            Promise* settled = gen->pending_promise_;
                            if (settled) settled->reject(a.empty() ? Value() : a[0]);
                            gen->advance_queue();
                            return Value();
                        }, 1);
                    // Handlers are pinned on the generator so GC keeps them alive
                    // until the awaited promise settles.
                    set_property("[[AwaitReturnOk]]", Value(on_ok.get()));
                    set_property("[[AwaitReturnErr]]", Value(on_err.get()));
                    static_cast<Promise*>(front.value.as_object())->then(on_ok.release(), on_err.release());
                    return;
                }
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
        request_queue_.pop_front();
        process_next_request();
        return;
    }

    pending_promise_ = front.promise;
    sent_value_  = front.value;
    return_arg_  = front.value;
    throwing_    = (front.type == Request::Type::Throw);
    returning_   = (front.type == Request::Type::Return);

    // AsyncGeneratorResume (27.6.3.12) always resumes synchronously, suspendedStart or
    // suspendedYield alike -- a `.return()` call's effects only *look* deferred because the
    // resumed code immediately hits Await(resumptionValue.Value), which itself always
    // defers by a tick.
    state_ = State::Executing;
    Collector::write_barrier(this);
    enter_fiber();
}

AsyncGenerator::AsyncGeneratorResult AsyncGenerator::enqueue_request(Request::Type type, const Value& value, std::unique_ptr<Promise> promise) {
    Promise* raw = promise.get();
    request_queue_.push_back({type, value, raw});
    // Direct-mutated traced field: re-gray for an open incremental cycle.
    Collector::write_barrier(this);
    if (!pending_promise_) {
        process_next_request();
    }
    return AsyncGeneratorResult(std::move(promise));
}

void AsyncGenerator::resume_from_await(Value result, bool is_throw) {
    await_result_   = result;
    await_is_throw_ = is_throw;
    Collector::write_barrier(this);
    enter_fiber();
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

    // %AsyncGeneratorPrototype%.[[Prototype]] = %AsyncIteratorPrototype% (spec 27.6.1).
    // Requires AsyncIterator::setup_async_iterator_prototype to have already run.
    Value async_iter_proto_val = ctx.get_binding("AsyncIteratorPrototype");
    if (async_iter_proto_val.is_object()) {
        async_gen_prototype->set_prototype(async_iter_proto_val.as_object());
    }

    auto next_fn = ObjectFactory::create_native_function("next", async_generator_next, 1);
    async_gen_prototype->set_property("next", Value(next_fn.release()), PropertyAttributes::BuiltinFunction);

    auto return_fn = ObjectFactory::create_native_function("return", async_generator_return, 1);
    async_gen_prototype->set_property("return", Value(return_fn.release()), PropertyAttributes::BuiltinFunction);

    auto throw_fn = ObjectFactory::create_native_function("throw", async_generator_throw, 1);
    async_gen_prototype->set_property("throw", Value(throw_fn.release()), PropertyAttributes::BuiltinFunction);
    // @@asyncIterator is inherited from %AsyncIteratorPrototype%, no own property here.

    Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
    if (tag_sym) {
        PropertyDescriptor ag_tag(Value(std::string("AsyncGenerator")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
        async_gen_prototype->set_property_descriptor(tag_sym->to_property_key(), ag_tag);
    }

    s_async_generator_prototype_ = async_gen_prototype.get();
    ctx.create_binding("AsyncGeneratorPrototype", Value(async_gen_prototype.release()));

    // %AsyncGeneratorFunction.prototype% -- [[Prototype]] of all async generator functions
    // Per spec: %AsyncGeneratorFunction.prototype%.[[Prototype]] = %Function.prototype%
    auto async_gen_fn_proto = ObjectFactory::create_object();
    Object* func_proto = ObjectFactory::get_function_prototype();
    async_gen_fn_proto->set_prototype(func_proto ? func_proto : s_async_generator_prototype_);
    if (tag_sym) {
        PropertyDescriptor agf_tag(Value(std::string("AsyncGeneratorFunction")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
        async_gen_fn_proto->set_property_descriptor(tag_sym->to_property_key(), agf_tag);
    }
    // %AsyncGeneratorFunction.prototype%.prototype = %AsyncGeneratorPrototype% (27.4.3.3: non-writable, non-enumerable, configurable)
    PropertyDescriptor agfp_proto_desc(Value(s_async_generator_prototype_), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    async_gen_fn_proto->set_property_descriptor("prototype", agfp_proto_desc);

    s_async_generator_function_prototype_ = async_gen_fn_proto.get();
    ctx.create_binding("@@AsyncGeneratorFunctionPrototype", Value(async_gen_fn_proto.release()));

    // AsyncGeneratorFunction constructor
    auto async_generator_function_constructor = ObjectFactory::create_native_constructor("AsyncGeneratorFunction",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::vector<std::string> param_names;
            std::string body_str = "";

            if (args.size() > 1) {
                body_str = args.back().to_string();
                for (size_t i = 0; i < args.size() - 1; ++i) {
                    param_names.push_back(args[i].to_string());
                }
            } else if (args.size() == 1) {
                body_str = args[0].to_string();
            }

            std::string params_str;
            for (size_t i = 0; i < param_names.size(); ++i) {
                if (i > 0) params_str += ",";
                params_str += param_names[i];
            }
            std::string toString_src = "async function* anonymous(" + params_str + "\n) {\n" + body_str + "\n}";
            std::string func_src = "(" + toString_src + ")";

            try {
                Lexer lexer(func_src);
                TokenSequence tokens = lexer.tokenize();
                Parser parser(tokens);
                parser.set_source(func_src);
                auto expr = parser.parse_expression();
                if (parser.has_errors()) {
                    auto& errors = parser.get_errors();
                    std::string msg = errors.empty() ? "SyntaxError" : errors[0].message;
                    if (msg.substr(0, 13) == "SyntaxError: ") msg = msg.substr(13);
                    ctx.throw_syntax_error(msg);
                    return Value();
                }
                // A single argument like "x, y" declares two parameters, so the
                // function's params (and its .length) come from the parse.
                auto clone_params = [](const std::vector<std::unique_ptr<Parameter>>& src) {
                    std::vector<std::unique_ptr<Parameter>> out;
                    for (const auto& p : src)
                        out.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
                    return out;
                };
                if (expr) {
                    if (expr->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* fe = static_cast<FunctionExpression*>(expr.get());
                        if (fe->is_async() && fe->is_generator()) {
                            auto body_clone = fe->get_body() ? fe->get_body()->clone() : nullptr;
                            auto gen_fn = std::make_unique<AsyncGeneratorFunction>("anonymous", clone_params(fe->get_params()), std::move(body_clone), &ctx);
                            gen_fn->set_source_text(toString_src);
                            return Value(gen_fn.release());
                        }
                    } else if (expr->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                        FunctionDeclaration* fd = static_cast<FunctionDeclaration*>(expr.get());
                        if (fd->is_async() && fd->is_generator()) {
                            auto body_clone = fd->get_body() ? fd->get_body()->clone() : nullptr;
                            auto gen_fn = std::make_unique<AsyncGeneratorFunction>("anonymous", clone_params(fd->get_params()), std::move(body_clone), &ctx);
                            gen_fn->set_source_text(toString_src);
                            return Value(gen_fn.release());
                        }
                    }
                }
            } catch (...) {}

            auto gen_fn = std::make_unique<AsyncGeneratorFunction>("anonymous", param_names, nullptr, &ctx);
            gen_fn->set_source_text(toString_src);
            return Value(gen_fn.release());
        });

    async_generator_function_constructor->set_property("name", Value(std::string("AsyncGeneratorFunction")));

    // Both constructor links are { writable: false, enumerable: false, configurable: true };
    // %AsyncGeneratorPrototype%.constructor is %AsyncGeneratorFunction.prototype%, not the ctor.
    if (s_async_generator_function_prototype_) {
        s_async_generator_function_prototype_->set_property("constructor", Value(async_generator_function_constructor.get()), PropertyAttributes::Configurable);
        async_generator_function_constructor->set_property("prototype", Value(s_async_generator_function_prototype_), PropertyAttributes::None);
        s_async_generator_prototype_->set_property("constructor", Value(s_async_generator_function_prototype_), PropertyAttributes::Configurable);
    }

    ctx.create_binding("AsyncGeneratorFunction", Value(async_generator_function_constructor.release()));
}

// AsyncGeneratorEnqueue: a bad `this` rejects the returned promise, never throws.
static Value reject_bad_generator(Context& ctx, const std::string& msg) {
    auto promise_obj = ObjectFactory::create_promise(&ctx);
    Promise* p = static_cast<Promise*>(promise_obj.release());
    ctx.throw_type_error(msg);
    Value err = ctx.get_exception();
    ctx.clear_exception();
    p->reject(err);
    return Value(p);
}

Value AsyncGenerator::async_generator_next(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    AsyncGenerator* async_gen = this_value.is_object() ? dynamic_cast<AsyncGenerator*>(this_value.as_object()) : nullptr;
    if (!async_gen) {
        return reject_bad_generator(ctx, "AsyncGenerator.prototype.next called on incompatible receiver");
    }

    Value value = args.empty() ? Value() : args[0];
    auto result = async_gen->next(value);

    return Value(result.promise.release());
}

Value AsyncGenerator::async_generator_return(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    AsyncGenerator* async_gen = this_value.is_object() ? dynamic_cast<AsyncGenerator*>(this_value.as_object()) : nullptr;
    if (!async_gen) {
        return reject_bad_generator(ctx, "AsyncGenerator.prototype.return called on incompatible receiver");
    }

    Value value = args.empty() ? Value() : args[0];
    auto result = async_gen->return_value(value);

    return Value(result.promise.release());
}

Value AsyncGenerator::async_generator_throw(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    AsyncGenerator* async_gen = this_value.is_object() ? dynamic_cast<AsyncGenerator*>(this_value.as_object()) : nullptr;
    if (!async_gen) {
        return reject_bad_generator(ctx, "AsyncGenerator.prototype.throw called on incompatible receiver");
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
    
    auto next_fn = ObjectFactory::create_native_function("next", async_iterator_next, 1);
    async_iterator_prototype->set_property("next", Value(next_fn.release()), PropertyAttributes::BuiltinFunction);

    auto return_fn = ObjectFactory::create_native_function("return", async_iterator_return, 1);
    async_iterator_prototype->set_property("return", Value(return_fn.release()), PropertyAttributes::BuiltinFunction);

    auto throw_fn = ObjectFactory::create_native_function("throw", async_iterator_throw, 1);
    async_iterator_prototype->set_property("throw", Value(throw_fn.release()), PropertyAttributes::BuiltinFunction);
    
    Symbol* async_iterator_symbol = Symbol::get_well_known(Symbol::ASYNC_ITERATOR);
    if (async_iterator_symbol) {
        auto self_async_iterator_fn = ObjectFactory::create_native_function("[Symbol.asyncIterator]",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                return ctx.get_binding("this");
            }, 0);
        async_iterator_prototype->set_property(async_iterator_symbol->to_property_key(),
            Value(self_async_iterator_fn.release()), PropertyAttributes::BuiltinFunction);
    }

    // %AsyncIteratorPrototype%[@@asyncDispose]: GetMethod(this, "return"), call it,
    // unwrap the result to undefined; every abrupt step rejects the returned promise.
    Symbol* async_dispose_symbol = Symbol::get_well_known(Symbol::ASYNC_DISPOSE);
    if (async_dispose_symbol) {
        auto async_dispose_fn = ObjectFactory::create_native_function("[Symbol.asyncDispose]",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                auto cap_obj = ObjectFactory::create_promise(&ctx);
                Promise* cap = static_cast<Promise*>(cap_obj.release());
                auto reject_pending = [&]() {
                    Value e = ctx.get_exception();
                    ctx.clear_exception();
                    cap->reject(e);
                    return Value(cap);
                };
                Object* obj = ctx.get_this_binding();
                if (!obj) {
                    ctx.throw_type_error("Symbol.asyncDispose requires an object this");
                    return reject_pending();
                }
                Value ret = obj->get_property("return");
                if (ctx.has_exception()) return reject_pending();
                if (ret.is_undefined() || ret.is_null()) {
                    cap->fulfill(Value());
                    return Value(cap);
                }
                if (!ret.is_function()) {
                    ctx.throw_type_error("return method is not callable");
                    return reject_pending();
                }
                Value result = ret.as_function()->call(ctx, {Value()}, Value(obj));
                if (ctx.has_exception()) return reject_pending();
                Promise* rp = (result.is_object() && result.as_object()->get_type() == Object::ObjectType::Promise)
                    ? static_cast<Promise*>(result.as_object())
                    : Promise::resolve(result);
                Value cap_val(static_cast<Object*>(cap));
                auto on_ok = ObjectFactory::create_native_function("",
                    [cap_val](Context&, const std::vector<Value>&) -> Value {
                        static_cast<Promise*>(cap_val.as_object())->fulfill(Value());
                        return Value();
                    }, 1);
                auto on_err = ObjectFactory::create_native_function("",
                    [cap_val](Context&, const std::vector<Value>& a) -> Value {
                        static_cast<Promise*>(cap_val.as_object())->reject(a.empty() ? Value() : a[0]);
                        return Value();
                    }, 1);
                on_ok->set_property("[[Capability]]", cap_val);
                on_err->set_property("[[Capability]]", cap_val);
                Function* on_ok_fn = on_ok.release();
                Function* on_err_fn = on_err.release();
                // Hardening, not a confirmed bug: rp is always a genuine Promise, whose
                // then_records_ already traces on_ok_fn/on_err_fn. Also pin them onto cap
                // (the value this function returns, so definitely reachable) so they don't
                // depend solely on rp's own reachability.
                cap->set_property("__ad_ok__", Value(on_ok_fn), PropertyAttributes::None);
                cap->set_property("__ad_err__", Value(on_err_fn), PropertyAttributes::None);
                rp->then(on_ok_fn, on_err_fn);
                return Value(cap);
            }, 0);
        async_iterator_prototype->set_property(async_dispose_symbol->to_property_key(),
            Value(async_dispose_fn.release()), PropertyAttributes::BuiltinFunction);
    }

    ctx.create_binding("AsyncIteratorPrototype", Value(async_iterator_prototype.release()));
}

Value AsyncIterator::async_iterator_next(Context& ctx, const std::vector<Value>& /* args */) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_type_error("AsyncIterator.next called on non-object");
        return Value();
    }
    
    AsyncIterator* async_iter = dynamic_cast<AsyncIterator*>(this_value.as_object());
    if (!async_iter) {
        ctx.throw_type_error("AsyncIterator.next called on wrong type");
        return Value();
    }
    
    auto promise = async_iter->next();
    
    return Value(promise.release());
}

Value AsyncIterator::async_iterator_return(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_type_error("AsyncIterator.return called on non-object");
        return Value();
    }
    
    AsyncIterator* async_iter = dynamic_cast<AsyncIterator*>(this_value.as_object());
    if (!async_iter) {
        ctx.throw_type_error("AsyncIterator.return called on wrong type");
        return Value();
    }
    
    Value value = args.empty() ? Value() : args[0];
    auto promise = async_iter->return_value(value);
    
    return Value(promise.release());
}

Value AsyncIterator::async_iterator_throw(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_type_error("AsyncIterator.throw called on non-object");
        return Value();
    }
    
    AsyncIterator* async_iter = dynamic_cast<AsyncIterator*>(this_value.as_object());
    if (!async_iter) {
        ctx.throw_type_error("AsyncIterator.throw called on wrong type");
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
    // Only a *callable* "then" makes a value thenable (Promise Resolve Functions step 8).
    return obj->get_property("then").is_function();
}

void call_thenable_job(Context* job_ctx, Function* then_fn, const Value& thenable,
                        const Value& resolve_arg, const Value& reject_arg, Promise* wrapper) {
    if (!job_ctx) return;
    // Queue on the global context, not job_ctx, so this job's relative order against unrelated
    // Promise chains (which always schedule on the global queue) matches real chronological
    // enqueue order -- per-instance queues don't interleave across each other.
    Context* queue_ctx = job_ctx->get_engine() && job_ctx->get_engine()->get_global_context()
        ? job_ctx->get_engine()->get_global_context() : job_ctx;
    queue_ctx->queue_microtask([job_ctx, then_fn, thenable, resolve_arg, reject_arg, wrapper]() {
        then_fn->call(*job_ctx, {resolve_arg, reject_arg}, thenable);
        if (job_ctx->has_exception()) {
            Value exc = job_ctx->get_exception();
            job_ctx->clear_exception();
            if (wrapper) wrapper->reject(exc);
        }
    }, {Value(then_fn), thenable, resolve_arg, reject_arg, Value(wrapper)});
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

            // Mirrors Promise::fulfill's __trp_res__/__trp_rej__ pin (Promise.cpp:176-177):
            // the thenable may call resolve/reject asynchronously, well after this
            // function returns, so pin both onto promise itself to keep them alive.
            promise->set_property("__trp_res__", Value(resolve_fn.get()), PropertyAttributes::None);
            promise->set_property("__trp_rej__", Value(reject_fn.get()), PropertyAttributes::None);

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

    auto async_function_constructor = ObjectFactory::create_native_constructor("AsyncFunction",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::string params_str = "";
            std::string body_str = "";

            if (args.size() > 1) {
                body_str = args.back().to_string();
                for (size_t i = 0; i < args.size() - 1; ++i) {
                    if (i > 0) params_str += ",";
                    params_str += args[i].to_string();
                }
            } else if (args.size() == 1) {
                body_str = args[0].to_string();
            }

            std::string toString_src = "async function anonymous(" + params_str + "\n) {\n" + body_str + "\n}";
            std::string func_code = "(" + toString_src + ")";
            try {
                Lexer lexer(func_code);
                TokenSequence tokens = lexer.tokenize();
                Parser::ParseOptions opts;
                Parser parser(tokens, opts);
                parser.set_source(func_code);
                auto expr = parser.parse_expression();
                if (parser.has_errors() || !expr) {
                    ctx.throw_syntax_error("Invalid async function body");
                    return Value();
                }
                // Build the AsyncFunction directly (like GeneratorFunction does) instead of
                // expr->evaluate(), which doesn't preserve toString_src.
                std::vector<std::unique_ptr<Parameter>> param_clones;
                ASTNode* body_node = nullptr;
                bool matched = false;
                if (expr->get_type() == ASTNode::Type::ASYNC_FUNCTION_EXPRESSION) {
                    AsyncFunctionExpression* fe = static_cast<AsyncFunctionExpression*>(expr.get());
                    for (const auto& p : fe->get_params()) param_clones.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
                    body_node = fe->get_body();
                    matched = true;
                } else if (expr->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
                    FunctionExpression* fe = static_cast<FunctionExpression*>(expr.get());
                    if (fe->is_async() && !fe->is_generator()) {
                        for (const auto& p : fe->get_params()) param_clones.push_back(std::unique_ptr<Parameter>(static_cast<Parameter*>(p->clone().release())));
                        body_node = fe->get_body();
                        matched = true;
                    }
                }
                if (matched) {
                    auto body_clone = body_node ? body_node->clone() : nullptr;
                    auto async_fn = std::make_unique<AsyncFunction>("anonymous", std::move(param_clones), std::move(body_clone), &ctx);
                    async_fn->set_source_text(toString_src);
                    if (ctx.has_binding("@@AsyncFunction")) {
                        Value async_ctor = ctx.get_binding("@@AsyncFunction");
                        if (async_ctor.is_function()) {
                            Value proto = async_ctor.as_function()->get_property("prototype");
                            if (proto.is_object()) async_fn->set_prototype(proto.as_object());
                        }
                    }
                    return Value(async_fn.release());
                }
                Value result = expr->evaluate(ctx);
                if (result.is_function()) result.as_function()->set_source_text(toString_src);
                return result;
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

    // Per spec, AsyncFunction's [[Prototype]] is the Function constructor itself, not Function.prototype.
    Object* fn_ctor = ctx.get_built_in_object("Function");
    if (fn_ctor) async_function_constructor->set_prototype(fn_ctor);

    ctx.create_binding("@@AsyncFunction", Value(async_function_constructor.release()));

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
    }
    return true;
}

EventLoop& EventLoop::instance() {
    static thread_local EventLoop instance;
    return instance;
}


AsyncGeneratorFunction::AsyncGeneratorFunction(const std::string& name,
                                               const std::vector<std::string>& params,
                                               std::unique_ptr<ASTNode> body,
                                               Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {
    set_is_constructor(false);
    // Each async generator function gets a unique 'prototype' object inheriting from %AsyncGeneratorPrototype%
    if (AsyncGenerator::s_async_generator_prototype_) {
        auto fn_proto = ObjectFactory::create_object();
        fn_proto->set_prototype(AsyncGenerator::s_async_generator_prototype_);
        PropertyDescriptor proto_desc(Value(fn_proto.release()), PropertyAttributes::Writable);
        this->set_property_descriptor("prototype", proto_desc);
        if (AsyncGenerator::s_async_generator_function_prototype_) {
            this->set_prototype(AsyncGenerator::s_async_generator_function_prototype_);
        }
    }
}

AsyncGeneratorFunction::AsyncGeneratorFunction(const std::string& name,
                                               std::vector<std::unique_ptr<Parameter>> params,
                                               std::unique_ptr<ASTNode> body,
                                               Context* closure_context)
    : Function(name, std::move(params), nullptr, closure_context), body_(std::move(body)) {
    set_is_constructor(false);
    if (AsyncGenerator::s_async_generator_prototype_) {
        auto fn_proto = ObjectFactory::create_object();
        fn_proto->set_prototype(AsyncGenerator::s_async_generator_prototype_);
        PropertyDescriptor proto_desc(Value(fn_proto.release()), PropertyAttributes::Writable);
        this->set_property_descriptor("prototype", proto_desc);
        if (AsyncGenerator::s_async_generator_function_prototype_) {
            this->set_prototype(AsyncGenerator::s_async_generator_function_prototype_);
        }
    }
}

Value AsyncGeneratorFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    auto gen_ctx = ContextFactory::create_function_context(ctx.get_engine(), &ctx, this);
    ExecContextScope gc_frame(gen_ctx.get());
    // See ContextSurvivorGuard's doc comment: no-op once gen_ctx is moved
    // into the AsyncGenerator below; catches an abrupt exit before that point
    // (e.g. a default-parameter closure that captured gen_ctx and throws).
    ContextSurvivorGuard survivor_guard(gen_ctx, ctx.get_engine());

    if (is_strict()) gen_ctx->set_strict_mode(true);

    Value bound_this = this_value;
    if (is_arrow() && has_property("__arrow_this__")) {
        bound_this = get_property("__arrow_this__");
    } else if (!gen_ctx->is_strict_mode()) {
        if (bound_this.is_undefined() || bound_this.is_null()) {
            Object* global = ctx.get_global_object();
            if (global) bound_this = Value(global);
        } else {
            bound_this = ObjectFactory::box_primitive_this_sloppy(ctx, bound_this);
        }
    }
    try {
        gen_ctx->create_binding("this", bound_this, true);
    } catch (...) {
        gen_ctx->set_binding("this", bound_this);
    }

    // A named class's own name is bound as an immutable self-reference inside its
    // methods (__closure_const_<name>) -- see ClassDeclaration::evaluate. Everything
    // else resolves through closure_environment_, no materialization needed.
    auto prop_keys = get_internal_property_keys();
    for (const auto& key : prop_keys) {
        if (key.length() > 10 && key.substr(0, 10) == "__closure_" && key.substr(0, 16) != "__closure_const_") {
            std::string var_name = key.substr(10);
            if (has_property("__closure_const_" + var_name)) {
                Value closure_value = get_property(key);
                gen_ctx->create_lexical_binding(var_name, closure_value, false);
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
                Value rest_val(rest_arr.release());
                if (param->has_destructuring()) {
                    auto* destr = dynamic_cast<DestructuringAssignment*>(param->get_destructuring_pattern());
                    if (destr) {
                        destr->evaluate_with_value(*gen_ctx, rest_val);
                        if (gen_ctx->has_exception()) {
                            gen_ctx->set_in_param_eval(false);
                            ctx.throw_exception(gen_ctx->get_exception(), true);
                            return Value();
                        }
                    }
                } else {
                    gen_ctx->create_binding(param->get_name()->get_name(), rest_val, false);
                }
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

    // Compile now so the arguments-object check below can see needs_arguments.
    const BytecodeChunk* susp_chunk = get_suspendable_chunk(*gen_ctx);

    // The chunk always compiles with an empty parameter list (see
    // VM::compile_suspendable), so needs_arguments only reflects the body;
    // check defaults/destructuring for `arguments` separately.
    bool params_need_arguments = false;
    for (const auto& p : param_objs) {
        if ((p->has_default() && BytecodeCompiler::references_arguments(p->get_default_value())) ||
            (p->has_destructuring() && BytecodeCompiler::references_arguments(p->get_destructuring_pattern()))) {
            params_need_arguments = true;
            break;
        }
    }

    // arguments object -- skipped when nothing needs it, mirroring Function::call.
    if (!susp_chunk || susp_chunk->needs_arguments || params_need_arguments) {
        auto arguments_obj = ObjectFactory::create_array(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            arguments_obj->set_element(static_cast<uint32_t>(i), args[i]);
        }
        arguments_obj->set_property("length", Value(static_cast<double>(args.size())));
        arguments_obj->set_type(Object::ObjectType::Arguments);
        setup_mapped_arguments(*gen_ctx, args, arguments_obj.get());
        gen_ctx->create_binding("arguments", Value(arguments_obj.release()), false);
    }

    // FDI step 27: param-default closures must not see the body's `var`s, so the body gets its own variable environment.
    {
        bool has_complex_params = false;
        for (const auto& p : param_objs) {
            if (p->has_default() || p->has_destructuring()) { has_complex_params = true; break; }
        }
        if (has_complex_params) {
            gen_ctx->push_block_scope();
            gen_ctx->set_variable_environment(gen_ctx->get_lexical_environment());
        }
    }

    // FunctionDeclarationInstantiation: hoist `var` declarations to the top of
    // the function body before it executes (see AsyncFunction::call for rationale).
    if (body_ && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        scan_for_var_declarations(body_.get(), *gen_ctx);
    }

    Context* outer_ctx = ctx.get_engine() ? ctx.get_engine()->get_global_context() : &ctx;
    auto async_gen = std::make_unique<AsyncGenerator>(std::move(gen_ctx), body_.get(), this, outer_ctx);
    // OrdinaryCreateFromConstructor: the instance inherits from this function's
    // own "prototype" object, not %AsyncGeneratorPrototype% directly.
    Value own_proto = get_property("prototype");
    if (own_proto.is_object()) async_gen->set_prototype(own_proto.as_object());
    return Value(async_gen.release());
}

const BytecodeChunk* AsyncGeneratorFunction::get_suspendable_chunk(Context& ctx) {
    if (suspendable_incompatible_) return nullptr;
    if (suspendable_chunk_) return suspendable_chunk_.get();
    // Same `with`-chain check as Function::call; fixed at closure creation.
    for (Environment* e = ctx.get_lexical_environment(); e; e = e->get_outer()) {
        if (e->is_with_environment()) { suspendable_incompatible_ = true; return nullptr; }
    }
    suspendable_chunk_ = VM::compile_suspendable(body_.get());
    if (!suspendable_chunk_) { suspendable_incompatible_ = true; return nullptr; }
    Collector::write_barrier(this);
    return suspendable_chunk_.get();
}

void AsyncGeneratorFunction::trace(Visitor& v) {
    Function::trace(v);
    if (suspendable_chunk_) suspendable_chunk_->trace(v);
}

}
