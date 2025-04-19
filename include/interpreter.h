#ifndef QUANTA_INTERPRETER_H
#define QUANTA_INTERPRETER_H

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <variant>
#include "ast.h"

namespace quanta {

// Forward declarations
class Environment;
class JSFunction;

/**
 * Error thrown during interpretation
 */
class RuntimeError : public std::runtime_error {
public:
    RuntimeError(const std::string& message, int line, int column);
    int get_line() const;
    int get_column() const;

private:
    int line_;
    int column_;
};

/**
 * JavaScript value representation
 */
using JSValue = std::variant<
    std::monostate,      // undefined
    std::nullptr_t,      // null
    bool,                // boolean
    double,              // number
    std::string,         // string
    std::shared_ptr<Environment>,  // for objects
    std::shared_ptr<JSFunction>    // for functions
>;

/**
 * Exception for handling return statements
 */
class ReturnException : public std::exception {
public:
    explicit ReturnException(const JSValue& value) : value_(value) {}
    const JSValue& getValue() const { return value_; }

private:
    JSValue value_;
};

/**
 * Interpreter class for the Quanta JavaScript Engine
 * Implements the visitor pattern to interpret the AST
 */
class Interpreter : public ExpressionVisitor, public StatementVisitor {
public:
    /**
     * Create a new interpreter
     */
    Interpreter();

    /**
     * Interpret a program
     * @param program The program AST
     * @return The result of the program
     */
    JSValue interpret(const std::shared_ptr<Program>& program);
    
    /**
     * Evaluate a source string
     * @param source The JavaScript source code to evaluate
     * @return The result of the evaluation
     */
    JSValue evaluate(const std::string& source);

    /**
     * Interpret a single statement
     * @param statement The statement AST
     */
    void interpret_statement(const std::shared_ptr<Statement>& statement);

    /**
     * Evaluate an expression
     * @param expression The expression AST
     * @return The result of the expression
     */
    JSValue evaluate(const std::shared_ptr<Expression>& expression);

    // Statement visitor methods
    void visit(Program& stmt) override;
    void visit(BlockStatement& stmt) override;
    void visit(ExpressionStatement& stmt) override;
    void visit(VariableDeclaration& stmt) override;
    void visit(FunctionDeclaration& stmt) override;
    void visit(ReturnStatement& stmt) override;
    void visit(IfStatement& stmt) override;
    void visit(WhileStatement& stmt) override;
    void visit(ForStatement& stmt) override;

    // Expression visitor methods
    void visit(Identifier& expr) override;
    void visit(Literal& expr) override;
    void visit(BinaryExpression& expr) override;
    void visit(LogicalExpression& expr) override;
    void visit(UnaryExpression& expr) override;
    void visit(AssignmentExpression& expr) override;
    void visit(CallExpression& expr) override;
    void visit(MemberExpression& expr) override;
    void visit(FunctionExpression& expr) override;
    void visit(ObjectExpression& expr) override;
    void visit(ArrayExpression& expr) override;
    void visit(ThisExpression& expr) override;

    /**
     * Get string representation of a JSValue
     */
    std::string value_to_string(const JSValue& value);

    /**
     * Check if a JSValue is truthy
     */
    static bool is_truthy(const JSValue& value);

private:
    std::shared_ptr<Environment> environment_;
    JSValue last_value_;

    // Helper methods
    void execute_block(const std::vector<std::shared_ptr<Statement>>& statements,
                      std::shared_ptr<Environment> environment);
    
    /**
     * Check if two JSValues are equal
     */
    bool is_equal(const JSValue& a, const JSValue& b);
    
    /**
     * Check if both operands are numbers
     * @throws RuntimeError if either operand is not a number
     */
    void checkNumberOperands(const std::string& op, const JSValue& left, const JSValue& right);
};

/**
 * Environment class for storing variables
 */
class Environment {
public:
    /**
     * Create a new environment
     * @param enclosing The enclosing environment (null for global)
     */
    explicit Environment(std::shared_ptr<Environment> enclosing = nullptr);

    /**
     * Define a variable in the current environment
     * @param name Variable name
     * @param value Variable value
     */
    void define(const std::string& name, const JSValue& value);

    /**
     * Get a variable's value
     * @param name Variable name
     * @return Variable value
     */
    JSValue get(const std::string& name);

    /**
     * Assign a new value to an existing variable
     * @param name Variable name
     * @param value New value
     */
    void assign(const std::string& name, const JSValue& value);

    /**
     * Check if a variable exists in this environment
     * @param name Variable name
     * @return True if the variable exists
     */
    bool contains(const std::string& name) const;

private:
    std::unordered_map<std::string, JSValue> values_;
    std::shared_ptr<Environment> enclosing_;
};

/**
 * JavaScript Function class
 */
class JSFunction {
public:
    JSFunction(std::shared_ptr<FunctionDeclaration> declaration, 
               std::shared_ptr<Environment> closure);
    
    JSFunction(std::shared_ptr<FunctionExpression> expression, 
               std::shared_ptr<Environment> closure);
    
    std::vector<std::shared_ptr<Identifier>> getParameters() const;
    std::shared_ptr<BlockStatement> getBody() const;
    std::shared_ptr<Environment> getClosure() const;
    
private:
    std::shared_ptr<FunctionDeclaration> declaration_;
    std::shared_ptr<FunctionExpression> expression_;
    std::shared_ptr<Environment> closure_;
};

} // namespace quanta

#endif // QUANTA_INTERPRETER_H 