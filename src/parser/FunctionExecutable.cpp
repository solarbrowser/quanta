/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/FunctionExecutable.h"
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
// Then by the deferred-body state (a token index and the parse context the
// body needs rebuilding in): 16 bytes each, so ~190KB across a bundle's worth
// of literals, against the megabytes of parse tree those literals stop
// holding. See defer_body. The body's start position is here for the same
// trade in the same direction: eight more bytes per executable, so that
// nothing has to keep a whole tree alive just to say what line a function is
// on. And by where the literal's own text sits, for the same trade again:
// four bytes each against the copy of that text every declaration used to
// carry for a toString() almost none of them receives.
static_assert(sizeof(FunctionExecutable) == 224);
#else
static_assert(sizeof(FunctionExecutable) <= 240);
#endif

constinit thread_local FunctionExecutable* FunctionExecutable::live_head_ = nullptr;

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
        // A suspendable function's parameter defaults are chunks of their own,
        // reached from nothing else.
        for (const auto& p : exe->parameter_objects) {
            if (!p) continue;
            if (p->default_chunk()) p->default_chunk()->trace(v);
            if (p->pattern_chunk()) p->pattern_chunk()->trace(v);
        }
    }
}

// Both body setters funnel through here: the directive is a fact about the
// tree, so it is read while the tree is certainly in hand rather than on the
// first call, which is what used to make Function::call reach for it.
static bool opens_with_use_strict(const ASTNode* node) {
    return node && node->get_type() == ASTNode::Type::BLOCK_STATEMENT &&
           static_cast<const BlockStatement*>(node)->has_use_strict_directive();
}

void FunctionExecutable::adopt_body_from(std::unique_ptr<ASTNode> node,
                                        const ExecutableRef<ScriptUnit>& unit) {
    adopt_body(std::move(node));
    unit_ = unit;
}

void FunctionExecutable::adopt_body(std::unique_ptr<ASTNode> node) {
    owned_body_ = std::move(node);
    unit_ = ExecutableRef<ScriptUnit>();
    body_ = owned_body_.get();
    if (body_) { body_start_ = body_->get_start(); body_end_offset_ = body_->get_end().offset; }
    body_has_use_strict = opens_with_use_strict(body_);
}

void FunctionExecutable::defer_body(const ExecutableRef<ScriptUnit>& unit,
                                   bool strict, bool is_generator, bool is_async,
                                   bool concise) {
    owned_body_.reset();
    unit_ = unit;
    body_ = nullptr;
    body_deferred_ = true;
    deferred_strict_ = strict;
    deferred_generator_ = is_generator;
    deferred_async_ = is_async;
    deferred_concise_ = concise;
    body_has_use_strict = strict;
}

bool FunctionExecutable::release_rebuilt_body() const {
    if (!owned_body_ || !unit_ || body_end_offset_ <= body_start_.offset) return false;
    owned_body_.reset();
    body_ = nullptr;
    body_deferred_ = true;
    return true;
}

ASTNode* FunctionExecutable::ensure_body() const {
    if (body_) return body_;
    if (!body_is_deferred() || !unit_) return nullptr;
    // Deliberately not adopt_body: that clears unit_, and the unit still backs
    // the source text this executable reports. The tree is owned outright from
    // here on -- the token range has done its job.
    // Lexed back out of the source it came from, using the range recorded when
    // the body was attached. The token stream that produced it does not have to
    // still exist -- which is the point: for a multi-megabyte script it ran to
    // tens of megabytes and was kept for the life of the program.
    auto parsed = unit_->parse_body_from_source(body_start_, body_end_offset_, deferred_strict_,
                                                deferred_generator_, deferred_async_,
                                                deferred_concise_);
    if (!parsed) {
        // Left deferred on purpose. A rebuild that did not finish is not the
        // same thing as a function without a body, and whoever asked has to be
        // able to tell them apart -- clearing the flag here made a failed
        // rebuild look like an empty function, which then quietly returned
        // undefined. Staying deferred also lets a later call try again, which
        // matters because the one way this fails is running out of stack, and
        // how much there is depends on where the call came from.
        return nullptr;
    }
    owned_body_ = std::move(parsed);
    body_ = owned_body_.get();
    if (body_) { body_start_ = body_->get_start(); body_end_offset_ = body_->get_end().offset; }
    body_deferred_ = false;
    return body_;
}

void FunctionExecutable::borrow_body(const ExecutableRef<ScriptUnit>& unit, ASTNode* node) {
    owned_body_.reset();
    unit_ = unit;
    body_ = node;
    if (node) { body_start_ = node->get_start(); body_end_offset_ = node->get_end().offset; }
    body_has_use_strict = opens_with_use_strict(body_);
}

// Cut on demand: a declaration site knows where its own text is, and asking
// for it is rare enough that keeping a copy of every function's source cost
// more than the bytecode compiled out of it.
const std::string& FunctionExecutable::source_text() const {
    if (source_end_ > source_start_ && unit_) {
        source_text_cut_ = unit_->source_range(source_start_, source_end_);
        source_end_ = source_start_;
    }
    return source_text_cut_;
}

const BodyScopeInfo* FunctionExecutable::body_scope_info() const {
    if (!unit_) return nullptr;
    return unit_->scope_info_at(body_start_.offset);
}


// The body a class without a written constructor gets: `constructor() {}`.
// Built per class rather than shared, so the class's own text -- which is what
// its constructor reports -- has somewhere to live.
ExecutableRef<FunctionExecutable> make_default_class_constructor_executable() {
    auto exe = make_executable_ref();
    exe->adopt_body(std::make_unique<BlockStatement>(
        std::vector<std::unique_ptr<ASTNode>>{}, Position{0, 0}, Position{0, 0}));
    return exe;
}

}  // namespace Quanta
