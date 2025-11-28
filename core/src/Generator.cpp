/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "Generator.h"
#include "Context.h"
#include "Symbol.h"
#include "../../parser/include/AST.h"
#include <iostream>

namespace Quanta {

//=============================================================================
// Generator Implementation
//=============================================================================

// Thread-local variables for generator execution tracking
thread_local Generator* Generator::current_generator_ = nullptr;
thread_local size_t Generator::current_yield_counter_ = 0;

Generator::Generator(Function* gen_func, Context* ctx, std::unique_ptr<ASTNode> body)
    : Object(ObjectType::Custom), generator_function_(gen_func), generator_context_(ctx),
      body_(std::move(body)), state_(State::SuspendedStart), pc_(0),
      current_yield_count_(0), target_yield_index_(0) {
    
    // Add JavaScript methods to this generator instance
    auto next_method = ObjectFactory::create_native_function("next", generator_next);
    this->set_property("next", Value(next_method.release()));
    
    auto return_method = ObjectFactory::create_native_function("return", generator_return);
    this->set_property("return", Value(return_method.release()));
    
    auto throw_method = ObjectFactory::create_native_function("throw", generator_throw);
    this->set_property("throw", Value(throw_method.release()));
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
        generator_context_->throw_exception(exception);
        return GeneratorResult(Value(), true);
    }
    
    // Throw exception in generator context
    generator_context_->throw_exception(exception);
    complete_generator(Value());
    return GeneratorResult(Value(), true);
}

Value Generator::get_iterator() {
    // Generator objects are their own iterators
    return Value(this);
}

Generator::GeneratorResult Generator::execute_until_yield(const Value& sent_value) {
    if (!body_) {
        complete_generator(Value());
        return GeneratorResult(Value(), true);
    }
    
    try {
        // Store sent value for yield expressions
        last_value_ = sent_value;
        
        // Set up generator tracking for yield expressions
        set_current_generator(this);
        reset_yield_counter();
        target_yield_index_++;  // Target the next yield point
        
        // Execute the generator body
        Value result = body_->evaluate(*generator_context_);
        
        // If we reach here without yielding, the generator is done
        set_current_generator(nullptr);
        complete_generator(result);
        return GeneratorResult(result, true);
        
    } catch (const YieldException& yield_ex) {
        // Handle yield: suspend the generator and return the yielded value
        set_current_generator(nullptr);
        state_ = State::SuspendedYield;
        return GeneratorResult(yield_ex.yielded_value, false);
        
    } catch (const std::exception& e) {
        // Handle other generator exceptions
        set_current_generator(nullptr);
        complete_generator(Value());
        generator_context_->throw_exception(Value(e.what()));
        return GeneratorResult(Value(), true);
    }
}

void Generator::complete_generator(const Value& value) {
    state_ = State::Completed;
    last_value_ = value;
}

// Generator built-in methods
Value Generator::generator_next(Context& ctx, const std::vector<Value>& args) {
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value("Generator.prototype.next called without proper this binding"));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value("Generator.prototype.next called on non-generator"));
        return Value();
    }
    
    Generator* generator = static_cast<Generator*>(this_obj);
    Value sent_value = args.empty() ? Value() : args[0];
    
    auto result = generator->next(sent_value);
    
    // Create iterator result object
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));
    
    return Value(result_obj.release());
}

Value Generator::generator_return(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Generator.prototype.return called on non-object"));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value("Generator.prototype.return called on non-generator"));
        return Value();
    }
    
    Generator* generator = static_cast<Generator*>(obj);
    Value return_value = args.empty() ? Value() : args[0];
    
    auto result = generator->return_value(return_value);
    
    // Create iterator result object
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));
    
    return Value(result_obj.release());
}

Value Generator::generator_throw(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Generator.prototype.throw called on non-object"));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value("Generator.prototype.throw called on non-generator"));
        return Value();
    }
    
    Generator* generator = static_cast<Generator*>(obj);
    Value exception = args.empty() ? Value() : args[0];
    
    auto result = generator->throw_exception(exception);
    
    // Create iterator result object
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));
    
    return Value(result_obj.release());
}

void Generator::setup_generator_prototype(Context& ctx) {
    // Create Generator.prototype
    auto gen_prototype = ObjectFactory::create_object();
    
    // Add next method
    auto next_fn = ObjectFactory::create_native_function("next", generator_next);
    gen_prototype->set_property("next", Value(next_fn.release()));
    
    // Add return method
    auto return_fn = ObjectFactory::create_native_function("return", generator_return);
    gen_prototype->set_property("return", Value(return_fn.release()));
    
    // Add throw method
    auto throw_fn = ObjectFactory::create_native_function("throw", generator_throw);
    gen_prototype->set_property("throw", Value(throw_fn.release()));
    
    // Add Symbol.iterator method (generators are iterable)
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto iterator_fn = ObjectFactory::create_native_function("@@iterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; // Unused parameter
                return ctx.get_binding("this");
            });
        gen_prototype->set_property(iterator_symbol->to_string(), Value(iterator_fn.release()));
    }
    
    ctx.create_binding("GeneratorPrototype", Value(gen_prototype.release()));

    // Setup GeneratorFunction constructor
    auto generator_function_constructor = ObjectFactory::create_native_function("GeneratorFunction",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Simple GeneratorFunction constructor implementation
            std::vector<std::string> params;
            std::string body_str = "return undefined;";

            if (args.size() > 1) {
                // Last argument is the function body
                body_str = args.back().to_string();

                // Previous arguments are parameter names
                for (size_t i = 0; i < args.size() - 1; ++i) {
                    params.push_back(args[i].to_string());
                }
            } else if (args.size() == 1) {
                body_str = args[0].to_string();
            }

            // Create a simple generator function
            auto gen_fn = std::make_unique<GeneratorFunction>("anonymous", params, nullptr, &ctx);
            return Value(gen_fn.release());
        });

    // Set name property for GeneratorFunction constructor
    generator_function_constructor->set_property("name", Value("GeneratorFunction"));

    // Register GeneratorFunction constructor
    ctx.create_binding("GeneratorFunction", Value(generator_function_constructor.release()));
}

// Static tracking methods for yield expressions
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

//=============================================================================
// GeneratorFunction Implementation
//=============================================================================

GeneratorFunction::GeneratorFunction(const std::string& name, 
                                   const std::vector<std::string>& params,
                                   std::unique_ptr<ASTNode> body,
                                   Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {
}

Value GeneratorFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    (void)this_value; // Unused parameter
    
    // Create new generator instance
    auto generator = create_generator(ctx, args);
    return Value(generator.release());
}

std::unique_ptr<Generator> GeneratorFunction::create_generator(Context& ctx, const std::vector<Value>& args) {
    // Create new context for generator execution
    auto gen_context = std::make_unique<Context>(ctx.get_engine(), &ctx, Context::Type::Function);
    
    // Bind parameters
    const auto& params = get_parameters();
    for (size_t i = 0; i < params.size(); ++i) {
        Value arg = i < args.size() ? args[i] : Value();
        gen_context->create_binding(params[i], arg);
    }
    
    // Clone the body for the generator
    auto body_clone = body_->clone();
    
    return std::make_unique<Generator>(this, gen_context.release(), std::move(body_clone));
}

// YieldExpression is now implemented in AST.cpp to avoid conflicts

} // namespace Quanta