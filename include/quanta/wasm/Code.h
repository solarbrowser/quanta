/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_CODE_H
#define QUANTA_WASM_CODE_H

#include <cstdint>
#include <vector>

namespace Quanta::Wasm {

// The code a function is run from. Validation makes it: what the validator knows at each instruction (the types on the stack, how high it is,
// where each label goes) is put into the instructions, so that running them checks nothing and decodes nothing.
//
// There are two stacks. Numbers (i32, i64, f32, f64, and v128 as two slots) are 64-bit slots of one; references are Values of the other,
// which the collector reads. The validator knows which each value is, so each instruction names the stack it works on.
//
// The numbers of a frame have a slot each, as locals first and then, above them, the operands: the operand stack of a height is always
// the same slot. The instructions on numbers give their slots (dst, then the sources) and do not touch the stack pointer; the others work
// from the stack pointer, which the code brings to the height they expect (kSyncSp) first.
//
// The code is words. The first word of an instruction is its opcode: a single byte opcode of WebAssembly under its own number, the ones with
// a prefix under a base and their number, and the instructions of this format from kJmp on. (The numbers are kept close together, so that the
// interpreter's switch is one table: a few hundred of them spread over thousands make the compiler search for the right one.) What follows is its immediates.
namespace Op {

// What is added to the opcode of a binary instruction on numbers whose second operand is a constant (its word, after the two slots, is the number itself:
// an i64 one is sign-extended from 32 bits); and to that of a comparison that jumps if it is true (slot, slot or number, target).
constexpr uint32_t kImm = 0x400;
constexpr uint32_t kJumpIf = 0x500;
constexpr uint32_t kJumpIfImm = 0x600;
// A load or store whose address is a constant has it, added to the offset, in the instruction (the word of the address slot is not used); a store of
// a constant has the number in the word of the slot of the value (sign-extended, if it is an i64).
constexpr uint32_t kAbs = 0x488;     // (these two are only added to the opcodes of memory instructions, 0x28 to 0x3E, and come to 0x4B0 and 0x4D0 on: holes of the numbers)
constexpr uint32_t kImmAbs = 0x4A8;

constexpr uint32_t kFcBase = 0x100;   // 0xFC n
constexpr uint32_t kFdBase = 0x200;   // 0xFD n (the vector instructions)
constexpr uint32_t kFbBase = 0x320;   // 0xFB n (the GC instructions)

enum : uint32_t {
    kJmp = 0x340,      // target
    kBr,                // target keepN keepR dropN dropR
    kBrIf,              // target keepN keepR dropN dropR (the i32 on top of the stack is the condition)
    kJz,                // target (an if: the i32 on top; jumps when it is 0)
    kBrTable,           // count, then count+1 of (target keepN keepR dropN dropR): the last is the default
    kReturn,            // resultsN resultsR
    kCall,              // function
    kCallIndirect,      // type(canonical) table
    kCallRef,           // type(canonical)
    kReturnCall,        // function (the frame is replaced)
    kReturnCallIndirect,
    kReturnCallRef,
    kDropN, kDropV, kDropR,
    kSelectN, kSelectV, kSelectR,
    kLocalGetN, kLocalGetV, kLocalGetR,   // slot
    kLocalSetN, kLocalSetV, kLocalSetR,
    kLocalTeeN, kLocalTeeV, kLocalTeeR,
    kGlobalGetN, kGlobalGetV, kGlobalGetR,  // global
    kGlobalSetN, kGlobalSetV, kGlobalSetR,
    kConstN,            // lo (an i32 or f32: one word)
    kConstW,            // lo hi (an i64 or f64)
    kConstV,            // four words
    kRefNull,           //
    kRefIsNull,
    kRefFunc,           // function
    kRefEq,
    kRefAsNonNull,
    kBrOnNull,          // target keepN keepR dropN dropR (a reference on top: if null it is taken off and the branch is taken; else it stays)
    kBrOnNonNull,       // (if not null it stays and the branch is taken; else it is taken off)
    kTableGet, kTableSet,  // table
    kLoad,              // memory offsetLo offsetHi: the opcode of the load is the next word
    kStore,
    kMemorySize, kMemoryGrow,   // memory
    kUnreachable,
    kCopyN,             // dst src: a slot of the frame to another
    kJzSlot, kJnzSlot,  // slot target: jumps if the number in the slot is zero, or is not
    kRet,               // resultsN resultsR: the numbers are in the first slots of the frame already
    kRet1,              // slot resultsR: the one number it gives is in the slot
    kBrTableS,          // slot count target...: the index is in the slot (the targets take no values)
    kCallR,             // function firstArgumentSlot
    kCallIndirectR,     // type(canonical) table firstArgumentSlot indexSlot
    kCallRefR,          // type(canonical) firstArgumentSlot (the reference is on the reference stack)
    kReturnCallR,       // function (the arguments are in the first slots of the frame already)
    kSelectSlot,        // dst a b condition
    kSelectRSlot,       // condition (the references are on the reference stack)
    kGlobalGetS,        // dst global
    kGlobalSetS,        // global src
    kConstSlot,         // dst lo hi
    kSyncSp,            // height: the stack pointer is brought to the height the code that follows works from
    kThrow,             // tag
    kThrowRef,
    kEntry,             // the first word of a function: counts its calls for the JIT (see Jit.h)
    kJitEnter,          // the same, once the function has native code (or cannot have any): runs it, and returns
    kEnd,
};

}  // namespace Op

// A catch clause of a try_table: what it catches, and what is done with the stack when it does.
struct CatchClause {
    uint8_t kind = 0;  // 0 catch, 1 catch_ref, 2 catch_all, 3 catch_all_ref
    uint32_t tag = 0;
    uint32_t target = 0;
    uint32_t dropN = 0, dropR = 0;  // what is taken off below what is pushed (the values of the tag, and the exception for the _ref ones)
};

// The code between `start` and `end` (word indices) is caught by these, in the order they are written; the stack is back to the heights
// the try_table began at when one of them takes it.
struct TryEntry {
    uint32_t start = 0, end = 0;
    uint32_t baseN = 0, baseR = 0;
    std::vector<CatchClause> catches;
};

struct CompiledFunction {
    std::vector<uint32_t> code;
    uint32_t type = 0;  // the canonical id of its type
    uint32_t paramsN = 0, paramsR = 0;      // the slots of its parameters on each stack
    uint32_t localsN = 0, localsR = 0;      // the slots of the locals that are not parameters
    uint32_t resultsN = 0, resultsR = 0;
    uint32_t maxN = 0, maxR = 0;            // the most the operands of it take above its locals
    std::vector<TryEntry> tries;
    // The native code of the function, if the JIT made some (see Jit.h); it is only ever a faster way of doing what the words above say.
    mutable struct WasmJitCode* jit = nullptr;
    mutable uint32_t calls = 0;
    mutable bool jitFailed = false;
};

}  // namespace Quanta::Wasm

#endif
