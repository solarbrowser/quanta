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
    // delegated to the tree-walker, which suspends the surrounding fiber). A
    // try/finally wrapping one gets its own generator-return landing pad
    // (Op::ReraiseGeneratorReturn) so a mid-suspend .return() still runs
    // finally instead of skipping it.
    static std::unique_ptr<BytecodeChunk> compile(
        const ASTNode* body, const std::vector<std::unique_ptr<Parameter>>& params,
        bool suspendable = false);

    // Script tier: compiles a Program's top-level statements. All hoisting
    // (vars on the global, the script lexical env with its TDZ bindings,
    // function declarations) has already run -- every top-level name is a
    // pre-existing outer binding reached via LdaLookup/StaLookup, whose
    // lookup cache works at full strength because the script env is
    // persistent. Nested lexicals get the same register treatment as in
    // function bodies. Null = tree-walk the statements instead.
    static std::unique_ptr<BytecodeChunk> compile_script(
        const std::vector<std::unique_ptr<ASTNode>>& statements);

    // The same `arguments` scan compile() runs over a body, exposed for
    // callers checking a parameter default/destructuring pattern directly
    // (a suspendable body always compiles with an empty param list -- see
    // VM::compile_suspendable -- so compile()'s own scan never sees those).
    static bool references_arguments(const ASTNode* node);

    // Conservative "could this subtree ever read identifier `name`" check
    // (true on eval/nested-class/unknown, same opacity rules as above).
    static bool references_identifier(const ASTNode* node, const std::string& name);

private:
    // env_resident: selective env_mode -- only these names live in the
    // Environment, everything else gets a register. Null = full env_mode
    // (every local in the env) when env_mode is true.
    BytecodeCompiler(const std::vector<std::string>& param_names, bool env_mode,
                     const std::unordered_set<std::string>* env_resident = nullptr);

    bool full_env_ = true;  // env_mode_ && !full_env_ = selective storage
    std::unordered_set<std::string> env_resident_;

    bool compile_statement(const ASTNode* node);
    bool compile_expression(const ASTNode* node);  // result in accumulator

    bool compile_for_each_loop(const ASTNode* left, const ASTNode* right,
                               const ASTNode* body, bool is_for_in,
                               int left_decl_kind = -1);
    bool compile_logical_assignment(const class AssignmentExpression* expr);

    bool is_local(const std::string& name) const;
    int lookup_local(const std::string& name) const;
    bool declare_local(const std::string& name);
    int alloc_temp();
    void free_temp(int reg);

    void emit_read_local(const std::string& name);
    void emit_write_local(const std::string& name, bool is_declaration);

    // Op::LdaEnvSlot/StaEnvSlot/StaEnvSlotInit eligibility: recorded only for
    // a name declared EXACTLY ONCE in the whole function (global_decl_count_
    // == 1 -- see compile()), at the point its owning scope's binding list
    // (env_locals/env_params, or one loop_envs[i]) is finalized. `slot` is a
    // best-effort predicted position (<4) within that scope's first-4
    // bindings -- Function::call() can insert extra bindings (self-name
    // recursion, class __closure_* self-reference, __super__, arguments)
    // into the SAME environment before env_locals/env_params seeding runs,
    // which this compiler cannot see, so the predicted position can be
    // wrong. That's why Interpreter.cpp's handlers re-validate by name
    // before trusting it (Environment::inline_slot) -- a wrong prediction
    // only costs the fast path, never correctness. `depth` is the
    // env_depth_ value that will be active while this declaration's scope
    // is the current one; emit_read_local/emit_write_local only take the
    // slot path when the access site's CURRENT env_depth_ matches, which is
    // exactly when ctx.get_lexical_environment() is guaranteed (by the
    // LIFO-balanced EnterLoopEnv/ExitLoopEnv nesting) to be that scope's
    // Environment -- no chain walk needed to find it.
    struct EnvSlotInfo { uint8_t slot; int depth; };
    std::unordered_map<std::string, int> global_decl_count_;
    std::unordered_map<std::string, EnvSlotInfo> env_slot_info_;
    // Names declared more than once (global_decl_count_ > 1) whose every
    // declaring region is still provably safe to slot-index, because all of
    // them are pairwise disjoint (siblings -- neither nested in the other),
    // e.g. two separate top-level `for (let i...)` loops reusing "i". See
    // compute_sibling_safe_names's doc comment for why disjointness alone
    // (without also re-proving escape-safety) is sufficient here.
    std::unordered_set<std::string> sibling_safe_names_;
    // Shared by every loop_envs.push_back call site (setup_loop_env, plain
    // blocks, catch clauses, switch): records slot info for each var at its
    // position in THIS scope's list, capped at the first 4, only for names
    // globally_decl_count_ == 1 (or sibling_safe_names_). Call with `depth` =
    // the env_depth_ value that will be active once this scope's
    // EnterLoopEnv has run (i.e. after the caller's env_depth_++).
    void record_env_slot_info(const std::vector<BytecodeChunk::LoopEnvVar>& vars, int depth);

    void emit(Op op);
    void emit_u8(uint8_t v);
    void emit_u16(uint16_t v);
    uint16_t add_constant(const Value& v);
    uint16_t add_name(const std::string& name);
    uint16_t alloc_feedback_slot();
    uint16_t alloc_private_feedback();
    uint16_t alloc_keyed_feedback();

    bool member_is_supported(const class MemberExpression* mem) const;
    bool emit_treewalker_delegate(const ASTNode* node);

    int setup_loop_env(std::vector<BytecodeChunk::LoopEnvVar> extra_vars, const ASTNode* body,
                       bool force_own_env = false,
                       const std::vector<const ASTNode*>& extra_capture_roots = {});
    // Parallel to chunk_->loop_envs (same index): whether AdvanceLoopEnv's
    // per-iteration fresh-Environment dance is actually needed for that
    // scope, per loop_vars_may_be_captured's closure-capture proof -- see
    // its doc comment. false means every AdvanceLoopEnv emission site for
    // this scope should be skipped (mutate the one binding in place).
    std::vector<bool> loop_env_needs_fresh_;
    bool loop_env_needs_fresh(int idx) const { return loop_env_needs_fresh_[static_cast<size_t>(idx)]; }

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
        int iterator_reg = -1;  // for-of/for-in only: IteratorClose target for an escaping return
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
    bool script_mode_ = false;  // top-level Program chunk, see compile_script()
    int try_env_depth_ = 0;
    int env_depth_ = 0;
    std::vector<size_t>* chain_shortcircuit_jumps_ = nullptr;
    bool failed_ = false;
};

// Whether a closure literal (params + body) needs its captured environment
// kept alive at all -- see BytecodeCompiler.cpp's collect_free_names doc
// comment for the full contract. Used at closure-creation sites (see
// FunctionExpression::evaluate) to decide whether pinning the enclosing
// scope's Environment (Function::capture_closure_environment's mark_escaped)
// is actually necessary, instead of doing it unconditionally for every
// closure regardless of whether anything inside it can ever observe it.
bool closure_needs_outer_environment(const std::vector<std::unique_ptr<Parameter>>& params,
                                      const ASTNode* body, bool is_arrow);

}

#endif
