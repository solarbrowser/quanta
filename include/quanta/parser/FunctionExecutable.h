/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_FUNCTION_EXECUTABLE_H
#define QUANTA_FUNCTION_EXECUTABLE_H

#include "quanta/core/vm/Bytecode.h"
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace Quanta {

class ASTNode;
class Parameter;
class Visitor;

// Shared, decl-site-scoped data for every Function instance built from one
// function-literal/declaration/method AST node -- body/parameter_objects/
// parameters never change after construction (a durable clone made once,
// not borrowed: Program trees, and eval/new Function() parses, are
// destroyed the moment the statement that ran them returns, so nothing
// else keeps a borrowed subtree alive). bytecode_chunk/vm_incompatible/the
// four *_state fields are lazily populated at most once (on first CALL of
// any instance sharing this executable) and idempotent if ever recomputed
// (pure functions of body/params), so sharing them across every Function
// instance from the same site is safe even though they're written after
// construction. Nested function literals inside this body get their own
// separate FunctionExecutable, cached directly on their own AST node (see
// FunctionExpression::cached_executable_) -- so unlike the old per-outer-
// instance nested_chunk_cache_ this replaced, there's nothing to cache here
// about them.
class FunctionExecutable {
public:
    FunctionExecutable();
    ~FunctionExecutable();
    FunctionExecutable(const FunctionExecutable&) = delete;
    FunctionExecutable& operator=(const FunctionExecutable&) = delete;

    std::unique_ptr<ASTNode> body;
    std::vector<std::unique_ptr<Parameter>> parameter_objects;
    std::vector<std::string> parameters;

    mutable std::shared_ptr<const BytecodeChunk> bytecode_chunk;
    mutable bool vm_incompatible = false;

    // -1 unknown, 0 no, 1 yes -- see Function::call_default's own doc
    // comments on the fields these replace.
    mutable int8_t strict_directive_state = -1;
    mutable int8_t closure_props_state = -1;
    mutable int8_t self_name_state = -1;
    mutable int8_t super_marker_state = -1;

    // GC-roots every live executable's compiled chunk, every cycle (minor
    // and major) -- called from Collector.cpp alongside Symbol::gc_trace_roots
    // and trace_atomics_gc_roots, the same "always-live global root" pattern.
    // Necessary despite executable ownership being plain C++ shared_ptr
    // refcounting, not GC-managed: an executable can outlive every Function
    // instance that ever shared it (cached forever on its owning AST node),
    // and bytecode_chunk's embedded constants are GC-managed cells that
    // would otherwise only be traced through some currently-LIVE Function
    // instance's trace(). If every sharer dies while the executable itself
    // survives, nothing would mark those constants and they'd be swept out
    // from under a still-alive, still-referencing chunk the next time some
    // new instance reuses it -- exactly the dangling-constant corruption
    // this registry prevents.
    static void gc_trace_roots(Visitor& v);

private:
    static thread_local std::unordered_set<FunctionExecutable*> live_executables_;
};

}

#endif
