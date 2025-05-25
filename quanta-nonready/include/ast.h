//<---------QUANTA JS ENGINE - AST HEADER--------->
// Stage 1: Core Engine & Runtime - AST Generator
// Purpose: Define Abstract Syntax Tree node structures
// Max Lines: 5000 (Current: ~200)

#ifndef QUANTA_AST_H
#define QUANTA_AST_H

#include <string>
#include <vector>
#include <memory>
#include <variant>

namespace Quanta {

//<---------AST NODE TYPES--------->
enum class ASTNodeType {
    PROGRAM,
    VARIABLE_DECLARATION,
    FUNCTION_DECLARATION,
    BLOCK_STATEMENT,
    EXPRESSION_STATEMENT,
    IF_STATEMENT,
    WHILE_STATEMENT,
    FOR_STATEMENT,
    RETURN_STATEMENT,
    
    // Expressions
    BINARY_EXPRESSION,
    UNARY_EXPRESSION,
    ASSIGNMENT_EXPRESSION,
    CALL_EXPRESSION,
    MEMBER_EXPRESSION,
    
    // Literals
    NUMBER_LITERAL,
    STRING_LITERAL,
    BOOLEAN_LITERAL,
    NULL_LITERAL,
    IDENTIFIER
};

//<---------BASE AST NODE--------->
class ASTNode {
public:
    ASTNodeType type;
    size_t line;
    size_t column;
    
    ASTNode(ASTNodeType t, size_t l, size_t c) : type(t), line(l), column(c) {}
    virtual ~ASTNode() = default;
};

//<---------PROGRAM NODE--------->
class ProgramNode : public ASTNode {
public:
    std::vector<std::unique_ptr<ASTNode>> statements;
    
    ProgramNode() : ASTNode(ASTNodeType::PROGRAM, 0, 0) {}
};

//<---------EXPRESSION NODES--------->
class BinaryExpressionNode : public ASTNode {
public:
    std::unique_ptr<ASTNode> left;
    std::string operator_;
    std::unique_ptr<ASTNode> right;
    
    BinaryExpressionNode(std::unique_ptr<ASTNode> l, const std::string& op, std::unique_ptr<ASTNode> r)
        : ASTNode(ASTNodeType::BINARY_EXPRESSION, 0, 0), left(std::move(l)), operator_(op), right(std::move(r)) {}
};

class UnaryExpressionNode : public ASTNode {
public:
    std::string operator_;
    std::unique_ptr<ASTNode> operand;
    
    UnaryExpressionNode(const std::string& op, std::unique_ptr<ASTNode> operand)
        : ASTNode(ASTNodeType::UNARY_EXPRESSION, 0, 0), operator_(op), operand(std::move(operand)) {}
};

class AssignmentExpressionNode : public ASTNode {
public:
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;
    
    AssignmentExpressionNode(std::unique_ptr<ASTNode> l, std::unique_ptr<ASTNode> r)
        : ASTNode(ASTNodeType::ASSIGNMENT_EXPRESSION, 0, 0), left(std::move(l)), right(std::move(r)) {}
};

//<---------LITERAL NODES--------->
class NumberLiteralNode : public ASTNode {
public:
    double value;
    
    NumberLiteralNode(double v) : ASTNode(ASTNodeType::NUMBER_LITERAL, 0, 0), value(v) {}
};

class StringLiteralNode : public ASTNode {
public:
    std::string value;
    
    StringLiteralNode(const std::string& v) : ASTNode(ASTNodeType::STRING_LITERAL, 0, 0), value(v) {}
};

class IdentifierNode : public ASTNode {
public:
    std::string name;
    
    IdentifierNode(const std::string& n) : ASTNode(ASTNodeType::IDENTIFIER, 0, 0), name(n) {}
};

//<---------STATEMENT NODES--------->
class VariableDeclarationNode : public ASTNode {
public:
    std::string kind; // "let", "const", "var"
    std::string name;
    std::unique_ptr<ASTNode> initializer;
    
    VariableDeclarationNode(const std::string& k, const std::string& n, std::unique_ptr<ASTNode> init)
        : ASTNode(ASTNodeType::VARIABLE_DECLARATION, 0, 0), kind(k), name(n), initializer(std::move(init)) {}
};

class BlockStatementNode : public ASTNode {
public:
    std::vector<std::unique_ptr<ASTNode>> statements;
    
    BlockStatementNode() : ASTNode(ASTNodeType::BLOCK_STATEMENT, 0, 0) {}
};

class ExpressionStatementNode : public ASTNode {
public:
    std::unique_ptr<ASTNode> expression;
    
    ExpressionStatementNode(std::unique_ptr<ASTNode> expr)
        : ASTNode(ASTNodeType::EXPRESSION_STATEMENT, 0, 0), expression(std::move(expr)) {}
};

} // namespace Quanta

#endif // QUANTA_AST_H
