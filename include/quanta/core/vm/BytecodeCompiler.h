/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_BYTECODE_COMPILER_H
#define QUANTA_VM_BYTECODE_COMPILER_H

#include "quanta/core/vm/Bytecode.h"
#include "quanta/parser/ScriptUnit.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
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
    // env_bound: names the caller has already bound in the environment this
    // chunk will run in. A suspendable body is compiled with no parameter list
    // (its parameters live in the Context, not in registers), so a `var` that
    // repeats a parameter's name would otherwise be given a register nothing
    // ever fills.
    static std::unique_ptr<BytecodeChunk> compile(
        const ASTNode* body, const std::vector<std::unique_ptr<Parameter>>& params,
        bool suspendable = false,
                                                 bool is_arrow = false,
                                                 bool is_strict = false,
                                                 const std::vector<std::string>* env_bound = nullptr,
                                                 bool outer_with = false,
                                                 bool allow_arguments = false,
                                                 const BodyScopeInfo* scope_info = nullptr);

    // Script tier: compiles a Program's top-level statements. All hoisting
    // (vars on the global, the script lexical env with its TDZ bindings,
    // function declarations) has already run -- every top-level name is a
    // pre-existing outer binding reached via LdaLookup/StaLookup, whose
    // lookup cache works at full strength because the script env is
    // persistent. Nested lexicals get the same register treatment as in
    // function bodies. Null = tree-walk the statements instead.
    // `track_completion`: the chunk answers with the script's completion value
    // rather than undefined, which is what `eval` is specified to return.
    static std::unique_ptr<BytecodeChunk> compile_script(
        const std::vector<std::unique_ptr<ASTNode>>& statements, bool outer_with = false,
        bool track_completion = false, bool suspendable = false);

    // A module body compiled for the suspendable convention, so a top-level
    // await suspends the fiber the module runs on instead of draining the
    // microtask queue where it stands. Null when the body has no suspension
    // point at all, or when it does not compile.
    // Whether the body has a suspension point of its own, which is what makes
    // a module async: everything waiting on it has to wait for a turn of the
    // microtask queue even if it never actually blocks.
    static bool module_body_suspends(const std::vector<std::unique_ptr<ASTNode>>& statements);

    static std::unique_ptr<BytecodeChunk> compile_module_body(
        const std::vector<std::unique_ptr<ASTNode>>& statements, bool outer_with = false);

    // A parameter's destructuring pattern, compiled on its own. A suspendable
    // function binds its parameters outside the chunk that owns its body, so
    // the pattern is a chunk of its own; it expects the value to bind in the
    // accumulator and creates the bindings in the environment it runs in.
    static std::unique_ptr<BytecodeChunk> compile_pattern_binder(const ASTNode* pattern);

    // A parameter's default expression, on its own. Same reason as the binder
    // above, and the same access to `arguments`, which exists by then.
    static std::unique_ptr<BytecodeChunk> compile_default_value(const ASTNode* expr,
                                                                bool suspendable);

    // The same `arguments` scan compile() runs over a body, exposed for
    // callers checking a parameter default/destructuring pattern directly
    // (a suspendable body always compiles with an empty param list -- see
    // VM::compile_suspendable -- so compile()'s own scan never sees those).
    static bool references_arguments(const ASTNode* node);

    // Whether an expression contains a yield/await that must suspend whatever
    // fiber it is evaluated on (a computed class key is the one place this is
    // asked from outside).
    static bool expression_suspends(const ASTNode* node);

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
    // `discard`: the caller will not read the accumulator afterwards. Only the
    // forms that do extra work purely to produce a value act on it (postfix
    // ++/-- keeps the old one around); everything else ignores it, and no
    // recursive call inherits it -- a subexpression's value is always needed.
    bool compile_expression(const ASTNode* node, bool discard = false);  // result in accumulator
    static bool operand_cannot_write_registers(const ASTNode* node);

    bool compile_for_each_loop(const ASTNode* left, const ASTNode* right,
                               const ASTNode* body, bool is_for_in,
                               int left_decl_kind = -1,
                               bool is_await = false);
    bool compile_logical_assignment(const class AssignmentExpression* expr);

    bool is_local(const std::string& name) const;
    bool needs_with_target_resolve(const ASTNode* target, bool is_lexical) const;
    bool compile_if_branch(const ASTNode* branch);
    bool lexical_out_of_scope(const std::string& name) const;
    void emit_completion_reset();
    void emit_completion_store();
    int lookup_local(const std::string& name) const;
    // The register a name lives in when reading it is a bare Ldar: not
    // env-resident, actually register-allocated here, and past its TDZ. -1
    // when any of those fails, because then the read is an opcode of its own
    // and its register number cannot stand in for it.
    int plain_local_register(const std::string& name) const;
    // Whether evaluating this expression is guaranteed to leave every local
    // register alone and to not throw. Deliberately a whitelist: it decides
    // whether a compound assignment may evaluate its right side first.
    bool leaves_locals_untouched(const ASTNode* expr) const;
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

    // Rewrites every "produce into the accumulator, then park it in a
    // register" pair into the single instruction that does both. Run once on
    // the finished body rather than at emit time, so the fifty-odd places
    // that emit a Star stay unaware of it and one place has to know that an
    // instruction somebody jumps to cannot be swallowed by the one before it.
    void fuse_store_pairs();
    void emit(Op op);
    void emit_u8(uint8_t v);
    void emit_u16(uint16_t v);
    uint32_t add_constant(const Value& v);
    void emit_load_const(const Value& v);
    void emit_u32(uint32_t v);
    uint16_t add_name(const std::string& name);
    uint16_t alloc_feedback_slot();
    uint16_t alloc_private_feedback();
    uint16_t alloc_keyed_feedback();

    // Emits a destructuring pattern. Consumes the source value from the
    // accumulator. pattern_is_emittable decides first, so a shape the emitter
    // cannot express costs no half-written bytecode.
    // `is_assignment` switches the same emitter from a declaration to the
    // assignment form (`[a, b] = [b, a]`), where a target need not be a name
    // this chunk owns and the write is not an initialisation.
    bool emit_pattern_bind(const ASTNode* pattern, bool is_lexical, bool is_const,
                           bool is_assignment = false);
    bool pattern_is_emittable(const ASTNode* pattern, bool is_lexical,
                              bool is_assignment = false) const;
    bool emit_array_pattern_bind(const ASTNode* pattern, bool is_lexical, bool is_const,
                                 bool is_assignment = false);
    int emit_with_target_resolve(const ASTNode* target, bool is_lexical);
    // A member target's reference is evaluated before the source is read
    // (spec: lRef ahead of GetV / of the iterator step), so its object and
    // computed key are parked in registers at that point and the store below
    // reuses them. Returns the object register, or -1 when the target is not
    // a member this path writes through.
    int emit_member_target_resolve(const ASTNode* target, bool is_assignment, int& key_reg);
    bool emit_pattern_target_store(const ASTNode* target, bool is_lexical, bool is_const,
                                   bool is_assignment,
                                   int with_target_reg = -1, int member_obj_reg = -1, int member_key_reg = -1);
    bool emit_pattern_assign(const ASTNode* pattern, const ASTNode* source);
    bool pattern_target_is_writable(const std::string& name) const;
    bool emit_tagged_template_args(const class CallExpression* call, int& args_start, uint8_t& argc);

    bool member_is_supported(const class MemberExpression* mem) const;
    static bool member_is_super(const class MemberExpression* mem);
    bool super_member_emittable(const class MemberExpression* mem) const;
    bool emit_super_load(const class MemberExpression* mem);
    bool deleg_at(int line, const ASTNode* node);

    // Builds a fresh Array from `elements`, expanding any SpreadElement
    // through the iterator protocol, and leaves it in the returned temp
    // register (caller frees it). Used by array literals and by the
    // spread forms of call/new, which all need the same "flatten an element
    // list whose length is only known at runtime" primitive. Returns -1 if
    // the list cannot be compiled; holes are rejected alongside spread
    // (a trailing hole's contribution to `length` cannot be expressed once
    // the index is dynamic).
    int emit_spread_array(const std::vector<std::unique_ptr<ASTNode>>& elements);

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
        bool iterator_is_async = false;  // for-await-of: closing it Awaits, so a different opcode
    };

    // A `return` inside a try/finally cannot just return: the finally has to
    // run first. The escape parks its value and jumps to a pad emitted after
    // the try statement, which is where it must live -- inside the try's own
    // handler range, a throw from the finally copy would reach that try's
    // catch instead of leaving.
    struct FinallyScope {
        const ASTNode* finally_node = nullptr;
        // What the pads run before letting the completion travel on: a
        // statement for `try/finally`, one instruction for a `using` block, and
        // nothing at all for `with`, whose whole cleanup is the RestoreEnv the
        // pads already emit.
        enum class Cleanup { Finally, Dispose, RestoreOnly };
        Cleanup cleanup = Cleanup::Finally;
        bool save_env = false;
        int value_reg = -1;
        // loop_stack_ depth when the try was entered: a break or continue whose
        // target sits at or above this never leaves the try, so it needs no pad.
        size_t loop_depth = 0;
        std::vector<size_t> return_jumps;
        // One pad per distinct target, since each performs a different jump.
        struct Escape { bool is_continue; std::string label; std::vector<size_t> sites; };
        std::vector<Escape> escapes;
    };
    std::vector<FinallyScope> finally_stack_;
    // Nonzero while emitting a block that opened a dispose scope. A `using`
    // outside one has nothing to register into, which is the function body's
    // own top level -- that scope belongs to the call, not to a block.
    int dispose_scope_depth_ = 0;
    // Nonzero inside a `with` body. An assignment there has to bind its target
    // before the right side runs, which the ordinary write-time resolution
    // cannot express.
    int with_depth_ = 0;
    // A `with` sits in the chain this closure captured, outside the body being
    // compiled. It cannot shadow a local, but it can hold any free name.
    bool outer_with_ = false;
    // Annex B if/label function declarations that did get a var binding.
    std::unordered_set<std::string> annexb_fn_vars_;
    bool emit_return_completion(bool has_argument, bool already_awaited);
    bool emit_loop_escape(bool is_continue, const std::string& label);
    void emit_iterator_closes_above(size_t from);
    bool emit_dispose_scope_body(const ASTNode* suspend_scope,
                                 const std::function<bool()>& emit_body,
                                 FinallyScope& escaped);
    bool emit_finally_body(const FinallyScope& scope);
    bool emit_finally_pads(FinallyScope& scope);

    std::vector<std::string> take_pending_labels();

    std::unique_ptr<BytecodeChunk> chunk_;
    // Builder-side storage for chunk_'s 4 always-populated fields: these grow
    // via push_back/random-access-mutate throughout compilation (jump
    // backpatching writes code_[pos] after the fact, etc.), which
    // BytecodeChunk's own FixedArray<T> fields can't support (fixed length
    // once frozen). Moved into chunk_->code/constants/names/feedback via
    // FixedArray<T>::from() exactly once, at the very end of compile()/
    // compile_script() -- see those functions' final lines.
    std::vector<uint8_t> code_;
    std::vector<Value> constants_;
    std::vector<std::string> names_;
    // Where each name already sits, so add_name does not compare against every
    // name already in the table. The table itself stays a vector because the
    // chunk indexes into it; this only exists while the chunk is being built.
    std::unordered_map<std::string, uint16_t> name_index_;
    std::vector<FeedbackSlot> feedback_;
    std::unordered_map<std::string, int> locals_;
    // Names declared `const` in this chunk. declare_local() only takes a name,
    // so constness was dropped and a keywordless for-of/for-in target compiled
    // to a bare Star that overwrote the binding without a word. Recorded here
    // so compile_for_each_loop can decline those and let the tree-walker,
    // which raises properly, take them.
    std::unordered_set<std::string> const_locals_;
    std::unordered_set<int> lexical_registers_;
    // Lexical registers whose declaration has provably run by every point that
    // reads them, so the dead-zone check is dead code. A register joins when
    // its declaration is emitted, which leaves uses compiled before that point
    // -- the ones that can still be in the dead zone -- checked. Suppressed
    // inside a switch, whose control flow enters the middle of a scope.
    std::unordered_set<int> initialized_lexicals_;
    int switch_body_depth_ = 0;
    std::unordered_set<std::string> env_names_;
    bool env_mode_ = false;
    int next_register_ = 0;
    int temp_watermark_ = 0;
    std::vector<LoopScope> loop_stack_;
    std::vector<std::string> pending_labels_;  // set by LABELED_STATEMENT, taken by the next loop/switch
    std::unordered_set<const ASTNode*> hoisted_fn_decls_;  // top-level fn decls bound by compile()'s prologue
    bool allow_arguments_ = false;
    // A direct eval here can add a binding between an assignment's reference
    // and its store, so those resolve ahead into a parked environment.
    bool eval_in_body_ = false;
    uint8_t resolved_env_slots_ = 0;  // `arguments` reads compile to LdaLookup (chunk needs_arguments set)
    bool suspendable_ = false;  // generator/async body, see compile()'s parameter
    bool script_mode_ = false;  // top-level Program chunk, see compile_script()
    // Where the script's completion value lives while the statements run, or
    // -1 when the caller does not ask for one. A statement that completes with
    // a value writes it; the constructs the spec makes UpdateEmpty(_, undefined)
    // clear it first (see emit_completion_reset).
    int completion_reg_ = -1;
    // The block-scoped names currently open, innermost last. Locals are
    // allocated for the whole chunk, so this is the only record of where a
    // `let` stops being visible -- which is what makes `typeof x` after its
    // block answer "undefined" rather than reading a register that is out of
    // scope.
    std::vector<std::vector<std::string>> lexical_scopes_;
    // Every name that has ever been pushed onto lexical_scopes_. A name absent
    // from it was never block-scoped here, so nothing can be said about it.
    std::unordered_set<std::string> block_scoped_names_;
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
