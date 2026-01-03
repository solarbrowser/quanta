#include "quanta/core/jit/SpeculativeOptimizer.h"
#include <iostream>

namespace Quanta {

void SpeculativeOptimizer::record_type(void* site, SpeculatedType type) {
    SpeculationGuard& guard = guards_[site];

    if (guard.expected_type == SpeculatedType::NONE) {
        guard.expected_type = type;
        guard.check_site = site;
    }

    if (guard.expected_type == type) {
        guard.hit_count++;
    } else {
        guard.miss_count++;
        if (guard.miss_count > 10) {
            guard.failed = true;
            std::cout << "[DEOPT] Speculation failed at site " << site << std::endl;
        }
    }
}

SpeculatedType SpeculativeOptimizer::get_speculated_type(void* site) {
    auto it = guards_.find(site);
    return (it != guards_.end()) ? it->second.expected_type : SpeculatedType::NONE;
}

bool SpeculativeOptimizer::should_speculate(void* site) {
    auto it = guards_.find(site);
    if (it == guards_.end()) return false;

    const SpeculationGuard& guard = it->second;
    return !guard.failed && guard.hit_count > 5;
}

void SpeculativeOptimizer::record_deopt(void* site) {
    auto it = guards_.find(site);
    if (it != guards_.end()) {
        it->second.failed = true;
        std::cout << "[DEOPT] Forced deoptimization at site " << site << std::endl;
    }
}

bool SpeculativeOptimizer::can_use_smi_ops(void* site) {
    return get_speculated_type(site) == SpeculatedType::SMI && should_speculate(site);
}

bool SpeculativeOptimizer::can_use_double_ops(void* site) {
    SpeculatedType type = get_speculated_type(site);
    return (type == SpeculatedType::DOUBLE || type == SpeculatedType::SMI) && should_speculate(site);
}

bool SpeculativeOptimizer::can_skip_boxing(void* site) {
    return can_use_smi_ops(site) || can_use_double_ops(site);
}

extern "C" int jit_smi_add(int a, int b) {
    return a + b;
}

extern "C" int jit_smi_mul(int a, int b) {
    return a * b;
}

extern "C" double jit_double_add(double a, double b) {
    return a + b;
}

extern "C" double jit_double_mul(double a, double b) {
    return a * b;
}

}
