/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_INTERPRETER_H
#define QUANTA_VM_INTERPRETER_H

#include "quanta/core/vm/Bytecode.h"

namespace Quanta {

class Context;

namespace VM {

// Executes a chunk to completion. The register file lives on the C++ stack
// so the conservative GC scan covers it for free (and generator fibers
// freeze it along with the rest of their stack). Exceptions are reported
// through ctx (same contract as ASTNode::evaluate).
// this_val: pre-resolved `this` for register-mode frames (skips the per-call
// "this" binding insert); null falls back to scope-chain resolution.
Value run(const BytecodeChunk& chunk, Context& ctx, const std::vector<Value>& args,
          const Value* this_val = nullptr);

// Compiles and runs a generator/async BODY inside its fiber: bindings (this,
// params, arguments) already live in ctx, yield/await suspend the fiber from
// inside the dispatch loop. Sets used_vm=false (and runs nothing) when the
// body doesn't compile -- the caller then tree-walks it as before.
Value run_suspendable(const ASTNode* body, Context& ctx, bool& used_vm);

// Process-wide opt-in switch, read once: QUANTA_VM=1 enables compilation
// and execution, QUANTA_VM=0/unset keeps everything on the tree-walker.
bool enabled();

}

}

#endif
