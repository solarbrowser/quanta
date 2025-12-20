/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Value.h"
#include "quanta/Object.h"
#include <memory>
#include <vector>
#include <functional>

namespace Quanta {

class Context;
class ASTNode;
class Function;

class YieldException : public std::exception {
public:
    Value yielded_value;
    
    YieldException(const Value& value) : yielded_value(value) {}
    
    const char* what() const noexcept override {
        return "Generator yield";
    }
};

/**
 * JavaScript Generator implementation
 * Supports ES6 generator functions and yield expressions
 */
class Generator : public Object {
public:
    enum class State {
        SuspendedStart,
        SuspendedYield,
        Completed
    };
    
    struct GeneratorResult {
        Value value;
        bool done;
        
        GeneratorResult(const Value& v, bool d) : value(v), done(d) {}
    };

private:
    Function* generator_function_;
    Context* generator_context_;
    std::unique_ptr<ASTNode> body_;
    State state_;
    
    size_t pc_;
    std::vector<Value> yield_stack_;
    
    size_t current_yield_count_;
    
    static thread_local Generator* current_generator_;
    static thread_local size_t current_yield_counter_;
    
public:
    Generator(Function* gen_func, Context* ctx, std::unique_ptr<ASTNode> body);
    virtual ~Generator() = default;
    
    GeneratorResult next(const Value& value = Value());
    GeneratorResult return_value(const Value& value);
    GeneratorResult throw_exception(const Value& exception);
    
    State get_state() const { return state_; }
    bool is_done() const { return state_ == State::Completed; }
    
    size_t target_yield_index_;
    Value last_value_;
    
    Value get_iterator();
    
    static Value generator_next(Context& ctx, const std::vector<Value>& args);
    static Value generator_return(Context& ctx, const std::vector<Value>& args);
    static Value generator_throw(Context& ctx, const std::vector<Value>& args);
    
    static Value generator_function_constructor(Context& ctx, const std::vector<Value>& args);
    
    static void setup_generator_prototype(Context& ctx);
    
    static void set_current_generator(Generator* gen);
    static Generator* get_current_generator();
    static size_t increment_yield_counter();
    static void reset_yield_counter();
    
private:
    GeneratorResult execute_until_yield(const Value& sent_value);
    void complete_generator(const Value& value);
};

/**
 * Generator Function implementation
 * Represents function* declarations
 */
class GeneratorFunction : public Function {
private:
    std::unique_ptr<ASTNode> body_;
    
public:
    GeneratorFunction(const std::string& name, 
                     const std::vector<std::string>& params,
                     std::unique_ptr<ASTNode> body,
                     Context* closure_context);
    
    Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value());
    
    std::unique_ptr<Generator> create_generator(Context& ctx, const std::vector<Value>& args);
};

class YieldExpression;

}
