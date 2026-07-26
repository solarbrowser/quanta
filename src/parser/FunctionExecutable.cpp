/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/FunctionExecutable.h"
#include "quanta/parser/AST.h"
#include "quanta/core/vm/Bytecode.h"

namespace Quanta {

// 176 in-struct, but no per-executable registry allocation at all -- the
// unordered_set this replaced cost ~33 more bytes per executable in node and
// bucket storage, so the real per-executable total went down, not up.
#if defined(__GLIBCXX__)
static_assert(sizeof(FunctionExecutable) == 176);
#else
static_assert(sizeof(FunctionExecutable) <= 224);
#endif

thread_local FunctionExecutable* FunctionExecutable::live_head_ = nullptr;

FunctionExecutable::FunctionExecutable() {
    live_next_ = live_head_;
    if (live_head_) live_head_->live_prev_ = this;
    live_head_ = this;
}

FunctionExecutable::~FunctionExecutable() {
    if (live_prev_) live_prev_->live_next_ = live_next_;
    else live_head_ = live_next_;
    if (live_next_) live_next_->live_prev_ = live_prev_;
}

void FunctionExecutable::gc_trace_roots(Visitor& v) {
    for (FunctionExecutable* exe = live_head_; exe; exe = exe->live_next_) {
        if (exe->bytecode_chunk) exe->bytecode_chunk->trace(v);
        if (exe->suspendable_chunk) exe->suspendable_chunk->trace(v);
    }
}

}
