/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_BYTECODE_H
#define QUANTA_VM_BYTECODE_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/vm/FixedArray.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Quanta {

class Visitor;
class Shape;
class ASTNode;
class Environment;
class Object;
struct ClosureTemplate;

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

    // Guarded direct-slot variants of LdaEnv/StaEnv/StaEnvInit: read/write
    // slot s of the current Environment's inline SlotMap directly, falling
    // back to the name-walk path on a guard miss (see BytecodeCompiler.h's
    // EnvSlotInfo and Environment::inline_slot). n is kept on all three:
    // needed for the guard compare and as the fallback's own operand.
    LdaEnvSlot,     // s n
    StaEnvSlot,     // s n
    StaEnvSlotInit, // s n

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

    CreateClosure,   // k -- instantiates a function literal (index into
                     // BytecodeChunk::closures); still runs the literal's own
                     // tree-walker evaluate() for now
    DeclareFunction, // k -- instantiates a hoisted function declaration from
                     // closures[k] AND binds its name (the binding target
                     // depends on the environment shape, not on the literal)
    EvalAst,         // k -- escape hatch: evaluates an arbitrary AST subtree
                     // (index into BytecodeChunk::treewalk_nodes) in the
                     // tree-walker. Every use is a construct the compiler
                     // cannot emit yet, so the remaining uses measure how far
                     // the VM still depends on the enclosing function's AST.
    // r_src r_keys -- acc = a fresh object with every own enumerable property
    // of r_src whose key is not in the r_keys array (an object pattern's
    // `...rest`).
    CopyRestProperties, // r_src r_keys
    CreateRestArray, // r -- acc = Array of args[r..argc), for a `...rest` parameter

    Call,         // r_callee r_args_start argc n
    CallResolved, // r_func r_this r_args_start argc n -- func already resolved (spec: before args)
    Construct,    // r_callee r_args_start argc n -- new.target = callee, calls Function::construct
    // Spread forms: argument count is only known at runtime, so the operand
    // is a spread SOURCE (the original iterable when the whole list is one
    // spread, otherwise an Array built by emit_spread_array).
    CallSpread,      // r_func r_this r_args_src n
    ConstructSpread, // r_callee r_args_src n
    // Appends everything acc spreads to onto the array in r_arr from index
    // r_idx, writing the next free index back. Expands in C++ rather than as
    // a bytecode loop so a plain Array skips the per-element iterator
    // protocol entirely.
    SpreadInto,      // r_arr r_idx
    HasPrivate,      // n -- acc = (#name in acc), the ergonomic brand check

    // A plain read can resolve its base inline. Nothing else can: 13.3.7.1 runs
    // GetSuperBase before the key expression, and PutValue runs it before the RHS,
    // so those park the base in a register first and read it back from there.
    GetSuper,        // n   -- acc = super.name
    ResolveSuperBase,// r   -- r = GetSuperBase() (undefined when there is none)
    SetSuper,        // r_base n     -- super.name = acc
    GetSuperKeyed,   // r_base       -- acc = super[acc]
    SetSuperKeyed,   // r_base r_key -- super[r_key] = acc
    SuperCall,       // r_args_start argc -- acc = the bound `this`

    GetNamed,     // r_obj n fb
    SetNamed,     // r_obj n fb
    GetPrivate,   // r_obj n fb -- literal `.#name`: brand check + qualified-slot access
    SetPrivate,   // r_obj n fb
    GetKeyed,     // r_obj
    SetKeyed,     // r_obj r_key
    DeleteNamed,  // r_obj n -- acc = delete r_obj.name
    DeleteKeyed,  // r_obj -- key in acc; acc = delete r_obj[key]
    DefineOwn,    // r_obj n fb -- literal property: CreateDataProperty, never
                  // a proto setter / inherited read-only check; fb is a
                  // shape-transition cache (see FeedbackSlot::TransitionEntry),
                  // safe here only because n is compile-time-constant
    DefineElement, // r_obj r_key -- array literal element (set_element)

    // Object-literal computed-key support (distinct from the generic
    // ToPropertyKey/SetKeyed opcodes above, which use Value::to_property_key()
    // -- a subtly different, already-established conversion used by member
    // assignment. Object literals need literal_to_property_key()'s stricter
    // GetMethod semantics for @@toPrimitive, so this gets its own opcode
    // rather than reusing ToPropertyKey).
    ToPropertyKeyStrict, // acc = Value::to_property_key_strict(acc)
    DefineOwnKeyed,      // r_obj r_key -- computed-key literal property:
                         // CreateDataProperty with a register-held key
                         // (mirrors DefineOwn, key already ToPropertyKey'd)

    // Object-literal method/getter/setter install: folds NamedEvaluation +
    // the spec 14.3.9 non-constructor finalize (Method) / prototype-strip
    // (Getter/Setter) + fetch-existing-descriptor-and-merge (Getter/Setter
    // only) + install into one opcode, so the finalize step -- easy to
    // forget, and load-bearing (a Method installed without it stays wrongly
    // constructible via `new`) -- can't be split from installation by a
    // future edit. acc holds the just-CreateClosure'd function.
    FinalizeStaticProperty,   // r_obj key_name_idx display_name_idx kind fb
                              // kind: 0=Method 1=Getter 2=Setter -- fb is a
                              // shape-transition cache, used only for kind==0
                              // (Method); key_name_idx is compile-time-constant
    FinalizeComputedProperty, // r_obj r_key r_raw_key kind
                              // kind: 0=ValueNoName 1=ValueWithName 2=Method
    SetFunctionNameIfUnnamed, // name_idx -- static-key Value property whose
                              // value is an anonymous-function-shaped AST
                              // node (NamedEvaluation step 6); acc holds the
                              // function. Only renames if currently unnamed
                              // (a `function x(){}` value keeps its own name).

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

// Inline-cache slot for one GetNamed/SetNamed site: mono -> poly (up to
// kMaxEntries distinct shapes) -> mega. count==0 is Uninit, count==1 behaves
// exactly like the old monomorphic-only cache (a length-1 scan), count>1 is
// the polymorphic case -- all three share the same scan/learn code path.
// Once `mega` is set the site is permanently uncached (same as fb==nullptr).
struct FeedbackSlot {
    // no_override_epoch: Object::descriptor_epoch() at the moment this
    // entry's receiver was last confirmed to have no descriptors_ override
    // for this site's key. GetNamed/SetNamed's own-property fast path
    // trusts "still no override" -- skipping a real descriptors_ scan --
    // only while the CURRENT epoch still matches; descriptors_ is per-
    // object, not per-shape, so this is what keeps a different instance's
    // later defineProperty from being silently missed by this shape's
    // cached entry.
    struct Entry { Shape* shape = nullptr; uint32_t slot_index = 0; uint64_t no_override_epoch = 0; };
    static constexpr uint8_t kMaxEntries = 8;
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

    // GetNamed-only: caches reading an INHERITED (not own) data property.
    // `prototype` is the receiver's immediate get_prototype() at learn time
    // (Shape alone doesn't encode [[Prototype]] -- two receivers can share
    // a shape while having different prototype chains, e.g. two
    // Object.create(x) results that later add the same keys). `holder` is
    // the ancestor where the property actually lives (possibly further up
    // than `prototype`); `proto_epoch` covers everything structural changing
    // anywhere between receiver and holder. Unlike Shape*, `prototype`/
    // `holder` are real GC cells -- see BytecodeChunk::trace and the
    // write_barrier call in learn_proto.
    struct ProtoEntry {
        Shape* receiver_shape = nullptr;
        Object* prototype = nullptr;
        uint64_t proto_epoch = 0;
        Object* holder = nullptr;
        uint32_t slot_index = 0;
    };
    std::array<ProtoEntry, kMaxEntries> proto_entries{};
    uint8_t proto_count = 0;
    bool proto_mega = false;
};

// Inline cache for one GetPrivate/SetPrivate site: the resolved qualified
// key ("#x@<brand>"). Once resolved, the per-access brand walk (CallStack
// scan + key concatenation) is gone; presence of the qualified slot on the
// receiver IS the brand check -- but that's only sound per class evaluation
// (the qualified key encodes ITS declaring brand), and a chunk can now be
// shared across many Function instances from separate evaluations of the
// same class/function literal (see FunctionExecutable). So this struct is
// only chunk-owned for chunks that are inherently single-instance (the
// top-level script chunk); every other call routes through the calling
// Function's own instance_private_feedback() instead -- see Interpreter.cpp's
// private_feedback_data comment, the same routing as lookup_cache_data/
// instance_lookup_cache().
struct PrivateFeedback {
    std::string qualified;  // empty until the slow path resolves a data field
};

// Inline cache for one GetKeyed/SetKeyed site. FeedbackSlot (GetNamed/
// SetNamed) can cache on shape alone because the property name is a
// compile-time constant for that one bytecode site; a GetKeyed/SetKeyed
// site's key is read from a register and can differ on every execution of
// the SAME instruction, so each entry must validate the key too, not just
// the shape, before trusting a hit. No string interning exists yet in this
// codebase, so `key` is a plain owned std::string (compared by value)
// rather than a cheap pointer identity -- still avoids Shape::find_slot's
// hashtable probe on a hit, but pays up to kMaxEntries string comparisons
// instead of one pointer comparison. Own-property only -- no proto-entry
// equivalent, inherited property reads through a keyed site always take
// the slow path (deliberate: GetNamed's proto-cache wasn't judged worth
// replicating here yet).
struct KeyedFeedback {
    struct Entry { Shape* shape = nullptr; std::string key; uint32_t slot_index = 0; };
    static constexpr uint8_t kMaxEntries = 4;
    std::array<Entry, kMaxEntries> entries{};
    uint8_t count = 0;
    bool mega = false;
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

// One per compiled function body -- owned by a FunctionExecutable and shared
// by every Function instance built from that decl site (see
// FunctionExecutable), not just every call of a single instance.
struct BytecodeChunk {
    // Built via a std::vector builder in BytecodeCompiler, frozen into these
    // FixedArray<T> (pointer+count, no capacity slack) exactly once at the
    // end of compile()/compile_script() -- see BytecodeCompiler.h's code_/
    // constants_/names_/feedback_ doc comment. feedback's element CONTENTS
    // still mutate at runtime (IC warming) even though its LENGTH is frozen;
    // FixedArray::operator[] hands out a non-const T& even through a const
    // BytecodeChunk&, so (unlike the old std::vector) no `mutable` is needed.
    FixedArray<uint8_t> code;
    FixedArray<Value> constants;   // GC-visible via Function::trace()
    FixedArray<std::string> names; // identifier names for LdaLookup/Call diagnostics
    FixedArray<FeedbackSlot> feedback; // written as call sites warm up

    // GetPrivate/SetPrivate and GetKeyed/SetKeyed sites are rare relative to
    // ordinary named property access -- lazily allocated together since both
    // are populated only by alloc_private_feedback()/alloc_keyed_feedback()
    // during compilation, never resized at runtime. unique_ptr::get() hands
    // out a non-const IcFeedback* even through a const BytecodeChunk&
    // (unlike std::vector, pointee constness isn't propagated), so runtime
    // readers need no `mutable` here.
    struct IcFeedback {
        std::vector<PrivateFeedback> private_feedback;
        std::vector<KeyedFeedback> keyed_feedback;
    };
    std::unique_ptr<IcFeedback> ic_feedback;
    IcFeedback& ensure_ic_feedback() { if (!ic_feedback) ic_feedback = std::make_unique<IcFeedback>(); return *ic_feedback; }
    // Per-name outer-variable cache for LdaLookup/StaLookup: a resolved
    // stable binding pointer is only valid for the one captured-environment
    // chain it was resolved against, so this chunk-level vector is only used
    // directly for chunks that are inherently single-instance (the top-level
    // script chunk) -- every other call routes through the calling
    // Function's own instance_lookup_cache() instead (see Interpreter.cpp's
    // lookup_cache_data comment). See Environment::stable_binding_slot for
    // the guards.
    struct LookupCacheEntry { Environment* env = nullptr; Value* slot = nullptr; };
    // Same frozen-length/mutable-contents profile as feedback above -- only
    // ever `= FixedArray<...>::filled(names.size(), ...)` once at compile
    // end (BytecodeCompiler.cpp), individual entries written in place at
    // runtime via a cached .data() pointer (see Interpreter.cpp's
    // lookup_cache_data). No `mutable` needed, same reasoning as feedback.
    FixedArray<LookupCacheEntry> lookup_cache; // indexed by name id
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

    // Function::call materializes the real arguments object before VM::run
    // (skipped otherwise -- it dominated call-heavy benchmarks).
    bool needs_arguments = false;

    // Closures/tree-walk escapes/destructuring/try-catch are each
    // independently rare (a chunk can have any one without the others), so
    // unlike IcFeedback these stay separate lazy pointers rather than one
    // bundle.

    // Function literals only (Op::CreateClosure). Separate from treewalk_nodes
    // because this is the permanent case: a literal always needs some
    // per-decl-site description to instantiate from. Holds prebuilt
    // ClosureTemplates rather than AST pointers, so creating a closure no
    // longer reaches back into the enclosing function's own body_ (V8's
    // CreateClosure reads a SharedFunctionInfo from the constant pool the
    // same way). Out of line: ClosureTemplate is only forward-declared here,
    // since its own header needs BytecodeChunk.
    std::unique_ptr<std::vector<ClosureTemplate>> closures;
    std::vector<ClosureTemplate>& ensure_closures();

    // Everything the compiler cannot emit and hands back to the tree-walker
    // (Op::EvalAst): class declarations, regex literals, and subtrees from
    // emit_treewalker_delegate. Unlike closures above every entry is a gap,
    // and only once this is always empty can a
    // VM-compatible function stop keeping its AST alive.
    std::unique_ptr<std::vector<const ASTNode*>> treewalk_nodes;
    std::vector<const ASTNode*>& ensure_treewalk_nodes() { if (!treewalk_nodes) treewalk_nodes = std::make_unique<std::vector<const ASTNode*>>(); return *treewalk_nodes; }

    std::unique_ptr<std::vector<HandlerEntry>> handlers;
    std::vector<HandlerEntry>& ensure_handlers() { if (!handlers) handlers = std::make_unique<std::vector<HandlerEntry>>(); return *handlers; }

    // env_params/env_locals/loop_envs are only ever populated when env_mode
    // is true (see BytecodeCompiler), so they're bundled behind one lazy
    // pointer -- unlike closures/treewalk_nodes/handlers above,
    // these three share a single real trigger condition.
    struct EnvBundle {
        std::vector<std::string> env_params;
        struct EnvLocal { std::string name; bool is_lexical; bool is_const; };
        std::vector<EnvLocal> env_locals;
        // Per-iteration Environment locals for one loop/block (Op::EnterLoopEnv).
        // copy_forward: a `for` header's own let/const carries across iterations;
        // everything else starts fresh each time.
        struct LoopEnvVar { std::string name; bool is_lexical; bool is_const; bool copy_forward; };
        std::vector<std::vector<LoopEnvVar>> loop_envs;
    };
    std::unique_ptr<EnvBundle> env;
    EnvBundle& ensure_env() { if (!env) env = std::make_unique<EnvBundle>(); return *env; }
    using LoopEnvVar = EnvBundle::LoopEnvVar; // BytecodeCompiler builds these before a chunk_ exists

    BytecodeChunk();
    // Out of line for the same reason ensure_closures is.
    ~BytecodeChunk();

    void trace(Visitor& v) const;
};

// Human-readable dump for QUANTA_VM_DISASM=1.
std::string disassemble_chunk(const BytecodeChunk& chunk, const std::string& name);

}

#endif
