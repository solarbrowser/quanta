#pragma once

#include "quanta/parser/AST.h"
#include "quanta/core/runtime/Value.h"
#include <unordered_map>
#include <string>

namespace Quanta {

struct ValueNumber {
    uint32_t number;
    ASTNode* node;
    Value constant_value;
    bool is_constant;
};

class GlobalValueNumbering {
private:
    std::unordered_map<std::string, ValueNumber> value_map_;
    uint32_t next_number_;

public:
    GlobalValueNumbering() : next_number_(0) {}

    uint32_t assign_value_number(ASTNode* node);
    bool has_same_value(ASTNode* a, ASTNode* b);
    void eliminate_redundant_loads(ASTNode* node);
    void eliminate_common_subexpressions(ASTNode* node);

private:
    std::string compute_hash(ASTNode* node);
    bool is_pure_expression(ASTNode* node);
};

}
