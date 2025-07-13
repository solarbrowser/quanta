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
        NEW_EXPRESSION,
        FUNCTION_EXPRESSION,
        OBJECT_LITERAL,
        ARRAY_LITERAL,
        
        // Statements
        EXPRESSION_STATEMENT,
        VARIABLE_DECLARATION,
        VARIABLE_DECLARATOR,
        BLOCK_STATEMENT,
        IF_STATEMENT,
        FOR_STATEMENT,
        WHILE_STATEMENT,
        FUNCTION_DECLARATION,
        RETURN_STATEMENT,
        TRY_STATEMENT,
        CATCH_CLAUSE,
        THROW_STATEMENT,
        SWITCH_STATEMENT,
        CASE_CLAUSE,
        
        // Stage 10: Modules
        IMPORT_STATEMENT,
        EXPORT_STATEMENT,
        IMPORT_SPECIFIER,
        EXPORT_SPECIFIER,
        
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
    
private:
    Value handle_array_method_call(Object* array, const std::string& method_name, Context& ctx);
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
 * New expression (constructor call with new operator)
 */
class NewExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> constructor_;
    std::vector<std::unique_ptr<ASTNode>> arguments_;

public:
    NewExpression(std::unique_ptr<ASTNode> constructor, 
                  std::vector<std::unique_ptr<ASTNode>> arguments,
                  const Position& start, const Position& end)
        : ASTNode(Type::NEW_EXPRESSION, start, end), 
          constructor_(std::move(constructor)), arguments_(std::move(arguments)) {}
    
    ASTNode* get_constructor() const { return constructor_.get(); }
    const std::vector<std::unique_ptr<ASTNode>>& get_arguments() const { return arguments_; }
    
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
 * For loop statement (e.g., "for (let i = 0; i < 10; i++) { ... }")
 */
class ForStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> init_;
    std::unique_ptr<ASTNode> test_;
    std::unique_ptr<ASTNode> update_;
    std::unique_ptr<ASTNode> body_;

public:
    ForStatement(std::unique_ptr<ASTNode> init, std::unique_ptr<ASTNode> test,
                 std::unique_ptr<ASTNode> update, std::unique_ptr<ASTNode> body,
                 const Position& start, const Position& end)
        : ASTNode(Type::FOR_STATEMENT, start, end), 
          init_(std::move(init)), test_(std::move(test)), 
          update_(std::move(update)), body_(std::move(body)) {}
    
    ASTNode* get_init() const { return init_.get(); }
    ASTNode* get_test() const { return test_.get(); }
    ASTNode* get_update() const { return update_.get(); }
    ASTNode* get_body() const { return body_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * While loop statement (e.g., "while (condition) { ... }")
 */
class WhileStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> test_;
    std::unique_ptr<ASTNode> body_;

public:
    WhileStatement(std::unique_ptr<ASTNode> test, std::unique_ptr<ASTNode> body,
                   const Position& start, const Position& end)
        : ASTNode(Type::WHILE_STATEMENT, start, end), 
          test_(std::move(test)), body_(std::move(body)) {}
    
    ASTNode* get_test() const { return test_.get(); }
    ASTNode* get_body() const { return body_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Function declaration (e.g., "function foo(x, y) { return x + y; }")
 */
class FunctionDeclaration : public ASTNode {
private:
    std::unique_ptr<Identifier> id_;
    std::vector<std::unique_ptr<Identifier>> params_;
    std::unique_ptr<BlockStatement> body_;

public:
    FunctionDeclaration(std::unique_ptr<Identifier> id, 
                       std::vector<std::unique_ptr<Identifier>> params,
                       std::unique_ptr<BlockStatement> body,
                       const Position& start, const Position& end)
        : ASTNode(Type::FUNCTION_DECLARATION, start, end), 
          id_(std::move(id)), params_(std::move(params)), body_(std::move(body)) {}
    
    Identifier* get_id() const { return id_.get(); }
    const std::vector<std::unique_ptr<Identifier>>& get_params() const { return params_; }
    BlockStatement* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Function expression (e.g., "function(x) { return x * 2; }" or "var f = function() { ... }")
 */
class FunctionExpression : public ASTNode {
private:
    std::unique_ptr<Identifier> id_; // optional name for named function expressions
    std::vector<std::unique_ptr<Identifier>> params_;
    std::unique_ptr<BlockStatement> body_;

public:
    FunctionExpression(std::unique_ptr<Identifier> id,
                      std::vector<std::unique_ptr<Identifier>> params,
                      std::unique_ptr<BlockStatement> body,
                      const Position& start, const Position& end)
        : ASTNode(Type::FUNCTION_EXPRESSION, start, end), 
          id_(std::move(id)), params_(std::move(params)), body_(std::move(body)) {}
    
    Identifier* get_id() const { return id_.get(); }
    const std::vector<std::unique_ptr<Identifier>>& get_params() const { return params_; }
    BlockStatement* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    bool is_named() const { return id_ != nullptr; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Object literal expression (e.g., "{key: value, method: function() {}}")
 */
class ObjectLiteral : public ASTNode {
public:
    struct Property {
        std::unique_ptr<ASTNode> key;   // Identifier or computed expression
        std::unique_ptr<ASTNode> value; // Value expression
        bool computed;                  // true for [expr]: value, false for key: value
        bool method;                    // true for method() {}, false for regular property
        
        Property(std::unique_ptr<ASTNode> k, std::unique_ptr<ASTNode> v, bool c = false, bool m = false)
            : key(std::move(k)), value(std::move(v)), computed(c), method(m) {}
    };

private:
    std::vector<std::unique_ptr<Property>> properties_;

public:
    ObjectLiteral(std::vector<std::unique_ptr<Property>> properties,
                  const Position& start, const Position& end)
        : ASTNode(Type::OBJECT_LITERAL, start, end), properties_(std::move(properties)) {}
    
    const std::vector<std::unique_ptr<Property>>& get_properties() const { return properties_; }
    size_t property_count() const { return properties_.size(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Array literal expression (e.g., "[1, 2, 3]" or "[obj, func, nested]")
 */
class ArrayLiteral : public ASTNode {
private:
    std::vector<std::unique_ptr<ASTNode>> elements_;

public:
    ArrayLiteral(std::vector<std::unique_ptr<ASTNode>> elements,
                 const Position& start, const Position& end)
        : ASTNode(Type::ARRAY_LITERAL, start, end), elements_(std::move(elements)) {}
    
    const std::vector<std::unique_ptr<ASTNode>>& get_elements() const { return elements_; }
    size_t element_count() const { return elements_.size(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Return statement (e.g., "return 42;" or "return;")
 */
class ReturnStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> argument_;

public:
    explicit ReturnStatement(std::unique_ptr<ASTNode> argument, const Position& start, const Position& end)
        : ASTNode(Type::RETURN_STATEMENT, start, end), argument_(std::move(argument)) {}
    
    ASTNode* get_argument() const { return argument_.get(); }
    bool has_argument() const { return argument_ != nullptr; }
    
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
 * Try statement (e.g., "try { ... } catch (e) { ... } finally { ... }")
 */
class TryStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> try_block_;
    std::unique_ptr<ASTNode> catch_clause_;  // CatchClause or nullptr
    std::unique_ptr<ASTNode> finally_block_; // BlockStatement or nullptr

public:
    TryStatement(std::unique_ptr<ASTNode> try_block, 
                std::unique_ptr<ASTNode> catch_clause,
                std::unique_ptr<ASTNode> finally_block,
                const Position& start, const Position& end)
        : ASTNode(Type::TRY_STATEMENT, start, end), 
          try_block_(std::move(try_block)),
          catch_clause_(std::move(catch_clause)),
          finally_block_(std::move(finally_block)) {}
    
    ASTNode* get_try_block() const { return try_block_.get(); }
    ASTNode* get_catch_clause() const { return catch_clause_.get(); }
    ASTNode* get_finally_block() const { return finally_block_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Catch clause (e.g., "catch (e) { ... }")
 */
class CatchClause : public ASTNode {
private:
    std::string parameter_name_;  // Exception parameter name
    std::unique_ptr<ASTNode> body_; // Block statement

public:
    CatchClause(const std::string& parameter_name,
               std::unique_ptr<ASTNode> body,
               const Position& start, const Position& end)
        : ASTNode(Type::CATCH_CLAUSE, start, end),
          parameter_name_(parameter_name),
          body_(std::move(body)) {}
    
    const std::string& get_parameter_name() const { return parameter_name_; }
    ASTNode* get_body() const { return body_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Throw statement (e.g., "throw new Error('message')")
 */
class ThrowStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> expression_;

public:
    ThrowStatement(std::unique_ptr<ASTNode> expression,
                  const Position& start, const Position& end)
        : ASTNode(Type::THROW_STATEMENT, start, end),
          expression_(std::move(expression)) {}
    
    ASTNode* get_expression() const { return expression_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Switch statement (e.g., "switch (expr) { case 1: ... default: ... }")
 */
class SwitchStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> discriminant_;
    std::vector<std::unique_ptr<ASTNode>> cases_;

public:
    SwitchStatement(std::unique_ptr<ASTNode> discriminant,
                   std::vector<std::unique_ptr<ASTNode>> cases,
                   const Position& start, const Position& end)
        : ASTNode(Type::SWITCH_STATEMENT, start, end),
          discriminant_(std::move(discriminant)),
          cases_(std::move(cases)) {}
    
    ASTNode* get_discriminant() const { return discriminant_.get(); }
    const std::vector<std::unique_ptr<ASTNode>>& get_cases() const { return cases_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Case clause (e.g., "case 1: statements..." or "default: statements...")
 */
class CaseClause : public ASTNode {
private:
    std::unique_ptr<ASTNode> test_;  // nullptr for default case
    std::vector<std::unique_ptr<ASTNode>> consequent_;

public:
    CaseClause(std::unique_ptr<ASTNode> test,
              std::vector<std::unique_ptr<ASTNode>> consequent,
              const Position& start, const Position& end)
        : ASTNode(Type::CASE_CLAUSE, start, end),
          test_(std::move(test)),
          consequent_(std::move(consequent)) {}
    
    ASTNode* get_test() const { return test_.get(); }
    const std::vector<std::unique_ptr<ASTNode>>& get_consequent() const { return consequent_; }
    bool is_default() const { return test_ == nullptr; }
    
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

/**
 * Stage 10: Import/Export statements
 */

// Import specifier: { name } or { name as alias }
class ImportSpecifier : public ASTNode {
private:
    std::string imported_name_;  // The name being imported
    std::string local_name_;     // The local alias (same as imported if no 'as')

public:
    ImportSpecifier(const std::string& imported_name, const std::string& local_name,
                   const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_SPECIFIER, start, end),
          imported_name_(imported_name), local_name_(local_name) {}

    const std::string& get_imported_name() const { return imported_name_; }
    const std::string& get_local_name() const { return local_name_; }

    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

// Import statement: import { name } from "module" or import * as name from "module"
class ImportStatement : public ASTNode {
private:
    std::vector<std::unique_ptr<ImportSpecifier>> specifiers_;
    std::string module_source_;
    std::string namespace_alias_;  // For import * as name
    std::string default_alias_;    // For import name from "module"
    bool is_namespace_import_;
    bool is_default_import_;

public:
    ImportStatement(std::vector<std::unique_ptr<ImportSpecifier>> specifiers,
                   const std::string& module_source,
                   const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_STATEMENT, start, end),
          specifiers_(std::move(specifiers)), module_source_(module_source),
          is_namespace_import_(false), is_default_import_(false) {}

    // Constructor for namespace import: import * as name from "module"
    ImportStatement(const std::string& namespace_alias, const std::string& module_source,
                   const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_STATEMENT, start, end),
          module_source_(module_source), namespace_alias_(namespace_alias),
          is_namespace_import_(true), is_default_import_(false) {}

    // Constructor for default import: import name from "module"
    ImportStatement(const std::string& default_alias, const std::string& module_source,
                   bool is_default, const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_STATEMENT, start, end),
          module_source_(module_source), default_alias_(default_alias),
          is_namespace_import_(false), is_default_import_(is_default) {}

    const std::vector<std::unique_ptr<ImportSpecifier>>& get_specifiers() const { return specifiers_; }
    const std::string& get_module_source() const { return module_source_; }
    const std::string& get_namespace_alias() const { return namespace_alias_; }
    const std::string& get_default_alias() const { return default_alias_; }
    bool is_namespace_import() const { return is_namespace_import_; }
    bool is_default_import() const { return is_default_import_; }

    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

// Export specifier: { name } or { name as alias }
class ExportSpecifier : public ASTNode {
private:
    std::string local_name_;     // The local name being exported
    std::string exported_name_;  // The exported alias (same as local if no 'as')

public:
    ExportSpecifier(const std::string& local_name, const std::string& exported_name,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_SPECIFIER, start, end),
          local_name_(local_name), exported_name_(exported_name) {}

    const std::string& get_local_name() const { return local_name_; }
    const std::string& get_exported_name() const { return exported_name_; }

    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

// Export statement: export { name } or export function name() {} or export default value
class ExportStatement : public ASTNode {
private:
    std::vector<std::unique_ptr<ExportSpecifier>> specifiers_;
    std::unique_ptr<ASTNode> declaration_;  // For export declaration
    std::unique_ptr<ASTNode> default_export_;  // For export default
    std::string source_module_;  // For re-exports: export { name } from "module"
    bool is_default_export_;
    bool is_declaration_export_;
    bool is_re_export_;

public:
    // Constructor for named exports: export { name1, name2 }
    ExportStatement(std::vector<std::unique_ptr<ExportSpecifier>> specifiers,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_STATEMENT, start, end),
          specifiers_(std::move(specifiers)),
          is_default_export_(false), is_declaration_export_(false), is_re_export_(false) {}

    // Constructor for declaration exports: export function name() {}
    ExportStatement(std::unique_ptr<ASTNode> declaration,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_STATEMENT, start, end),
          declaration_(std::move(declaration)),
          is_default_export_(false), is_declaration_export_(true), is_re_export_(false) {}

    // Constructor for default exports: export default value
    ExportStatement(std::unique_ptr<ASTNode> default_export, bool is_default,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_STATEMENT, start, end),
          default_export_(std::move(default_export)),
          is_default_export_(is_default), is_declaration_export_(false), is_re_export_(false) {}

    // Constructor for re-exports: export { name } from "module"
    ExportStatement(std::vector<std::unique_ptr<ExportSpecifier>> specifiers,
                   const std::string& source_module,
                   const Position& start, const Position& end)
        : ASTNode(Type::EXPORT_STATEMENT, start, end),
          specifiers_(std::move(specifiers)), source_module_(source_module),
          is_default_export_(false), is_declaration_export_(false), is_re_export_(true) {}

    const std::vector<std::unique_ptr<ExportSpecifier>>& get_specifiers() const { return specifiers_; }
    ASTNode* get_declaration() const { return declaration_.get(); }
    ASTNode* get_default_export() const { return default_export_.get(); }
    const std::string& get_source_module() const { return source_module_; }
    bool is_default_export() const { return is_default_export_; }
    bool is_declaration_export() const { return is_declaration_export_; }
    bool is_re_export() const { return is_re_export_; }

    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

} // namespace Quanta

#endif // QUANTA_AST_H