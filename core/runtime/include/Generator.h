/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include "Object.h"
#include <memory>
#include <vector>
#include <functional>

namespace Quanta {

class Context;
class ASTNode;
class Function;

// Exception used for generator yield control flow
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
    
    // Generator execution state
    size_t pc_;  // Program counter for yield points
    std::vector<Value> yield_stack_;
    
    // Yield tracking for proper generator resumption
    size_t current_yield_count_;
    
    // Static generator tracking for yield expressions
    static thread_local Generator* current_generator_;
    static thread_local size_t current_yield_counter_;
    
public:
    Generator(Function* gen_func, Context* ctx, std::unique_ptr<ASTNode> body);
    virtual ~Generator() = default;
    
    // Generator protocol methods
    GeneratorResult next(const Value& value = Value());
    GeneratorResult return_value(const Value& value);
    GeneratorResult throw_exception(const Value& exception);
    
    // Generator state
    State get_state() const { return state_; }
    bool is_done() const { return state_ == State::Completed; }
    
    // Yield tracking access for YieldExpression
    size_t target_yield_index_;
    Value last_value_;
    
    // Iterator protocol
    Value get_iterator();
    
    // Generator built-in methods
    static Value generator_next(Context& ctx, const std::vector<Value>& args);
    static Value generator_return(Context& ctx, const std::vector<Value>& args);
    static Value generator_throw(Context& ctx, const std::vector<Value>& args);
    
    // Generator function constructor
    static Value generator_function_constructor(Context& ctx, const std::vector<Value>& args);
    
    // Generator prototype setup
    static void setup_generator_prototype(Context& ctx);
    
    // Static tracking methods for yield expressions
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
    
    // Override call to return generator
    Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value());
    
    // Create generator instance
    std::unique_ptr<Generator> create_generator(Context& ctx, const std::vector<Value>& args);
};

// Forward declaration - YieldExpression is defined in AST.h
class YieldExpression;

} // namespace Quanta