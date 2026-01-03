#pragma once

#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Value.h"
#include <unordered_map>
#include <vector>

namespace Quanta {

struct ObjectAllocation {
    std::string var_name;
    std::unordered_map<std::string, Value> fields;
    bool escapes;
    bool can_scalarize;
};

class ScalarReplacement {
public:
    void analyze_allocation(ASTNode* node);
    bool can_replace(const std::string& obj_name);
    void replace_object_with_scalars(ASTNode* node);

private:
    std::unordered_map<std::string, ObjectAllocation> allocations_;

    void track_field_access(const std::string& obj, const std::string& field);
    bool check_escape(ASTNode* node, const std::string& var_name);
};

}
