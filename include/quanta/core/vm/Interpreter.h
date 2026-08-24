/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_INTERPRETER_H
#define QUANTA_VM_INTERPRETER_H

#include <span>
#include "quanta/core/vm/Bytecode.h"

namespace Quanta {

class Parameter;

class Context;
class Function;

namespace VM {

// Executes a chunk to completion. The register file lives on the C++ stack
// so the conservative GC scan covers it for free (and generator fibers
// freeze it along with the rest of their stack). Exceptions are reported
// through ctx (same contract as ASTNode::evaluate).
// this_val: pre-resolved `this` for register-mode frames (skips the per-call
// "this" binding insert); null falls back to scope-chain resolution.
// owner: the Function whose BytecodeChunk this is, needed to write_barrier
// when GetNamed's prototype-chain cache learns a new holder/prototype
// reference (see FeedbackSlot::ProtoEntry). Null for run_script's ownerless
// top-level chunk -- that cache is simply inert there (see run_script).
Value run(const BytecodeChunk& chunk, Context& ctx, std::span<const Value> args,
          const Value* this_val = nullptr, Function* owner = nullptr);

// Compiles a generator/async BODY for the suspendable calling convention
// (bindings already live in ctx; yield/await suspend the fiber from inside
// the dispatch loop). Null means the body doesn't compile -- caller
// tree-walks instead. Callers cache the result on the owning Function
// (see GeneratorFunction::get_suspendable_chunk) instead of recompiling
// per call/fiber.
std::unique_ptr<BytecodeChunk> compile_suspendable(const ASTNode* body,
                                                   const std::vector<std::string>& env_bound,
                                                   bool outer_with = false);

// A parameter's default expression. Suspendable functions bind their
// parameters outside the compiled body, so this is the one place a default is
// evaluated on its own rather than as part of the chunk that owns it.
Value run_default_value(const Parameter* param, Context& ctx);

// One expression belonging to a class definition -- a computed key, the
// heritage, a static field's value, a static block -- evaluated by the
// compiler instead of the tree-walker. `ok` reports whether it compiled; a
// false leaves the result untouched so the caller can fall back.
Value run_expression(const ASTNode* expr, Context& ctx, bool& ok);

// Runs a chunk from compile_suspendable. Traced through its owning Function.
Value run_suspendable_chunk(const BytecodeChunk& chunk, Context& ctx, Function* owner);

// Script tier: compile+run a Program's top-level statements (hoisting must
// already be done by Program::evaluate). used_vm=false -> caller tree-walks.
Value run_script(const std::vector<std::unique_ptr<ASTNode>>& statements,
                 Context& ctx, bool& used_vm, bool track_completion = false);


}

}

#endif
