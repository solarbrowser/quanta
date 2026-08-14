/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/FunctionExecutable.h"
#include <cstdio>
#include <cstdlib>
#include "quanta/parser/AST.h"
#include "quanta/parser/ScriptUnit.h"
#include "quanta/core/vm/Bytecode.h"

namespace Quanta {

// Bigger in-struct than a hash-set registry would need, but with no
// per-executable allocation at all: the intrusive links below cost less than
// a set node plus its share of the bucket array.
#if defined(__GLIBCXX__)
// Grew by a borrowed-body pointer and the ScriptUnitRef that keeps it alive.
// Paid once per function literal, against a body that is no longer copied.
static_assert(sizeof(FunctionExecutable) == 192);
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

// Both body setters funnel through here: the directive is a fact about the
// tree, so it is read while the tree is certainly in hand rather than on the
// first call, which is what used to make Function::call reach for it.
static bool opens_with_use_strict(const ASTNode* node) {
    return node && node->get_type() == ASTNode::Type::BLOCK_STATEMENT &&
           static_cast<const BlockStatement*>(node)->has_use_strict_directive();
}

void FunctionExecutable::adopt_body(std::unique_ptr<ASTNode> node) {
    owned_body_ = std::move(node);
    unit_ = ExecutableRef<ScriptUnit>();
    body_ = owned_body_.get();
    body_has_use_strict = opens_with_use_strict(body_);
}

void FunctionExecutable::borrow_body(const ExecutableRef<ScriptUnit>& unit, ASTNode* node) {
    owned_body_.reset();
    unit_ = unit;
    body_ = node;
    body_has_use_strict = opens_with_use_strict(body_);
}

}  // namespace Quanta
