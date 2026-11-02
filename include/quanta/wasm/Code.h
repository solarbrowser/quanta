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
    kTrapStackless,
    kEnd,
};

}  // namespace Op

struct CompiledFunction {
    std::vector<uint32_t> code;
    uint32_t type = 0;  // the canonical id of its type
    uint32_t paramsN = 0, paramsR = 0;      // the slots of its parameters on each stack
    uint32_t localsN = 0, localsR = 0;      // the slots of the locals that are not parameters
    uint32_t resultsN = 0, resultsR = 0;
    uint32_t maxN = 0, maxR = 0;            // the most the operands of it take above its locals
};

}  // namespace Quanta::Wasm

#endif
