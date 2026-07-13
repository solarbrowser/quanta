/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_BYTECODE_COMPILER_H
#define QUANTA_VM_BYTECODE_COMPILER_H

#include "quanta/core/vm/Bytecode.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Quanta {

class ASTNode;
class Parameter;

// Single-pass AST -> bytecode compiler. Returns nullptr for any function it
// cannot fully compile -- that function then permanently runs on the
// tree-walker (no mixed execution).
class BytecodeCompiler {
public:
    // `suspendable`: generator/async body -- forces env_mode (yield/await are
    // delegated to the tree-walker, which suspends the surrounding fiber) and
    // rejects bodies with yield/await inside try (a C++ GeneratorReturnException
    // from return() would skip VM finally blocks).
    static std::unique_ptr<BytecodeChunk> compile(
        const ASTNode* body, const std::vector<std::unique_ptr<Parameter>>& params,
        bool suspendable = false);

private:
    BytecodeCompiler(const std::vector<std::string>& param_names, bool env_mode);

    bool compile_statement(const ASTNode* node);
    bool compile_expression(const ASTNode* node);  // result in accumulator

    bool compile_for_each_loop(const ASTNode* left, const ASTNode* right,
                               const ASTNode* body, bool is_for_in);

    bool is_local(const std::string& name) const;
    int lookup_local(const std::string& name) const;
    bool declare_local(const std::string& name);
    int alloc_temp();
    void free_temp(int reg);

    void emit_read_local(const std::string& name);
    void emit_write_local(const std::string& name, bool is_declaration);

    void emit(Op op);
    void emit_u8(uint8_t v);
    void emit_u16(uint16_t v);
    uint16_t add_constant(const Value& v);
    uint16_t add_name(const std::string& name);
    uint16_t alloc_feedback_slot();

    bool member_is_supported(const class MemberExpression* mem) const;
    bool emit_treewalker_delegate(const ASTNode* node);

    int setup_loop_env(std::vector<BytecodeChunk::LoopEnvVar> extra_vars, const ASTNode* body);

    size_t emit_jump(Op op);
    bool patch_jump(size_t operand_pos);
    bool emit_jump_back(Op op, size_t target_pc);

    struct LoopScope {
        size_t continue_target;
        std::vector<size_t> break_patches;
        std::vector<size_t> continue_patches;
        bool continue_is_forward;
        int base_env_depth;  // env_depth_ at loop entry, see BREAK/CONTINUE_STATEMENT
        int base_try_depth;  // try_env_depth_ at loop entry
        bool is_switch = false;  // break-only: continue skips past this to the enclosing loop
        std::vector<std::string> labels;  // labels a labeled break/continue can target this by
    };

    std::vector<std::string> take_pending_labels();

    std::unique_ptr<BytecodeChunk> chunk_;
    std::unordered_map<std::string, int> locals_;
    std::unordered_set<int> lexical_registers_;
    std::unordered_set<std::string> env_names_;
    bool env_mode_ = false;
    int next_register_ = 0;
    int temp_watermark_ = 0;
    std::vector<LoopScope> loop_stack_;
    std::vector<std::string> pending_labels_;  // set by LABELED_STATEMENT, taken by the next loop/switch
    std::unordered_set<const ASTNode*> hoisted_fn_decls_;  // top-level fn decls bound by compile()'s prologue
    bool allow_arguments_ = false;  // `arguments` reads compile to LdaLookup (chunk needs_arguments set)
    bool suspendable_ = false;  // generator/async body, see compile()'s parameter
    int try_env_depth_ = 0;
    int env_depth_ = 0;
    std::vector<size_t>* chain_shortcircuit_jumps_ = nullptr;
    bool failed_ = false;
};

}

#endif
