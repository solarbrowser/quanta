#ifndef QUANTA_AST_PRINTER_H
#define QUANTA_AST_PRINTER_H

#include <iostream>
#include <string>
#include "ast.h"

namespace quanta {

/**
 * AST Printer for debugging
 * Implements the visitor pattern to traverse and print the AST
 */
class ASTPrinter : public ExpressionVisitor, public StatementVisitor {
public:
    /**
     * Print the AST starting from the program root
     * @param program The program node
     */
    void print(const std::shared_ptr<Program>& program);

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

private:
    int indent_level_ = 0;

    /**
     * Print indentation
     */
    void indent();

    /**
     * Increase indentation level
     */
    void increase_indent();

    /**
     * Decrease indentation level
     */
    void decrease_indent();

    /**
     * Print a line with indentation
     * @param text The text to print
     */
    void println(const std::string& text);
};

} // namespace quanta

#endif // QUANTA_AST_PRINTER_H 