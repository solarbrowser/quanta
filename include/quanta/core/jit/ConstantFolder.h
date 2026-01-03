#pragma once

#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Value.h"

namespace Quanta {

class ConstantFolder {
public:
    static Value fold_binary_op(BinaryExpression::Operator op, const Value& left, const Value& right);
    static Value fold_unary_op(UnaryExpression::Operator op, const Value& operand);

    static bool is_constant(ASTNode* node);
    static Value evaluate_constant(ASTNode* node);

    static ASTNode* optimize(ASTNode* node);

private:
    static ASTNode* fold_arithmetic(BinaryExpression* binop);
    static ASTNode* fold_logical(BinaryExpression* binop);
    static ASTNode* fold_comparison(BinaryExpression* binop);
    static ASTNode* strength_reduce(BinaryExpression* binop);
};

}
