#pragma once

#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Value.h"
#include <unordered_map>

namespace Quanta {

enum class SpeculatedType {
    NONE,
    SMI,
    DOUBLE,
    STRING,
    OBJECT,
    ARRAY,
    FUNCTION
};

struct SpeculationGuard {
    void* check_site;
    SpeculatedType expected_type;
    int hit_count;
    int miss_count;
    bool failed;
};

class SpeculativeOptimizer {
private:
    std::unordered_map<void*, SpeculationGuard> guards_;

public:
    void record_type(void* site, SpeculatedType type);
    SpeculatedType get_speculated_type(void* site);
    bool should_speculate(void* site);
    void record_deopt(void* site);

    bool can_use_smi_ops(void* site);
    bool can_use_double_ops(void* site);
    bool can_skip_boxing(void* site);
};

extern "C" {
    int jit_smi_add(int a, int b);
    int jit_smi_mul(int a, int b);
    double jit_double_add(double a, double b);
    double jit_double_mul(double a, double b);
}

}
