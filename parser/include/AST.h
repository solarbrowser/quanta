#ifndef QUANTA_AST_H
#define QUANTA_AST_H

#include "../../lexer/include/Token.h"
#include "../../core/include/Value.h"
#include <memory>
#include <vector>
#include <string>

namespace Quanta {

// Forward declarations
class Context;

/**
 * Abstract Syntax Tree nodes for JavaScript
 * High-performance, memory-efficient AST representation
 */

// Base AST Node
class ASTNode {
public:
    enum class Type {
        // Literals
        NUMBER_LITERAL,
        STRING_LITERAL,
        BOOLEAN_LITERAL,
        NULL_LITERAL,
        UNDEFINED_LITERAL,
        
        // Identifiers
        IDENTIFIER,
        
        // Expressions
        BINARY_EXPRESSION,
        UNARY_EXPRESSION,
        ASSIGNMENT_EXPRESSION,
        CALL_EXPRESSION,
        MEMBER_EXPRESSION,
        
        // Statements
        EXPRESSION_STATEMENT,
        VARIABLE_DECLARATION,
        VARIABLE_DECLARATOR,
        BLOCK_STATEMENT,
        IF_STATEMENT,
        
        // Program
        PROGRAM
    };

protected:
    Type type_;
    Position start_;
    Position end_;

public:
    ASTNode(Type type, const Position& start, const Position& end)
        : type_(type), start_(start), end_(end) {}
    
    virtual ~ASTNode() = default;
    
    Type get_type() const { return type_; }
    const Position& get_start() const { return start_; }
    const Position& get_end() const { return end_; }
    
    // Visitor pattern for AST traversal
    virtual Value evaluate(Context& ctx) = 0;
    virtual std::string to_string() const = 0;
    virtual std::unique_ptr<ASTNode> clone() const = 0;
};

/**
 * Literal nodes
 */
class NumberLiteral : public ASTNode {
private:
    double value_;

public:
    NumberLiteral(double value, const Position& start, const Position& end)
        : ASTNode(Type::NUMBER_LITERAL, start, end), value_(value) {}
    
    double get_value() const { return value_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class StringLiteral : public ASTNode {
private:
    std::string value_;

public:
    StringLiteral(const std::string& value, const Position& start, const Position& end)
        : ASTNode(Type::STRING_LITERAL, start, end), value_(value) {}
    
    const std::string& get_value() const { return value_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class BooleanLiteral : public ASTNode {
private:
    bool value_;

public:
    BooleanLiteral(bool value, const Position& start, const Position& end)
        : ASTNode(Type::BOOLEAN_LITERAL, start, end), value_(value) {}
    
    bool get_value() const { return value_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class NullLiteral : public ASTNode {
public:
    NullLiteral(const Position& start, const Position& end)
        : ASTNode(Type::NULL_LITERAL, start, end) {}
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

class UndefinedLiteral : public ASTNode {
public:
    UndefinedLiteral(const Position& start, const Position& end)
        : ASTNode(Type::UNDEFINED_LITERAL, start, end) {}
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Identifier node
 */
class Identifier : public ASTNode {
private:
    std::string name_;

public:
    Identifier(const std::string& name, const Position& start, const Position& end)
        : ASTNode(Type::IDENTIFIER, start, end), name_(name) {}
    
    const std::string& get_name() const { return name_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Binary expression (e.g., a + b, x * y)
 */
class BinaryExpression : public ASTNode {
public:
    enum class Operator {
        // Arithmetic
        ADD,            // +
        SUBTRACT,       // -
        MULTIPLY,       // *
        DIVIDE,         // /
        MODULO,         // %
        EXPONENT,       // **
        
        // Comparison
        EQUAL,          // ==
        NOT_EQUAL,      // !=
        STRICT_EQUAL,   // ===
        STRICT_NOT_EQUAL, // !==
        LESS_THAN,      // <
        GREATER_THAN,   // >
        LESS_EQUAL,     // <=
        GREATER_EQUAL,  // >=
        
        // Logical
        LOGICAL_AND,    // &&
        LOGICAL_OR,     // ||
        
        // Bitwise
        BITWISE_AND,    // &
        BITWISE_OR,     // |
        BITWISE_XOR,    // ^
        LEFT_SHIFT,     // <<
        RIGHT_SHIFT,    // >>
        UNSIGNED_RIGHT_SHIFT, // >>>
        
        // Assignment
        ASSIGN,         // =
        PLUS_ASSIGN,    // +=
        MINUS_ASSIGN,   // -=
        MULTIPLY_ASSIGN, // *=
        DIVIDE_ASSIGN,  // /=
        MODULO_ASSIGN   // %=
    };

private:
    std::unique_ptr<ASTNode> left_;
    std::unique_ptr<ASTNode> right_;
    Operator operator_;

public:
    BinaryExpression(std::unique_ptr<ASTNode> left, Operator op, std::unique_ptr<ASTNode> right,
                    const Position& start, const Position& end)
        : ASTNode(Type::BINARY_EXPRESSION, start, end), 
          left_(std::move(left)), right_(std::move(right)), operator_(op) {}
    
    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    Operator get_operator() const { return operator_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
    
    static std::string operator_to_string(Operator op);
    static Operator token_type_to_operator(TokenType type);
    static int get_precedence(Operator op);
    static bool is_right_associative(Operator op);
};

/**
 * Unary expression (e.g., -x, !flag, ++count)
 */
class UnaryExpression : public ASTNode {
public:
    enum class Operator {
        PLUS,           // +
        MINUS,          // -
        LOGICAL_NOT,    // !
        BITWISE_NOT,    // ~
        TYPEOF,         // typeof
        VOID,           // void
        DELETE,         // delete
        PRE_INCREMENT,  // ++x
        POST_INCREMENT, // x++
        PRE_DECREMENT,  // --x
        POST_DECREMENT  // x--
    };

private:
    std::unique_ptr<ASTNode> operand_;
    Operator operator_;
    bool prefix_;

public:
    UnaryExpression(Operator op, std::unique_ptr<ASTNode> operand, bool prefix,
                   const Position& start, const Position& end)
        : ASTNode(Type::UNARY_EXPRESSION, start, end), 
          operand_(std::move(operand)), operator_(op), prefix_(prefix) {}
    
    ASTNode* get_operand() const { return operand_.get(); }
    Operator get_operator() const { return operator_; }
    bool is_prefix() const { return prefix_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
    
    static std::string operator_to_string(Operator op);
};

/**
 * Call expression (e.g., func(a, b), console.log("hello"))
 */
class CallExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> callee_;
    std::vector<std::unique_ptr<ASTNode>> arguments_;

public:
    CallExpression(std::unique_ptr<ASTNode> callee, std::vector<std::unique_ptr<ASTNode>> arguments,
                  const Position& start, const Position& end)
        : ASTNode(Type::CALL_EXPRESSION, start, end), 
          callee_(std::move(callee)), arguments_(std::move(arguments)) {}
    
    ASTNode* get_callee() const { return callee_.get(); }
    const std::vector<std::unique_ptr<ASTNode>>& get_arguments() const { return arguments_; }
    size_t argument_count() const { return arguments_.size(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Member expression (e.g., obj.prop, console.log)
 */
class MemberExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> object_;
    std::unique_ptr<ASTNode> property_;
    bool computed_; // true for obj[prop], false for obj.prop

public:
    MemberExpression(std::unique_ptr<ASTNode> object, std::unique_ptr<ASTNode> property, 
                    bool computed, const Position& start, const Position& end)
        : ASTNode(Type::MEMBER_EXPRESSION, start, end), 
          object_(std::move(object)), property_(std::move(property)), computed_(computed) {}
    
    ASTNode* get_object() const { return object_.get(); }
    ASTNode* get_property() const { return property_.get(); }
    bool is_computed() const { return computed_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Variable declarator (single variable in a declaration)
 */
class VariableDeclarator : public ASTNode {
public:
    enum class Kind {
        VAR,
        LET,
        CONST
    };

private:
    std::unique_ptr<Identifier> id_;
    std::unique_ptr<ASTNode> init_;
    Kind kind_;

public:
    VariableDeclarator(std::unique_ptr<Identifier> id, std::unique_ptr<ASTNode> init, Kind kind,
                      const Position& start, const Position& end)
        : ASTNode(Type::VARIABLE_DECLARATOR, start, end), 
          id_(std::move(id)), init_(std::move(init)), kind_(kind) {}
    
    Identifier* get_id() const { return id_.get(); }
    ASTNode* get_init() const { return init_.get(); }
    Kind get_kind() const { return kind_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
    
    static std::string kind_to_string(Kind kind);
};

/**
 * Variable declaration statement (e.g., "var x = 5;", "let y;")
 */
class VariableDeclaration : public ASTNode {
private:
    std::vector<std::unique_ptr<VariableDeclarator>> declarations_;
    VariableDeclarator::Kind kind_;

public:
    VariableDeclaration(std::vector<std::unique_ptr<VariableDeclarator>> declarations, 
                       VariableDeclarator::Kind kind, const Position& start, const Position& end)
        : ASTNode(Type::VARIABLE_DECLARATION, start, end), 
          declarations_(std::move(declarations)), kind_(kind) {}
    
    const std::vector<std::unique_ptr<VariableDeclarator>>& get_declarations() const { return declarations_; }
    VariableDeclarator::Kind get_kind() const { return kind_; }
    size_t declaration_count() const { return declarations_.size(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Block statement (e.g., "{ ... }")
 */
class BlockStatement : public ASTNode {
private:
    std::vector<std::unique_ptr<ASTNode>> statements_;

public:
    BlockStatement(std::vector<std::unique_ptr<ASTNode>> statements, const Position& start, const Position& end)
        : ASTNode(Type::BLOCK_STATEMENT, start, end), statements_(std::move(statements)) {}
    
    const std::vector<std::unique_ptr<ASTNode>>& get_statements() const { return statements_; }
    size_t statement_count() const { return statements_.size(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * If statement (e.g., "if (condition) statement", "if (condition) statement else statement")
 */
class IfStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> test_;
    std::unique_ptr<ASTNode> consequent_;
    std::unique_ptr<ASTNode> alternate_;

public:
    IfStatement(std::unique_ptr<ASTNode> test, std::unique_ptr<ASTNode> consequent, 
               std::unique_ptr<ASTNode> alternate, const Position& start, const Position& end)
        : ASTNode(Type::IF_STATEMENT, start, end), 
          test_(std::move(test)), consequent_(std::move(consequent)), alternate_(std::move(alternate)) {}
    
    ASTNode* get_test() const { return test_.get(); }
    ASTNode* get_consequent() const { return consequent_.get(); }
    ASTNode* get_alternate() const { return alternate_.get(); }
    bool has_alternate() const { return alternate_ != nullptr; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Expression statement (e.g., "42;", "console.log('hello');")
 */
class ExpressionStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> expression_;

public:
    ExpressionStatement(std::unique_ptr<ASTNode> expression, const Position& start, const Position& end)
        : ASTNode(Type::EXPRESSION_STATEMENT, start, end), expression_(std::move(expression)) {}
    
    ASTNode* get_expression() const { return expression_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Program node (root of AST)
 */
class Program : public ASTNode {
private:
    std::vector<std::unique_ptr<ASTNode>> statements_;

public:
    Program(std::vector<std::unique_ptr<ASTNode>> statements, const Position& start, const Position& end)
        : ASTNode(Type::PROGRAM, start, end), statements_(std::move(statements)) {}
    
    const std::vector<std::unique_ptr<ASTNode>>& get_statements() const { return statements_; }
    size_t statement_count() const { return statements_.size(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

} // namespace Quanta

#endif // QUANTA_AST_H