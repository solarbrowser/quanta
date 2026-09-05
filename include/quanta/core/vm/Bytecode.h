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
    // r_start count -- acc = the concatenation of count consecutive
    // registers starting at r_start, each already a string (literal pieces
    // via LdaConst, interpolated ones via ToTemplateString first). One
    // allocation for the whole result instead of the count-1 intermediate
    // Strings a left-to-right chain of Add would build and immediately
    // discard.
    BuildTemplateString,
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

    BindEnvLocals,  // u8: 1 opens a child scope for them (parameter
                    //     expressions have their own). Creates env_locals bindings
                    //     (deferred past parameter
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
    // r_dst r_iter r_next_fn o -- a `[..., ...rest]` pattern's rest element.
    // r_next_fn holding a number (GetIterator's array fast-path marker) means
    // r_iter IS the source array; the rest of it, from the number's index
    // onward, is copied into r_dst in one call instead of one
    // IteratorNextOrJump+DefineElement pair per element, and o is taken.
    // Anything else about the source (not that fast form, or the array
    // stopped being dense mid-loop) falls through to pc+size unchanged, for
    // the ordinary per-element loop right after this to run exactly as if
    // this opcode were never there.
    TryCollectRestArray,

    JumpIfNotNullish, // o
    JumpIfNullish,    // o
    JumpIfNotUndefined, // o -- default-parameter check (spec: explicit undefined too, not just omitted)

    CreateClosure,   // k -- instantiates a function literal (index into
                     // BytecodeChunk::closures); still runs the literal's own
                     // tree-walker evaluate() for now
    DeclareFunction, // k -- instantiates a hoisted function declaration from
                     // closures[k] AND binds its name (the binding target
                     // depends on the environment shape, not on the literal)
    // r_src r_keys -- acc = a fresh object with every own enumerable property
    // of r_src whose key is not in the r_keys array (an object pattern's
    // `...rest`).
    CopyRestProperties, // r_src r_keys
    CreateRestArray, // r -- acc = Array of args[r..argc), for a `...rest` parameter

    Call,         // r_callee r_args_start argc n
    CallResolved, // r_func r_this r_args_start argc n -- func already resolved (spec: before args)
    CallViaFunctionCall, // r_obj r_args_start argc fb -- X.call(...), skips Function.prototype.call when fb proves it's still that
    CallViaFunctionApply, // r_obj r_args_start fb -- X.apply(thisArg, argsArray), same idea for "apply" (exactly 2 args)
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
    // Merges everything acc's value contributes as an object spread into the
    // object in r_obj, in place and in source order. Shares
    // object_spread_into with the tree-walker's ObjectLiteral::evaluate, so
    // the primitive/boxed-string/Proxy rules cannot drift between the two.
    ObjectSpreadInto, // r_obj
    HasPrivate,      // n -- acc = (#name in acc), the ergonomic brand check
    // k -- acc = the engine helper for EngineHelper::Kind k, read from the
    // global object's internal slots. Not a name lookup: these back the
    // class-field and import.source machinery and must not be reachable,
    // shadowable or replaceable from script.
    LdaEngineHelper,
    // n_pattern n_flags -- a fresh RegExp per evaluation, as the spec requires.
    // Only the compiled pattern is shared, cached inside RegExp itself.
    CreateRegExp,

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

    // Produce a value and park it in a register, as one instruction instead
    // of two. The accumulator ends up holding what it would have held either
    // way, so nothing downstream can tell which form it came from. Written by
    // fuse_store_pairs after the body is compiled, never emitted directly.
    LdarStar,      // rr: src, dst
    LdaSmiStar,    // i8, dst
    LdaConstStar,  // k16, dst
    LdaZeroStar,   // dst
    LdaThisStar,   // dst
    LdaEnvStar,    // n16, dst
    LdaLookupStar, // n16, dst
    LdaEnvSlotStar,// slot, n16, dst
    GetNamedStar,  // recv, n16, fb16, dst

    // Suspends the generator or async generator this body is running in, with
    // the value in the accumulator, and resumes with what next() sent -- the
    // fiber switch a plain `yield` is, and nothing else. `yield*` still goes to
    // the tree-walker: the delegation protocol is a loop over another iterator,
    // not one suspension.
    Yield,
    // The same for `await`: suspend on the operand in the accumulator and
    // resume with what it settled to. The operand byte says whether there was
    // one at all -- a bare `await` suspends once and answers undefined rather
    // than awaiting undefined.
    Await,   // u8: 1 when the expression had an operand

    LdaNewTarget,
    LdaImportMeta,

    // Runs the completion half of a `return` in a suspendable body on the
    // accumulator. Emitted before Op::Return, never instead of it.
    SettleReturn,   // u8 bits: 1 Await the operand, 2 record the value

    // `yield*` on the accumulator: the whole delegation protocol, which
    // suspends as many times as the inner iterator has values.
    YieldStar,

    // `for await...of`: the async forms of the three above. The extra register
    // says whether the iterator came from @@asyncIterator or is a sync one
    // driven through AsyncFromSyncIteratorContinuation, which decides what
    // each step Awaits.
    GetAsyncIterator,          // r_next_fn r_from_sync
    AsyncIteratorNextOrJump,   // r_iter r_next_fn r_from_sync o
    AsyncIteratorClose,        // r_iter mode (0=validate, 1=re-raise pending)

    // A getter or setter whose key is only known at run time. Separate from
    // FinalizeComputedProperty because an accessor merges with the other half
    // of its pair rather than replacing what is there.
    FinalizeComputedAccessor,  // r_obj r_key r_raw_key kind

    // `__proto__: v` written literally in an object literal: sets [[Prototype]]
    // rather than creating a property. Which spellings count is the compiler's
    // decision, so this is only emitted where it applies.
    SetLiteralProto,           // r_obj

    // `delete name`. The operand byte says the name is a register-resident
    // local of this frame: a declared binding is not configurable, so the
    // answer is false, and a chain lookup would otherwise find and delete some
    // outer binding of the same name.
    DeleteLookup,              // n16, u8: 0 chain lookup, 1 frame local, 2 frame local under a with

    // `using`/`await using`. A block that declares one opens a dispose scope on
    // entry and runs it on every exit, which is the same shape a finally has,
    // so the compiler drives these with the same pads.
    PushDisposeScope,
    RegisterDisposable,        // u8: 1 for `await using`
    DisposeScope,              // u8 mode (0=plain, 1=acc holds a pending exception)

    // Enters a `with`: the accumulator holds the object, and what goes on the
    // chain is an object Environment. Leaving is Op::RestoreEnv, since the
    // scope push is the whole of what has to be undone.
    PushWithEnv,

    // Assignment inside a `with` resolves its target before the right side
    // runs, which is observable: the right side can delete the property the
    // reference was bound to, and the write must recreate it there rather than
    // land further out. ResolveWithTarget answers with the with object the name
    // was found on, or undefined when it resolved to an ordinary binding, and
    // the store honours that verdict.
    // Reading a name inside a `with`: one walk of the chain, no name cache. The
    // cache remembers which environment answered last time, and a with object
    // that has to be asked first -- observably, through a Proxy trap -- is
    // exactly what it would skip.
    LdaWith,                   // n16, u8: 1 when a miss yields undefined (typeof)
    ResolveWithTarget,         // n16
    LdaWithResolved,           // r_target n16
    StaWithResolved,           // r_target n16

    // A constant past the 65535th: the pool index no longer fits the narrow
    // form's operand. Only a literal large enough to fill the pool reaches it.
    LdaConstWide,              // k32

    // Same operands as Call. A direct eval runs in the caller's scope, and the
    // eval builtin learns that from a flag on the calling context.
    CallDirectEval,            // r_callee r_args_start argc n

    // An assignment's reference is made before its right side runs. Nothing can
    // move it in between except a direct eval, which can declare a nearer
    // binding, so only a body containing one resolves ahead and parks it.
    ResolveBindingEnv,         // n16 u8slot
    LdaResolvedEnv,            // u8slot n16
    StaResolvedEnv,            // u8slot n16

    // Around the parameter section when an initializer holds a direct eval:
    // EvalDeclarationInstantiation reads the parameter names and whether
    // `arguments` collides off the calling context, not off its own arguments.
    EnterParamEval,            // u8: bit0 enter/leave, bit1 arguments conflict
    // Arms the next call as a direct eval. CallDirectEval carries its own flag;
    // a spread call takes it this way because its opcode has no room left.
    SetDirectEval,             // u8
    // k -- builds the class at BytecodeChunk::ast_nodes[k]. The node is
    // read as the class's description, not walked as a tree: it is the one
    // construct still described by its own node rather than by something
    // built at compile time.
    DefineClass,               // k
    // r_ctor r_proto u8flags -- ties a constructor to its prototype and stamps
    // the two facts that make it a class rather than a function: the prototype
    // is fixed in place (non-writable, non-enumerable, non-configurable), the
    // prototype names it back, and calling it without `new` is an error. What
    // the class holds is emitted around this rather than read off a node.
    // flags bit 0: no constructor was written, so construction supplies one.
    BuildClass,
    // r_ctor r_proto -- acc holds what the class extends. Puts the two
    // prototype links in place, marks the constructor derived (or, for
    // `extends null`, derived with no reachable super), and hands every member
    // already installed the constructor its `super` resolves against.
    LinkClassHeritage,
    // r_ctor n -- adds one instance field to the class in that register: the
    // key is the name operand, and the accumulator holds the function that
    // computes its value, or undefined for a field written without one. The
    // fields are recorded on the constructor and run against each instance as
    // it is built, rather than folded into the constructor's own body.
    AddFieldInitializer,
    // r_ctor -- runs one of the class's static elements: the accumulator holds
    // the function standing for it, which is stamped as class code and called
    // with the constructor as its receiver. What it answers is left in the
    // accumulator, which a static field then defines and a static block drops.
    RunStaticElement,
    // r_ctor r_key u8flags -- AddFieldInitializer for a field whose key was
    // computed: the key was resolved once, where the class was built, and the
    // register holds what it came to.
    AddFieldInitializerKeyed,
    // r_ctor r_proto n u8flags -- records one of the class's private names on
    // the constructor, against the object that brands it: the prototype for an
    // instance name, the constructor itself for a static one. flags bit 0:
    // static; bit 1: a method or accessor rather than a field, which is what a
    // brand check has to tell apart. The map starts from the one the enclosing
    // class left on the running frame, so a nested class still sees the names
    // written around it.
    DeclarePrivateName,
    // r_holder n u8kind -- installs a private method or accessor on the object
    // that brands it, under the key its name resolves to there. kind: 0
    // method, 1 getter, 2 setter. The accumulator holds the function.
    DefinePrivateMember,
    // r_ctor r_proto -- hands every member the class's private-name map, so a
    // name written in one member's body resolves wherever that body runs.
    LinkPrivateBrands,
    // r_ctor n -- a static private field: the accumulator's value becomes a
    // slot of the constructor's own, under the key the name resolves to
    // there, rather than a property anything could read.
    DefinePrivateStatic,
    // n -- binds the class's own name in the running scope, to the class in
    // the accumulator. A class declaration's name is created where the
    // declaration stands rather than reserved with the scope's other lexicals,
    // so a reader of the name finds it through the scope chain.
    BindClassName,
    // r -- super(...spread): the arguments are already gathered into the array
    // in that register, so the ceremony reads them from there rather than from
    // a run of registers the way SuperCall does.
    SuperCallSpread,           // r
    // `delete super.x` always throws (13.5.1.2 step 4.b), but only after the
    // reference is evaluated -- which the opcodes before this one did.
    ThrowSuperDelete,
    // k -- applies the export record at BytecodeChunk::exports[k]. The
    // accumulator holds a default export's value where one was evaluated.
    LinkExports,

    // Depth-carrying counterparts of LdaEnvSlot/StaEnvSlot, for a name whose
    // declaring scope is a fixed, known number of Environments out from the
    // one active at this instruction -- one loop or block short of the exact
    // match those two require, not an unrelated case: the compiler already
    // computes this as env_depth_ minus the declaration's own recorded depth
    // (see BytecodeCompiler::emit_read_local/emit_write_local and
    // EnvSlotInfo's doc comment), the same difference that already makes
    // break/continue's environment unwind exact. LdaEnvSlot/StaEnvSlot are
    // left untouched rather than widened to carry a hop count that is zero
    // for almost every read: this only exists for the reads that need it.
    LdaEnvSlotAt,   // hops slot n
    StaEnvSlotAt,   // hops slot n

    // for-in's "does the object still have this key" check, which the loop
    // makes once per key per pass. TestIn answers it by finding the key's
    // shape slot, which hashes the name -- the same cost the body's own o[k]
    // pays, doubled for nothing when nothing was deleted. This takes the keys
    // array too, so it can recognize that the array being walked IS the one
    // remembered for the receiver's current shape, and answer from that.
    ForInKeyPresent,  // obj keys key

    kCount
};

// Inline-cache slot for one GetNamed/SetNamed site: mono -> poly (up to
// kMaxEntries distinct shapes) -> mega. count==0 is Uninit, count==1 behaves
// exactly like the old monomorphic-only cache (a length-1 scan), count>1 is
// the polymorphic case -- all three share the same scan/learn code path.
// Once `mega` is set the site is permanently uncached (same as fb==nullptr).
// What one GetNamed/SetNamed site has actually learned. Held behind a pointer
// by FeedbackSlot rather than inline, because a site that has never run, or
// that always took the slow path, learns nothing and most never do: on a real
// script fewer than one site in eight ever caches a single entry, while this
// is large enough that carrying one per site cost more memory than everything
// else the compiler emitted put together.
struct FeedbackBody {
    // is_accessor: slot_index names an accessor-kind shape slot, so the slot
    // holds the getter rather than the property's value and a hit has to call
    // it. Lives in the padding slot_index's alignment already leaves. Every
    // reader that stores through slot_index, or hands its contents back as a
    // value, has to exclude these.
    struct Entry { Shape* shape = nullptr; uint32_t slot_index = 0; bool is_accessor = false; };
    static constexpr uint8_t kMaxEntries = 8;
    std::array<Entry, kMaxEntries> entries{};
    uint8_t count = 0;
    bool mega = false;
    // Set the first time this site turns out to be reading an Array's length.
    // A GetNamed site's name is a compile-time constant, so once it is "length"
    // it stays "length"; what still has to hold per execution is that the
    // receiver really is a dense Array. Without this the read cannot use the
    // shape cache at all -- an Array's length is computed from the element
    // count, never mirrored in a slot -- so every one of them fell through to
    // the uncached path and paid a string compare there.
    bool array_length = false;

    // SetNamed-only: caches adding a brand-new own property (a shape
    // transition), keyed by the shape BEFORE the add. `proto_epoch` is
    // Object::proto_epoch() when last validated blocker-free -- only
    // trusted while it still matches. `prototype` must match too, for the
    // reason spelled out for ProtoEntry below: a shape does not encode a
    // prototype, so `class A {}` and `class B {}` instances both arrive
    // here as Shape::root() while only one chain may carry a setter for
    // this key. GetNamed sites carry these fields but never touch them.
    struct TransitionEntry {
        Shape* from_shape = nullptr;
        Shape* to_shape = nullptr;
        Object* prototype = nullptr;
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
        // A holder whose property lives in its descriptor map rather than in a
        // shape slot -- which is every builtin prototype method, since those
        // have to be non-enumerable and a shape slot carries no attributes.
        // Such an entry caches the value itself, and `desc_epoch` is what
        // keeps it honest: any write that could change it moves the global
        // descriptor epoch. slot_index is meaningless when this is set.
        bool from_descriptor = false;
        // The walk reached the end of the chain without finding the key, so the
        // read yields undefined. `holder`, `slot_index` and `cached_value` mean
        // nothing on such an entry; proto_epoch alone keeps it honest, because
        // a key can only APPEAR on a chain through a property add, a
        // defineProperty or a [[Prototype]] change, and each of those bumps it
        // (see Object::proto_epoch). Absence on the receiver's own side is
        // re-established per hit instead: its shape is part of the key, and the
        // descriptor map it cannot show is what the override guard covers.
        bool absent = false;
        // cached_value is the getter, not the value: an inherited accessor whose
        // getter sits in the holder's descriptor map, which is where a class's
        // `get x()` lands. A hit still has to call it -- what it skips is the
        // rebuild of two descriptors and the walk between them. Guarded by
        // desc_epoch exactly like from_descriptor, since it caches out of the
        // same map. Never set together with from_descriptor or absent.
        bool is_getter = false;
        // SetNamed-only mirror of is_getter: cached_value is the SETTER of an
        // inherited accessor, learned from Object::ordinary_set's own
        // prototype-chain walk (its optional out-params report which
        // function it invoked, see set_named). A hit still has to call it --
        // what it skips is ordinary_set's walk itself (has_own_property +
        // get_property_descriptor at every link) on every single write.
        // Same desc_epoch guard, same never-together-with-from_descriptor/
        // absent rule. Never true together with is_getter either: a single
        // entry caches one function, and a property can be looked up for
        // reading or writing at a given site, never both from the same
        // ProtoEntry slot (GetNamed and SetNamed sites never share a
        // FeedbackSlot).
        bool is_setter = false;
        uint64_t desc_epoch = 0;
        Value cached_value;
    };
    std::array<ProtoEntry, kMaxEntries> proto_entries{};
    uint8_t proto_count = 0;
    bool proto_mega = false;

    // GetNamed on a primitive receiver. The prototype is fixed by the
    // primitive's type and the site's name is a compile-time constant, so one
    // entry per site is the entire cache -- there is no receiver shape to key a
    // table on. A builtin prototype method has to be non-enumerable and so
    // lives in the descriptor map, where the shape-slot caches cannot reach it.
    // The value is cached, not the descriptor's address: pointing into the map
    // looked cheaper but skipped whatever get_property_descriptor does on the
    // way to the same value, and the result was heap corruption that ASan's
    // allocator did not reproduce. This learns from what that call returned.
    // `prim_value` and `prim_proto` are real GC cells -- see BytecodeChunk::trace.
    Object* prim_proto = nullptr;
    Value prim_value;
    bool prim_is_getter = false;
    bool prim_valid = false;
    uint64_t prim_desc_epoch = 0;

    // GetNamed on an OWN property that lives in the receiver's descriptor map
    // rather than in a shape slot. Every namespace builtin is one of these --
    // Math.floor, JSON.stringify -- because a builtin has to be non-enumerable
    // and a shape slot carries no attributes. Such a read reaches neither the
    // shape-slot cache nor the prototype cache, so it hashed the key and
    // searched the map for it twice over: once for the prototype gate's
    // "is there an override" question and once for the value itself.
    // A shape cannot key this entry -- descriptors_ is per-object, not
    // per-shape -- so the receiver's own identity is the key, and the global
    // descriptor epoch is what retires it, on exactly the terms a
    // from_descriptor prototype entry is retired: anything that could change
    // the value, a plain assignment included, moves that epoch.
    // `own_desc_receiver` and `own_desc_value` are real GC cells -- see
    // BytecodeChunk::trace and the barrier where this is learned.
    Object* own_desc_receiver = nullptr;
    Value own_desc_value;
    uint64_t own_desc_epoch = 0;
};

// One cache site. Empty until the site learns something; see FeedbackBody.
struct FeedbackSlot {
    // Kept here as well so the many `FeedbackSlot::kMaxEntries` readers, and
    // the nested entry types they name, do not all have to be rewritten.
    static constexpr uint8_t kMaxEntries = FeedbackBody::kMaxEntries;
    using Entry = FeedbackBody::Entry;
    using TransitionEntry = FeedbackBody::TransitionEntry;
    using ProtoEntry = FeedbackBody::ProtoEntry;

    std::unique_ptr<FeedbackBody> body;

    // Null while the site has learned nothing, which every reader already
    // treats as a miss -- the same answer an all-zero body would give.
    FeedbackBody* peek() const { return body.get(); }
    // For readers that would rather not carry the null: an untouched body
    // answers every question with the miss the absent one means. A namespace
    // static, not a function-local one, so reading it costs no guard on a
    // path the interpreter takes for every property access.
    static const FeedbackBody kEmpty;
    const FeedbackBody& read() const { return body ? *body : kEmpty; }
    FeedbackBody& ensure() {
        if (!body) body = std::make_unique<FeedbackBody>();
        return *body;
    }
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
    // Monomorphic receiver-pointer cache, one step past `qualified`: skips
    // even the sparse_overflow_ hash lookup that a qualified-only hit still
    // pays every time. Sound with no epoch, unlike every other pointer cache
    // in this file: sparse_overflow_ is a std::unordered_map, whose value
    // pointers survive rehashing and the insertion/erasure of OTHER keys --
    // the standard guarantees a pointer to an element is invalidated only by
    // erasing that SAME element -- and a private field has no delete syntax,
    // so once learned for a given receiver this pointer is good for the
    // receiver's whole lifetime. A different receiver at the same call site
    // (a polymorphic access pattern) just falls back to the qualified-keyed
    // path below, same cost as before this cache existed.
    Object* cached_receiver = nullptr;
    Value* cached_slot = nullptr;
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
    // Bigger budget than FeedbackBody's own kMaxEntries (8): a keyed site's
    // entries are keyed on (shape, key) instead of shape alone, so a single
    // site enumerating a wider object (`for (const k in o) sum += o[k]`,
    // any object with more than 8 own keys) legitimately needs one entry
    // per key, not per shape -- with 8 it went mega long before a
    // realistically-sized object's key set fit, and every access after
    // that paid a full Shape::find_slot hash lookup for keys the site
    // would otherwise have cached. 16 doubles the per-site footprint
    // (~976B -> ~1920B, KeyedFeedback is lazily allocated only for chunks
    // that actually use computed property access) in exchange for that.
    static constexpr uint8_t kMaxEntries = 32;
    std::array<Entry, kMaxEntries> entries{};
    uint8_t count = 0;
    bool mega = false;

    // SetKeyed-only: mirrors FeedbackBody::TransitionEntry (caches adding a
    // brand-new own property, keyed by the shape BEFORE the add, the
    // receiver's prototype and proto_epoch -- see that struct's own comment
    // for why prototype/epoch matter), with `key` added since a keyed site's
    // key is read from a register and can differ on every execution, unlike
    // SetNamed's compile-time-constant name. Without this, `obj[k] = v` on a
    // freshly-shaped object -- the common `const o = {}; o[k] = v;` pattern
    // -- always missed `entries` above (which only matches a key the shape
    // already has) and fell all the way to Shape::transition(key)'s hash
    // lookup on every single call, never able to reuse a previous transition
    // the way a plain SetNamed site already could.
    struct TransitionEntry {
        Shape* from_shape = nullptr;
        std::string key;
        Shape* to_shape = nullptr;
        Object* prototype = nullptr;
        uint32_t slot_index = 0;
        uint64_t proto_epoch = 0;
    };
    std::array<TransitionEntry, kMaxEntries> transitions{};
    uint8_t transition_count = 0;
    bool transition_mega = false;
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
    // Identifier names carried by name-bearing opcodes. Interned once at
    // compile end (Shape::intern, the same pool Shape's slot tables and
    // Environment::SlotMap::InlineEntry::key use), so an entry is one 8-byte
    // pointer instead of a 32-byte std::string -- and, more importantly, an
    // opcode holding one already has the interned key every binding lookup
    // wants: two interned keys are equal exactly when they are the same
    // pointer, which turns a scope-chain walk's per-entry string compare into
    // a pointer compare (see Environment::SlotMap::find_interned).
    // name_at(i) recovers the string itself for the paths that need it
    // (diagnostics, object environments, property access).
    FixedArray<const std::string*> names;
    const std::string& name_at(size_t i) const { return *names[i]; }
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
    struct LookupCacheEntry {
        Environment* env = nullptr;
        Value* slot = nullptr;
        // Object-environment read form, for a global. The value lives in a
        // shape slot of env's binding object, whose ADDRESS moves when the
        // object grows, so the INDEX is cached and re-resolved through the
        // object. A non-null obj_shape selects this form; only LdaLookup takes
        // it, since writing a global needs the full [[Set]] path. Valid while
        // the object still has this shape (a property add or delete changes
        // it) and descriptor_epoch has not moved (an accessor may have
        // replaced the data property without changing the shape).
        Shape* obj_shape = nullptr;
        uint64_t descriptor_epoch = 0;
        uint32_t obj_slot_index = 0;
        // A const binding's value never changes, which makes it MORE cacheable
        // to read, not less -- but LdaLookup and StaLookup share this entry,
        // and StaLookup's fast path stores through `slot` without asking
        // anyone. So the entry records whether writing through it is allowed;
        // a store to a non-writable one falls back to the slow path, which is
        // where "Assignment to constant variable" is raised. Sits in the
        // padding after obj_slot_index.
        bool writable = false;
        // A resolved name can be shadowed later by a binding created in a
        // scope closer than this one -- Annex B's block function is the way
        // that happens after code has already run. No shape or descriptor
        // epoch shows it, so the entry carries the binding-shadow epoch and
        // is refused once it moves. Sits in the padding after `writable`.
        uint32_t shadow_epoch = 0;
    };
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
    bool env_mode : 1 = false;
    // Parameter lists with initializers get spec FunctionDeclarationInstantiation
    // ordering: params seeded uninitialized (TDZ), initialized left-to-right by
    // bytecode, and env_locals bound only afterwards (Op::BindEnvLocals), so a
    // default expression can't see a later parameter or a body-level binding.
    bool env_params_tdz : 1 = false;
    // Sloppy FDI step 29: the body's top-level lexical declarations live one
    // scope in from the variable environment, so a direct eval can tell that a
    // `var` it wants to add collides with one. Only a body with an eval in it
    // pays for the extra scope.
    bool lex_scope_split = false;
    // Top-level script chunk: the frame's lexical env is the PERSISTENT
    // script env (not per-call), so the lookup cache may point into it.
    bool script_mode : 1 = false;

    // Function::call materializes the real arguments object before VM::run
    // (skipped otherwise -- it dominated call-heavy benchmarks).
    bool needs_arguments : 1 = false;
    // Set by emit() when an Op::LdaLookup or Op::StaLookup goes into the code,
    // which are the only two readers of lookup_cache. A chunk that resolves
    // every name to a register or an env slot never touches the cache, so
    // neither the chunk-level array nor the per-instance copy is allocated --
    // and .data() is then null, so a missed emission site faults on the spot
    // instead of silently sharing one instance's resolved slots with another.
    bool uses_lookup_cache : 1 = false;
    // Set when the body can observe `this`, which is exactly when it emits
    // Op::LdaThis -- the only opcode that reads the frame's this. A call into
    // a chunk without it can skip setting one up, including the sloppy-mode
    // substitution that boxes a primitive or reaches for the global object.
    bool uses_this : 1 = false;
    // Set by emit() when any opcode that can call await_value goes into the
    // code: Op::Await itself, Op::AsyncIteratorNextOrJump/AsyncIteratorClose
    // (for-await-of's own per-iteration await, a different opcode entirely
    // -- it never emits Op::Await), and Op::DisposeScope (an `await using`
    // resource's disposal is awaited when the scope unwinds; a plain
    // `using` in the same scope still emits this opcode, so its presence is
    // a conservative "maybe", not a precise one). These are the only
    // suspend sources a plain (non-generator) async function's body can
    // ever reach: SettleReturn's own await bit (set on every
    // `return <value>` in an async body) only suspends inside an async
    // GENERATOR (perform_return_completion gates it on
    // AsyncGenerator::get_current()), so it is not one of them.
    // AsyncFunction::call uses this to skip the fiber altogether for a body
    // that provably never awaits.
    bool has_await : 1 = false;

    // Closures/tree-walk escapes/destructuring/try-catch are each
    // independently rare (a chunk can have any one without the others), so
    // unlike IcFeedback these stay separate lazy pointers rather than one
    // bundle.

    // Function literals only (Op::CreateClosure). Separate from ast_nodes
    // because this is the permanent case: a literal always needs some
    // per-decl-site description to instantiate from. Holds prebuilt
    // ClosureTemplates rather than AST pointers, so creating a closure no
    // longer reaches back into the enclosing function's own body_ (V8's
    // CreateClosure reads a SharedFunctionInfo from the constant pool the
    // same way). Out of line: ClosureTemplate is only forward-declared here,
    // since its own header needs BytecodeChunk.
    std::unique_ptr<std::vector<ClosureTemplate>> closures;
    std::vector<ClosureTemplate>& ensure_closures();

    // Class definitions (Op::DefineClass). The node is read as the class's
    // description -- its members, their keys and flags -- not walked: every
    // expression a class definition owns is compiled and run.
    // The constructs still described by their own node: classes and module
    // declarations. Everything else the chunk needs was built at compile time.
    // What one `export` declaration says, settled while compiling. An export
    // is a record the grammar fixes, not code -- the only part that runs is a
    // default export's expression, which is emitted like any other.
    struct ExportRecord {
        struct Entry {
            std::string export_name;
            std::string local_name;
            // A name a declaration binds is exported only once it exists; a
            // name written in a specifier list has to exist.
            bool from_declaration = false;
        };
        std::vector<Entry> entries;
        std::string source_module;   // set only for a re-export
        std::string default_local;   // the name a default export also binds
        bool is_default = false;
        bool default_is_hoistable = false;
        bool is_re_export = false;
    };
    // Both are rare and neither is ever the only one worth a pointer of its
    // own, so one allocation carries them and the chunk stays its size.
    struct SideTables {
        std::vector<const ASTNode*> ast_nodes;
        std::vector<ExportRecord> exports;
    };
    std::unique_ptr<SideTables> side_tables;
    SideTables& ensure_side_tables() {
        if (!side_tables) side_tables = std::make_unique<SideTables>();
        return *side_tables;
    }
    std::vector<const ASTNode*>& ensure_ast_nodes() { return ensure_side_tables().ast_nodes; }
    std::vector<ExportRecord>& ensure_exports() { return ensure_side_tables().exports; }
    // Whether anything of the parse tree is still pointed at from here.
    bool keeps_ast_nodes() const { return side_tables && !side_tables->ast_nodes.empty(); }

    std::unique_ptr<std::vector<HandlerEntry>> handlers;
    std::vector<HandlerEntry>& ensure_handlers() { if (!handlers) handlers = std::make_unique<std::vector<HandlerEntry>>(); return *handlers; }

    // env_params/env_locals/loop_envs are only ever populated when env_mode
    // is true (see BytecodeCompiler), so they're bundled behind one lazy
    // pointer -- unlike closures/ast_nodes/handlers above,
    // these three share a single real trigger condition.
    struct EnvBundle {
        std::vector<std::string> env_params;
        struct EnvLocal { std::string name; bool is_lexical; bool is_const; };
        std::vector<EnvLocal> env_locals;
        // The same names, interned once (Shape::intern). Every call through
        // this chunk binds the same list, and interning is a hash and a probe
        // per name per call -- so it is done once for the chunk instead. The
        // pointers are stable for the thread's lifetime, which is what makes
        // caching them safe. Filled on the first call; see VM::run.
        std::vector<const std::string*> env_param_keys;
        std::vector<const std::string*> env_local_keys;
        bool env_keys_ready = false;
        // Per-iteration Environment locals for one loop/block (Op::EnterLoopEnv).
        // copy_forward: a `for` header's own let/const carries across iterations;
        // everything else starts fresh each time.
        struct LoopEnvVar { std::string name; bool is_lexical; bool is_const; bool copy_forward; };
        std::vector<std::vector<LoopEnvVar>> loop_envs;
        // loop_envs' names interned once, same rationale and same lifetime as
        // env_param_keys above. This list is rebound on every iteration rather
        // than once per call, so paying the pool probe per name here cost the
        // most of any of them.
        std::vector<std::vector<const std::string*>> loop_env_keys;
        bool loop_env_keys_ready = false;
        // How many bindings this chunk seeds into the call's own environment,
        // in the order the slot indices above were predicted. The environment
        // is sized from it once, so a declared binding gets an index instead
        // of a hash node.
        uint16_t env_slot_total = 0;
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
// Operand layout, for the passes that have to walk instructions without
// caring what any of them do: the peephole in BytecodeCompiler and anything
// else that needs to find instruction boundaries or an embedded jump offset.
int op_operand_bytes(Op op);
char op_operand_kind(Op op);

std::string disassemble_chunk(const BytecodeChunk& chunk, const std::string& name);

// Every register operand a chunk carries must name a register the chunk
// declared. The VM indexes its register bank with these bytes and does not
// re-check them, so a compiler that emitted one too large would write past
// the bank -- past the inline array in VM::run, into that frame's saved
// registers. Nothing at run time can tell that apart from correct code, and
// the check belongs where the bug is, in the compiler that just emitted it,
// not on a path taken millions of times a second. Compiled in only for
// -DQUANTA_VALIDATE_BYTECODE builds (debug and asan), where it runs once per
// chunk and aborts on the first bad operand.
#ifdef QUANTA_VALIDATE_BYTECODE
void validate_chunk_registers(const BytecodeChunk& chunk, const std::string& name);
#endif

}

#endif
