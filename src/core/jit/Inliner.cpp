#include "quanta/core/jit/Inliner.h"
#include <iostream>

namespace Quanta {

void Inliner::register_function(const std::string& name, ASTNode* body) {
    InlineCandidate& candidate = candidates_[name];
    candidate.name = name;
    candidate.body = body;
    candidate.size = calculate_size(body);
    candidate.call_count = 0;
    candidate.is_hot = false;
    candidate.is_recursive = false;

    std::cout << "[INLINER] Registered function: " << name << " (size: " << candidate.size << ")" << std::endl;
}

void Inliner::record_call(const std::string& name) {
    auto it = candidates_.find(name);
    if (it != candidates_.end()) {
        it->second.call_count++;
        if (it->second.call_count > 5) {
            it->second.is_hot = true;
        }
    }
}

bool Inliner::should_inline(const std::string& name) {
    auto it = candidates_.find(name);
    if (it == candidates_.end()) return false;

    const InlineCandidate& candidate = it->second;

    if (candidate.is_recursive) return false;
    if (candidate.size <= SMALL_FUNC_SIZE) return true;
    if (candidate.size <= MEDIUM_FUNC_SIZE && candidate.is_hot) return true;
    if (candidate.size <= MAX_INLINE_SIZE && candidate.call_count > 100) return true;

    return false;
}

ASTNode* Inliner::get_inlined_body(const std::string& name) {
    auto it = candidates_.find(name);
    return (it != candidates_.end()) ? it->second.body : nullptr;
}

int Inliner::calculate_size(ASTNode* node) {
    if (!node) return 0;

    int size = 1;

    switch (node->get_type()) {
        case ASTNode::Type::BLOCK_STATEMENT: {
            auto* block = static_cast<BlockStatement*>(node);
            for (const auto& stmt : block->get_statements()) {
                size += calculate_size(stmt.get());
            }
            break;
        }

        case ASTNode::Type::IF_STATEMENT: {
            auto* ifstmt = static_cast<IfStatement*>(node);
            size += calculate_size(ifstmt->get_consequent());
            size += calculate_size(ifstmt->get_alternate());
            break;
        }

        case ASTNode::Type::FOR_STATEMENT: {
            auto* forstmt = static_cast<ForStatement*>(node);
            size += calculate_size(forstmt->get_body());
            size *= 2;
            break;
        }

        case ASTNode::Type::FUNCTION_DECLARATION:
        case ASTNode::Type::FUNCTION_EXPRESSION:
            size += 10;
            break;

        default:
            break;
    }

    return size;
}

bool Inliner::is_inlinable(ASTNode* node) {
    if (!node) return false;

    switch (node->get_type()) {
        case ASTNode::Type::RETURN_STATEMENT:
        case ASTNode::Type::THROW_STATEMENT:
        case ASTNode::Type::YIELD_EXPRESSION:
        case ASTNode::Type::AWAIT_EXPRESSION:
            return false;

        default:
            return true;
    }
}

}
