/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_BYTECODE_H
#define QUANTA_VM_BYTECODE_H

#include "quanta/core/runtime/Value.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Quanta {

class Visitor;
class Shape;
class ASTNode;

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
    Dec,

    LdaLookup,    // n -- chain walk (globals/closures, non-env_mode)
    LdaLookupTypeof, // n -- like LdaLookup, but an unresolved name yields undefined instead of throwing
    LdaEnv,       // n -- env_mode chain walk
    StaEnv,       // n
    StaEnvInit,   // n -- current environment only, no chain walk

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

    CreateClosure,   // k -- runs the closure's own tree-walker evaluate()
    DestructureBind, // k

    Call,         // r_callee r_args_start argc n
    CallResolved, // r_func r_this r_args_start argc n -- func already resolved (spec: before args)
    Construct,    // r_callee r_args_start argc n -- new.target = callee, calls Function::construct

    GetNamed,     // r_obj n fb
    SetNamed,     // r_obj n fb
    GetKeyed,     // r_obj
    SetKeyed,     // r_obj r_key

    CreateObject, // n
    CreateArray,  // n

    Jump,         // o
    JumpIfTrue,   // o
    JumpIfFalse,  // o
    Return,
    Throw,

    kCount
};

struct SourceEntry {
    uint32_t pc;
    uint32_t line;
    uint32_t column;
};

// Inline-cache slot for one GetNamed/SetNamed site (monomorphic only).
struct FeedbackSlot {
    Shape* shape = nullptr;
    uint32_t slot_index = 0;
};

// One try region: [start_pc, end_pc) -> handler_pc.
struct HandlerEntry {
    uint32_t start_pc;
    uint32_t end_pc;
    uint32_t handler_pc;
};

// One per compiled function body, owned by its Function, shared by every call.
struct BytecodeChunk {
    std::vector<uint8_t> code;
    std::vector<Value> constants;   // GC-visible via Function::trace()
    std::vector<std::string> names; // identifier names for LdaLookup/Call diagnostics
    std::vector<SourceEntry> positions;
    mutable std::vector<FeedbackSlot> feedback; // written as call sites warm up
    uint16_t register_count = 0;
    uint8_t parameter_count = 0;    // params occupy regs[0..parameter_count)

    // env_mode: every local lives in ctx.get_lexical_environment() instead of
    // a register. env_params/env_locals seed function entry, via VM::run.
    bool env_mode = false;
    std::vector<std::string> env_params;
    struct EnvLocal { std::string name; bool is_lexical; bool is_const; };
    std::vector<EnvLocal> env_locals;

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
