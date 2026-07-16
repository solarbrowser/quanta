/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_BYTECODE_H
#define QUANTA_VM_BYTECODE_H

#include "quanta/core/runtime/Value.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Quanta {

class Visitor;
class Shape;
class ASTNode;
class Environment;

// Register-based, accumulator-centric instruction set (V8 Ignition model).
// Encoding: u8 opcode + fixed operands -- r: u8 register, k: u16 constant-
// pool index, o: i16 jump offset relative to the pc after the instruction.
enum class Op : uint8_t {
    LdaConst,     // k
    LdaZero,
    LdaSmi,       // i8
    LdaUndefined,
    LdaNull,
    LdaTrue,
    LdaFalse,

    LdaThis,      // acc = this (resolved once per frame, cached)
    Ldar,         // r
    Star,         // r
    Mov,          // r r (dst, src)

    LdaTdz,       // seeds a register's TDZ; LdarChecked/StarChecked throw if hit
    LdarChecked,  // r n
    StarChecked,  // r n

    Add, Sub, Mul, Div, Mod, Exp,
    BitAnd, BitOr, BitXor, Shl, Shr, Sar,

    TestEq, TestNe, TestStrictEq, TestStrictNe,
    TestLt, TestGt, TestLe, TestGe,
    TestInstanceOf, TestIn,

    Neg, LogicalNot, BitNot, TypeOf,
    ToNumber,     // throws on BigInt
    ToNumeric,    // BigInt passes through
    Inc,
    ToTemplateString, // template interpolation's own stringify
    ToPropertyKey, // acc = ToPropertyKey(acc): once, before the RHS runs
                   // (computed member writes -- toString observably one call)
    CheckObjectCoercible, // throws TypeError if acc is null/undefined; acc
                          // unchanged otherwise (RequireObjectCoercible)
    Dec,

    LdaLookup,    // n -- chain walk (globals/closures, non-env_mode)
    LdaLookupTypeof, // n -- like LdaLookup, but an unresolved name yields undefined instead of throwing
    StaLookup,    // n -- chain-walk write: TDZ/const checks, sloppy-mode global fallback
    CheckLookupResolvable, // n -- acc = bool: does `name` resolve right now (checked pre-RHS)
    StaLookupChecked,     // r_resolved n -- StaLookup honoring a pre-RHS CheckLookupResolvable verdict
    LdaEnv,       // n -- env_mode chain walk
    StaEnv,       // n
    StaEnvInit,   // n -- current environment only, no chain walk

    BindEnvLocals,  // create env_locals bindings (deferred past parameter
                    // resolution when env_params_tdz -- see BytecodeChunk)

    EnterLoopEnv,   // k -- push a per-iteration Environment
    AdvanceLoopEnv, // k -- fresh sibling; copy_forward names carried over
    ExitLoopEnv,

    SaveEnv,      // push current environment (try entry)
    RestoreEnv,   // pop + restore (catch/finally entry)
    PopEnvSave,   // pop, discard (non-exceptional exit)

    GetIterator,        // r_next_fn
    IteratorNextOrJump, // r_iter r_next_fn o
    IteratorClose,      // r_iter mode (0=validate, 1=re-raise pending)
    CreateForInKeys,

    JumpIfNotNullish, // o
    JumpIfNullish,    // o
    JumpIfNotUndefined, // o -- default-parameter check (spec: explicit undefined too, not just omitted)

    CreateClosure,   // k -- runs the closure's own tree-walker evaluate()
    DestructureBind, // k
    CreateRestArray, // r -- acc = Array of args[r..argc), for a `...rest` parameter

    Call,         // r_callee r_args_start argc n
    CallResolved, // r_func r_this r_args_start argc n -- func already resolved (spec: before args)
    Construct,    // r_callee r_args_start argc n -- new.target = callee, calls Function::construct

    GetNamed,     // r_obj n fb
    SetNamed,     // r_obj n fb
    GetPrivate,   // r_obj n fb -- literal `.#name`: brand check + qualified-slot access
    SetPrivate,   // r_obj n fb
    GetKeyed,     // r_obj
    SetKeyed,     // r_obj r_key
    DeleteNamed,  // r_obj n -- acc = delete r_obj.name
    DeleteKeyed,  // r_obj -- key in acc; acc = delete r_obj[key]
    DefineOwn,    // r_obj n -- literal property: CreateDataProperty, never a
                  // proto setter / inherited read-only check
    DefineElement, // r_obj r_key -- array literal element (set_element)

    CreateObject, // n
    CreateArray,  // n

    Jump,         // o
    JumpIfTrue,   // o
    JumpIfFalse,  // o
    Return,
    Throw,
    ReraiseGeneratorReturn, // acc holds the .return() value -- a finally-only
                            // landing pad's tail: re-throw as a C++
                            // GeneratorReturnException once finally is done.

    kCount
};

struct SourceEntry {
    uint32_t pc;
    uint32_t line;
    uint32_t column;
};

// Inline-cache slot for one GetNamed/SetNamed site: mono -> poly (up to
// kMaxEntries distinct shapes) -> mega. count==0 is Uninit, count==1 behaves
// exactly like the old monomorphic-only cache (a length-1 scan), count>1 is
// the polymorphic case -- all three share the same scan/learn code path.
// Once `mega` is set the site is permanently uncached (same as fb==nullptr).
struct FeedbackSlot {
    struct Entry { Shape* shape = nullptr; uint32_t slot_index = 0; };
    static constexpr uint8_t kMaxEntries = 4;
    std::array<Entry, kMaxEntries> entries{};
    uint8_t count = 0;
    bool mega = false;

    // SetNamed-only: caches adding a brand-new own property (a shape
    // transition), keyed by the shape BEFORE the add. `proto_epoch` is
    // Object::proto_epoch() when last validated blocker-free -- only
    // trusted while it still matches. GetNamed sites carry these fields
    // too but never touch them.
    struct TransitionEntry {
        Shape* from_shape = nullptr;
        Shape* to_shape = nullptr;
        uint32_t slot_index = 0;
        uint64_t proto_epoch = 0;
    };
    std::array<TransitionEntry, kMaxEntries> transitions{};
    uint8_t transition_count = 0;
    bool transition_mega = false;
};

// Inline cache for one GetPrivate/SetPrivate site: the resolved qualified
// key ("#x@<brand>"). Site-constant -- a chunk belongs to one Function,
// which belongs to one class evaluation -- so once resolved, the per-access
// brand walk (CallStack scan + key concatenation) is gone; presence of the
// qualified slot on the receiver IS the brand check.
struct PrivateFeedback {
    std::string qualified;  // empty until the slow path resolves a data field
};

// One try region: [start_pc, end_pc) -> handler_pc.
struct HandlerEntry {
    uint32_t start_pc;
    uint32_t end_pc;
    uint32_t handler_pc;
    // A generator .return() mid-suspend unwinds as a C++ exception, not a
    // catchable JS value, and must skip any catch clause -- so it needs its
    // own landing pad (finally-only) instead of reusing handler_pc. -1 when
    // this region has no suspend point in it (not a suspendable chunk, or no
    // finally to run).
    int32_t genreturn_pc = -1;
};

// One per compiled function body, owned by its Function, shared by every call.
struct BytecodeChunk {
    std::vector<uint8_t> code;
    std::vector<Value> constants;   // GC-visible via Function::trace()
    std::vector<std::string> names; // identifier names for LdaLookup/Call diagnostics
    std::vector<SourceEntry> positions;
    mutable std::vector<FeedbackSlot> feedback; // written as call sites warm up
    mutable std::vector<PrivateFeedback> private_feedback; // GetPrivate/SetPrivate sites
    // Per-name outer-variable cache for LdaLookup/StaLookup: a captured
    // chain is fixed per Function instance (and the chunk belongs to one),
    // so a resolved stable binding pointer stays valid for the chunk's
    // lifetime. See Environment::stable_binding_slot for the guards.
    struct LookupCacheEntry { Environment* env = nullptr; Value* slot = nullptr; };
    mutable std::vector<LookupCacheEntry> lookup_cache; // indexed by name id
    uint16_t register_count = 0;
    uint8_t parameter_count = 0;    // params occupy regs[0..parameter_count)

    // env_mode: every local lives in ctx.get_lexical_environment() instead of
    // a register. env_params/env_locals seed function entry, via VM::run.
    bool env_mode = false;
    // Parameter lists with initializers get spec FunctionDeclarationInstantiation
    // ordering: params seeded uninitialized (TDZ), initialized left-to-right by
    // bytecode, and env_locals bound only afterwards (Op::BindEnvLocals), so a
    // default expression can't see a later parameter or a body-level binding.
    bool env_params_tdz = false;
    // Top-level script chunk: the frame's lexical env is the PERSISTENT
    // script env (not per-call), so the lookup cache may point into it.
    bool script_mode = false;
    std::vector<std::string> env_params;
    struct EnvLocal { std::string name; bool is_lexical; bool is_const; };
    std::vector<EnvLocal> env_locals;

    // Function::call materializes the real arguments object before VM::run
    // (skipped otherwise -- it dominated call-heavy benchmarks).
    bool needs_arguments = false;

    std::vector<const ASTNode*> closures; // raw pointers into the owning Function's own body_

    struct DestructuringSite { const ASTNode* pattern; bool as_lexical; bool is_const; };
    std::vector<DestructuringSite> destructuring_patterns;

    std::vector<HandlerEntry> handlers;

    // Per-iteration Environment locals for one loop/block (Op::EnterLoopEnv).
    // copy_forward: a `for` header's own let/const carries across iterations;
    // everything else starts fresh each time.
    struct LoopEnvVar { std::string name; bool is_lexical; bool is_const; bool copy_forward; };
    std::vector<std::vector<LoopEnvVar>> loop_envs;

    void trace(Visitor& v) const;
};

// Human-readable dump for QUANTA_VM_DISASM=1.
std::string disassemble_chunk(const BytecodeChunk& chunk, const std::string& name);

}

#endif
