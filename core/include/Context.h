/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_CONTEXT_H
#define QUANTA_CONTEXT_H

#include "Value.h"
#include "Object.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>

namespace Quanta {

// Forward declarations
class Engine;
class Function;
class StackFrame;
class Environment;
class Error;
class WebAPIInterface;

/**
 * JavaScript execution context
 * Manages scope, variable bindings, and execution state
 */
class Context {
public:
    // Context types for different execution modes
    enum class Type {
        Global,         // Global execution context
        Function,       // Function execution context
        Eval,           // Eval execution context
        Module          // Module execution context
    };

    // Execution state
    enum class State {
        Running,        // Normal execution
        Suspended,      // Generator/async function suspended
        Completed,      // Execution completed
        Thrown          // Exception thrown
    };

private:
    // Context identification
    Type type_;
    State state_;
    uint32_t context_id_;
    
    // Scope management
    Environment* lexical_environment_;
    Environment* variable_environment_;
    Object* this_binding_;
    
    // Execution stack
    std::vector<std::unique_ptr<StackFrame>> call_stack_;
    
    // Recursion protection
    mutable int execution_depth_;
    static const int max_execution_depth_ = 500;
    
    // Global objects and built-ins
    Object* global_object_;
    std::unordered_map<std::string, Object*> built_in_objects_;
    std::unordered_map<std::string, Function*> built_in_functions_;
    
    // Exception handling
    Value current_exception_;
    bool has_exception_;
    std::vector<std::pair<size_t, size_t>> try_catch_blocks_;
    
    // Return value handling
    Value return_value_;
    bool has_return_value_;
    
    // Break/Continue handling
    bool has_break_;
    bool has_continue_;
    
    // Strict mode
    bool strict_mode_;
    
    // Engine reference
    Engine* engine_;
    
    // Source file tracking for stack traces
    std::string current_filename_;
    
    // Web API interface for external implementations
    WebAPIInterface* web_api_interface_;
    
    static uint32_t next_context_id_;

public:
    // Constructors
    explicit Context(Engine* engine, Type type = Type::Global);
    explicit Context(Engine* engine, Context* parent, Type type);
    ~Context();

    // Context information
    Type get_type() const { return type_; }
    State get_state() const { return state_; }
    uint32_t get_id() const { return context_id_; }
    Engine* get_engine() const { return engine_; }
    
    // Source file tracking
    const std::string& get_current_filename() const { return current_filename_; }
    void set_current_filename(const std::string& filename) { current_filename_ = filename; }
    
    // Strict mode
    bool is_strict_mode() const { return strict_mode_; }
    void set_strict_mode(bool strict) { strict_mode_ = strict; }

    // Global object access
    Object* get_global_object() const { return global_object_; }
    void set_global_object(Object* global);

    // This binding
    Object* get_this_binding() const { return this_binding_; }
    void set_this_binding(Object* this_obj) { this_binding_ = this_obj; }

    // Environment management
    Environment* get_lexical_environment() const { return lexical_environment_; }
    Environment* get_variable_environment() const { return variable_environment_; }
    void set_lexical_environment(Environment* env) { lexical_environment_ = env; }
    void set_variable_environment(Environment* env) { variable_environment_ = env; }
    
    // Block scope management
    void push_block_scope();
    void pop_block_scope();

    // Variable operations
    bool has_binding(const std::string& name) const;
    Value get_binding(const std::string& name) const;
    bool set_binding(const std::string& name, const Value& value);
    bool create_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    bool create_var_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    bool create_lexical_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    bool delete_binding(const std::string& name);

    // Call stack management
    void push_frame(std::unique_ptr<StackFrame> frame);
    std::unique_ptr<StackFrame> pop_frame();
    StackFrame* current_frame() const;
    size_t stack_depth() const { return call_stack_.size(); }
    bool is_stack_overflow() const { return stack_depth() > 10000; }
    
    // Recursion protection
    bool check_execution_depth() const;
    void increment_execution_depth() const { execution_depth_++; }
    void decrement_execution_depth() const { execution_depth_--; }

    // Exception handling
    bool has_exception() const { return has_exception_; }
    const Value& get_exception() const { return current_exception_; }
    void throw_exception(const Value& exception);
    void clear_exception();
    
    // Error throwing helpers
    void throw_error(const std::string& message);
    void throw_type_error(const std::string& message);
    void throw_reference_error(const std::string& message);
    void throw_syntax_error(const std::string& message);
    void throw_range_error(const std::string& message);
    
    // Return value handling
    bool has_return_value() const { return has_return_value_; }
    const Value& get_return_value() const { return return_value_; }
    void set_return_value(const Value& value);
    void clear_return_value();
    
    // Break/Continue handling
    bool has_break() const { return has_break_; }
    bool has_continue() const { return has_continue_; }
    void set_break();
    void set_continue();
    void clear_break_continue();

    // Built-in objects
    void register_built_in_object(const std::string& name, Object* object);
    void register_built_in_function(const std::string& name, Function* function);
    Object* get_built_in_object(const std::string& name) const;
    Function* get_built_in_function(const std::string& name) const;

    // Context state management
    void suspend() { state_ = State::Suspended; }
    void resume() { state_ = State::Running; }
    void complete() { state_ = State::Completed; }

    // Debugging and introspection
    std::string get_stack_trace() const;
    std::vector<std::string> get_variable_names() const;
    std::string debug_string() const;

    // Memory management
    void mark_references() const;
    
    // Web API interface management
    void set_web_api_interface(WebAPIInterface* interface) { web_api_interface_ = interface; }
    WebAPIInterface* get_web_api_interface() const { return web_api_interface_; }
    bool has_web_api(const std::string& name) const;
    Value call_web_api(const std::string& name, const std::vector<Value>& args);

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
    // Frame types
    enum class Type {
        Script,         // Top-level script
        Function,       // Function call
        Constructor,    // Constructor call
        Method,         // Method call
        Eval,           // Eval call
        Native          // Native function call
    };

private:
    Type type_;
    Function* function_;
    Object* this_binding_;
    std::vector<Value> arguments_;
    std::unordered_map<std::string, Value> local_variables_;
    Environment* environment_;
    
    // Execution state
    size_t program_counter_;
    std::string source_location_;
    uint32_t line_number_;
    uint32_t column_number_;

public:
    StackFrame(Type type, Function* function, Object* this_binding);
    ~StackFrame() = default;

    // Frame information
    Type get_type() const { return type_; }
    Function* get_function() const { return function_; }
    Object* get_this_binding() const { return this_binding_; }
    Environment* get_environment() const { return environment_; }

    // Arguments
    void set_arguments(const std::vector<Value>& args) { arguments_ = args; }
    const std::vector<Value>& get_arguments() const { return arguments_; }
    size_t argument_count() const { return arguments_.size(); }
    Value get_argument(size_t index) const;

    // Local variables
    bool has_local(const std::string& name) const;
    Value get_local(const std::string& name) const;
    void set_local(const std::string& name, const Value& value);

    // Execution state
    size_t get_program_counter() const { return program_counter_; }
    void set_program_counter(size_t pc) { program_counter_ = pc; }
    
    void set_source_location(const std::string& location, uint32_t line, uint32_t column);
    std::string get_source_location() const { return source_location_; }
    uint32_t get_line_number() const { return line_number_; }
    uint32_t get_column_number() const { return column_number_; }

    // Debugging
    std::string to_string() const;
};

/**
 * Environment for variable bindings
 */
class Environment {
public:
    // Environment types
    enum class Type {
        Declarative,    // Declarative environment (let, const, function parameters)
        Object,         // Object environment (with statement, global object)
        Function,       // Function environment (function scope)
        Module,         // Module environment (module scope)
        Global          // Global environment
    };

private:
    Type type_;
    Environment* outer_environment_;
    std::unordered_map<std::string, Value> bindings_;
    std::unordered_map<std::string, bool> mutable_flags_;
    std::unordered_map<std::string, bool> initialized_flags_;
    Object* binding_object_;  // For object environments

public:
    Environment(Type type, Environment* outer = nullptr);
    Environment(Object* binding_object, Environment* outer = nullptr); // Object environment
    ~Environment() = default;

    // Environment information
    Type get_type() const { return type_; }
    Environment* get_outer() const { return outer_environment_; }
    Object* get_binding_object() const { return binding_object_; }

    // Binding operations
    bool has_binding(const std::string& name) const;
    Value get_binding(const std::string& name) const;
    Value get_binding_with_depth(const std::string& name, int depth) const;
    bool set_binding(const std::string& name, const Value& value);
    bool create_binding(const std::string& name, const Value& value = Value(), bool mutable_binding = true);
    bool delete_binding(const std::string& name);

    // Binding state
    bool is_mutable_binding(const std::string& name) const;
    bool is_initialized_binding(const std::string& name) const;
    void initialize_binding(const std::string& name, const Value& value);

    // Debugging
    std::vector<std::string> get_binding_names() const;
    std::string debug_string() const;

    // Memory management
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

} // namespace Quanta

#endif // QUANTA_CONTEXT_H