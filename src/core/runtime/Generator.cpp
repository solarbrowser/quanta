/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Generator.h"
#include "quanta/core/runtime/FiberStackPool.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/gc/FiberRegistry.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/parser/AST.h"
#include "quanta/parser/Parser.h"
#include "quanta/core/vm/Interpreter.h"
#include <iostream>

namespace Quanta {

void Generator::trace(Visitor& v) {
    Object::trace(v);
    v.visit_object(generator_function_);
    v.visit_context(generator_context_);
    v.visit_context(outer_context_);
    v.visit(yielded_value_);
    v.visit(yielded_result_);
    v.visit(sent_value_);
    v.visit(throw_value_);
    v.visit(return_argument_);
    v.visit(last_value_);
    for (const auto& val : sent_values_) v.visit(val);
}



thread_local Generator* Generator::current_generator_ = nullptr;
thread_local size_t Generator::current_yield_counter_ = 0;
thread_local Object* Generator::s_generator_prototype_ = nullptr;
thread_local Object* Generator::s_generator_function_prototype_ = nullptr;

void Generator::fiber_entry(uint32_t lo, uint32_t hi) {
    uintptr_t ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    Generator* gen = reinterpret_cast<Generator*>(ptr);
    gen->run_body();
}

void Generator::run_body() {
    try {
        if (body_) {
            // Bindings already live in generator_context_; a delegated yield
            // suspends the fiber from inside the VM dispatch loop, so the
            // compiled form needs no resumable state of its own.
            bool used_vm = false;
            Value vm_result = VM::run_suspendable(body_.get(), *generator_context_, used_vm);
            if (used_vm) {
                if (!vm_result.is_undefined() && !generator_context_->has_return_value() &&
                    !generator_context_->has_exception()) {
                    generator_context_->set_return_value(vm_result);
                }
            } else {
                body_->evaluate(*generator_context_);
            }
        }
    } catch (const GeneratorReturnException&) {
        // return() called -- generator terminated cleanly
    } catch (const std::exception&) {
        // Other exceptions -- propagated via generator_context_
    } catch (...) {}
    state_ = State::Completed;
    swapcontext(&fiber_->fiber_ctx, &fiber_->caller_ctx);
}

Generator::Generator(Function* gen_func, Context* ctx, std::unique_ptr<ASTNode> body, Context* outer_ctx)
    : Object(ObjectType::Custom), generator_function_(gen_func), generator_context_(ctx),
      body_(std::move(body)), state_(State::SuspendedStart),
      fiber_stack_(FiberStackPool::acquire(STACK_SIZE)), outer_context_(outer_ctx) {

    if (gen_func) {
        Value fn_proto = gen_func->get_property("prototype");
        if (fn_proto.is_object()) {
            set_prototype(fn_proto.as_object());
        } else if (s_generator_prototype_) {
            set_prototype(s_generator_prototype_);
        }
    }

    // Set up fiber context
    getcontext(&fiber_->fiber_ctx);
    fiber_->fiber_ctx.uc_stack.ss_sp   = fiber_stack_;
    fiber_->fiber_ctx.uc_stack.ss_size = STACK_SIZE;
    fiber_->fiber_ctx.uc_link = nullptr;
    uintptr_t ptr = reinterpret_cast<uintptr_t>(this);
    makecontext(&fiber_->fiber_ctx, (void(*)())fiber_entry, 2,
                (uint32_t)(ptr & 0xFFFFFFFF), (uint32_t)(ptr >> 32));
    FiberRegistry::register_fiber(this, fiber_stack_, STACK_SIZE, fiber_.get(), this);
}

Generator::~Generator() {
    FiberRegistry::unregister_fiber(this);
    if (fiber_stack_) FiberStackPool::release(fiber_stack_, STACK_SIZE);
}

Generator::GeneratorResult Generator::next(const Value& value) {
    if (state_ == State::Completed) {
        return GeneratorResult(Value(), true);
    }
    sent_value_ = value;
    throwing_ = false;
    returning_ = false;

    state_ = State::Executing;
    Generator* prev = current_generator_;
    current_generator_ = this;
    {
        FiberEnterScope enter_scope;
        swapcontext(&fiber_->caller_ctx, &fiber_->fiber_ctx);
    }
    current_generator_ = prev;

    if (state_ == State::Completed) {
        if (generator_context_->has_exception()) {
            Value exc = generator_context_->get_exception();
            generator_context_->clear_exception();
            return GeneratorResult::make_exception(exc);
        }
        Value ret_val;
        if (generator_context_->has_return_value()) {
            ret_val = generator_context_->get_return_value();
            generator_context_->clear_return_value();
        }
        return GeneratorResult(ret_val, true);
    }
    if (generator_context_->has_exception()) {
        Value exc = generator_context_->get_exception();
        generator_context_->clear_exception();
        return GeneratorResult::make_exception(exc);
    }
    if (yield_raw_result_) {
        yield_raw_result_ = false;
        return GeneratorResult::make_raw(yielded_result_);
    }
    return GeneratorResult(yielded_value_, false);
}

Generator::GeneratorResult Generator::return_value(const Value& value) {
    if (state_ == State::Completed || state_ == State::SuspendedStart) {
        state_ = State::Completed;
        return GeneratorResult(value, true);
    }
    returning_ = true;
    return_argument_ = value;
    sent_value_ = Value();

    state_ = State::Executing;
    Generator* prev = current_generator_;
    current_generator_ = this;
    {
        FiberEnterScope enter_scope;
        swapcontext(&fiber_->caller_ctx, &fiber_->fiber_ctx);
    }
    current_generator_ = prev;

    // Check if the fiber propagated an exception (e.g. IteratorClose threw TypeError).
    if (generator_context_->has_exception()) {
        Value exc = generator_context_->get_exception();
        generator_context_->clear_exception();
        return GeneratorResult::make_exception(exc);
    }
    // A `finally` block's own return overrides the return() completion -- same precedence
    // as next()'s check below.
    if (generator_context_->has_return_value()) {
        Value ret_val = generator_context_->get_return_value();
        generator_context_->clear_return_value();
        return GeneratorResult(ret_val, true);
    }
    // The fiber may have updated return_argument_ (e.g. yield* delegating to inner
    // iterator whose return() returns a different value). Use it as the return value.
    return GeneratorResult(return_argument_, true);
}

Generator::GeneratorResult Generator::throw_exception(const Value& exception) {
    if (state_ == State::Completed || state_ == State::SuspendedStart) {
        // Per spec 27.5.3.4: throw() on a never-started/completed generator completes it directly.
        state_ = State::Completed;
        generator_context_->throw_exception(exception, true);
        return GeneratorResult(Value(), true);
    }
    throwing_ = true;
    throw_value_ = exception;
    sent_value_ = Value();

    state_ = State::Executing;
    Generator* prev = current_generator_;
    current_generator_ = this;
    {
        FiberEnterScope enter_scope;
        swapcontext(&fiber_->caller_ctx, &fiber_->fiber_ctx);
    }
    current_generator_ = prev;

    if (state_ == State::Completed) {
        if (generator_context_->has_exception()) {
            Value exc = generator_context_->get_exception();
            generator_context_->clear_exception();
            return GeneratorResult::make_exception(exc);
        }
        Value ret_val;
        if (generator_context_->has_return_value()) {
            ret_val = generator_context_->get_return_value();
            generator_context_->clear_return_value();
        }
        return GeneratorResult(ret_val, true);
    }
    if (generator_context_->has_exception()) {
        Value exc = generator_context_->get_exception();
        generator_context_->clear_exception();
        return GeneratorResult::make_exception(exc);
    }
    return GeneratorResult(yielded_value_, false);
}

Value Generator::get_iterator() {
    return Value(this);
}

Generator::GeneratorResult Generator::execute_until_yield(const Value& sent_value) {
    if (!body_) {
        complete_generator(Value());
        return GeneratorResult(Value(), true);
    }

    try {
        last_value_ = sent_value;

        size_t store_idx = target_yield_index_;
        if (store_idx >= sent_values_.size()) {
            sent_values_.resize(store_idx + 1);
        }
        sent_values_[store_idx] = sent_value;

        set_current_generator(this);
        reset_yield_counter();
        target_yield_index_++;

        Value result = body_->evaluate(*generator_context_);

        set_current_generator(nullptr);
        if (generator_context_->has_exception()) {
            Value exc = generator_context_->get_exception();
            generator_context_->clear_exception();
            complete_generator(Value());
            return GeneratorResult::make_exception(exc);
        }
        complete_generator(result);
        return GeneratorResult(result, true);

    } catch (const YieldException& yield_ex) {
        set_current_generator(nullptr);
        state_ = State::SuspendedYield;
        return GeneratorResult(yield_ex.yielded_value, false);

    } catch (const std::exception& e) {
        set_current_generator(nullptr);
        complete_generator(Value());
        return GeneratorResult::make_exception(Value(std::string(e.what())));
    }
}

Generator::GeneratorResult Generator::execute_until_yield_throw(const Value& exception) {
    if (!body_) {
        complete_generator(Value());
        generator_context_->throw_exception(exception);
        return GeneratorResult(Value(), true);
    }

    try {
        throwing_ = true;
        throw_value_ = exception;

        // Clear any stale exception on the generator context
        generator_context_->clear_exception();

        set_current_generator(this);
        reset_yield_counter();
        // Do NOT increment target_yield_index_ - throw at the current suspended yield

        Value result = body_->evaluate(*generator_context_);

        set_current_generator(nullptr);
        throwing_ = false;
        if (generator_context_->has_exception()) {
            Value exc = generator_context_->get_exception();
            generator_context_->clear_exception();
            complete_generator(Value());
            return GeneratorResult::make_exception(exc);
        }
        complete_generator(result);

        return GeneratorResult(result, true);

    } catch (const YieldException& yield_ex) {
        set_current_generator(nullptr);
        throwing_ = false;
        state_ = State::SuspendedYield;
        return GeneratorResult(yield_ex.yielded_value, false);

    } catch (const std::exception& e) {
        set_current_generator(nullptr);
        throwing_ = false;
        complete_generator(Value());
        return GeneratorResult::make_exception(Value(std::string(e.what())));
    }
}

Generator::GeneratorResult Generator::execute_until_yield_return(const Value& value) {
    if (!body_) {
        complete_generator(value);
        return GeneratorResult(value, true);
    }

    // If the generator is already suspended after yielding, completing it via return()
    // should NOT re-run the body (the replay mechanism would re-execute pre-yield code).
    // Per spec, return() causes a return completion at the current suspension point.
    if (state_ == State::SuspendedYield && target_yield_index_ > 0) {
        complete_generator(value);
        return GeneratorResult(value, true);
    }

    try {
        returning_ = true;
        return_argument_ = value;

        generator_context_->clear_exception();

        set_current_generator(this);
        reset_yield_counter();

        body_->evaluate(*generator_context_);

        set_current_generator(nullptr);
        returning_ = false;
        complete_generator(value);
        return GeneratorResult(value, true);

    } catch (const YieldException&) {
        set_current_generator(nullptr);
        returning_ = false;
        complete_generator(value);
        return GeneratorResult(value, true);

    } catch (const std::exception& e) {
        set_current_generator(nullptr);
        returning_ = false;
        complete_generator(Value());
        generator_context_->throw_exception(Value(std::string(e.what())));
        return GeneratorResult(Value(), true);
    }
}

void Generator::complete_generator(const Value& value) {
    state_ = State::Completed;
    last_value_ = value;
}

Value Generator::generator_next(Context& ctx, const std::vector<Value>& args) {
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_type_error("Generator.prototype.next requires a generator this");
        return Value();
    }

    if (this_obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_type_error("Generator.prototype.next called on non-generator");
        return Value();
    }

    Generator* generator = static_cast<Generator*>(this_obj);
    if (generator->get_state() == Generator::State::Executing) {
        ctx.throw_type_error("Generator is already executing");
        return Value();
    }
    Value sent_value = args.empty() ? Value() : args[0];

    auto result = generator->next(sent_value);

    if (result.has_exception) {
        ctx.throw_exception(result.exception, true);
        return Value();
    }

    // yield* propagates the inner iterator's result object directly
    if (result.raw_result) return result.value;

    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));

    return Value(result_obj.release());
}

Value Generator::generator_return(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_type_error("Generator.prototype.return called on non-object");
        return Value();
    }

    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_type_error("Generator.prototype.return called on non-generator");
        return Value();
    }

    Generator* generator = static_cast<Generator*>(obj);
    if (generator->get_state() == Generator::State::Executing) {
        ctx.throw_type_error("Generator is already executing");
        return Value();
    }
    Value return_val = args.empty() ? Value() : args[0];

    auto result = generator->return_value(return_val);

    if (result.has_exception) {
        ctx.throw_exception(result.exception, true);
        return Value();
    }

    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));

    return Value(result_obj.release());
}

Value Generator::generator_throw(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_type_error("Generator.prototype.throw called on non-object");
        return Value();
    }

    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_type_error("Generator.prototype.throw called on non-generator");
        return Value();
    }

    Generator* generator = static_cast<Generator*>(obj);
    if (generator->get_state() == Generator::State::Executing) {
        ctx.throw_type_error("Generator is already executing");
        return Value();
    }
    Value exception = args.empty() ? Value() : args[0];

    auto result = generator->throw_exception(exception);

    if (result.has_exception) {
        ctx.throw_exception(result.exception, true);
        return Value();
    }

    // If the generator context has an uncaught exception, propagate to outer context
    if (generator->generator_context_->has_exception()) {
        Value ex = generator->generator_context_->get_exception();
        generator->generator_context_->clear_exception();
        ctx.throw_exception(ex);
        return Value();
    }

    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));

    return Value(result_obj.release());
}

void Generator::setup_generator_prototype(Context& ctx) {
    // %GeneratorPrototype% inherits from %IteratorPrototype%, has own next/return/throw
    auto gen_prototype = ObjectFactory::create_object();
    if (Iterator::s_iterator_prototype_) {
        gen_prototype->set_prototype(Iterator::s_iterator_prototype_);
    }

    auto next_fn = ObjectFactory::create_native_function("next", generator_next, 1);
    gen_prototype->set_property("next", Value(next_fn.release()), PropertyAttributes::BuiltinFunction);

    auto return_fn = ObjectFactory::create_native_function("return", generator_return, 1);
    gen_prototype->set_property("return", Value(return_fn.release()), PropertyAttributes::BuiltinFunction);

    auto throw_fn = ObjectFactory::create_native_function("throw", generator_throw, 1);
    gen_prototype->set_property("throw", Value(throw_fn.release()), PropertyAttributes::BuiltinFunction);

    // Generators are both iterators and iterables: [Symbol.iterator]() returns this
    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
    if (iter_sym) {
        auto iter_fn = ObjectFactory::create_native_function("@@iterator",
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args;
                return ctx.get_binding("this");
            });
        gen_prototype->set_property(iter_sym->to_property_key(), Value(iter_fn.release()));
    }

    Symbol* tag_sym = Symbol::get_well_known(Symbol::TO_STRING_TAG);
    if (tag_sym) {
        PropertyDescriptor gen_tag(Value(std::string("Generator")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
        gen_prototype->set_property_descriptor(tag_sym->to_property_key(), gen_tag);
    }

    s_generator_prototype_ = gen_prototype.get();
    ctx.create_binding("@@GeneratorPrototype", Value(gen_prototype.release()));

    // %GeneratorFunction.prototype% -- [[Prototype]] of all generator functions
    // Per spec: %GeneratorFunction.prototype%.[[Prototype]] = %Function.prototype%
    auto gen_fn_proto = ObjectFactory::create_object();
    Object* func_proto = ObjectFactory::get_function_prototype();
    gen_fn_proto->set_prototype(func_proto ? func_proto : s_generator_prototype_);
    if (tag_sym) {
        PropertyDescriptor gf_tag(Value(std::string("GeneratorFunction")), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
        gen_fn_proto->set_property_descriptor(tag_sym->to_property_key(), gf_tag);
    }
    // %GeneratorFunction.prototype%.prototype = %GeneratorPrototype% (27.3.3.2: non-writable, non-enumerable, configurable)
    PropertyDescriptor gfp_proto_desc(Value(s_generator_prototype_), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    gen_fn_proto->set_property_descriptor("prototype", gfp_proto_desc);

    s_generator_function_prototype_ = gen_fn_proto.get();
    ctx.create_binding("@@GeneratorFunctionPrototype", Value(gen_fn_proto.release()));

    // GeneratorFunction constructor
    auto generator_function_constructor = ObjectFactory::create_native_constructor("GeneratorFunction",
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
            std::string toString_src = "function* anonymous(" + params_str + "\n) {\n" + body_str + "\n}";
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
                        if (fe->is_generator()) {
                            auto body_clone = fe->get_body() ? fe->get_body()->clone() : nullptr;
                            auto gen_fn = std::make_unique<GeneratorFunction>("anonymous", clone_params(fe->get_params()), std::move(body_clone), &ctx);
                            gen_fn->set_source_text(toString_src);
                            return Value(gen_fn.release());
                        }
                    } else if (expr->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                        FunctionDeclaration* fd = static_cast<FunctionDeclaration*>(expr.get());
                        if (fd->is_generator()) {
                            auto body_clone = fd->get_body() ? fd->get_body()->clone() : nullptr;
                            auto gen_fn = std::make_unique<GeneratorFunction>("anonymous", clone_params(fd->get_params()), std::move(body_clone), &ctx);
                            gen_fn->set_source_text(toString_src);
                            return Value(gen_fn.release());
                        }
                    }
                }
            } catch (...) {}

            auto gen_fn = std::make_unique<GeneratorFunction>("anonymous", param_names, nullptr, &ctx);
            gen_fn->set_source_text(toString_src);
            return Value(gen_fn.release());
        });

    generator_function_constructor->set_property("name", Value(std::string("GeneratorFunction")));

    if (s_generator_function_prototype_) {
        s_generator_function_prototype_->set_property("constructor", Value(generator_function_constructor.get()), PropertyAttributes::Configurable);
        generator_function_constructor->set_property("prototype", Value(s_generator_function_prototype_), PropertyAttributes::None);
    }
    // Per spec 27.5.1.1: %GeneratorPrototype%.constructor is %GeneratorFunction.prototype%, not %GeneratorFunction%.
    PropertyDescriptor gp_ctor_desc(Value(s_generator_function_prototype_),
        static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
    s_generator_prototype_->set_property_descriptor("constructor", gp_ctor_desc);

    ctx.create_binding("GeneratorFunction", Value(generator_function_constructor.release()));
}

void Generator::set_current_generator(Generator* gen) {
    current_generator_ = gen;
}

Generator* Generator::get_current_generator() {
    return current_generator_;
}

size_t Generator::increment_yield_counter() {
    return ++current_yield_counter_;
}

void Generator::reset_yield_counter() {
    current_yield_counter_ = 0;
}


GeneratorFunction::GeneratorFunction(const std::string& name,
                                   std::vector<std::unique_ptr<Parameter>> params,
                                   std::unique_ptr<ASTNode> body,
                                   Context* closure_context)
    : Function(name, std::move(params), nullptr, closure_context), body_(std::move(body)) {
    set_is_constructor(false);
    if (Generator::s_generator_prototype_) {
        auto fn_proto = ObjectFactory::create_object();
        fn_proto->set_prototype(Generator::s_generator_prototype_);
        // Spec 25.2.4.2: no own properties; 25.2.4.3: writable, non-enumerable, non-configurable
        PropertyDescriptor proto_desc(Value(fn_proto.release()), PropertyAttributes::Writable);
        this->set_property_descriptor("prototype", proto_desc);
        if (Generator::s_generator_function_prototype_) {
            this->set_prototype(Generator::s_generator_function_prototype_);
        }
    }
}

GeneratorFunction::GeneratorFunction(const std::string& name,
                                   const std::vector<std::string>& params,
                                   std::unique_ptr<ASTNode> body,
                                   Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {
    set_is_constructor(false);
    // Each generator function gets a unique 'prototype' object inheriting from %GeneratorPrototype%
    if (Generator::s_generator_prototype_) {
        auto fn_proto = ObjectFactory::create_object();
        fn_proto->set_prototype(Generator::s_generator_prototype_);
        // Spec 25.2.4.2: no own properties; 25.2.4.3: writable, non-enumerable, non-configurable
        PropertyDescriptor proto_desc(Value(fn_proto.release()), PropertyAttributes::Writable);
        this->set_property_descriptor("prototype", proto_desc);

        if (Generator::s_generator_function_prototype_) {
            this->set_prototype(Generator::s_generator_function_prototype_);
        }
    }
}

Value GeneratorFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    auto generator = create_generator(ctx, args, this_value);
    return Value(generator.release());
}

std::unique_ptr<Generator> GeneratorFunction::create_generator(Context& ctx, const std::vector<Value>& args, Value this_value) {
    // Use proper function context with lexical environment (same as Function::call)
    auto gen_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), &ctx, this);
    ExecContextScope gc_frame(gen_context_ptr.get());
    // See ContextSurvivorGuard's doc comment: an abrupt exit (e.g. a default
    // parameter's closure throwing) must not free a context a live closure
    // still references. No-op on the success path (context is .release()'d
    // into the Generator below, before this guard's destructor runs).
    ContextSurvivorGuard survivor_guard(gen_context_ptr, ctx.get_engine());
    Context& gen_context = *gen_context_ptr;

    if (is_strict()) gen_context.set_strict_mode(true);

    // Bind 'this' with sloppy-mode global coercion
    Value bound_this_g = this_value;
    if (is_arrow() && has_property("__arrow_this__")) {
        bound_this_g = get_property("__arrow_this__");
    } else if (!gen_context.is_strict_mode()) {
        if (bound_this_g.is_undefined() || bound_this_g.is_null()) {
            Object* global = ctx.get_global_object();
            if (global) bound_this_g = Value(global);
        } else {
            bound_this_g = ObjectFactory::box_primitive_this_sloppy(ctx, bound_this_g);
        }
    }
    try {
        gen_context.create_binding("this", bound_this_g, true);
    } catch (...) {
        gen_context.set_binding("this", bound_this_g);
    }

    // A named class's own name is bound as an immutable self-reference inside its
    // methods (__closure_const_<name>) -- see ClassDeclaration::evaluate. Everything
    // else resolves through closure_environment_, no materialization needed.
    auto prop_keys = this->get_internal_property_keys();
    for (const auto& key : prop_keys) {
        if (key.length() > 10 && key.substr(0, 10) == "__closure_" && key.substr(0, 16) != "__closure_const_") {
            std::string var_name = key.substr(10);
            if (this->has_property("__closure_const_" + var_name)) {
                Value closure_value = this->get_property(key);
                gen_context.create_lexical_binding(var_name, closure_value, false);
            }
        }
    }

    // Spec 15.8.4 NamedEvaluation / FunctionDeclarationInstantiation: a named
    // GeneratorExpression binds its own name as an immutable self-reference
    // inside its body (assignment is silently ignored in sloppy mode, throws in strict).
    {
        const std::string& fn_name = this->get_name();
        if (!fn_name.empty() && fn_name != "<anonymous>" && !gen_context.has_binding(fn_name)) {
            gen_context.create_binding(fn_name, Value(this), false);
        }
    }

    // Create arguments object BEFORE default param evaluation (default exprs can reference arguments)
    {
        auto arguments_obj = ObjectFactory::create_array(args.size());
        for (size_t i = 0; i < args.size(); ++i)
            arguments_obj->set_element(static_cast<uint32_t>(i), args[i]);
        arguments_obj->set_property("length", Value(static_cast<double>(args.size())));
        arguments_obj->set_type(Object::ObjectType::Arguments);
        setup_mapped_arguments(gen_context, args, arguments_obj.get());
        gen_context.create_binding("arguments", Value(arguments_obj.release()), false);
    }

    // Bind parameters
    const auto& param_objs = get_parameter_objects();
    if (!param_objs.empty()) {
        size_t regular_count = 0;
        for (const auto& p : param_objs) { if (!p->is_rest()) regular_count++; }
        gen_context.set_eval_arguments_conflict(true);
        {
            std::unordered_set<std::string> pnames;
            for (const auto& p : param_objs) {
                if (p->get_name() && !p->get_name()->get_name().empty())
                    pnames.insert(p->get_name()->get_name());
            }
            gen_context.set_eval_param_names(std::move(pnames));
        }
        gen_context.set_in_param_eval(true);
        for (size_t i = 0; i < param_objs.size(); ++i) {
            const auto& param = param_objs[i];
            if (param->is_rest()) {
                auto rest_arr = ObjectFactory::create_array(0);
                for (size_t j = regular_count; j < args.size(); ++j) rest_arr->push(args[j]);
                Value rest_val(rest_arr.release());
                if (param->has_destructuring()) {
                    auto* destr = dynamic_cast<DestructuringAssignment*>(param->get_destructuring_pattern());
                    if (destr) {
                        destr->evaluate_with_value(gen_context, rest_val);
                        if (gen_context.has_exception()) {
                            gen_context.set_in_param_eval(false);
                            ctx.throw_exception(gen_context.get_exception(), true);
                            return nullptr;
                        }
                    }
                } else {
                    gen_context.create_binding(param->get_name()->get_name(), rest_val, false);
                }
            } else {
                const std::string& pname = param->get_name() ? param->get_name()->get_name() : std::string();
                // Create TDZ binding first so self-referential defaults (x = x) throw ReferenceError
                if (!pname.empty() && !param->has_destructuring()) {
                    if (gen_context.get_lexical_environment())
                        gen_context.get_lexical_environment()->create_uninitialized_binding(pname);
                }
                Value arg_val;
                if (i < args.size() && !args[i].is_undefined()) {
                    arg_val = args[i];
                } else if (param->has_default()) {
                    arg_val = param->get_default_value()->evaluate(gen_context);
                    if (gen_context.has_exception()) {
                        gen_context.set_in_param_eval(false);
                        ctx.throw_exception(gen_context.get_exception(), true);
                        return nullptr;
                    }
                }
                if (param->has_destructuring()) {
                    auto* pat = param->get_destructuring_pattern();
                    auto* destr = dynamic_cast<DestructuringAssignment*>(pat);
                    if (destr) {
                        destr->evaluate_with_value(gen_context, arg_val);
                        if (gen_context.has_exception()) {
                            gen_context.set_in_param_eval(false);
                            ctx.throw_exception(gen_context.get_exception(), true);
                            return nullptr;
                        }
                    }
                } else if (!pname.empty()) {
                    // Initialize the binding (was in TDZ during default evaluation)
                    if (gen_context.get_lexical_environment())
                        gen_context.get_lexical_environment()->initialize_binding(pname, arg_val);
                    else
                        gen_context.create_binding(pname, arg_val, true);
                }
            }
        }
        gen_context.set_in_param_eval(false);
    } else {
        const auto& params = get_parameters();
        for (size_t i = 0; i < params.size(); ++i) {
            Value arg = i < args.size() ? args[i] : Value();
            gen_context.create_binding(params[i], arg);
        }
    }

    // FDI step 27: param-default closures must not see the body's `var`s, so the body gets its own variable environment.
    {
        bool has_complex_params = false;
        for (const auto& p : param_objs) {
            if (p->has_default() || p->has_destructuring()) { has_complex_params = true; break; }
        }
        if (has_complex_params) {
            gen_context.push_block_scope();
            gen_context.set_variable_environment(gen_context.get_lexical_environment());
        }
    }

    // FunctionDeclarationInstantiation: hoist `var` declarations to the top of
    // the function body before it executes (see AsyncFunction::call for rationale).
    if (body_ && body_->get_type() == ASTNode::Type::BLOCK_STATEMENT) {
        scan_for_var_declarations(body_.get(), gen_context);
    }

    std::unique_ptr<ASTNode> body_clone;
    if (body_) {
        body_clone = body_->clone();
    }

    return std::make_unique<Generator>(this, gen_context_ptr.release(), std::move(body_clone), &ctx);
}


}

namespace Quanta {

thread_local std::vector<FiberStackPool::Bucket> FiberStackPool::buckets_;

char* FiberStackPool::acquire(size_t size) {
    for (auto& b : buckets_) {
        if (b.size == size && !b.free.empty()) {
            char* p = b.free.back();
            b.free.pop_back();
            return p;
        }
    }
    return new char[size];
}

void FiberStackPool::release(char* p, size_t size) {
    for (auto& b : buckets_) {
        if (b.size == size) {
            if (b.free.size() >= kMaxPerBucket) { delete[] p; return; }
            b.free.push_back(p);
            return;
        }
    }
    buckets_.push_back({size, {p}});
}

}
