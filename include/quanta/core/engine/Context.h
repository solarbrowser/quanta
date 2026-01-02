/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_CONTEXT_H
#define QUANTA_CONTEXT_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/gc/GC.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>

namespace Quanta {

class Engine;
class Function;
class StackFrame;
class Environment;
class Error;
class WebAPIInterface;
class GarbageCollector;

/**
 * JavaScript execution context
 * Manages scope, variable bindings, and execution state
 */
class Context {
public:
    enum class Type {
        Global,
        Function,
        Eval,
        Module
    };

    enum class State {
        Running,
        Suspended,
        Completed,
        Thrown
    };

private:
    Type type_;
    State state_;
    uint32_t context_id_;
    
    Environment* lexical_environment_;
    Environment* variable_environment_;
    Object* this_binding_;
    
    std::vector<std::unique_ptr<StackFrame>> call_stack_;
    
    mutable int execution_depth_;
    static const int max_execution_depth_ = 500;
    
    Object* global_object_;
    std::unordered_map<std::string, Object*> built_in_objects_;
    std::unordered_map<std::string, Function*> built_in_functions_;
    
    Value current_exception_;
    bool has_exception_;
    std::vector<std::pair<size_t, size_t>> try_catch_blocks_;
    
    Value return_value_;
    bool has_return_value_;
    
    bool has_break_;
    bool has_continue_;
    
    bool strict_mode_;
    
    Engine* engine_;
    
    std::string current_filename_;
    
    WebAPIInterface* web_api_interface_;

    // Garbage collector for memory management
    GarbageCollector* gc_;  // Points to engine's GC (not owned)

    static uint32_t next_context_id_;

public:
    explicit Context(Engine* engine, Type type = Type::Global);
    explicit Context(Engine* engine, Context* parent, Type type);
    ~Context();

    Type get_type() const { return type_; }
    State get_state() const { return state_; }
    uint32_t get_id() const { return context_id_; }
    Engine* get_engine() const { return engine_; }
    
    const std::string& get_current_filename() const { return current_filename_; }
    void set_current_filename(const std::string& filename) { current_filename_ = filename; }
    
    bool is_strict_mode() const { return strict_mode_; }
    void set_strict_mode(bool strict) { strict_mode_ = strict; }

    Object* get_global_object() const { return global_object_; }
    void set_global_object(Object* global);

    Object* get_this_binding() const { return this_binding_; }
    void set_this_binding(Object* this_obj) { this_binding_ = this_obj; }

    Environment* get_lexical_environment() const { return lexical_environment_; }
    Environment* get_variable_environment() const { return variable_environment_; }
    void set_lexical_environment(Environment* env) { lexical_environment_ = env; }
    void set_variable_environment(Environment* env) { variable_environment_ = env; }
    
    void push_block_scope();
    void pop_block_scope();

    bool has_binding(const std::string& name) const;
    Value get_binding(const std::string& name) const;
    bool set_binding(const std::string& name, const Value& value);
    bool create_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    bool create_var_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    bool create_lexical_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    bool delete_binding(const std::string& name);

    void push_frame(std::unique_ptr<StackFrame> frame);
    std::unique_ptr<StackFrame> pop_frame();
    StackFrame* current_frame() const;
    size_t stack_depth() const { return call_stack_.size(); }
    bool is_stack_overflow() const { return stack_depth() > 10000; }
    
    bool check_execution_depth() const;
    void increment_execution_depth() const { execution_depth_++; }
    void decrement_execution_depth() const { execution_depth_--; }

    bool has_exception() const { return has_exception_; }
    const Value& get_exception() const { return current_exception_; }
    void throw_exception(const Value& exception);
    void clear_exception();
    
    void throw_error(const std::string& message);
    void throw_type_error(const std::string& message);
    void throw_reference_error(const std::string& message);
    void throw_syntax_error(const std::string& message);
    void throw_range_error(const std::string& message);
    
    bool has_return_value() const { return has_return_value_; }
    const Value& get_return_value() const { return return_value_; }
    void set_return_value(const Value& value);
    void clear_return_value();
    
    bool has_break() const { return has_break_; }
    bool has_continue() const { return has_continue_; }
    void set_break();
    void set_continue();
    void clear_break_continue();

    void register_built_in_object(const std::string& name, Object* object);
    void register_built_in_function(const std::string& name, Function* function);
    Object* get_built_in_object(const std::string& name) const;
    Function* get_built_in_function(const std::string& name) const;

    void suspend() { state_ = State::Suspended; }
    void resume() { state_ = State::Running; }
    void complete() { state_ = State::Completed; }

    std::string get_stack_trace() const;
    std::vector<std::string> get_variable_names() const;
    std::string debug_string() const;

    void mark_references() const;
    
    void set_web_api_interface(WebAPIInterface* interface) { web_api_interface_ = interface; }
    WebAPIInterface* get_web_api_interface() const { return web_api_interface_; }
    bool has_web_api(const std::string& name) const;
    Value call_web_api(const std::string& name, const std::vector<Value>& args);

    // Garbage collector access
    GarbageCollector* get_gc() const { return gc_; }
    void register_object(Object* obj, size_t size = 0);
    void trigger_gc();

    // Bootstrap loading
    void load_bootstrap();

    // Helper to release and auto-register objects with GC
    template<typename T>
    T* track(std::unique_ptr<T> obj) {
        T* raw_ptr = obj.release();
        if (raw_ptr) {
            register_object(static_cast<Object*>(raw_ptr), sizeof(T));
        }
        return raw_ptr;
    }

private:
    void initialize_global_context();
    void initialize_built_ins();
    void setup_test262_helpers();
    void setup_global_bindings();
    void register_typed_array_constructors();
};

/**
 * Stack frame for function calls
 */
class StackFrame {
public:
    enum class Type {
        Script,
        Function,
        Constructor,
        Method,
        Eval,
        Native
    };

private:
    Type type_;
    Function* function_;
    Object* this_binding_;
    std::vector<Value> arguments_;
    std::unordered_map<std::string, Value> local_variables_;
    Environment* environment_;
    
    size_t program_counter_;
    std::string source_location_;
    uint32_t line_number_;
    uint32_t column_number_;

public:
    StackFrame(Type type, Function* function, Object* this_binding);
    ~StackFrame() = default;

    Type get_type() const { return type_; }
    Function* get_function() const { return function_; }
    Object* get_this_binding() const { return this_binding_; }
    Environment* get_environment() const { return environment_; }

    void set_arguments(const std::vector<Value>& args) { arguments_ = args; }
    const std::vector<Value>& get_arguments() const { return arguments_; }
    size_t argument_count() const { return arguments_.size(); }
    Value get_argument(size_t index) const;

    bool has_local(const std::string& name) const;
    Value get_local(const std::string& name) const;
    void set_local(const std::string& name, const Value& value);

    size_t get_program_counter() const { return program_counter_; }
    void set_program_counter(size_t pc) { program_counter_ = pc; }
    
    void set_source_location(const std::string& location, uint32_t line, uint32_t column);
    std::string get_source_location() const { return source_location_; }
    uint32_t get_line_number() const { return line_number_; }
    uint32_t get_column_number() const { return column_number_; }

    std::string to_string() const;
};

/**
 * Environment for variable bindings
 */
class Environment {
public:
    enum class Type {
        Declarative,
        Object,
        Function,
        Module,
        Global
    };

private:
    Type type_;
    Environment* outer_environment_;
    std::unordered_map<std::string, Value> bindings_;
    std::unordered_map<std::string, bool> mutable_flags_;
    std::unordered_map<std::string, bool> initialized_flags_;
    Object* binding_object_;

public:
    Environment(Type type, Environment* outer = nullptr);
    Environment(Object* binding_object, Environment* outer = nullptr);
    ~Environment() = default;

    Type get_type() const { return type_; }
    Environment* get_outer() const { return outer_environment_; }
    Object* get_binding_object() const { return binding_object_; }

    bool has_binding(const std::string& name) const;
    Value get_binding(const std::string& name) const;
    Value get_binding_with_depth(const std::string& name, int depth) const;
    bool set_binding(const std::string& name, const Value& value);
    bool create_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    bool delete_binding(const std::string& name);

    bool is_mutable_binding(const std::string& name) const;
    bool is_initialized_binding(const std::string& name) const;
    void initialize_binding(const std::string& name, const Value& value);

    std::vector<std::string> get_binding_names() const;
    std::string debug_string() const;

    void mark_references() const;

private:
    bool has_own_binding(const std::string& name) const;
};

/**
 * Context factory for creating specialized contexts
 */
namespace ContextFactory {
    std::unique_ptr<Context> create_global_context(Engine* engine);
    std::unique_ptr<Context> create_function_context(Engine* engine, Context* parent, Function* function);
    std::unique_ptr<Context> create_eval_context(Engine* engine, Context* parent);
    std::unique_ptr<Context> create_module_context(Engine* engine);
}

}

#endif
