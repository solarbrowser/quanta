#pragma once

#include "quanta/parser/AST.h"
#include "quanta/core/engine/Context.h"

namespace Quanta {

struct OSREntry {
    void* optimized_code;
    int bytecode_offset;
    int loop_depth;
    bool can_enter;
};

class OnStackReplacement {
private:
    std::unordered_map<ASTNode*, OSREntry> osr_points_;
    int osr_threshold_;

public:
    OnStackReplacement() : osr_threshold_(100) {}

    void mark_osr_point(ASTNode* node, int offset);
    bool can_osr(ASTNode* node, int iteration_count);
    void* get_optimized_entry(ASTNode* node);

    void perform_osr(Context* ctx, ASTNode* node);

private:
    void save_interpreter_state(Context* ctx);
    void restore_optimized_state(Context* ctx);
};

}
