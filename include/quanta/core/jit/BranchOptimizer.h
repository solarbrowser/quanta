#pragma once

#include "quanta/parser/AST.h"
#include <unordered_map>

namespace Quanta {

struct BranchProfile {
    int true_count;
    int false_count;
    double true_probability;
    bool is_predictable;
};

class BranchOptimizer {
private:
    std::unordered_map<void*, BranchProfile> profiles_;

public:
    void record_branch(void* site, bool taken);
    BranchProfile get_profile(void* site);

    bool should_eliminate_branch(void* site);
    void optimize_branch_layout(ASTNode* node);
    void hoist_invariants(ASTNode* node);

private:
    void calculate_probabilities(BranchProfile& profile);
};

}
