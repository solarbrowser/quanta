/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_GC_COLLECTOR_H
#define QUANTA_GC_COLLECTOR_H

#include "quanta/core/gc/Heap.h"
#include "quanta/core/vm/FixedArray.h"
#include <chrono>
#include <cstddef>
#include <vector>

namespace Quanta {

class Context;
class Environment;
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
    //   QUANTA_GC_STRESS=1   full collection at every safepoint
    //   QUANTA_GC_STRESS=2   minor collection at every safepoint (barrier soak)
    //   QUANTA_GC_VERIFY=1   after marking, check every marked cell's edges
    //   QUANTA_GC_LOG=1      one summary line per collection to stderr
    //   QUANTA_GC_MARK_ONLY=1  skip the sweep (marking soak-test mode)
    //   QUANTA_GC_PROFILE=1  per-phase timing breakdown to stderr

    // The interpreter's per-back-edge hook: collects when requested/stressed.
    //
    // The armed test is inline and the work is not. Out of line, the common
    // "nothing to do" case was still a call, and a call tells the compiler
    // every caller-saved register is clobbered -- which spilled the dispatch
    // loop's hot state to the stack once per loop iteration.
    //
    // Deliberately reads the same variables safepoint_slow() branches on
    // rather than a derived "armed" flag: a second copy of this state would
    // have to be updated at every site that arms or disarms one of them, and
    // an undercount there means a collection that silently never runs.
    static void safepoint() {
        if (Heap::gc_requested() || Heap::major_gc_requested() ||
            major_in_progress_ || stress_mode_ != 0) {
            safepoint_slow();
        }
    }
    static void safepoint_slow();

    // True between an incremental major cycle's first slice and its last;
    // read directly by safepoint() above and by the barriers in Collector.cpp.
    static thread_local bool major_in_progress_;

    // QUANTA_GC_STRESS, resolved on the first safepoint_slow(). Starts at -1
    // ("not resolved yet"), which reads as armed, so the inline test above
    // can never skip a stress-mode collection before the value is known and
    // needs no function-local static guard of its own.
    static int stress_mode_;

    // Unconditional full collection (gc() builtin, tests).
    static void collect();

    // Sticky mark-bit minor collection: marks survive from previous cycles,
    // so only unmarked (young) cells are traced and swept; old cells mutated
    // since the last cycle re-enter the trace via the remembered sets.
    static void collect_minor();

    enum class SliceResult { Continuing, CycleComplete };

    // One bounded unit of the shared, cycle-lived MarkVisitor's work: drains
    // fully to quiescence if `budget` is negative, or stops and returns
    // Continuing once `budget` has elapsed. Never interrupts mid-Context/
    // Environment resolution, only between fully-quiesced points.
    static SliceResult mark_step(std::chrono::microseconds budget);

    // True between a major cycle's first slice and its last (quiescence).
    // No minor collection runs while true.
    static bool major_in_progress() { return major_in_progress_; }

    // Records `cell` (base address of a live cell) as mutated. Needed on
    // every post-construction write of a traced field or property slot;
    // no-op for young cells since a minor trace reaches them anyway.
    static void write_barrier(const void* cell);
    // Same for environments (not cells; flag-deduped per cycle).
    static void write_barrier_env(Environment* env);
    // Frees a popped, unescaped block environment. Deferred, not immediate:
    // the remembered set may still reference it until the cycle's cleanup;
    // a size threshold flushes even on GC-quiet workloads.
    static void release_env(Environment* env);

    struct CycleStats {
        size_t marked_cells = 0;
        size_t swept_cells = 0;
        size_t verify_violations = 0;
        bool minor = false;
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

    // Same idea, for a frozen BytecodeChunk::constants -- used where the
    // chunk itself isn't reachable via any owning GC object graph node at
    // the point it's used (VM::run_script's raw top-level chunk, which
    // isn't wrapped in a Function/FunctionExecutable the way every other
    // chunk is). Unlike push_value_vector's target, a FixedArray never
    // reallocates after BytecodeCompiler freezes it, but it's still
    // re-scanned each collection the same way, for symmetry/simplicity.
    static void push_value_array(const FixedArray<Value>* arr);
    static void pop_value_array(const FixedArray<Value>* arr);
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

// Same as ValueVectorRoot, for a FixedArray<Value> (BytecodeChunk::constants).
class ValueArrayRoot {
public:
    explicit ValueArrayRoot(const FixedArray<Value>* arr) : arr_(arr) {
        Collector::push_value_array(arr_);
    }
    ~ValueArrayRoot() { Collector::pop_value_array(arr_); }
    ValueArrayRoot(const ValueArrayRoot&) = delete;
    ValueArrayRoot& operator=(const ValueArrayRoot&) = delete;

private:
    const FixedArray<Value>* arr_;
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
