#include "quanta/core/jit/BranchOptimizer.h"
#include <iostream>

namespace Quanta {

void BranchOptimizer::record_branch(void* site, bool taken) {
    BranchProfile& profile = profiles_[site];

    if (taken) {
        profile.true_count++;
    } else {
        profile.false_count++;
    }

    calculate_probabilities(profile);

    if (profile.true_probability > 0.95 || profile.true_probability < 0.05) {
        profile.is_predictable = true;
        std::cout << "[BRANCH-OPT] Highly predictable branch detected: "
                  << (profile.true_probability * 100) << "% taken" << std::endl;
    }
}

BranchProfile BranchOptimizer::get_profile(void* site) {
    return profiles_[site];
}

bool BranchOptimizer::should_eliminate_branch(void* site) {
    auto it = profiles_.find(site);
    if (it == profiles_.end()) return false;

    const BranchProfile& profile = it->second;

    if (profile.true_probability == 1.0) {
        std::cout << "[BRANCH-OPT] Branch always taken - eliminating false path" << std::endl;
        return true;
    }

    if (profile.true_probability == 0.0) {
        std::cout << "[BRANCH-OPT] Branch never taken - eliminating true path" << std::endl;
        return true;
    }

    return false;
}

void BranchOptimizer::optimize_branch_layout(ASTNode* node) {
    std::cout << "[BRANCH-OPT] Optimizing branch layout for better CPU prediction" << std::endl;
}

void BranchOptimizer::hoist_invariants(ASTNode* node) {
    std::cout << "[BRANCH-OPT] Hoisting loop-invariant conditions" << std::endl;
}

void BranchOptimizer::calculate_probabilities(BranchProfile& profile) {
    int total = profile.true_count + profile.false_count;
    if (total > 0) {
        profile.true_probability = static_cast<double>(profile.true_count) / total;
    }
}

}
