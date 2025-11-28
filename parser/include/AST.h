/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

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
class FunctionExpression;

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
        BIGINT_LITERAL,
        UNDEFINED_LITERAL,
        
        // Identifiers
        IDENTIFIER,
        PARAMETER,
        
        // Expressions
        BINARY_EXPRESSION,
        UNARY_EXPRESSION,
        ASSIGNMENT_EXPRESSION,
        CONDITIONAL_EXPRESSION,
        DESTRUCTURING_ASSIGNMENT,
        CALL_EXPRESSION,
        MEMBER_EXPRESSION,
        OPTIONAL_CHAINING_EXPRESSION,
        NULLISH_COALESCING_EXPRESSION,
        NEW_EXPRESSION,
        META_PROPERTY,
        FUNCTION_EXPRESSION,
        ARROW_FUNCTION_EXPRESSION,
        ASYNC_FUNCTION_EXPRESSION,
        AWAIT_EXPRESSION,
        YIELD_EXPRESSION,
        OBJECT_LITERAL,
        ARRAY_LITERAL,
        TEMPLATE_LITERAL,
        REGEX_LITERAL,
        SPREAD_ELEMENT,
        
        // Statements
        EXPRESSION_STATEMENT,
        EMPTY_STATEMENT,
        VARIABLE_DECLARATION,
        VARIABLE_DECLARATOR,
        BLOCK_STATEMENT,
        IF_STATEMENT,
        FOR_STATEMENT,
        FOR_IN_STATEMENT,
        FOR_OF_STATEMENT,
        WHILE_STATEMENT,
        DO_WHILE_STATEMENT,
        FUNCTION_DECLARATION,
        CLASS_DECLARATION,
        METHOD_DEFINITION,
        RETURN_STATEMENT,
        BREAK_STATEMENT,
        CONTINUE_STATEMENT,
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
        
        // JSX
        JSX_ELEMENT,
        JSX_TEXT,
        JSX_EXPRESSION,
        JSX_ATTRIBUTE,
        
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

class BigIntLiteral : public ASTNode {
private:
    std::string value_;

public:
    BigIntLiteral(const std::string& value, const Position& start, const Position& end)
        : ASTNode(Type::BIGINT_LITERAL, start, end), value_(value) {}
    
    const std::string& get_value() const { return value_; }
    
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
 * Template literal (e.g., `Hello ${name}!`)
 */
class TemplateLiteral : public ASTNode {
public:
    struct Element {
        enum class Type { TEXT, EXPRESSION };
        Type type;
        std::string text;  // For text elements
        std::unique_ptr<ASTNode> expression;  // For expression elements
        
        Element(const std::string& t) : type(Type::TEXT), text(t) {}
        Element(std::unique_ptr<ASTNode> expr) : type(Type::EXPRESSION), expression(std::move(expr)) {}
    };

private:
    std::vector<Element> elements_;

public:
    TemplateLiteral(std::vector<Element> elements, const Position& start, const Position& end)
        : ASTNode(Type::TEMPLATE_LITERAL, start, end), elements_(std::move(elements)) {}
    
    const std::vector<Element>& get_elements() const { return elements_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Regular expression literal (e.g., /pattern/flags)
 */
class RegexLiteral : public ASTNode {
private:
    std::string pattern_;
    std::string flags_;

public:
    RegexLiteral(const std::string& pattern, const std::string& flags,
                 const Position& start, const Position& end)
        : ASTNode(Type::REGEX_LITERAL, start, end), pattern_(pattern), flags_(flags) {}
    
    const std::string& get_pattern() const { return pattern_; }
    const std::string& get_flags() const { return flags_; }
    
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
        INSTANCEOF,     // instanceof
        IN,             // in
        
        // Logical
        LOGICAL_AND,    // &&
        LOGICAL_OR,     // ||
        
        // Sequence
        COMMA,          // ,
        
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
 * Conditional expression (ternary operator: test ? consequent : alternate)
 */
class ConditionalExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> test_;
    std::unique_ptr<ASTNode> consequent_;
    std::unique_ptr<ASTNode> alternate_;

public:
    ConditionalExpression(std::unique_ptr<ASTNode> test, std::unique_ptr<ASTNode> consequent,
                         std::unique_ptr<ASTNode> alternate, const Position& start, const Position& end)
        : ASTNode(Type::CONDITIONAL_EXPRESSION, start, end),
          test_(std::move(test)), consequent_(std::move(consequent)), alternate_(std::move(alternate)) {}
    
    ASTNode* get_test() const { return test_.get(); }
    ASTNode* get_consequent() const { return consequent_.get(); }
    ASTNode* get_alternate() const { return alternate_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Assignment expression (e.g., x = 5, y += 10)
 */
class AssignmentExpression : public ASTNode {
public:
    enum class Operator {
        ASSIGN,        // =
        PLUS_ASSIGN,   // +=
        MINUS_ASSIGN,  // -=
        MUL_ASSIGN,    // *=
        DIV_ASSIGN,    // /=
        MOD_ASSIGN     // %=
    };

private:
    std::unique_ptr<ASTNode> left_;
    std::unique_ptr<ASTNode> right_;
    Operator operator_;

public:
    AssignmentExpression(std::unique_ptr<ASTNode> left, Operator op, std::unique_ptr<ASTNode> right,
                        const Position& start, const Position& end)
        : ASTNode(Type::ASSIGNMENT_EXPRESSION, start, end),
          left_(std::move(left)), right_(std::move(right)), operator_(op) {}
    
    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    Operator get_operator() const { return operator_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Destructuring assignment (e.g., [a, b] = array, {x, y} = obj)
 */
class DestructuringAssignment : public ASTNode {
public:
    enum class Type {
        ARRAY,    // [a, b] = array
        OBJECT    // {x, y} = obj
    };
    
    // Structure to handle object property mapping (renaming)
    struct PropertyMapping {
        std::string property_name;  // Original property name in the object
        std::string variable_name;  // Variable name to bind to
        
        PropertyMapping(const std::string& prop, const std::string& var)
            : property_name(prop), variable_name(var) {}
    };
    
    // Structure to handle default values for array destructuring
    struct DefaultValue {
        size_t index;                      // Index in the targets array
        std::unique_ptr<ASTNode> expr;     // Default value expression
        
        DefaultValue(size_t idx, std::unique_ptr<ASTNode> expression)
            : index(idx), expr(std::move(expression)) {}
    };

private:
    std::vector<std::unique_ptr<Identifier>> targets_;
    std::vector<PropertyMapping> property_mappings_; // For object destructuring with renaming
    std::vector<DefaultValue> default_values_; // For array destructuring default values
    std::unique_ptr<ASTNode> source_;
    Type type_;

public:
    DestructuringAssignment(std::vector<std::unique_ptr<Identifier>> targets,
                           std::unique_ptr<ASTNode> source, Type type,
                           const Position& start, const Position& end)
        : ASTNode(ASTNode::Type::DESTRUCTURING_ASSIGNMENT, start, end),
          targets_(std::move(targets)), source_(std::move(source)), type_(type) {}
    
    const std::vector<std::unique_ptr<Identifier>>& get_targets() const { return targets_; }
    const std::vector<PropertyMapping>& get_property_mappings() const { return property_mappings_; }
    const std::vector<DefaultValue>& get_default_values() const { return default_values_; }
    ASTNode* get_source() const { return source_.get(); }
    Type get_type() const { return type_; }
    void set_source(std::unique_ptr<ASTNode> source) { source_ = std::move(source); }
    void add_property_mapping(const std::string& property_name, const std::string& variable_name) {
        property_mappings_.emplace_back(property_name, variable_name);
    }
    void add_default_value(size_t index, std::unique_ptr<ASTNode> expr) {
        default_values_.emplace_back(index, std::move(expr));
    }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
    
private:
    bool handle_complex_object_destructuring(Object* obj, Context& ctx);
    void handle_nested_object_destructuring(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx);
    void handle_nested_object_destructuring_with_source(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx, DestructuringAssignment* source_destructuring);
    void handle_nested_object_destructuring_with_mappings(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx);
    void handle_nested_object_destructuring_smart(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx, DestructuringAssignment* source);
    void handle_nested_object_destructuring_enhanced(Object* nested_obj, const std::vector<std::string>& var_names, Context& ctx, const std::string& property_key);
    void handle_infinite_depth_destructuring(Object* obj, const std::string& nested_pattern, Context& ctx);
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
    Value handle_string_method_call(const std::string& str, const std::string& method_name, Context& ctx);
    Value handle_bigint_method_call(BigInt* bigint, const std::string& method_name, Context& ctx);
    Value handle_member_expression_call(Context& ctx);
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
 * Optional chaining expression (e.g., "obj?.prop", "obj?.method()")
 */
class OptionalChainingExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> object_;
    std::unique_ptr<ASTNode> property_;
    bool computed_; // true for obj?.[prop], false for obj?.prop

public:
    OptionalChainingExpression(std::unique_ptr<ASTNode> object, std::unique_ptr<ASTNode> property, 
                              bool computed, const Position& start, const Position& end)
        : ASTNode(Type::OPTIONAL_CHAINING_EXPRESSION, start, end), 
          object_(std::move(object)), property_(std::move(property)), computed_(computed) {}
    
    ASTNode* get_object() const { return object_.get(); }
    ASTNode* get_property() const { return property_.get(); }
    bool is_computed() const { return computed_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Nullish coalescing expression (e.g., "a ?? b")
 */
class NullishCoalescingExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> left_;
    std::unique_ptr<ASTNode> right_;

public:
    NullishCoalescingExpression(std::unique_ptr<ASTNode> left, std::unique_ptr<ASTNode> right,
                               const Position& start, const Position& end)
        : ASTNode(Type::NULLISH_COALESCING_EXPRESSION, start, end), 
          left_(std::move(left)), right_(std::move(right)) {}
    
    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    
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
 * MetaProperty expression (e.g., new.target)
 */
class MetaProperty : public ASTNode {
private:
    std::string meta_;
    std::string property_;

public:
    MetaProperty(const std::string& meta, const std::string& property,
                 const Position& start, const Position& end)
        : ASTNode(Type::META_PROPERTY, start, end),
          meta_(meta), property_(property) {}

    const std::string& get_meta() const { return meta_; }
    const std::string& get_property() const { return property_; }

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
    
    // Standard loop optimization methods
    bool can_optimize_as_simple_loop() const;
    Value execute_optimized_loop(Context& ctx) const;
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * For...in loop statement (e.g., "for (const key in obj) { ... }")
 */
class ForInStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> left_;     // variable declaration or identifier
    std::unique_ptr<ASTNode> right_;    // object expression
    std::unique_ptr<ASTNode> body_;     // loop body
public:
    ForInStatement(std::unique_ptr<ASTNode> left, std::unique_ptr<ASTNode> right,
                   std::unique_ptr<ASTNode> body, const Position& start, const Position& end)
        : ASTNode(Type::FOR_IN_STATEMENT, start, end), 
          left_(std::move(left)), right_(std::move(right)), body_(std::move(body)) {}
    
    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    ASTNode* get_body() const { return body_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * For...of loop statement (e.g., "for (const item of array) { ... }")
 */
class ForOfStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> left_;     // variable declaration or identifier
    std::unique_ptr<ASTNode> right_;    // iterable expression
    std::unique_ptr<ASTNode> body_;     // loop body
    bool is_await_;                     // true for for-await-of loops
public:
    ForOfStatement(std::unique_ptr<ASTNode> left, std::unique_ptr<ASTNode> right,
                   std::unique_ptr<ASTNode> body, bool is_await, const Position& start, const Position& end)
        : ASTNode(Type::FOR_OF_STATEMENT, start, end),
          left_(std::move(left)), right_(std::move(right)), body_(std::move(body)), is_await_(is_await) {}

    ASTNode* get_left() const { return left_.get(); }
    ASTNode* get_right() const { return right_.get(); }
    ASTNode* get_body() const { return body_.get(); }
    bool is_await() const { return is_await_; }

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
 * Do-while statement (e.g., "do statement while (condition)")
 */
class DoWhileStatement : public ASTNode {
private:
    std::unique_ptr<ASTNode> body_;
    std::unique_ptr<ASTNode> test_;

public:
    DoWhileStatement(std::unique_ptr<ASTNode> body, std::unique_ptr<ASTNode> test,
                     const Position& start, const Position& end)
        : ASTNode(Type::DO_WHILE_STATEMENT, start, end), 
          body_(std::move(body)), test_(std::move(test)) {}
    
    ASTNode* get_body() const { return body_.get(); }
    ASTNode* get_test() const { return test_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Function parameter with optional default value
 */
class Parameter : public ASTNode {
private:
    std::unique_ptr<Identifier> name_;
    std::unique_ptr<ASTNode> default_value_; // nullptr if no default
    bool is_rest_; // true if this is a rest parameter (...args)

public:
    Parameter(std::unique_ptr<Identifier> name, std::unique_ptr<ASTNode> default_value,
              bool is_rest, const Position& start, const Position& end)
        : ASTNode(Type::PARAMETER, start, end), 
          name_(std::move(name)), default_value_(std::move(default_value)), is_rest_(is_rest) {}
    
    Identifier* get_name() const { return name_.get(); }
    ASTNode* get_default_value() const { return default_value_.get(); }
    bool has_default() const { return default_value_ != nullptr; }
    bool is_rest() const { return is_rest_; }
    
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
    std::vector<std::unique_ptr<Parameter>> params_;
    std::unique_ptr<BlockStatement> body_;
    bool is_async_;
    bool is_generator_;

public:
    FunctionDeclaration(std::unique_ptr<Identifier> id, 
                       std::vector<std::unique_ptr<Parameter>> params,
                       std::unique_ptr<BlockStatement> body,
                       const Position& start, const Position& end,
                       bool is_async = false, bool is_generator = false)
        : ASTNode(Type::FUNCTION_DECLARATION, start, end), 
          id_(std::move(id)), params_(std::move(params)), body_(std::move(body)), is_async_(is_async), is_generator_(is_generator) {}
    
    Identifier* get_id() const { return id_.get(); }
    const std::vector<std::unique_ptr<Parameter>>& get_params() const { return params_; }
    BlockStatement* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    bool is_async() const { return is_async_; }
    bool is_generator() const { return is_generator_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Class declaration (e.g., "class MyClass extends BaseClass { constructor() {} method() {} }")
 */
class ClassDeclaration : public ASTNode {
private:
    std::unique_ptr<Identifier> id_;
    std::unique_ptr<Identifier> superclass_;
    std::unique_ptr<BlockStatement> body_;

public:
    ClassDeclaration(std::unique_ptr<Identifier> id,
                    std::unique_ptr<Identifier> superclass,
                    std::unique_ptr<BlockStatement> body,
                    const Position& start, const Position& end)
        : ASTNode(Type::CLASS_DECLARATION, start, end),
          id_(std::move(id)), superclass_(std::move(superclass)), body_(std::move(body)) {}

    ClassDeclaration(std::unique_ptr<Identifier> id,
                    std::unique_ptr<BlockStatement> body,
                    const Position& start, const Position& end)
        : ASTNode(Type::CLASS_DECLARATION, start, end),
          id_(std::move(id)), superclass_(nullptr), body_(std::move(body)) {}
    
    Identifier* get_id() const { return id_.get(); }
    Identifier* get_superclass() const { return superclass_.get(); }
    BlockStatement* get_body() const { return body_.get(); }
    bool has_superclass() const { return superclass_ != nullptr; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Method definition within a class (e.g., "constructor() { ... }", "method() { ... }")
 */
class MethodDefinition : public ASTNode {
public:
    enum Kind {
        CONSTRUCTOR,
        METHOD,
        STATIC_METHOD,
        GETTER,
        SETTER
    };

private:
    std::unique_ptr<ASTNode> key_;
    std::unique_ptr<FunctionExpression> value_;
    Kind kind_;
    bool is_static_;
    bool computed_;

public:
    MethodDefinition(std::unique_ptr<ASTNode> key,
                    std::unique_ptr<FunctionExpression> value,
                    Kind kind,
                    bool is_static,
                    bool computed,
                    const Position& start, const Position& end)
        : ASTNode(Type::METHOD_DEFINITION, start, end),
          key_(std::move(key)), value_(std::move(value)), kind_(kind), is_static_(is_static), computed_(computed) {}

    ASTNode* get_key() const { return key_.get(); }
    bool is_computed() const { return computed_; }
    FunctionExpression* get_value() const { return value_.get(); }
    Kind get_kind() const { return kind_; }
    bool is_static() const { return is_static_; }
    bool is_constructor() const { return kind_ == CONSTRUCTOR; }
    
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
    std::vector<std::unique_ptr<Parameter>> params_;
    std::unique_ptr<BlockStatement> body_;

public:
    FunctionExpression(std::unique_ptr<Identifier> id,
                      std::vector<std::unique_ptr<Parameter>> params,
                      std::unique_ptr<BlockStatement> body,
                      const Position& start, const Position& end)
        : ASTNode(Type::FUNCTION_EXPRESSION, start, end), 
          id_(std::move(id)), params_(std::move(params)), body_(std::move(body)) {}
    
    Identifier* get_id() const { return id_.get(); }
    const std::vector<std::unique_ptr<Parameter>>& get_params() const { return params_; }
    BlockStatement* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    bool is_named() const { return id_ != nullptr; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Arrow function expression (e.g., "(x, y) => x + y", "x => x * 2")
 */
class ArrowFunctionExpression : public ASTNode {
private:
    std::vector<std::unique_ptr<Parameter>> params_;
    std::unique_ptr<ASTNode> body_; // Can be BlockStatement or Expression
    bool is_async_;

public:
    ArrowFunctionExpression(std::vector<std::unique_ptr<Parameter>> params,
                           std::unique_ptr<ASTNode> body,
                           bool is_async,
                           const Position& start, const Position& end)
        : ASTNode(Type::ARROW_FUNCTION_EXPRESSION, start, end), 
          params_(std::move(params)), body_(std::move(body)), is_async_(is_async) {}
    
    const std::vector<std::unique_ptr<Parameter>>& get_params() const { return params_; }
    ASTNode* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    bool is_async() const { return is_async_; }
    bool has_block_body() const { return body_->get_type() == Type::BLOCK_STATEMENT; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Await expression (e.g., "await promise")
 */
class AwaitExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> argument_;

public:
    AwaitExpression(std::unique_ptr<ASTNode> argument, const Position& start, const Position& end)
        : ASTNode(Type::AWAIT_EXPRESSION, start, end), argument_(std::move(argument)) {}
    
    ASTNode* get_argument() const { return argument_.get(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Yield expression (e.g., "yield value", "yield* iterator")
 */
class YieldExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> argument_;
    bool is_delegate_;  // true for yield*, false for yield

public:
    YieldExpression(std::unique_ptr<ASTNode> argument, bool is_delegate, const Position& start, const Position& end)
        : ASTNode(Type::YIELD_EXPRESSION, start, end), argument_(std::move(argument)), is_delegate_(is_delegate) {}
    
    ASTNode* get_argument() const { return argument_.get(); }
    bool is_delegate() const { return is_delegate_; }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Async function expression (e.g., "async function() { ... }")
 */
class AsyncFunctionExpression : public ASTNode {
private:
    std::unique_ptr<Identifier> id_;
    std::vector<std::unique_ptr<Parameter>> params_;
    std::unique_ptr<BlockStatement> body_;

public:
    AsyncFunctionExpression(std::unique_ptr<Identifier> id,
                           std::vector<std::unique_ptr<Parameter>> params,
                           std::unique_ptr<BlockStatement> body,
                           const Position& start, const Position& end)
        : ASTNode(Type::ASYNC_FUNCTION_EXPRESSION, start, end),
          id_(std::move(id)), params_(std::move(params)), body_(std::move(body)) {}
    
    Identifier* get_id() const { return id_.get(); }
    const std::vector<std::unique_ptr<Parameter>>& get_params() const { return params_; }
    BlockStatement* get_body() const { return body_.get(); }
    size_t param_count() const { return params_.size(); }
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Object literal expression (e.g., "{key: value, method: function() {}}")
 */
class ObjectLiteral : public ASTNode {
public:
    enum class PropertyType {
        Value,    // Regular property: key: value
        Method,   // Method syntax: key() {}
        Getter,   // Getter syntax: get key() {}
        Setter    // Setter syntax: set key() {}
    };

    struct Property {
        std::unique_ptr<ASTNode> key;   // Identifier or computed expression
        std::unique_ptr<ASTNode> value; // Value expression or function for getter/setter
        bool computed;                  // true for [expr]: value, false for key: value
        PropertyType type;              // Property type (value, method, getter, setter)

        Property(std::unique_ptr<ASTNode> k, std::unique_ptr<ASTNode> v, bool c = false, PropertyType t = PropertyType::Value)
            : key(std::move(k)), value(std::move(v)), computed(c), type(t) {}

        // Backward compatibility constructor
        Property(std::unique_ptr<ASTNode> k, std::unique_ptr<ASTNode> v, bool c, bool m)
            : key(std::move(k)), value(std::move(v)), computed(c), type(m ? PropertyType::Method : PropertyType::Value) {}
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
 * Spread element (e.g., "...array" in function calls or array literals)
 */
class SpreadElement : public ASTNode {
private:
    std::unique_ptr<ASTNode> argument_;

public:
    explicit SpreadElement(std::unique_ptr<ASTNode> argument, 
                          const Position& start, const Position& end)
        : ASTNode(Type::SPREAD_ELEMENT, start, end), argument_(std::move(argument)) {}
    
    ASTNode* get_argument() const { return argument_.get(); }
    
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
 * Break statement (e.g., "break;")
 */
class BreakStatement : public ASTNode {
public:
    explicit BreakStatement(const Position& start, const Position& end)
        : ASTNode(Type::BREAK_STATEMENT, start, end) {}
    
    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * Continue statement (e.g., "continue;")
 */
class ContinueStatement : public ASTNode {
public:
    explicit ContinueStatement(const Position& start, const Position& end)
        : ASTNode(Type::CONTINUE_STATEMENT, start, end) {}
    
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
 * Empty statement (e.g., ";")
 */
class EmptyStatement : public ASTNode {
public:
    EmptyStatement(const Position& start, const Position& end)
        : ASTNode(Type::EMPTY_STATEMENT, start, end) {}

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
    
    void check_use_strict_directive(Context& ctx);
    void hoist_var_declarations(Context& ctx);
    void scan_for_var_declarations(ASTNode* node, Context& ctx);

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

    // Constructor for mixed import: import defaultThing, { namedThing } from "module"
    ImportStatement(const std::string& default_alias, 
                   std::vector<std::unique_ptr<ImportSpecifier>> specifiers,
                   const std::string& module_source,
                   const Position& start, const Position& end)
        : ASTNode(Type::IMPORT_STATEMENT, start, end),
          specifiers_(std::move(specifiers)), module_source_(module_source), 
          default_alias_(default_alias),
          is_namespace_import_(false), is_default_import_(true) {}

    const std::vector<std::unique_ptr<ImportSpecifier>>& get_specifiers() const { return specifiers_; }
    const std::string& get_module_source() const { return module_source_; }
    const std::string& get_namespace_alias() const { return namespace_alias_; }
    const std::string& get_default_alias() const { return default_alias_; }
    bool is_namespace_import() const { return is_namespace_import_; }
    bool is_default_import() const { return is_default_import_; }
    bool is_mixed_import() const { return is_default_import_ && !specifiers_.empty(); }

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

//=============================================================================
// JSX Nodes
//=============================================================================

/**
 * JSX Element (e.g., <div>content</div>, <Component prop={value} />)
 */
class JSXElement : public ASTNode {
private:
    std::string tag_name_;
    std::vector<std::unique_ptr<ASTNode>> attributes_;
    std::vector<std::unique_ptr<ASTNode>> children_;
    bool self_closing_;

public:
    JSXElement(const std::string& tag_name,
               std::vector<std::unique_ptr<ASTNode>> attributes,
               std::vector<std::unique_ptr<ASTNode>> children,
               bool self_closing,
               const Position& start, const Position& end)
        : ASTNode(Type::JSX_ELEMENT, start, end),
          tag_name_(tag_name), attributes_(std::move(attributes)),
          children_(std::move(children)), self_closing_(self_closing) {}

    const std::string& get_tag_name() const { return tag_name_; }
    const std::vector<std::unique_ptr<ASTNode>>& get_attributes() const { return attributes_; }
    const std::vector<std::unique_ptr<ASTNode>>& get_children() const { return children_; }
    bool is_self_closing() const { return self_closing_; }

    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * JSX Text (e.g., the "Hello World" in <div>Hello World</div>)
 */
class JSXText : public ASTNode {
private:
    std::string text_;

public:
    JSXText(const std::string& text, const Position& start, const Position& end)
        : ASTNode(Type::JSX_TEXT, start, end), text_(text) {}

    const std::string& get_text() const { return text_; }

    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * JSX Expression (e.g., {expression} in JSX)
 */
class JSXExpression : public ASTNode {
private:
    std::unique_ptr<ASTNode> expression_;

public:
    JSXExpression(std::unique_ptr<ASTNode> expression, const Position& start, const Position& end)
        : ASTNode(Type::JSX_EXPRESSION, start, end), expression_(std::move(expression)) {}

    ASTNode* get_expression() const { return expression_.get(); }

    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

/**
 * JSX Attribute (e.g., className="active" or onClick={handler})
 */
class JSXAttribute : public ASTNode {
private:
    std::string name_;
    std::unique_ptr<ASTNode> value_; // Can be string literal or JSXExpression

public:
    JSXAttribute(const std::string& name, std::unique_ptr<ASTNode> value,
                 const Position& start, const Position& end)
        : ASTNode(Type::JSX_ATTRIBUTE, start, end), name_(name), value_(std::move(value)) {}

    const std::string& get_name() const { return name_; }
    ASTNode* get_value() const { return value_.get(); }

    Value evaluate(Context& ctx) override;
    std::string to_string() const override;
    std::unique_ptr<ASTNode> clone() const override;
};

} // namespace Quanta

#endif // QUANTA_AST_H