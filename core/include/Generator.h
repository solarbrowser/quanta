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
    Value last_value_;
    
    // Generator execution state
    size_t pc_;  // Program counter for yield points
    std::vector<Value> yield_stack_;
    
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