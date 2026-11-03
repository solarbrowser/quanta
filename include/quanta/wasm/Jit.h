/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_JIT_H
#define QUANTA_WASM_JIT_H

// A baseline JIT for the register code of Code.h: one function at a time, each instruction a fixed piece of machine code that works on the slots of
// the frame where they are (nothing is kept in registers between instructions, except the base and size of memory 0). It does what the interpreter
// does, minus the dispatch. A function with an instruction it does not know (references, exception handlers, vectors, floats for now) stays with the
// interpreter, and so does everything on a target without it (x86-64 outside Windows for now).

#include <cstdint>

#include "quanta/wasm/Runtime.h"

#if defined(__x86_64__) && !defined(_WIN32)
#define QUANTA_WASM_JIT 1
#else
#define QUANTA_WASM_JIT 0
#endif

namespace Quanta::Wasm {

// What generated code reads of the world (the offsets are known to the code generator).
struct JitCtx {
    uint8_t* memBase;        // 0: memory 0 (null if there is none)
    uint64_t memBytes;       // 8
    uint64_t reserved;       // 16
    GlobalObject** globals;  // 24
    InstanceObject* instance;
    Context* context;
    Machine* machine;
    Value* refs;             // where the reference stack is free (generated code uses none; what it calls may)
    Machine::Status status;  // set when generated code returns 1
};

struct WasmJitCode {
    void* memory = nullptr;
    size_t size = 0;
    uint32_t (*entry)(uint64_t* frame, JitCtx* ctx) = nullptr;
    bool usesMemory = false;
};

// Native code for `function` (as run by `instance`), or false (and `function.jitFailed`) if it has an instruction the JIT does not do.
bool JitCompile(const CompiledFunction& function, InstanceObject* instance);

// Whether it is time for `function` to be compiled (it has been called enough, or it has a loop), and doing it.
void JitConsider(const CompiledFunction& function, InstanceObject* instance);

// Runs the native code of `function` (which has some) with its frame at `frame`: its arguments are there, its results are left there.
Machine::Status JitRun(Machine& machine, Context& context, const CompiledFunction& function, InstanceObject* instance, uint64_t* frame, Value* refs);

// Whether the native stack has room for one more activation of generated code.
bool JitDepthOk();

// For generated code: calls function number `index` of the instance with its arguments at `frame`. 0 if it went well, 1 if not (ctx->status says).
uint32_t JitCallHelper(JitCtx* ctx, uint64_t* frame, uint32_t index);

// The same through a table: the type the call is made at, the table, the index (see the instruction call_indirect).
uint32_t JitCallIndirectHelper(JitCtx* ctx, uint64_t* frame, uint32_t type, uint32_t table, uint64_t index);

}  // namespace Quanta::Wasm

#endif
