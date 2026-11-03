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
// a prefix under a base and their number, and the instructions of this format from kJmp on. What follows is its immediates.
namespace Op {

constexpr uint32_t kFcBase = 0x100;   // 0xFC n
constexpr uint32_t kFdBase = 0x200;   // 0xFD n (the vector instructions)
constexpr uint32_t kFbBase = 0x400;   // 0xFB n (the GC instructions)

enum : uint32_t {
    kJmp = 0x1000,      // target
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
    kConstSlot,         // dst lo hi
    kSyncSp,            // height: the stack pointer is brought to the height the code that follows works from
    kThrow,             // tag
    kThrowRef,
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
};

}  // namespace Quanta::Wasm

#endif
