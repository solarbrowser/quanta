/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_FUNCTION_EXECUTABLE_H
#define QUANTA_FUNCTION_EXECUTABLE_H

#include "quanta/core/vm/Bytecode.h"
#include "quanta/lexer/Token.h"
#include <memory>
#include <string>
#include <vector>

namespace Quanta {

class ASTNode;
class ScriptUnit;
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

private:
    // mutable for the same reason bytecode_chunk and the *_state fields are:
    // a deferred body is populated on first use and the result is a pure
    // function of what was already recorded, so materializing it does not
    // change what this executable means.
    mutable std::unique_ptr<ASTNode> owned_body_;  // set only by adopt_body/ensure_body
    ExecutableRef<ScriptUnit> unit_;           // set only by borrow_body
    mutable ASTNode* body_ = nullptr;
    // Where the body ENDS in unit_'s source. Its start is body_start_ below,
    // recorded for its own reasons; the two together are what a deferred body
    // is rebuilt from. This used to be an index into the unit's token stream,
    // which meant that stream had to be kept for the life of the program --
    // tens of megabytes for a large script, against the few bytes the source
    // range costs.
    mutable uint32_t body_end_offset_ = 0;
    // Where the body begins in the source. Recorded when the body is attached
    // rather than read back off it, so a stack frame can say where a function
    // is without the tree still being there to ask -- which is the one thing
    // that kept every compiled body's AST alive for the whole run.
    mutable Position body_start_{1, 1, 0};

public:
    // Backs ExecutableRef<T> above -- intentionally not atomic, see that
    // template's own doc comment.
    void ref() const { ++ref_count_; }
    void unref() const { if (--ref_count_ == 0) delete this; }

    // Field order below is deliberately grouped widest-first (pointers and
    // containers, then 4-byte counters, then the 1-byte flags last) so the
    // small members share one tail block instead of each forcing its own
    // alignment padding before the next 8-byte-aligned field.
    // The body is reached through body(). It is either owned outright
    // (adopt_body, for a tree nobody else keeps) or borrowed from the
    // ScriptUnit held alongside it (borrow_body). That is why the pointer is
    // private: there is no way to aim this at a node without also saying what
    // keeps that node alive, so a later eval-like path cannot quietly leave a
    // dangling body behind.
    void adopt_body(std::unique_ptr<ASTNode> node);
    void borrow_body(const ExecutableRef<ScriptUnit>& unit, ASTNode* node);
    // Third form: keep the unit and the source range the body occupies in it,
    // but not the body. Only ever used for a LEAF body -- one holding no
    // nested literal -- because materializing rebuilds the subtree, and a
    // rebuilt inner literal would be a different node from the one its
    // executable is cached on.
    // The range comes from the body that is being dropped: adopt_body and
    // borrow_body both record it, so it is already known by the time anyone
    // decides to let the tree go.
    void defer_body(const ExecutableRef<ScriptUnit>& unit,
                    bool strict, bool is_generator, bool is_async);
    // Reads the body, parsing it back from the unit's tokens if it was
    // deferred. Every consumer that needs a tree must come through here;
    // body() stays the raw accessor for the paths that only test for one.
    ASTNode* ensure_body() const;
    ASTNode* body() const { return body_; }
    const Position& body_start() const { return body_start_; }
    bool has_body() const { return body_ != nullptr; }
    bool body_is_deferred() const { return body_deferred_ && body_ == nullptr; }

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
    // Intrusive list of every live executable, walked front-to-back by
    // gc_trace_roots(). Two inline pointers and no allocation, where a hash
    // set would cost a node plus a malloc per construction and a hash+erase
    // per destruction. Doubly linked so unlinking stays O(1): executables die
    // in arbitrary order, not LIFO.
    static constinit thread_local FunctionExecutable* live_head_;
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
    // Set together with the token range below, describing the context the body
    // was originally parsed in -- a body's grammar depends on it, since yield
    // and await are identifiers or operators according to the enclosing kind.
    bool deferred_strict_ = false;
    bool deferred_generator_ = false;
    bool deferred_async_ = false;
    mutable bool body_deferred_ = false;
    // Whether the body opens with a "use strict" directive. Read once, at the
    // moment the body is attached, so that resolving strict_directive_state
    // below never has to reach for the tree -- which is the whole point: a
    // ScriptUnit's tree is kept alive by any executable still needing it, so a
    // claim that outlives compilation keeps the entire parse tree resident.
    bool body_has_use_strict = false;

    // -1 unknown, 0 no, 1 yes -- see Function::call_default's own doc
    // comments on the fields these replace.
    mutable int8_t strict_directive_state = -1;
    mutable int8_t closure_props_state = -1;
    mutable int8_t self_name_state = -1;
    mutable int8_t super_marker_state = -1;
    // Whether this body can observe its own `arguments` object. The register
    // path already asks the chunk (needs_arguments); the tree-walker had no
    // equivalent and materialized one on every call, so a function that never
    // names `arguments` still paid for an Arguments object, its butterfly and
    // the descriptor map that retyping `length` forces.
    mutable int8_t needs_arguments_state = -1;

    // Every term of Function::call_default's register-mode gate that lives on
    // this executable, ANDed once. The gate used to read six fields across two
    // objects on every call, following a pointer into the chunk to ask one
    // question of it; this answers all of them from the line the executable is
    // already on. Recomputed by every write to an input rather than cached on
    // the calling Function: the inputs are shared by every instance of this
    // declaration site, so an instance-side cache would go stale when a
    // sibling changed one.
    mutable bool fast_gate = false;
    // Read on the register-mode path immediately after the gate, and both were
    // reached by chasing a pointer for a single bit: the strict flag one load
    // into the executable, uses_this two -- executable, then chunk. Cached
    // beside the gate so that path loads the executable once and answers all
    // three from the same line. Maintained here rather than at the reads for
    // the same reason the gate is: every write to an input already recomputes.
    mutable bool fast_strict = false;
    mutable bool fast_uses_this = false;
    // The same question for the functions the compiler put in environment
    // mode. They cannot use the register-mode path -- their bindings live in a
    // real Environment, which the context has to own -- but everything the
    // general path does around that is decl-site constant, so it is answered
    // here once instead of per call.
    mutable bool fast_env_gate = false;
    void recompute_fast_gate() const {
        fast_gate = !vm_incompatible && bytecode_chunk && !bytecode_chunk->env_mode &&
                    strict_directive_state >= 0 && closure_props_state == 0 &&
                    (self_name_state == 0 || self_name_state == 2);
        fast_env_gate = !vm_incompatible && bytecode_chunk && bytecode_chunk->env_mode &&
                        !bytecode_chunk->needs_arguments && strict_directive_state >= 0 &&
                        closure_props_state == 0 &&
                        (self_name_state == 0 || self_name_state == 2);
        fast_strict = strict_directive_state == 1;
        fast_uses_this = bytecode_chunk && bytecode_chunk->uses_this;
    }

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

// Everything instantiating a function literal needs that is fixed by the
// literal's own source text: computed once per decl site, cached on the AST
// node, shared by every instantiation. The other half of instantiation reads
// the Context (the enclosing `this`/new.target an arrow captures, strict-mode
// inheritance, the self-reference Environment a named function expression
// gets) and stays in instantiate_closure.
//
// This is what Op::CreateClosure holds instead of an ASTNode*, so a compiled
// function no longer needs its own AST alive just to build its closures
// (V8's CreateClosure reads a SharedFunctionInfo the same way).
struct ClosureTemplate {
    // Which literal form this stands in for. The four differ in small fixed
    // ways -- only the declaration and the sync arrow link Function.prototype
    // explicitly, only a function expression installs the strict
    // caller/arguments throwers, only the sync arrow captures new.target --
    // so the form is carried rather than unified. Merging them would be a
    // behavior change wearing a refactor's clothes.
    enum class Form : uint8_t { FunctionExpr, Arrow, AsyncFunctionExpr, Declaration };

    ExecutableRef<FunctionExecutable> executable;
    std::string name;
    Form form = Form::FunctionExpr;
    bool is_async = false;
    bool is_generator = false;
    bool is_arrow = false;
    bool is_method_shorthand = false;  // spec 14.3.9: no .prototype, not a constructor
    bool needs_self_binding = false;   // named function expression: immutable self-reference
    bool needs_outer_env = true;       // closure_needs_outer_environment, or the
                                       // unconditional pin the other forms always took
    bool body_is_strict = false;       // "use strict" directive in the body itself
    bool has_direct_eval = false;
    uint32_t declared_length = 0;
};

}

#endif
