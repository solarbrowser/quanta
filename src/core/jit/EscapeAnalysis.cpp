#include "quanta/core/jit/EscapeAnalysis.h"
#include <iostream>

namespace Quanta {

EscapeInfo EscapeAnalysis::analyze(ASTNode* node, const std::string& var_name) {
    EscapeInfo info = {false, false, false, false, 0};

    if (!node) return info;

    switch (node->get_type()) {
        case ASTNode::Type::RETURN_STATEMENT: {
            auto* ret = static_cast<ReturnStatement*>(node);
            if (ret->get_argument()) {
                info.returned = true;
                info.escapes = true;
            }
            break;
        }

        case ASTNode::Type::FUNCTION_EXPRESSION:
        case ASTNode::Type::ARROW_FUNCTION_EXPRESSION:
            info.used_in_closure = true;
            info.escapes = true;
            break;

        case ASTNode::Type::BINARY_EXPRESSION: {
            auto* binop = static_cast<BinaryExpression*>(node);
            if (binop->get_operator() == BinaryExpression::Operator::ASSIGN) {
                ASTNode* left = binop->get_left();
                if (left && left->get_type() == ASTNode::Type::MEMBER_EXPRESSION) {
                    info.stored_in_global = true;
                    info.escapes = true;
                }
            }
            break;
        }

        case ASTNode::Type::CALL_EXPRESSION:
            info.reference_count++;
            info.escapes = true;
            break;

        default:
            break;
    }

    return info;
}

bool EscapeAnalysis::can_stack_allocate(const EscapeInfo& info) {
    return !info.escapes && !info.returned && !info.used_in_closure;
}

bool EscapeAnalysis::can_eliminate_allocation(const EscapeInfo& info) {
    return info.reference_count == 0 && !info.escapes;
}

}
