/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/FunctionExecutable.h"
#include "quanta/parser/AST.h"
#include "quanta/core/vm/Bytecode.h"

namespace Quanta {

thread_local std::unordered_set<FunctionExecutable*> FunctionExecutable::live_executables_;

FunctionExecutable::FunctionExecutable() {
    live_executables_.insert(this);
}

FunctionExecutable::~FunctionExecutable() {
    live_executables_.erase(this);
}

void FunctionExecutable::gc_trace_roots(Visitor& v) {
    for (FunctionExecutable* exe : live_executables_) {
        if (exe->bytecode_chunk) exe->bytecode_chunk->trace(v);
    }
}

}
