/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_GC_COLLECTOR_H
#define QUANTA_GC_COLLECTOR_H

#include <cstddef>
#include <vector>

namespace Quanta {

class Context;
class Value;

// Per-thread mark(-sweep) collector. Runs only at interpreter safepoints,
// never inside allocation -- a half-constructed cell (vtable not yet
// written) is therefore never traced, only kept alive conservatively.
// A collection spans every heap the calling thread owns: cross-realm edges
// within one thread are ordinary edges, but two threads never scan each
// other's heaps, so no cross-thread coordination is needed.
class Collector {
public:
    // Environment knobs, read once:
    //   QUANTA_GC_STRESS=1   collect at every safepoint
    //   QUANTA_GC_VERIFY=1   after marking, check every marked cell's edges
    //   QUANTA_GC_LOG=1      one summary line per collection to stderr
    //   QUANTA_GC_MARK_ONLY=1  skip the sweep (marking soak-test mode)

    // The interpreter's per-statement hook: collects when requested/stressed.
    static void safepoint();

    // Unconditional collection (gc() builtin, tests).
    static void collect();

    struct CycleStats {
        size_t marked_cells = 0;
        size_t swept_cells = 0;
        size_t verify_violations = 0;
    };
    static const CycleStats& last_cycle();

    // Live JS call frames. Contexts are not cells and only exist as raw
    // pointers on the C++ stack, which the conservative scanner cannot
    // trace through -- every running frame must register itself.
    static void push_exec_context(Context* ctx);
    // Pop by identity, not LIFO: a suspending fiber leaves its frames on the
    // stack while the host keeps pushing, so unwind order is not LIFO.
    static void pop_exec_context(Context* ctx);

    // In-flight temporaries: argument lists and similar Value vectors live in
    // malloc'd std::vector storage that the conservative stack scan cannot
    // reach. A vector registered here is traced (its current data pointer is
    // re-read each collection, so reallocation during push_back is safe).
    static void push_value_vector(const std::vector<Value>* vec);
    static void pop_value_vector(const std::vector<Value>* vec);
};

// RAII: keeps a Value vector reachable for the collector while it is built or
// held on the C++ stack (argument evaluation, etc.).
class ValueVectorRoot {
public:
    explicit ValueVectorRoot(const std::vector<Value>* vec) : vec_(vec) {
        Collector::push_value_vector(vec_);
    }
    ~ValueVectorRoot() { Collector::pop_value_vector(vec_); }
    ValueVectorRoot(const ValueVectorRoot&) = delete;
    ValueVectorRoot& operator=(const ValueVectorRoot&) = delete;

private:
    const std::vector<Value>* vec_;
};

// RAII frame registration for Function::call and friends.
class ExecContextScope {
public:
    explicit ExecContextScope(Context* ctx) : ctx_(ctx) { Collector::push_exec_context(ctx); }
    ~ExecContextScope() { Collector::pop_exec_context(ctx_); }

private:
    Context* ctx_;

public:
    ExecContextScope(const ExecContextScope&) = delete;
    ExecContextScope& operator=(const ExecContextScope&) = delete;
};

}

#endif
