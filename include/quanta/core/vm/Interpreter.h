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
Value run(const BytecodeChunk& chunk, Context& ctx, const std::vector<Value>& args);

// Process-wide opt-in switch, read once: QUANTA_VM=1 enables compilation
// and execution, QUANTA_VM=0/unset keeps everything on the tree-walker.
bool enabled();

}

}

#endif
