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
#include <vector>

namespace Quanta {

class ASTNode;
class Parameter;
class Visitor;
class FunctionExecutable;

// Lightweight intrusive-refcounted smart pointer for FunctionExecutable --
// replaces std::shared_ptr<FunctionExecutable> (16 bytes: object pointer +
// separate control-block pointer) with a single 8-byte pointer, since every
// FunctionExecutable already has somewhere to keep its own count. Single-
// threaded, non-atomic refcount -- same "main thread only" model as the
// rest of the GC/heap machinery (see Heap's own single-writer comment).
// Deliberately narrow (not a general-purpose utility): only the operations
// Function::executable_/the AST nodes' cached_executable_ fields actually
// use, including the one-way FunctionExecutable -> const FunctionExecutable
// converting constructor those two need between them.
template <typename T>
class ExecutableRef {
public:
    ExecutableRef() = default;
    explicit ExecutableRef(T* p) : ptr_(p) { if (ptr_) ptr_->ref(); }
    ExecutableRef(const ExecutableRef& other) : ptr_(other.ptr_) { if (ptr_) ptr_->ref(); }
    ExecutableRef(ExecutableRef&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    template <typename U>
    ExecutableRef(const ExecutableRef<U>& other) : ptr_(other.get()) { if (ptr_) ptr_->ref(); }
    ~ExecutableRef() { if (ptr_) ptr_->unref(); }

    ExecutableRef& operator=(const ExecutableRef& other) {
        if (other.ptr_) other.ptr_->ref();
        if (ptr_) ptr_->unref();
        ptr_ = other.ptr_;
        return *this;
    }
    ExecutableRef& operator=(ExecutableRef&& other) noexcept {
        if (this != &other) {
            if (ptr_) ptr_->unref();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    bool operator==(std::nullptr_t) const { return ptr_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
    template <typename U> friend class ExecutableRef;
};

inline ExecutableRef<FunctionExecutable> make_executable_ref();

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

    // Backs ExecutableRef<T> above -- intentionally not atomic, see that
    // template's own doc comment.
    void ref() const { ++ref_count_; }
    void unref() const { if (--ref_count_ == 0) delete this; }

    // Field order below is deliberately grouped widest-first (pointers and
    // containers, then 4-byte counters, then the 1-byte flags last) so the
    // small members share one tail block instead of each forcing its own
    // alignment padding before the next 8-byte-aligned field.
    std::unique_ptr<ASTNode> body;
    std::vector<std::unique_ptr<Parameter>> parameter_objects;
    std::vector<std::string> parameters;

    // unique_ptr, not shared_ptr: nothing ever co-owns a chunk. Both are
    // assigned once from a factory that already returns
    // unique_ptr<BytecodeChunk> (BytecodeCompiler::compile /
    // VM::compile_suspendable) and every reader takes a raw
    // `const BytecodeChunk*` out via .get(), so the second control-block
    // pointer shared_ptr carries was pure overhead.
    mutable std::unique_ptr<const BytecodeChunk> bytecode_chunk;

    // Generator/AsyncFunction/AsyncGeneratorFunction's own compiled form
    // (VM::compile_suspendable, not the plain call bytecode above -- it
    // supports mid-function suspend/resume) -- mirrors bytecode_chunk/
    // vm_incompatible exactly, just for the suspendable calling convention.
    // Lazily populated on first call of any instance sharing this executable,
    // idempotent if recomputed (pure function of body), so sharing across
    // every instance from the same decl site is safe. Plain (non-suspendable)
    // Function instances never touch these.
    mutable std::unique_ptr<const BytecodeChunk> suspendable_chunk;

private:
    // Intrusive doubly-linked list of every live executable, replacing the
    // thread_local unordered_set this used to be: the set cost ~33 bytes per
    // executable in node + bucket storage (measured) on top of a malloc per
    // construction and a hash+erase per destruction, all to serve a registry
    // that is only ever walked front-to-back by gc_trace_roots(). Two inline
    // pointers cost 16 bytes and no allocation at all. Doubly linked (not
    // singly) so unlinking in ~FunctionExecutable stays O(1) -- executables
    // die in arbitrary order, not LIFO.
    static thread_local FunctionExecutable* live_head_;
    FunctionExecutable* live_next_ = nullptr;
    FunctionExecutable* live_prev_ = nullptr;

public:
    // Decl-site defaults for Function's own lazy "length"/toString() sources:
    // both are pure functions of body/params (source_text is the literal's own
    // source slice, declared_length is the ES6 spec length -- params before
    // the first rest/default), so every instance sharing this executable
    // wants the identical value. Mutable so callers holding an
    // ExecutableRef<const FunctionExecutable> (every Function instance) can
    // still populate them once. Native functions have no executable at all,
    // so their own per-instance equivalents live in NativeFunctionData
    // instead (see Function::native_data()).
    mutable std::string source_text;

    // Decl-site default name -- almost always identical for every instance
    // sharing this executable (the constructor parameter or the static
    // binding/property-key NamedEvaluation infers), same sharing rationale
    // as source_text. The rare exception (a computed object-literal/class
    // property key whose runtime value differs across separate evaluations
    // of the same literal) is handled by Function's own per-instance
    // InstanceOverrides, not here -- see Function::set_name.
    mutable std::string name;

    // uint32_t, not size_t: this is a formal parameter count (the compiler
    // itself refuses anything past 64 params). set_declared_length()/
    // get_declared_length() still take and return size_t.
    mutable uint32_t declared_length = 0;
    // Class field count for pre-sizing new instances' shape slots -- fixed by
    // the class body, so identical for every evaluation of the same class
    // declaration/expression (only ever set on a class constructor's own
    // executable; plain functions leave this at 0).
    mutable uint32_t construct_slot_hint = 0;

    mutable bool vm_incompatible = false;
    mutable bool suspendable_incompatible = false;

    // -1 unknown, 0 no, 1 yes -- see Function::call_default's own doc
    // comments on the fields these replace.
    mutable int8_t strict_directive_state = -1;
    mutable int8_t closure_props_state = -1;
    mutable int8_t self_name_state = -1;
    mutable int8_t super_marker_state = -1;

    // GC-roots every live executable's compiled chunk, every cycle (minor
    // and major) -- called from Collector.cpp alongside Symbol::gc_trace_roots
    // and trace_atomics_gc_roots, the same "always-live global root" pattern.
    // Necessary despite executable ownership being plain C++ (intrusive)
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
    mutable uint32_t ref_count_ = 0;
};

inline ExecutableRef<FunctionExecutable> make_executable_ref() {
    return ExecutableRef<FunctionExecutable>(new FunctionExecutable());
}

}

#endif
