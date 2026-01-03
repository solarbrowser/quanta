#pragma once

#include "quanta/parser/AST.h"
#include <unordered_set>
#include <string>

namespace Quanta {

struct EscapeInfo {
    bool escapes;
    bool used_in_closure;
    bool returned;
    bool stored_in_global;
    int reference_count;
};

class EscapeAnalysis {
public:
    static EscapeInfo analyze(ASTNode* node, const std::string& var_name);
    static bool can_stack_allocate(const EscapeInfo& info);
    static bool can_eliminate_allocation(const EscapeInfo& info);
};

}
