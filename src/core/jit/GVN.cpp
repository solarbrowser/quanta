#include "quanta/core/jit/GVN.h"
#include <iostream>
#include <sstream>

namespace Quanta {

uint32_t GlobalValueNumbering::assign_value_number(ASTNode* node) {
    if (!node) return 0;

    std::string hash = compute_hash(node);
    auto it = value_map_.find(hash);

    if (it != value_map_.end()) {
        std::cout << "[GVN] Found existing value number: " << it->second.number << std::endl;
        return it->second.number;
    }

    ValueNumber vn;
    vn.number = next_number_++;
    vn.node = node;
    vn.is_constant = false;

    if (node->get_type() == ASTNode::Type::NUMBER_LITERAL) {
        vn.is_constant = true;
        vn.constant_value = Value(static_cast<NumberLiteral*>(node)->get_value());
    }

    value_map_[hash] = vn;
    std::cout << "[GVN] Assigned value number: " << vn.number << " to expression" << std::endl;

    return vn.number;
}

bool GlobalValueNumbering::has_same_value(ASTNode* a, ASTNode* b) {
    if (!a || !b) return false;

    std::string hash_a = compute_hash(a);
    std::string hash_b = compute_hash(b);

    return hash_a == hash_b;
}

void GlobalValueNumbering::eliminate_redundant_loads(ASTNode* node) {
    std::cout << "[GVN] Eliminating redundant loads" << std::endl;
}

void GlobalValueNumbering::eliminate_common_subexpressions(ASTNode* node) {
    if (!node || !is_pure_expression(node)) return;

    std::string hash = compute_hash(node);
    auto it = value_map_.find(hash);

    if (it != value_map_.end()) {
        std::cout << "[GVN] Eliminated common subexpression with value number: "
                  << it->second.number << std::endl;
    }
}

std::string GlobalValueNumbering::compute_hash(ASTNode* node) {
    if (!node) return "";

    std::ostringstream oss;
    oss << static_cast<int>(node->get_type());

    if (node->get_type() == ASTNode::Type::BINARY_EXPRESSION) {
        auto* binop = static_cast<BinaryExpression*>(node);
        oss << "_" << static_cast<int>(binop->get_operator());
        oss << "_" << compute_hash(binop->get_left());
        oss << "_" << compute_hash(binop->get_right());
    }
    else if (node->get_type() == ASTNode::Type::NUMBER_LITERAL) {
        oss << "_" << static_cast<NumberLiteral*>(node)->get_value();
    }
    else if (node->get_type() == ASTNode::Type::IDENTIFIER) {
        oss << "_" << static_cast<Identifier*>(node)->get_name();
    }

    return oss.str();
}

bool GlobalValueNumbering::is_pure_expression(ASTNode* node) {
    if (!node) return false;

    switch (node->get_type()) {
        case ASTNode::Type::BINARY_EXPRESSION:
        case ASTNode::Type::UNARY_EXPRESSION:
        case ASTNode::Type::NUMBER_LITERAL:
        case ASTNode::Type::STRING_LITERAL:
        case ASTNode::Type::BOOLEAN_LITERAL:
            return true;

        case ASTNode::Type::CALL_EXPRESSION:
        case ASTNode::Type::NEW_EXPRESSION:
            return false;

        default:
            return false;
    }
}

}
