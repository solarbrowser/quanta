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
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/FiberState.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
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
    Heap::for_each_cell([&](void* cell, CellKind kind, bool marked) {
        if (!marked) dead.push_back({cell, kind});
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

void Collector::collect() {
    static const bool verify = env_flag("QUANTA_GC_VERIFY");
    static const bool log = env_flag("QUANTA_GC_LOG");

    Heap::clear_gc_request();
    Heap::clear_all_marks();

    MarkVisitor v;
    scan_stacks(v);

    for (Engine* engine : Engine::all_engines()) {
        v.visit_context(engine->get_global_context());
        for (Context* c : engine->get_survivor_contexts()) v.visit_context(c);
    }
    v.visit_context(Object::current_context_);
    for (Context* c : exec_context_stack()) v.visit_context(c);
    for (const std::vector<Value>* vec : value_vector_roots())
        for (const Value& val : *vec) v.visit(val);
    Symbol::gc_trace_roots(v);
    trace_atomics_gc_roots(v);

    v.drain();

    g_last_cycle = CycleStats{};
    g_last_cycle.marked_cells = v.marked_cells;
    if (verify) run_verify(g_last_cycle);

    static const bool mark_only = env_flag("QUANTA_GC_MARK_ONLY");
    if (!mark_only) g_last_cycle.swept_cells = run_sweep();

    if (log) {
        std::fprintf(stderr, "[gc] marked=%zu swept=%zu verify_violations=%zu\n",
                     g_last_cycle.marked_cells, g_last_cycle.swept_cells,
                     g_last_cycle.verify_violations);
    }
}

void Collector::safepoint() {
    static const bool stress = env_flag("QUANTA_GC_STRESS");
    if (stress || Heap::gc_requested()) collect();
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
