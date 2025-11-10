/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/engine_execution.h"
#include "../include/engine_core.h"
#include "../../include/Context.h"
#include "../../parser/include/Parser.h"
#include "../../parser/include/AST.h"
#include "../../lexer/include/Lexer.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <chrono>

namespace Quanta {

//=============================================================================
// EngineExecution Implementation
//=============================================================================

EngineExecution::EngineExecution(EngineCore* core)
    : engine_core_(core) {

    if (core && core->get_config().enable_optimizations) {
        bytecode_executor_ = std::make_unique<FastBytecodeExecutor>();
        bytecode_executor_->enable_optimization(true);
    }
}

EngineExecution::~EngineExecution() = default;

ExecutionResult EngineExecution::execute(const std::string& source) {
    return execute(source, "<anonymous>");
}

ExecutionResult EngineExecution::execute(const std::string& source, const std::string& filename) {
    if (!engine_core_ || !engine_core_->is_initialized()) {
        return ExecutionResult("Engine not initialized");
    }

    return execute_internal(source, filename);
}

ExecutionResult EngineExecution::execute_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return ExecutionResult("Cannot open file: " + filename);
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return execute(buffer.str(), filename);
}

ExecutionResult EngineExecution::evaluate(const std::string& expression) {
    if (!engine_core_ || !engine_core_->is_initialized()) {
        return ExecutionResult("Engine not initialized");
    }

    try {
        auto context = engine_core_->get_global_context();
        if (!context) {
            return ExecutionResult("No global context available");
        }

        return evaluate_in_context(expression, context);

    } catch (const std::exception& e) {
        return ExecutionResult("Error evaluating expression: " + std::string(e.what()));
    }
}

ExecutionResult EngineExecution::evaluate_in_context(const std::string& expression, Context* context) {
    try {
        // Create lexer and parser for expression evaluation
        Lexer lexer(expression);
        Parser parser(lexer.tokenize());

        // Parse the expression into AST
        auto expr_ast = parser.parse_expression();
        if (!expr_ast) {
            return ExecutionResult("Failed to parse expression");
        }

        // Evaluate the AST node
        Value result = evaluate_ast_node(expr_ast.get(), context);
        return ExecutionResult(result);

    } catch (const std::exception& e) {
        return ExecutionResult("Expression evaluation error: " + std::string(e.what()));
    }
}

ExecutionResult EngineExecution::execute_module(const std::string& module_path) {
    try {
        std::string resolved_path = resolve_module_path(module_path, "");
        return execute_file(resolved_path);

    } catch (const std::exception& e) {
        return ExecutionResult("Module execution failed: " + std::string(e.what()));
    }
}

ExecutionResult EngineExecution::import_module(const std::string& module_specifier) {
    try {
        if (is_core_module(module_specifier)) {
            // Handle core modules
            return ExecutionResult("Core module import not implemented");
        }

        return execute_module(module_specifier);

    } catch (const std::exception& e) {
        return ExecutionResult("Module import failed: " + std::string(e.what()));
    }
}

ExecutionResult EngineExecution::execute_ast(std::shared_ptr<AST> ast, const ExecutionContext& context) {
    try {
        auto exec_context = engine_core_->get_global_context();
        if (!exec_context) {
            return ExecutionResult("No execution context available");
        }

        // Execute AST nodes
        Value result;
        for (auto& node : ast->get_statements()) {
            result = evaluate_ast_node(node.get(), exec_context);
        }

        return ExecutionResult(result);

    } catch (const std::exception& e) {
        return handle_runtime_error(e, context);
    }
}

ExecutionResult EngineExecution::execute_ast_node(ASTNode* node, Context* context) {
    if (!node || !context) {
        return ExecutionResult("Invalid AST node or context");
    }

    try {
        Value result = evaluate_ast_node(node, context);
        return ExecutionResult(result);

    } catch (const std::exception& e) {
        return ExecutionResult("AST execution error: " + std::string(e.what()));
    }
}

ExecutionResult EngineExecution::execute_bytecode(const std::vector<uint8_t>& bytecode, Context* context) {
    if (!bytecode_executor_) {
        return ExecutionResult("Bytecode executor not available");
    }

    if (!bytecode_executor_->is_valid_bytecode(bytecode)) {
        return ExecutionResult("Invalid bytecode");
    }

    return bytecode_executor_->execute(bytecode, context);
}

ExecutionResult EngineExecution::execute_interactive(const std::string& input) {
    if (is_complete_expression(input)) {
        return evaluate(input);
    } else {
        return ExecutionResult("Incomplete expression");
    }
}

bool EngineExecution::is_complete_expression(const std::string& input) const {
    try {
        Lexer lexer(input);
        auto tokens = lexer.tokenize();

        // Simple balance check for braces, brackets, parentheses
        int brace_count = 0, bracket_count = 0, paren_count = 0;

        for (const auto& token : tokens) {
            switch (token.type) {
                case TokenType::LeftBrace: brace_count++; break;
                case TokenType::RightBrace: brace_count--; break;
                case TokenType::LeftBracket: bracket_count++; break;
                case TokenType::RightBracket: bracket_count--; break;
                case TokenType::LeftParen: paren_count++; break;
                case TokenType::RightParen: paren_count--; break;
                default: break;
            }
        }

        return brace_count == 0 && bracket_count == 0 && paren_count == 0;

    } catch (...) {
        return false;
    }
}

bool EngineExecution::should_compile_to_bytecode(const std::string& source) const {
    // Simple heuristic: compile if source is large enough or contains loops
    return source.length() > 1000 ||
           source.find("for") != std::string::npos ||
           source.find("while") != std::string::npos;
}

std::vector<uint8_t> EngineExecution::compile_to_bytecode(std::shared_ptr<AST> ast) const {
    // Placeholder bytecode compilation
    std::vector<uint8_t> bytecode;

    // This would contain actual bytecode compilation logic
    bytecode.push_back(0x01); // Example opcode
    bytecode.push_back(0x00); // Example operand

    return bytecode;
}

void EngineExecution::enable_profiling(bool enabled) {
    // Enable execution profiling
    // This would set up performance monitoring
}

void EngineExecution::print_execution_stats() const {
    if (engine_core_) {
        std::cout << "=== Execution Statistics ===" << std::endl;
        std::cout << "Total executions: " << engine_core_->get_execution_count() << std::endl;
        std::cout << "Total allocations: " << engine_core_->get_total_allocations() << std::endl;
        std::cout << "Total GC runs: " << engine_core_->get_total_gc_runs() << std::endl;

        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::high_resolution_clock::now() - engine_core_->get_start_time()
        );
        std::cout << "Engine uptime: " << uptime.count() << " seconds" << std::endl;
    }
}

void EngineExecution::setup_error_handlers() {
    // Set up global error handlers
}

ExecutionResult EngineExecution::handle_runtime_error(const std::exception& e, const ExecutionContext& context) {
    std::string error_msg = format_error_message(e.what(), context);
    return ExecutionResult(error_msg);
}

// Private implementation methods
ExecutionResult EngineExecution::execute_internal(const std::string& source, const std::string& filename) {
    ExecutionContext context(filename, source);

    try {
        return parse_and_execute(source, context);

    } catch (const std::exception& e) {
        return handle_runtime_error(e, context);
    }
}

ExecutionResult EngineExecution::parse_and_execute(const std::string& source, const ExecutionContext& context) {
    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // Parse source code
        auto ast = parse_source(source, context.filename);
        if (!ast) {
            return ExecutionResult("Failed to parse source code");
        }

        // Execute AST
        auto result = execute_ast(ast, context);

        // Update statistics
        auto end_time = std::chrono::high_resolution_clock::now();
        auto execution_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        update_execution_stats(context, execution_time);

        return result;

    } catch (const std::exception& e) {
        return ExecutionResult("Parse and execute error: " + std::string(e.what()));
    }
}

std::shared_ptr<AST> EngineExecution::parse_source(const std::string& source, const std::string& filename) {
    try {
        Lexer lexer(source);
        auto tokens = lexer.tokenize();

        Parser parser(tokens);
        return parser.parse();

    } catch (const std::exception& e) {
        std::cerr << "Parse error in " << filename << ": " << e.what() << std::endl;
        return nullptr;
    }
}

Value EngineExecution::evaluate_ast_node(ASTNode* node, Context* context) {
    if (!node) {
        return Value();
    }

    // This would contain the actual AST evaluation logic
    // For now, return a placeholder value
    return Value(42.0);
}

Value EngineExecution::call_function(Value function, const std::vector<Value>& args, Context* context) {
    // Function call implementation
    return Value();
}

Value EngineExecution::construct_object(Value constructor, const std::vector<Value>& args, Context* context) {
    // Object construction implementation
    return Value();
}

std::string EngineExecution::resolve_module_path(const std::string& specifier, const std::string& current_file) {
    // Module path resolution logic
    return specifier;
}

bool EngineExecution::is_core_module(const std::string& specifier) const {
    // Check if it's a core Node.js module
    return specifier == "fs" || specifier == "path" || specifier == "http";
}

bool EngineExecution::should_use_jit(const std::string& source) const {
    return engine_core_ && engine_core_->get_config().enable_jit &&
           should_compile_to_bytecode(source);
}

void EngineExecution::update_execution_stats(const ExecutionContext& context,
                                            std::chrono::microseconds execution_time) {
    // Update performance statistics
}

std::string EngineExecution::format_error_message(const std::string& message,
                                                 const ExecutionContext& context) const {
    return context.filename + ":" + std::to_string(context.line_number) + " - " + message;
}

void EngineExecution::print_stack_trace(Context* context) const {
    // Print execution stack trace
    std::cout << "Stack trace not implemented" << std::endl;
}

//=============================================================================
// FastBytecodeExecutor Implementation
//=============================================================================

FastBytecodeExecutor::FastBytecodeExecutor()
    : optimization_enabled_(true), max_stack_size_(1024 * 1024) {
}

FastBytecodeExecutor::~FastBytecodeExecutor() = default;

ExecutionResult FastBytecodeExecutor::execute(const std::vector<uint8_t>& bytecode, Context* context) {
    if (!context) {
        return ExecutionResult("No execution context");
    }

    try {
        setup_execution_stack(context);

        // Execute bytecode instructions
        for (size_t i = 0; i < bytecode.size(); ++i) {
            uint8_t opcode = bytecode[i];
            Value result = execute_instruction(opcode, context);

            // Handle execution result
            if (result.is_error()) {
                cleanup_execution_stack();
                return ExecutionResult("Bytecode execution error");
            }
        }

        cleanup_execution_stack();
        return ExecutionResult(Value(42.0)); // Placeholder result

    } catch (const std::exception& e) {
        cleanup_execution_stack();
        return ExecutionResult("Bytecode executor error: " + std::string(e.what()));
    }
}

bool FastBytecodeExecutor::is_valid_bytecode(const std::vector<uint8_t>& bytecode) const {
    // Basic bytecode validation
    return !bytecode.empty() && bytecode.size() < max_stack_size_;
}

Value FastBytecodeExecutor::execute_instruction(uint8_t opcode, Context* context) {
    // Bytecode instruction execution
    switch (opcode) {
        case 0x01: // Example: load constant
            return Value(42.0);
        case 0x02: // Example: add
            return Value(1.0);
        default:
            return Value();
    }
}

void FastBytecodeExecutor::setup_execution_stack(Context* context) {
    // Initialize execution stack
}

void FastBytecodeExecutor::cleanup_execution_stack() {
    // Clean up execution stack
}

} // namespace Quanta