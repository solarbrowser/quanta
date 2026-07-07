/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/gc/Collector.h"
#include "quanta/core/gc/FiberRegistry.h"
#include "quanta/core/gc/Heap.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/builtins/AtomicsBuiltin.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/FiberState.h"
#include "quanta/core/runtime/MapSet.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <ucontext.h>
#include <unordered_set>
#include <vector>

namespace Quanta {

namespace {

// Marking visitor: worklist-based, no C++ recursion, so arbitrarily deep
// object graphs cannot overflow the stack.
class MarkVisitor final : public Visitor {
public:
    size_t marked_cells = 0;

    void mark_word(uint64_t word) {
        mark(Heap::probe_word(word));
    }

    // Remembered-set entry: already marked in a previous cycle, but its
    // fields changed since -- re-trace without re-marking.
    void push_remembered(const Heap::ProbeResult& p) {
        if (p.kind == CellKind::Object || p.kind == CellKind::String) {
            gray_.push_back(p);
        }
    }

    void visit_object(Object* o) override { if (o) mark(Heap::exact_cell(o)); }
    void visit_string(String* s) override { if (s) mark(Heap::exact_cell(s)); }
    void visit_symbol(Symbol* s) override { if (s) mark(Heap::exact_cell(s)); }
    void visit_bigint(BigInt* b) override { if (b) mark(Heap::exact_cell(b)); }

    void visit_context(Context* ctx) override {
        if (ctx && seen_.insert(ctx).second) context_work_.push_back(ctx);
    }
    void visit_environment(Environment* env) override {
        if (env && seen_.insert(env).second) environment_work_.push_back(env);
    }

    // True once `ctx` has been visited this cycle, as a root or via tracing
    // (e.g. a live Function's closure_context_) -- see run_collection's
    // survivor-pruning step.
    bool context_seen(Context* ctx) const { return seen_.count(ctx) > 0; }

    void visit_weak_map(WeakMap* w) override { if (w) pending_weak_maps_.push_back(w); }
    void visit_weak_set(WeakSet* w) override { if (w) pending_weak_sets_.push_back(w); }
    void visit_weak_ref(WeakRef* w) override { if (w) pending_weak_refs_.push_back(w); }
    void visit_finalization_registry(FinalizationRegistry* r) override {
        if (r) pending_fin_registries_.push_back(r);
    }

    void drain() {
        while (!gray_.empty() || !context_work_.empty() || !environment_work_.empty()) {
            if (!gray_.empty()) {
                Heap::ProbeResult p = gray_.back();
                gray_.pop_back();
                trace_cell(p);
            } else if (!environment_work_.empty()) {
                Environment* e = environment_work_.back();
                environment_work_.pop_back();
                e->gc_trace(*this);
            } else {
                Context* c = context_work_.back();
                context_work_.pop_back();
                c->gc_trace(*this);
            }
        }
    }

    static bool alive(const void* p) {
        if (!p) return false;
        return Heap::test_mark(Heap::exact_cell(p));
    }

    // A WeakMap/WeakSet value is only reachable through a live key; marking
    // it can itself unlock other maps' entries (ephemeron chains), so this
    // iterates to a fixpoint, redraining ordinary edges after each pass.
    void drain_ephemerons() {
        bool progress = true;
        while (progress) {
            progress = false;
            for (WeakMap* wm : pending_weak_maps_) {
                for (auto& e : wm->raw_entries()) {
                    if (!alive(e.first)) continue;
                    size_t before = marked_cells;
                    visit(e.second);
                    if (marked_cells != before) progress = true;
                }
                for (auto& e : wm->raw_symbol_entries()) {
                    if (!alive(e.first)) continue;
                    size_t before = marked_cells;
                    visit(e.second);
                    if (marked_cells != before) progress = true;
                }
            }
            if (progress) drain();
        }
    }

    // Keys/targets still unmarked after the fixpoint are truly dead. Erases
    // dead WeakMap/WeakSet entries (a stale Object* key must not linger --
    // once its memory is reused, pointer-identity lookups would alias a new
    // object), clears dead WeakRef targets, and schedules FinalizationRegistry
    // cleanup jobs for cells whose target died. Must run before sweep frees
    // anything: it reads mark bits, not post-sweep memory.
    void finalize_ephemerons() {
        for (WeakMap* wm : pending_weak_maps_) {
            auto& m = wm->raw_entries();
            for (auto it = m.begin(); it != m.end();) {
                if (!alive(it->first)) it = m.erase(it); else ++it;
            }
            auto& sm = wm->raw_symbol_entries();
            for (auto it = sm.begin(); it != sm.end();) {
                if (!alive(it->first)) it = sm.erase(it); else ++it;
            }
        }
        for (WeakSet* ws : pending_weak_sets_) {
            auto& s = ws->raw_values();
            for (auto it = s.begin(); it != s.end();) {
                if (!alive(*it)) it = s.erase(it); else ++it;
            }
            auto& ss = ws->raw_symbol_values();
            for (auto it = ss.begin(); it != ss.end();) {
                if (!alive(*it)) it = ss.erase(it); else ++it;
            }
        }
        for (WeakRef* wr : pending_weak_refs_) {
            if (!alive(wr->target_object()) && !alive(wr->target_symbol())) wr->clear_target();
        }
        for (FinalizationRegistry* fr : pending_fin_registries_) {
            bool any_cleared = false;
            for (auto& cell : fr->raw_cells()) {
                // Independent of target liveness: a dead token can no longer
                // be matched by unregister(), so its stale pointer is nulled.
                if (cell.token_object && !alive(cell.token_object)) cell.token_object = nullptr;
                if (cell.token_symbol && !alive(cell.token_symbol)) cell.token_symbol = nullptr;

                if (cell.cleared) continue;
                const void* target = cell.target_object
                    ? static_cast<const void*>(cell.target_object)
                    : static_cast<const void*>(cell.target_symbol);
                if (target && !alive(target)) {
                    cell.cleared = true;
                    cell.target_object = nullptr;
                    cell.target_symbol = nullptr;
                    any_cleared = true;
                }
            }
            if (any_cleared) fr->enqueue_cleanup_job();
        }
        pending_weak_maps_.clear();
        pending_weak_sets_.clear();
        pending_weak_refs_.clear();
        pending_fin_registries_.clear();
    }

private:
    void mark(const Heap::ProbeResult& p) {
        if (!p.cell || Heap::test_mark(p)) return;
        Heap::set_mark(p);
        marked_cells++;
        // Symbols and BigInts are leaves; their marking is complete here.
        if (p.kind == CellKind::Object || p.kind == CellKind::String) {
            gray_.push_back(p);
        }
    }

    void trace_cell(const Heap::ProbeResult& p) {
        if (p.kind == CellKind::Object) {
            static_cast<Object*>(p.cell)->trace(*this);
        } else {
            static_cast<String*>(p.cell)->gc_trace(*this);
        }
    }

    std::vector<Heap::ProbeResult> gray_;
    std::vector<Context*> context_work_;
    std::vector<Environment*> environment_work_;
    std::unordered_set<const void*> seen_;

    std::vector<WeakMap*> pending_weak_maps_;
    std::vector<WeakSet*> pending_weak_sets_;
    std::vector<WeakRef*> pending_weak_refs_;
    std::vector<FinalizationRegistry*> pending_fin_registries_;
};

// Post-mark verifier: the single-pass form of the tri-color invariant.
// Every edge out of a marked cell must land on a marked cell; a violation
// means a trace() implementation is missing an edge.
class VerifyVisitor final : public Visitor {
public:
    size_t violations = 0;

    void check_target(void* p) {
        if (!p) return;
        Heap::ProbeResult r = Heap::exact_cell(p);
        if (r.cell && !Heap::test_mark(r)) {
            violations++;
            std::fprintf(stderr, "[gc-verify] marked cell points at unmarked cell %p (kind %d)\n",
                         r.cell, static_cast<int>(r.kind));
        }
    }

    void visit_object(Object* o) override { check_target(o); }
    void visit_string(String* s) override { check_target(s); }
    void visit_symbol(Symbol* s) override { check_target(s); }
    void visit_bigint(BigInt* b) override { check_target(b); }
    // Contexts/environments are not cells; their contents were walked during
    // marking, so the invariant check stops at cell edges.
    void visit_context(Context*) override {}
    void visit_environment(Environment*) override {}
};

// Conservative stack scanning intentionally reads past individual local
// variables' declared bounds -- that's the whole point. ASAN's stack
// redzones flag this as a false-positive overflow; suppress on this pair.
__attribute__((no_sanitize("address")))
void scan_range(MarkVisitor& v, const void* lo, const void* hi) {
    auto a = (reinterpret_cast<uintptr_t>(lo) + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1);
    auto b = reinterpret_cast<uintptr_t>(hi) & ~(sizeof(uint64_t) - 1);
    for (const uint64_t* p = reinterpret_cast<const uint64_t*>(a);
         p < reinterpret_cast<const uint64_t*>(b); p++) {
        if (*p) v.mark_word(*p);
    }
}

void main_thread_stack_bounds(const char** lo, const char** hi) {
    static thread_local const char* cached_lo = nullptr;
    static thread_local const char* cached_hi = nullptr;
    if (!cached_lo) {
        pthread_attr_t attr;
        pthread_getattr_np(pthread_self(), &attr);
        void* addr = nullptr;
        size_t size = 0;
        pthread_attr_getstack(&attr, &addr, &size);
        pthread_attr_destroy(&attr);
        cached_lo = static_cast<const char*>(addr);
        cached_hi = cached_lo + size;
    }
    *lo = cached_lo;
    *hi = cached_hi;
}

__attribute__((no_sanitize("address")))
void scan_stacks(MarkVisitor& v) {
    // Spill callee-saved registers into a scanned stack local.
    ucontext_t spilled_registers;
    getcontext(&spilled_registers);

    const char* main_lo;
    const char* main_hi;
    main_thread_stack_bounds(&main_lo, &main_hi);

    char probe;
    const char* sp = &probe;
    if (sp >= main_lo && sp < main_hi) {
        // Running on the host stack: live region is [sp, top].
        scan_range(v, sp, main_hi);
    } else {
        // Running inside a fiber: the host stack is suspended somewhere at or
        // below its deepest recorded fiber-enter point.
        const char* deepest = main_hi;
        FiberRegistry::for_each_enter_sp([&](const void* esp) {
            const char* c = static_cast<const char*>(esp);
            if (c >= main_lo && c < main_hi && c < deepest) deepest = c;
        });
        scan_range(v, deepest, main_hi);
    }

    // Fiber stacks are zero-initialized vectors: scanning the full buffer is
    // safe, and untouched regions are all zeros (skipped fast). The FiberState
    // pair carries the saved register file of suspended fibers.
    FiberRegistry::for_each([&](const FiberRegistry::Record& rec) {
        scan_range(v, rec.stack_lo, rec.stack_hi);
        if (rec.state) scan_range(v, rec.state, rec.state + 1);
        if (rec.extra_roots) rec.extra_roots(v);
    });
}


thread_local Collector::CycleStats g_last_cycle;

std::vector<Context*>& exec_context_stack() {
    static thread_local std::vector<Context*> stack;
    return stack;
}

std::vector<const std::vector<Value>*>& value_vector_roots() {
    static thread_local std::vector<const std::vector<Value>*> roots;
    return roots;
}

std::vector<Heap::ProbeResult>& remembered_cells() {
    static thread_local std::vector<Heap::ProbeResult> cells;
    return cells;
}

std::vector<Environment*>& remembered_envs() {
    static thread_local std::vector<Environment*> envs;
    return envs;
}

bool env_flag(const char* name) {
    const char* val = std::getenv(name);
    return val && *val && *val != '0';
}

size_t run_sweep() {
    // Two passes: destructors may consult other cells' memory only through
    // their own backing stores (audited rule), but collecting the dead list
    // first keeps the block bitmaps stable while destructors run.
    struct Dead { void* cell; CellKind kind; };
    std::vector<Dead> dead;
    Heap::for_each_dead_cell([&](void* cell, CellKind kind) {
        dead.push_back({cell, kind});
    });
    // QUANTA_GC_POISON=1: fill freed cells with a recognizable pattern and
    // leak the slot instead of reusing it -- any use-after-free then crashes
    // deterministically on the poison instead of silently reading a
    // recycled cell. Debug tool for hunting invisible lambda captures.
    static const bool poison = env_flag("QUANTA_GC_POISON");
    for (const Dead& d : dead) {
        switch (d.kind) {
            case CellKind::Object: static_cast<Object*>(d.cell)->~Object(); break;
            case CellKind::String: static_cast<String*>(d.cell)->~String(); break;
            case CellKind::Symbol: static_cast<Symbol*>(d.cell)->~Symbol(); break;
            case CellKind::BigInt: static_cast<BigInt*>(d.cell)->~BigInt(); break;
            default: break;
        }
        if (poison && BlockAllocator::owns_address(d.cell)) {
            HeapBlock* block = HeapBlock::from_cell(d.cell);
            std::memset(d.cell, 0xD9, block->cell_size());
            block->retire_cell(d.cell);  // slot leaked on purpose, never reused
            continue;
        }
        Heap::cell_free(d.cell);
    }
    return dead.size();
}

void run_verify(Collector::CycleStats& stats) {
    VerifyVisitor check;
    Heap::for_each_cell([&](void* cell, CellKind kind, bool marked) {
        if (!marked) return;
        if (kind == CellKind::Object) static_cast<Object*>(cell)->trace(check);
        else if (kind == CellKind::String) static_cast<String*>(cell)->gc_trace(check);
    });
    stats.verify_violations = check.violations;
}

}

namespace {

void run_collection(bool minor) {
    static const bool verify = env_flag("QUANTA_GC_VERIFY");
    static const bool log = env_flag("QUANTA_GC_LOG");

    Heap::clear_gc_request();
    if (!minor) Heap::clear_all_marks();

    static const bool prof = env_flag("QUANTA_GC_PROFILE");
    auto t0 = std::chrono::steady_clock::now();

    MarkVisitor v;
    if (minor) {
        // Old cells/environments mutated since the last cycle: their new
        // edges may be the only path to young cells.
        for (const Heap::ProbeResult& p : remembered_cells()) v.push_remembered(p);
        for (Environment* e : remembered_envs()) v.visit_environment(e);
        // See FiberRegistry::Record::owner_cell for why these are minor roots.
        FiberRegistry::for_each([&](const FiberRegistry::Record& rec) {
            if (rec.owner_cell) v.visit_object(rec.owner_cell);
        });
    }
    scan_stacks(v);
    auto t1 = std::chrono::steady_clock::now();

    for (Engine* engine : Engine::all_engines()) {
        v.visit_context(engine->get_global_context());
        // A minor cycle doesn't retrace old, unmutated cells, so a survivor
        // only reachable through one wouldn't be rediscovered this cycle --
        // keep rooting all of them here unconditionally. Majors clear all
        // marks and retrace everything, so "not reached" there is precise
        // (see the reachability-based prune below instead).
        if (minor) {
            for (Context* c : engine->get_survivor_contexts()) v.visit_context(c);
        }
    }
    v.visit_context(Object::current_context_);
    for (Context* c : exec_context_stack()) v.visit_context(c);
    for (const std::vector<Value>* vec : value_vector_roots())
        for (const Value& val : *vec) v.visit(val);
    Symbol::gc_trace_roots(v);
    trace_atomics_gc_roots(v);
    auto t2 = std::chrono::steady_clock::now();

    v.drain();

    // Reachability-based survivor prune (major cycles only -- see above).
    // Function::call hands every call's Context to the survivor pool
    // unconditionally on the chance a closure captured it; most don't. A
    // survivor already reached above is genuinely still needed; one
    // EventLoop still has a pending timer/Promise/fiber against gets force-
    // traced (nothing else would mark its subgraph) and kept; anything else
    // is unreachable and deleted.
    if (!minor) {
        bool any_forced = false;
        for (Engine* engine : Engine::all_engines()) {
            std::vector<Context*>& survivors = engine->mutable_survivor_contexts();
            std::vector<Context*> still_alive;
            still_alive.reserve(survivors.size());
            for (Context* ctx : survivors) {
                if (v.context_seen(ctx)) {
                    still_alive.push_back(ctx);
                } else if (EventLoop::instance().is_context_in_use(ctx)) {
                    v.visit_context(ctx);
                    still_alive.push_back(ctx);
                    any_forced = true;
                } else {
                    delete ctx;
                }
            }
            survivors = std::move(still_alive);
        }
        if (any_forced) v.drain();  // trace the force-kept ones' own subgraph
    }
    auto t3 = std::chrono::steady_clock::now();
    v.drain_ephemerons();
    v.finalize_ephemerons();
    auto t4 = std::chrono::steady_clock::now();

    g_last_cycle = Collector::CycleStats{};
    g_last_cycle.minor = minor;
    g_last_cycle.marked_cells = v.marked_cells;
    if (verify) run_verify(g_last_cycle);
    auto t5 = std::chrono::steady_clock::now();

    // Must run before sweep/decommit below: a fully-dead block can be
    // decommitted (madvise'd, pages zeroed) once swept, and a stale entry
    // here pointing into it would then read cell_size back as 0 --
    // slot_index's offset/cell_size divides by that and traps (SIGFPE).
    for (const Heap::ProbeResult& p : remembered_cells()) Heap::clear_remembered(p);
    remembered_cells().clear();
    for (Environment* e : remembered_envs()) e->gc_remembered_ = false;
    remembered_envs().clear();

    static const bool mark_only = env_flag("QUANTA_GC_MARK_ONLY");
    if (!mark_only) {
        g_last_cycle.swept_cells = run_sweep();
        Heap::rebuild_allocation_candidates();
        // Full idle-chunk scan: major-only, not part of the minor pause budget.
        if (!minor) Heap::decommit_idle_memory();
    }
    auto t6 = std::chrono::steady_clock::now();

    if (prof) {
        auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
        std::fprintf(stderr, "[gc-prof] %s scan_stacks=%ldus roots=%ldus drain=%ldus ephemeron=%ldus verify=%ldus sweep+rebuild=%ldus marked=%zu\n",
                     minor ? "minor" : "major", us(t0,t1), us(t1,t2), us(t2,t3), us(t3,t4), us(t4,t5), us(t5,t6), v.marked_cells);
    }

    if (log) {
        std::fprintf(stderr, "[gc] %s marked=%zu swept=%zu verify_violations=%zu\n",
                     minor ? "minor" : "major",
                     g_last_cycle.marked_cells, g_last_cycle.swept_cells,
                     g_last_cycle.verify_violations);
    }
}

}

namespace {

// QUANTA_GC_NO_BARRIER=1: perf-diagnosis knob. With barriers off, the
// safepoint policy runs full collections only -- minors would miscollect.
bool barriers_disabled() {
    static const bool disabled = env_flag("QUANTA_GC_NO_BARRIER");
    return disabled;
}

}

void Collector::collect() {
    run_collection(false);
}

void Collector::collect_minor() {
    run_collection(!barriers_disabled());
}

void Collector::write_barrier(const void* cell) {
    if (barriers_disabled() || !cell) return;
    Heap::ProbeResult p = Heap::exact_cell(cell);
    // Young (or non-cell) targets need no record: a minor trace reaches every
    // live young cell from the roots; only marked-old cells go dark.
    if (!p.cell || !Heap::test_mark(p)) return;
    if (Heap::test_and_set_remembered(p)) return;
    remembered_cells().push_back(p);
}

void Collector::write_barrier_env(Environment* env) {
    if (barriers_disabled() || !env || env->gc_remembered_) return;
    env->gc_remembered_ = true;
    remembered_envs().push_back(env);
}

void Collector::safepoint() {
    // QUANTA_GC_STRESS: "2" = minor at every safepoint (write-barrier soak,
    // full every 64th); any other truthy value = full at every safepoint.
    static const int stress = [] {
        const char* v = std::getenv("QUANTA_GC_STRESS");
        if (!v || !*v || *v == '0') return 0;
        return *v == '2' ? 2 : 1;
    }();
    static thread_local uint32_t cycle_count = 0;

    // Survivor contexts can only be pruned by a major (see run_collection).
    // Their growth feeds gc_requested()'s budget directly (see
    // Engine::add_survivor_context/Heap::note_extra_bytes), so a call-heavy
    // workload earns proportionally more frequent majors, no fixed threshold.
    if (Heap::major_gc_requested()) {
        Heap::clear_major_gc_request();
        Heap::clear_gc_request();  // consumed together, run_collection clears it too
        run_collection(false);
        return;
    }

    if (stress == 1) {
        run_collection(false);
        return;
    }
    if (stress == 2) {
        run_collection(++cycle_count % 64 != 0);
        return;
    }
    if (Heap::gc_requested()) {
        // Every 8th requested collection is a full one: minors never reclaim
        // old-generation garbage, so majors must keep coming.
        run_collection(!barriers_disabled() && ++cycle_count % 8 != 0);
    }
}

const Collector::CycleStats& Collector::last_cycle() {
    return g_last_cycle;
}

void Collector::push_exec_context(Context* ctx) {
    exec_context_stack().push_back(ctx);
}

void Collector::push_value_vector(const std::vector<Value>* vec) {
    value_vector_roots().push_back(vec);
}

void Collector::pop_value_vector(const std::vector<Value>* vec) {
    auto& roots = value_vector_roots();
    // Almost always LIFO (a call returns before its caller); fast-path the back.
    if (!roots.empty() && roots.back() == vec) { roots.pop_back(); return; }
    for (size_t i = roots.size(); i-- > 0;) {
        if (roots[i] == vec) { roots.erase(roots.begin() + i); return; }
    }
}

void Collector::pop_exec_context(Context* ctx) {
    auto& stack = exec_context_stack();
    if (!stack.empty() && stack.back() == ctx) { stack.pop_back(); return; }
    for (size_t i = stack.size(); i-- > 0;) {
        if (stack[i] == ctx) { stack.erase(stack.begin() + i); return; }
    }
}

}
