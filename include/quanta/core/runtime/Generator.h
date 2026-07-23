/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
#include "quanta/core/runtime/FiberState.h"

namespace Quanta {

class Context;
class ASTNode;
class Function;
class BytecodeChunk;
class Visitor;

class YieldException : public std::exception {
public:
    Value yielded_value;
    YieldException(const Value& value) : yielded_value(value) {}
    const char* what() const noexcept override { return "Generator yield"; }
};

class GeneratorReturnException : public std::exception {
public:
    Value return_value;
    GeneratorReturnException(const Value& v) : return_value(v) {}
    const char* what() const noexcept override { return "Generator return"; }
};

/**
 * JavaScript Generator implementation
 * Supports ES6 generator functions and yield expressions
 */
class Generator : public CustomObjectBase {
public:
    enum class State {
        SuspendedStart,
        SuspendedYield,
        Executing,
        Completed
    };
    
    struct GeneratorResult {
        Value value;
        bool done;
        bool has_exception = false;
        bool raw_result = false; // if true, value is the entire result object
        Value exception;

        GeneratorResult(const Value& v, bool d) : value(v), done(d) {}
        static GeneratorResult make_exception(const Value& exc) {
            GeneratorResult r(Value(), true);
            r.has_exception = true;
            r.exception = exc;
            return r;
        }
        static GeneratorResult make_raw(const Value& result_obj) {
            GeneratorResult r(result_obj, false);
            r.raw_result = true;
            return r;
        }
    };

private:
    Function* generator_function_;
    Context* generator_context_;
    // Non-owning: points into generator_function_'s own body, kept alive by
    // trace() visiting generator_function_.
    ASTNode* body_;
    State state_;

    // Fiber-based (stackful coroutine) implementation
    static constexpr size_t STACK_SIZE = 512 * 1024;
public:
    std::unique_ptr<FiberState> fiber_ = std::make_unique<FiberState>();
private:
    static thread_local Generator* current_generator_;
    static thread_local size_t current_yield_counter_;

    static void fiber_entry(mco_coro* co);
    void run_body();

public:
    // Accessible from YieldExpression
    Value yielded_value_;
    Value yielded_result_; // for yield*: full inner result object (may have undefined done)
    bool yield_raw_result_ = false; // if true, return yielded_result_ as-is
    Value sent_value_;
    Value throw_value_;
    Value return_argument_;
    bool throwing_ = false;
    bool returning_ = false;
    Generator(Function* gen_func, Context* ctx, ASTNode* body, Context* outer_ctx = nullptr);
    void trace(Visitor& v);
    // Non-virtual: the GC sweep (Collector.cpp) reads get_custom_kind() and
    // destructs through the correct concrete type itself.
    ~Generator();

    GeneratorResult next(const Value& value = Value());
    GeneratorResult return_value(const Value& value);
    GeneratorResult throw_exception(const Value& exception);

    State get_state() const { return state_; }
    void set_state(State s) { state_ = s; }
    bool is_done() const { return state_ == State::Completed; }

    Context* outer_context_ = nullptr;
    // Legacy fields needed by YieldExpression (kept for compatibility, fiber supersedes replay)
    size_t target_yield_index_ = 0;
    Value last_value_;
    std::vector<Value> sent_values_;

    Context* get_context() const { return generator_context_; }

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

    // %GeneratorPrototype%. Thread-local: each agent owns its own intrinsics.
    static thread_local Object* s_generator_prototype_;
    // %GeneratorFunction.prototype%
    static thread_local Object* s_generator_function_prototype_;
    
private:
    GeneratorResult execute_until_yield(const Value& sent_value);
    GeneratorResult execute_until_yield_throw(const Value& exception);
    GeneratorResult execute_until_yield_return(const Value& value);
    void complete_generator(const Value& value);
};

/**
 * Generator Function implementation
 * Represents function* declarations
 */
class GeneratorFunction : public Function {
private:
    std::unique_ptr<ASTNode> body_;
    // Lazy body->bytecode cache, shared by every generator this function
    // produces (mirrors Function::bytecode_chunk_/vm_incompatible_).
    std::unique_ptr<BytecodeChunk> suspendable_chunk_;
    bool suspendable_incompatible_ = false;

public:
    GeneratorFunction(const std::string& name,
                     const std::vector<std::string>& params,
                     std::unique_ptr<ASTNode> body,
                     Context* closure_context);

    GeneratorFunction(const std::string& name,
                     std::vector<std::unique_ptr<class Parameter>> params,
                     std::unique_ptr<ASTNode> body,
                     Context* closure_context);

    Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value());

    std::unique_ptr<Generator> create_generator(Context& ctx, const std::vector<Value>& args, Value this_value = Value());

    // Compiles on first call and caches; permanently null if the body or the
    // captured scope chain (a `with`) is incompatible.
    const BytecodeChunk* get_suspendable_chunk(Context& ctx);
    void trace(Visitor& v);
};

class YieldExpression;

}
