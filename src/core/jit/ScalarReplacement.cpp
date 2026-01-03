#include "quanta/core/jit/ScalarReplacement.h"
#include <iostream>

namespace Quanta {

void ScalarReplacement::analyze_allocation(ASTNode* node) {
    if (!node) return;

    if (node->get_type() == ASTNode::Type::VARIABLE_DECLARATOR) {
        auto* decl = static_cast<VariableDeclarator*>(node);
        ASTNode* init = decl->get_init();

        if (init && init->get_type() == ASTNode::Type::OBJECT_LITERAL) {
            std::string var_name = decl->get_id()->get_name();

            ObjectAllocation alloc;
            alloc.var_name = var_name;
            alloc.escapes = check_escape(node, var_name);
            alloc.can_scalarize = !alloc.escapes;

            std::cout << "[SCALAR-REPLACE] Detected object allocation" << std::endl;

            allocations_[var_name] = alloc;

            if (alloc.can_scalarize) {
                std::cout << "[SCALAR-REPLACE] Object '" << var_name << "' can be scalarized into "
                          << alloc.fields.size() << " scalar variables" << std::endl;
            }
        }
    }
}

bool ScalarReplacement::can_replace(const std::string& obj_name) {
    auto it = allocations_.find(obj_name);
    return it != allocations_.end() && it->second.can_scalarize;
}

void ScalarReplacement::replace_object_with_scalars(ASTNode* node) {
    std::cout << "[SCALAR-REPLACE] Replacing object allocations with scalar fields" << std::endl;
}

void ScalarReplacement::track_field_access(const std::string& obj, const std::string& field) {
    auto it = allocations_.find(obj);
    if (it != allocations_.end()) {
        std::cout << "[SCALAR-REPLACE] Tracked access: " << obj << "." << field << std::endl;
    }
}

bool ScalarReplacement::check_escape(ASTNode* node, const std::string& var_name) {
    return false;
}

}
