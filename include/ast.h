#ifndef QUANTA_AST_H
#define QUANTA_AST_H

#include <string>
#include <vector>
#include <memory>
#include <variant>
#include "token.h"

namespace quanta {

// Forward declarations for all node types
class Expression;
class Statement;
class Program;
class BlockStatement;
class ExpressionStatement;
class VariableDeclaration;
class VariableDeclarator;
class FunctionDeclaration;
class ReturnStatement;
class IfStatement;
class WhileStatement;
class ForStatement;
class Identifier;
class Literal;
class BinaryExpression;
class LogicalExpression;
class UnaryExpression;
class AssignmentExpression;
class CallExpression;
class MemberExpression;
class FunctionExpression;
class ObjectExpression;
class Property;
class ArrayExpression;
class ThisExpression;

// Visitor pattern interfaces
class ExpressionVisitor {
public:
    virtual ~ExpressionVisitor() = default;
    virtual void visit(Identifier& expr) = 0;
    virtual void visit(Literal& expr) = 0;
    virtual void visit(BinaryExpression& expr) = 0;
    virtual void visit(LogicalExpression& expr) = 0;
    virtual void visit(UnaryExpression& expr) = 0;
    virtual void visit(AssignmentExpression& expr) = 0;
    virtual void visit(CallExpression& expr) = 0;
    virtual void visit(MemberExpression& expr) = 0;
    virtual void visit(FunctionExpression& expr) = 0;
    virtual void visit(ObjectExpression& expr) = 0;
    virtual void visit(ArrayExpression& expr) = 0;
    virtual void visit(ThisExpression& expr) = 0;
};

class StatementVisitor {
public:
    virtual ~StatementVisitor() = default;
    virtual void visit(Program& stmt) = 0;
    virtual void visit(BlockStatement& stmt) = 0;
    virtual void visit(ExpressionStatement& stmt) = 0;
    virtual void visit(VariableDeclaration& stmt) = 0;
    virtual void visit(FunctionDeclaration& stmt) = 0;
    virtual void visit(ReturnStatement& stmt) = 0;
    virtual void visit(IfStatement& stmt) = 0;
    virtual void visit(WhileStatement& stmt) = 0;
    virtual void visit(ForStatement& stmt) = 0;
};

// Base classes for AST nodes
class Node {
public:
    virtual ~Node() = default;
    int line = 0;
    int column = 0;
};

class Expression : public Node {
public:
    virtual ~Expression() = default;
    virtual void accept(ExpressionVisitor& visitor) = 0;
};

class Statement : public Node {
public:
    virtual ~Statement() = default;
    virtual void accept(StatementVisitor& visitor) = 0;
};

// Statements

class Program : public Statement {
public:
    std::vector<std::shared_ptr<Statement>> body;

    void accept(StatementVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class BlockStatement : public Statement {
public:
    std::vector<std::shared_ptr<Statement>> body;

    void accept(StatementVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class ExpressionStatement : public Statement {
public:
    std::shared_ptr<Expression> expression;

    explicit ExpressionStatement(std::shared_ptr<Expression> expr)
        : expression(std::move(expr)) {}

    void accept(StatementVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class VariableDeclaration : public Statement {
public:
    enum class Kind { Var, Let, Const };
    
    Kind kind;
    std::vector<std::shared_ptr<VariableDeclarator>> declarations;

    explicit VariableDeclaration(Kind k)
        : kind(k) {}

    void accept(StatementVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class VariableDeclarator : public Node {
public:
    std::shared_ptr<Identifier> id;
    std::shared_ptr<Expression> init;  // Can be nullptr for uninitialized variables

    VariableDeclarator(std::shared_ptr<Identifier> identifier, std::shared_ptr<Expression> initializer)
        : id(std::move(identifier)), init(std::move(initializer)) {}
};

class FunctionDeclaration : public Statement {
public:
    std::shared_ptr<Identifier> id;
    std::vector<std::shared_ptr<Identifier>> params;
    std::shared_ptr<BlockStatement> body;

    FunctionDeclaration(std::shared_ptr<Identifier> name, 
                         std::vector<std::shared_ptr<Identifier>> parameters,
                         std::shared_ptr<BlockStatement> functionBody)
        : id(std::move(name)), params(std::move(parameters)), body(std::move(functionBody)) {}

    void accept(StatementVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class ReturnStatement : public Statement {
public:
    std::shared_ptr<Expression> argument;  // Can be nullptr for empty return

    explicit ReturnStatement(std::shared_ptr<Expression> arg = nullptr)
        : argument(std::move(arg)) {}

    void accept(StatementVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class IfStatement : public Statement {
public:
    std::shared_ptr<Expression> test;
    std::shared_ptr<Statement> consequent;
    std::shared_ptr<Statement> alternate;  // Can be nullptr if no else clause

    IfStatement(std::shared_ptr<Expression> condition, 
                 std::shared_ptr<Statement> thenBranch,
                 std::shared_ptr<Statement> elseBranch = nullptr)
        : test(std::move(condition)), consequent(std::move(thenBranch)), alternate(std::move(elseBranch)) {}

    void accept(StatementVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class WhileStatement : public Statement {
public:
    std::shared_ptr<Expression> test;
    std::shared_ptr<Statement> body;

    WhileStatement(std::shared_ptr<Expression> condition, std::shared_ptr<Statement> loopBody)
        : test(std::move(condition)), body(std::move(loopBody)) {}

    void accept(StatementVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class ForStatement : public Statement {
public:
    // init can be nullptr, VariableDeclaration, or Expression
    std::variant<std::monostate, std::shared_ptr<VariableDeclaration>, std::shared_ptr<Expression>> init;
    std::shared_ptr<Expression> test;  // Can be nullptr
    std::shared_ptr<Expression> update;  // Can be nullptr
    std::shared_ptr<Statement> body;

    ForStatement(
        std::variant<std::monostate, std::shared_ptr<VariableDeclaration>, std::shared_ptr<Expression>> initializer,
        std::shared_ptr<Expression> condition,
        std::shared_ptr<Expression> increment,
        std::shared_ptr<Statement> loopBody)
        : init(std::move(initializer)), test(std::move(condition)), update(std::move(increment)), body(std::move(loopBody)) {}

    void accept(StatementVisitor& visitor) override {
        visitor.visit(*this);
    }
};

// Expressions

class Identifier : public Expression {
public:
    std::string name;

    explicit Identifier(std::string n)
        : name(std::move(n)) {}

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class Literal : public Expression {
public:
    LiteralValue value;

    explicit Literal(const LiteralValue& val)
        : value(val) {}

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class BinaryExpression : public Expression {
public:
    std::string operator_;
    std::shared_ptr<Expression> left;
    std::shared_ptr<Expression> right;

    BinaryExpression(const std::string& op, std::shared_ptr<Expression> l, std::shared_ptr<Expression> r)
        : operator_(op), left(std::move(l)), right(std::move(r)) {}

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class LogicalExpression : public Expression {
public:
    std::string operator_;  // "&&" or "||"
    std::shared_ptr<Expression> left;
    std::shared_ptr<Expression> right;

    LogicalExpression(const std::string& op, std::shared_ptr<Expression> l, std::shared_ptr<Expression> r)
        : operator_(op), left(std::move(l)), right(std::move(r)) {}

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class UnaryExpression : public Expression {
public:
    std::string operator_;  // "-", "!", etc.
    std::shared_ptr<Expression> argument;
    bool prefix;  // Always true for now

    UnaryExpression(const std::string& op, std::shared_ptr<Expression> arg, bool pre = true)
        : operator_(op), argument(std::move(arg)), prefix(pre) {}

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class AssignmentExpression : public Expression {
public:
    std::string operator_;  // "=", "+=", "-=", etc.
    std::shared_ptr<Expression> left;  // Must be valid LHS (Identifier or MemberExpression)
    std::shared_ptr<Expression> right;

    AssignmentExpression(const std::string& op, std::shared_ptr<Expression> l, std::shared_ptr<Expression> r)
        : operator_(op), left(std::move(l)), right(std::move(r)) {}

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class CallExpression : public Expression {
public:
    std::shared_ptr<Expression> callee;
    std::vector<std::shared_ptr<Expression>> arguments;

    explicit CallExpression(std::shared_ptr<Expression> c)
        : callee(std::move(c)) {}

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class MemberExpression : public Expression {
public:
    std::shared_ptr<Expression> object;
    std::shared_ptr<Expression> property;
    bool computed;  // True for obj[expr], false for obj.expr

    MemberExpression(std::shared_ptr<Expression> obj, std::shared_ptr<Expression> prop, bool comp)
        : object(std::move(obj)), property(std::move(prop)), computed(comp) {}

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class FunctionExpression : public Expression {
public:
    std::shared_ptr<Identifier> id;  // Can be nullptr for anonymous functions
    std::vector<std::shared_ptr<Identifier>> params;
    std::shared_ptr<BlockStatement> body;

    FunctionExpression(std::shared_ptr<Identifier> name, 
                       std::vector<std::shared_ptr<Identifier>> parameters,
                       std::shared_ptr<BlockStatement> functionBody)
        : id(std::move(name)), params(std::move(parameters)), body(std::move(functionBody)) {}

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class Property : public Node {
public:
    enum class Kind { Init, Get, Set };
    
    std::shared_ptr<Expression> key;  // Identifier or Literal
    std::shared_ptr<Expression> value;
    Kind kind;

    Property(std::shared_ptr<Expression> k, std::shared_ptr<Expression> v, Kind knd = Kind::Init)
        : key(std::move(k)), value(std::move(v)), kind(knd) {}
};

class ObjectExpression : public Expression {
public:
    std::vector<std::shared_ptr<Property>> properties;

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class ArrayExpression : public Expression {
public:
    std::vector<std::shared_ptr<Expression>> elements;  // Can contain nullptr for holes

    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

class ThisExpression : public Expression {
public:
    void accept(ExpressionVisitor& visitor) override {
        visitor.visit(*this);
    }
};

} // namespace quanta

#endif // QUANTA_AST_H 