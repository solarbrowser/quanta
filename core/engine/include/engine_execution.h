/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include "../../include/Context.h"
#include "engine_core.h"
#include <string>
#include <memory>
#include <vector>

namespace Quanta {

class AST;
class ASTNode;
class FastBytecodeExecutor;

/**
 * Engine Execution Result
 */
struct ExecutionResult {
    Value value;
    std::string error_message;
    bool success;

    ExecutionResult() : success(true) {}
    explicit ExecutionResult(const Value& val) : value(val), success(true) {}
    explicit ExecutionResult(const std::string& error) : error_message(error), success(false) {}

    operator bool() const { return success; }
};

/**
 * Execution Context Information
 */
struct ExecutionContext {
    std::string filename;
    std::string source_code;
    bool is_module = false;
    bool is_strict_mode = false;
    uint32_t line_number = 1;
    uint32_t column_number = 1;

    ExecutionContext() = default;
    ExecutionContext(const std::string& file, const std::string& source)
        : filename(file), source_code(source) {}
};

/**
 * Engine Execution and Evaluation System
 */
class EngineExecution {
private:
    EngineCore* engine_core_;
    std::unique_ptr<FastBytecodeExecutor> bytecode_executor_;

public:
    explicit EngineExecution(EngineCore* core);
    ~EngineExecution();

    // Script execution
    ExecutionResult execute(const std::string& source);
    ExecutionResult execute(const std::string& source, const std::string& filename);
    ExecutionResult execute_file(const std::string& filename);

    // Expression evaluation
    ExecutionResult evaluate(const std::string& expression);
    ExecutionResult evaluate_in_context(const std::string& expression, Context* context);

    // Module execution
    ExecutionResult execute_module(const std::string& module_path);
    ExecutionResult import_module(const std::string& module_specifier);

    // AST execution
    ExecutionResult execute_ast(std::shared_ptr<AST> ast, const ExecutionContext& context);
    ExecutionResult execute_ast_node(ASTNode* node, Context* context);

    // Bytecode execution
    ExecutionResult execute_bytecode(const std::vector<uint8_t>& bytecode, Context* context);

    // Interactive execution (REPL support)
    ExecutionResult execute_interactive(const std::string& input);
    bool is_complete_expression(const std::string& input) const;

    // Optimization and compilation
    bool should_compile_to_bytecode(const std::string& source) const;
    std::vector<uint8_t> compile_to_bytecode(std::shared_ptr<AST> ast) const;

    // Performance monitoring
    void enable_profiling(bool enabled);
    void print_execution_stats() const;

    // Error handling
    void setup_error_handlers();
    ExecutionResult handle_runtime_error(const std::exception& e, const ExecutionContext& context);

private:
    // Internal execution methods
    ExecutionResult execute_internal(const std::string& source, const std::string& filename);
    ExecutionResult parse_and_execute(const std::string& source, const ExecutionContext& context);

    // AST processing
    std::shared_ptr<AST> parse_source(const std::string& source, const std::string& filename);
    Value evaluate_ast_node(ASTNode* node, Context* context);

    // Built-in execution helpers
    Value call_function(Value function, const std::vector<Value>& args, Context* context);
    Value construct_object(Value constructor, const std::vector<Value>& args, Context* context);

    // Module loading helpers
    std::string resolve_module_path(const std::string& specifier, const std::string& current_file);
    bool is_core_module(const std::string& specifier) const;

    // Performance optimization
    bool should_use_jit(const std::string& source) const;
    void update_execution_stats(const ExecutionContext& context,
                               std::chrono::microseconds execution_time);

    // Error reporting
    std::string format_error_message(const std::string& message,
                                   const ExecutionContext& context) const;
    void print_stack_trace(Context* context) const;
};

/**
 * Fast Bytecode Executor
 * Optimized execution environment for compiled bytecode
 */
class FastBytecodeExecutor {
public:
    FastBytecodeExecutor();
    ~FastBytecodeExecutor();

    ExecutionResult execute(const std::vector<uint8_t>& bytecode, Context* context);
    bool is_valid_bytecode(const std::vector<uint8_t>& bytecode) const;

    // Performance optimization
    void enable_optimization(bool enabled) { optimization_enabled_ = enabled; }
    void set_max_stack_size(size_t size) { max_stack_size_ = size; }

private:
    bool optimization_enabled_;
    size_t max_stack_size_;

    // Execution helpers
    Value execute_instruction(uint8_t opcode, Context* context);
    void setup_execution_stack(Context* context);
    void cleanup_execution_stack();
};

} // namespace Quanta