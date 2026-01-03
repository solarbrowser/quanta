#pragma once

#include "quanta/parser/AST.h"
#include <unordered_map>
#include <string>

namespace Quanta {

struct InlineCandidate {
    std::string name;
    ASTNode* body;
    int size;
    int call_count;
    bool is_hot;
    bool is_recursive;
};

class Inliner {
private:
    std::unordered_map<std::string, InlineCandidate> candidates_;
    static constexpr int MAX_INLINE_SIZE = 20;
    static constexpr int SMALL_FUNC_SIZE = 3;
    static constexpr int MEDIUM_FUNC_SIZE = 10;

public:
    void register_function(const std::string& name, ASTNode* body);
    void record_call(const std::string& name);
    bool should_inline(const std::string& name);
    ASTNode* get_inlined_body(const std::string& name);

    static int calculate_size(ASTNode* node);
    static bool is_inlinable(ASTNode* node);
};

}
