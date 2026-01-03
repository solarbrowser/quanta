#include "quanta/core/jit/ConstantFolder.h"
#include <iostream>
#include <cmath>

namespace Quanta {

Value ConstantFolder::fold_binary_op(BinaryExpression::Operator op, const Value& left, const Value& right) {
    if (!left.is_number() || !right.is_number()) {
        return Value();
    }

    double l = left.as_number();
    double r = right.as_number();

    switch (op) {
        case BinaryExpression::Operator::ADD:
            return Value(l + r);
        case BinaryExpression::Operator::SUBTRACT:
            return Value(l - r);
        case BinaryExpression::Operator::MULTIPLY:
            return Value(l * r);
        case BinaryExpression::Operator::DIVIDE:
            return Value(r != 0.0 ? l / r : INFINITY);
        case BinaryExpression::Operator::MODULO:
            return Value(std::fmod(l, r));
        case BinaryExpression::Operator::EXPONENT:
            return Value(std::pow(l, r));
        default:
            return Value();
    }
}

Value ConstantFolder::fold_unary_op(UnaryExpression::Operator op, const Value& operand) {
    if (!operand.is_number()) {
        return Value();
    }

    double val = operand.as_number();

    switch (op) {
        case UnaryExpression::Operator::PLUS:
            return Value(val);
        case UnaryExpression::Operator::MINUS:
            return Value(-val);
        case UnaryExpression::Operator::BITWISE_NOT:
            return Value(static_cast<double>(~static_cast<int32_t>(val)));
        default:
            return Value();
    }
}

bool ConstantFolder::is_constant(ASTNode* node) {
    if (!node) return false;

    switch (node->get_type()) {
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
        case ASTNode::Type::NULL_LITERAL:
        case ASTNode::Type::UNDEFINED_LITERAL:
            return true;

        case ASTNode::Type::BINARY_EXPRESSION: {
            auto* binop = static_cast<BinaryExpression*>(node);
            return is_constant(binop->get_left()) && is_constant(binop->get_right());
        }

        case ASTNode::Type::UNARY_EXPRESSION: {
            auto* unop = static_cast<UnaryExpression*>(node);
            return is_constant(unop->get_operand());
        }

        default:
            return false;
    }
}

Value ConstantFolder::evaluate_constant(ASTNode* node) {
    if (!node) return Value();

    switch (node->get_type()) {
        case ASTNode::Type::NUMBER_LITERAL:
            return Value(static_cast<NumberLiteral*>(node)->get_value());

        case ASTNode::Type::BOOLEAN_LITERAL:
            return Value(static_cast<BooleanLiteral*>(node)->get_value());

        case ASTNode::Type::NULL_LITERAL:
            return Value::null();

        case ASTNode::Type::UNDEFINED_LITERAL:
            return Value();

        default:
            return Value();
    }
}

ASTNode* ConstantFolder::optimize(ASTNode* node) {
    if (!node) return nullptr;

    if (node->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
        auto* binop = static_cast<BinaryExpression*>(node);

        if (ASTNode* folded = fold_arithmetic(binop)) {
            std::cout << "[CONST-FOLD] Folded arithmetic expression" << std::endl;
            return folded;
        }

        if (ASTNode* reduced = strength_reduce(binop)) {
            std::cout << "[STRENGTH-REDUCE] Applied strength reduction" << std::endl;
            return reduced;
        }
    }

    return node;
}

ASTNode* ConstantFolder::fold_arithmetic(BinaryExpression* binop) {
    if (!binop) return nullptr;

    ASTNode* left = binop->get_left();
    ASTNode* right = binop->get_right();

    if (is_constant(left) && is_constant(right)) {
        Value lval = evaluate_constant(left);
        Value rval = evaluate_constant(right);
        Value result = fold_binary_op(binop->get_operator(), lval, rval);

        if (result.is_number()) {
            return new NumberLiteral(result.as_number(), binop->get_start(), binop->get_end());
        }
    }

    auto op = binop->get_operator();
    if (left && left->get_type() == ASTNode::Type::NUMBER_LITERAL) {
        double lval = static_cast<NumberLiteral*>(left)->get_value();

        if (op == BinaryExpression::Operator::MULTIPLY && lval == 0.0) {
            return new NumberLiteral(0.0, binop->get_start(), binop->get_end());
        }
        if (op == BinaryExpression::Operator::MULTIPLY && lval == 1.0) {
            return right;
        }
        if (op == BinaryExpression::Operator::ADD && lval == 0.0) {
            return right;
        }
    }

    return nullptr;
}

ASTNode* ConstantFolder::fold_logical(BinaryExpression* binop) {
    return nullptr;
}

ASTNode* ConstantFolder::fold_comparison(BinaryExpression* binop) {
    return nullptr;
}

ASTNode* ConstantFolder::strength_reduce(BinaryExpression* binop) {
    if (!binop) return nullptr;

    ASTNode* right = binop->get_right();
    if (!right || right->get_type() != ASTNode::Type::NUMBER_LITERAL) {
        return nullptr;
    }

    double val = static_cast<NumberLiteral*>(right)->get_value();
    auto op = binop->get_operator();

    if (op == BinaryExpression::Operator::MULTIPLY) {
        if (val == 2.0 || val == 4.0 || val == 8.0 || val == 16.0) {
            return binop;
        }
    }
    else if (op == BinaryExpression::Operator::DIVIDE) {
        if (val == 2.0 || val == 4.0 || val == 8.0) {
            return binop;
        }
    }

    return nullptr;
}

}
