/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Generator.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/parser/AST.h"
#include "quanta/parser/Parser.h"
#include <iostream>

namespace Quanta {


thread_local Generator* Generator::current_generator_ = nullptr;
thread_local size_t Generator::current_yield_counter_ = 0;
Object* Generator::s_generator_prototype_ = nullptr;

Generator::Generator(Function* gen_func, Context* ctx, std::unique_ptr<ASTNode> body)
    : Object(ObjectType::Custom), generator_function_(gen_func), generator_context_(ctx),
      body_(std::move(body)), state_(State::SuspendedStart), pc_(0),
      current_yield_count_(0), target_yield_index_(0) {

    // Set prototype from generatorFn.prototype (which inherits from %GeneratorPrototype%)
    if (gen_func) {
        Value fn_proto = gen_func->get_property("prototype");
        if (fn_proto.is_object()) {
            set_prototype(fn_proto.as_object());
        } else if (s_generator_prototype_) {
            set_prototype(s_generator_prototype_);
        }
    }
}

Generator::GeneratorResult Generator::next(const Value& value) {
    if (state_ == State::Completed) {
        return GeneratorResult(Value(), true);
    }

    if (state_ == State::SuspendedStart) {
        state_ = State::SuspendedYield;
        return execute_until_yield(Value());
    }

    return execute_until_yield(value);
}

Generator::GeneratorResult Generator::return_value(const Value& value) {
    if (state_ == State::Completed) {
        return GeneratorResult(value, true);
    }

    complete_generator(value);
    return GeneratorResult(value, true);
}

Generator::GeneratorResult Generator::throw_exception(const Value& exception) {
    if (state_ == State::Completed) {
        generator_context_->throw_exception(exception, true);
        return GeneratorResult(Value(), true);
    }

    return execute_until_yield_throw(exception);
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

        // Store this sent_value at the current target index (before incrementing).
        // When replaying, yield at index T returns sent_values_[T].
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
        complete_generator(result);
        return GeneratorResult(result, true);

    } catch (const YieldException& yield_ex) {
        set_current_generator(nullptr);
        state_ = State::SuspendedYield;
        return GeneratorResult(yield_ex.yielded_value, false);

    } catch (const std::exception& e) {
        set_current_generator(nullptr);
        complete_generator(Value());
        generator_context_->throw_exception(Value(std::string(e.what())));
        return GeneratorResult(Value(), true);
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
        ctx.throw_exception(Value(std::string("Generator.prototype.next called without proper this binding")));
        return Value();
    }

    if (this_obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value(std::string("Generator.prototype.next called on non-generator")));
        return Value();
    }

    Generator* generator = static_cast<Generator*>(this_obj);
    Value sent_value = args.empty() ? Value() : args[0];

    auto result = generator->next(sent_value);

    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));

    return Value(result_obj.release());
}

Value Generator::generator_return(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("Generator.prototype.return called on non-object")));
        return Value();
    }

    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value(std::string("Generator.prototype.return called on non-generator")));
        return Value();
    }

    Generator* generator = static_cast<Generator*>(obj);
    Value return_val = args.empty() ? Value() : args[0];

    auto result = generator->return_value(return_val);

    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));

    return Value(result_obj.release());
}

Value Generator::generator_throw(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("Generator.prototype.throw called on non-object")));
        return Value();
    }

    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value(std::string("Generator.prototype.throw called on non-generator")));
        return Value();
    }

    Generator* generator = static_cast<Generator*>(obj);
    Value exception = args.empty() ? Value() : args[0];

    auto result = generator->throw_exception(exception);

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

    auto next_fn = ObjectFactory::create_native_function("next", generator_next);
    gen_prototype->set_property("next", Value(next_fn.release()));

    auto return_fn = ObjectFactory::create_native_function("return", generator_return);
    gen_prototype->set_property("return", Value(return_fn.release()));

    auto throw_fn = ObjectFactory::create_native_function("throw", generator_throw);
    gen_prototype->set_property("throw", Value(throw_fn.release()));

    // Note: [Symbol.iterator] is NOT on %GeneratorPrototype% - it's on %IteratorPrototype%

    s_generator_prototype_ = gen_prototype.get();
    ctx.create_binding("@@GeneratorPrototype", Value(gen_prototype.release()));

    // GeneratorFunction constructor - parses body string to create a real generator function
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

            // Build source: function* anonymous(params) { body }
            std::string params_str;
            for (size_t i = 0; i < param_names.size(); ++i) {
                if (i > 0) params_str += ", ";
                params_str += param_names[i];
            }
            std::string func_src = "function* anonymous(" + params_str + ") {" + body_str + "}";

            try {
                Lexer lexer(func_src);
                TokenSequence tokens = lexer.tokenize();
                Parser parser(tokens);
                auto expr = parser.parse_expression();
                if (!parser.has_errors() && expr) {
                    if (expr->get_type() == ASTNode::Type::FUNCTION_EXPRESSION) {
                        FunctionExpression* fe = static_cast<FunctionExpression*>(expr.get());
                        if (fe->is_generator()) {
                            auto body_clone = fe->get_body() ? fe->get_body()->clone() : nullptr;
                            auto gen_fn = std::make_unique<GeneratorFunction>("anonymous", param_names, std::move(body_clone), &ctx);
                            return Value(gen_fn.release());
                        }
                    } else if (expr->get_type() == ASTNode::Type::FUNCTION_DECLARATION) {
                        FunctionDeclaration* fd = static_cast<FunctionDeclaration*>(expr.get());
                        if (fd->is_generator()) {
                            auto body_clone = fd->get_body() ? fd->get_body()->clone() : nullptr;
                            auto gen_fn = std::make_unique<GeneratorFunction>("anonymous", param_names, std::move(body_clone), &ctx);
                            return Value(gen_fn.release());
                        }
                    }
                }
            } catch (...) {}

            // Fallback: create with no body
            auto gen_fn = std::make_unique<GeneratorFunction>("anonymous", param_names, nullptr, &ctx);
            return Value(gen_fn.release());
        });

    generator_function_constructor->set_property("name", Value(std::string("GeneratorFunction")));

    // %GeneratorPrototype%.constructor = GeneratorFunction
    // This means: generator_instance.constructor === GeneratorFunction ✓
    // Also: generator_function.constructor === GeneratorFunction ✓
    // (since generator_function.__proto__ = %GeneratorPrototype% via set_prototype below)
    s_generator_prototype_->set_property("constructor", Value(generator_function_constructor.get()));

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
                                   const std::vector<std::string>& params,
                                   std::unique_ptr<ASTNode> body,
                                   Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {
    // Each generator function gets a unique 'prototype' object inheriting from %GeneratorPrototype%
    if (Generator::s_generator_prototype_) {
        auto fn_proto = ObjectFactory::create_object();
        fn_proto->set_prototype(Generator::s_generator_prototype_);
        fn_proto->set_property("constructor", Value(static_cast<Function*>(this)));
        this->set_property("prototype", Value(fn_proto.release()));

        // Set this GeneratorFunction's own __proto__ to %GeneratorPrototype%
        // so that g.constructor walks: g -> %GeneratorPrototype% -> finds constructor = GeneratorFunction
        this->set_prototype(Generator::s_generator_prototype_);
    }
}

Value GeneratorFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    auto generator = create_generator(ctx, args, this_value);
    return Value(generator.release());
}

std::unique_ptr<Generator> GeneratorFunction::create_generator(Context& ctx, const std::vector<Value>& args, Value this_value) {
    // Use proper function context with lexical environment (same as Function::call)
    auto gen_context_ptr = ContextFactory::create_function_context(ctx.get_engine(), &ctx, this);
    Context& gen_context = *gen_context_ptr;

    // Bind 'this'
    try {
        gen_context.create_binding("this", this_value, true);
    } catch (...) {
        gen_context.set_binding("this", this_value);
    }

    // Copy closure variables stored as __closure_xxx properties on this GeneratorFunction
    auto prop_keys = this->get_own_property_keys();
    for (const auto& key : prop_keys) {
        if (key.length() > 10 && key.substr(0, 10) == "__closure_") {
            std::string var_name = key.substr(10);
            Value closure_value = this->get_property(key);
            if (var_name != "arguments" && var_name != "this") {
                if (ctx.has_binding(var_name)) {
                    Value parent_val = ctx.get_binding(var_name);
                    if (!parent_val.is_undefined() && !parent_val.is_function()) {
                        closure_value = parent_val;
                    }
                }
                gen_context.create_binding(var_name, closure_value, true);
            }
        }
    }

    // Bind parameters
    const auto& params = get_parameters();
    for (size_t i = 0; i < params.size(); ++i) {
        Value arg = i < args.size() ? args[i] : Value();
        gen_context.create_binding(params[i], arg);
    }

    std::unique_ptr<ASTNode> body_clone;
    if (body_) {
        body_clone = body_->clone();
    }

    return std::make_unique<Generator>(this, gen_context_ptr.release(), std::move(body_clone));
}


}
