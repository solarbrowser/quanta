/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/gc/Collector.h"
#include "quanta/core/gc/FiberRegistry.h"
#include "quanta/core/runtime/RegExp.h"
#include "quanta/core/gc/Heap.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/SmallMapPool.h"
#include "quanta/core/engine/builtins/AtomicsBuiltin.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/Async.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/DataView.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/FiberState.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/core/runtime/DOMObject.h"
#include "quanta/core/runtime/Generator.h"
#include "quanta/core/modules/ModuleLoader.h"
#include "quanta/parser/FunctionExecutable.h"
#include "quanta/core/runtime/MapSet.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/ObjectTrace.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/TypedArray.h"
#include <algorithm>
#include <atomic>
#include <mimalloc.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // (windows.h defines min and max, which std::min and std::max are not)
#endif
#include <windows.h>
#else
#include <pthread.h>
#ifdef __APPLE__
// Apple's SDK hard-errors on the POSIX-deprecated ucontext routines unless
// this is defined; glibc needs no such guard.
#define _XOPEN_SOURCE 600
#endif
#include <ucontext.h>
#endif

// How far ahead of the trace the mark pipeline runs. Two lines are requested
// per entry (the cell and its storage block), so the ring's depth is half the
// number of misses it keeps in flight; measured best at 16 on a core with 24
// outstanding-miss slots, and measurably worse at 32, where the pipeline asks
// for more than the core can track.
#define QUANTA_GC_PREFETCH_RING 16u

// The pause of a spin-wait, for the architecture the collector is built for.
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
#define QUANTA_CPU_RELAX() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define QUANTA_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
#define QUANTA_CPU_RELAX() std::this_thread::yield()
#endif
static inline void cpu_relax() { QUANTA_CPU_RELAX(); }

namespace Quanta {

constinit thread_local bool Collector::major_in_progress_ = false;
constinit thread_local uint64_t Collector::sweep_generation_ = 0;
int Collector::stress_mode_ = -1;  // -1: not resolved yet, reads as armed

namespace {

// Marking visitor: worklist-based, no C++ recursion, so arbitrarily deep
// object graphs cannot overflow the stack.
// Defined below, next to env_flag; declared here for MarkVisitor and
// release_env, both of which sit above it.
// Bumped by every major cycle that opens, i.e. by every clear_all_marks. A
// Context survivor not stamped with the current value has not been reached
// through a real edge by any collection since every mark bit in the heap was
// cleared, so no marked cell is known to point at it -- see
// run_minor_collection's survivor prune.
constinit thread_local uint8_t g_major_epoch = 1;
// What the last minor and the last major marked: the guess of how much work the next one has (see MarkVisitor::should_parallelize).
constinit thread_local size_t g_expect_minor = 0;
constinit thread_local size_t g_expect_major = 0;
// QUANTA_GC_PROFILE: what the helper threads did (rounds, cells each of them marked, cells handed back to the collecting thread, work left when a round ended).
constinit thread_local size_t g_par_rounds = 0, g_par_left = 0, g_par_deferred = 0, g_par_maxfront = 0;
constinit thread_local size_t g_par_marked[8] = {};
constinit thread_local size_t g_par_deferred_type[48] = {};
constinit thread_local uint64_t g_env_ns = 0, g_ctx_ns = 0, g_env_n = 0, g_ctx_n = 0;
const bool g_trace_all_envs = std::getenv("QUANTA_GC_TRACE_ALL_ENVS") != nullptr;
const bool g_prof_cost = std::getenv("QUANTA_GC_PROFILE_COST") != nullptr;

} // anonymous namespace

uint8_t Collector::current_major_epoch() { return g_major_epoch; }

namespace {


// ---- Marking on several threads ----
//
// The mutator is stopped for as long as a slice of marking runs (the slice is run by the mutator's own thread, at a safepoint), so what the tracers read does not change
// under them; what they share is the mark bits (set with an atomic or) and the work. A helper traces the cells whose trace only reads their own storage and names other
// cells (ordinary objects, arrays, strings): any other cell (a function, a map, a host object...) is marked by the helper that reaches it and handed back to the collecting
// thread, which traces it itself, since its trace may reach contexts and environments, which are not cells and have no mark bit to be shared.
class ParMarker final : public Visitor {
public:
    std::vector<Heap::ProbeResult> local;     // cells marked, to be traced
    std::vector<Heap::ProbeResult> deferred;  // cells marked, to be traced by the collecting thread
    std::vector<Context*> contexts;
    std::vector<Environment*> environments;
    std::vector<WeakMap*> weak_maps;
    std::vector<WeakSet*> weak_sets;
    std::vector<WeakRef*> weak_refs;
    std::vector<FinalizationRegistry*> registries;
    size_t marked = 0;
    bool clear_remembered = false;

    // Reserved by the thread that makes the marker, so the helpers do not allocate in their rounds: what a thread allocates and another frees stays with the allocating thread's
    // heap until it takes it back, and a helper asleep between rounds never does.
    ParMarker() {
        local.reserve(16384);
        deferred.reserve(4096);
    }

    void reset() {
        local.clear(); deferred.clear(); contexts.clear(); environments.clear();
        weak_maps.clear(); weak_sets.clear(); weak_refs.clear(); registries.clear();
        marked = 0;
        ring_head = ring_tail = ring_count = 0;
    }

    [[gnu::always_inline]] void edge_object(Object* o) {
        Heap::ProbeResult r = Heap::mark_exact_par(o, CellKind::Object);
        if (!r.cell) return;
        marked++;
        local.push_back(r);
    }
    [[gnu::always_inline]] void edge_string(String* s) {
        Heap::ProbeResult r = Heap::mark_string_edge_par(s);
        if (!r.cell) return;
        marked++;
        if (r.kind == CellKind::String) local.push_back(r);
    }
    [[gnu::always_inline]] void edge_value(const Value& v) {
        switch (v.gc_edge_kind()) {
            case 0: return;
            case 1: edge_object(v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object()); return;
            case 2: edge_string(v.as_string()); return;
            case 3: leaf(v.as_symbol(), CellKind::Symbol); return;
            default: leaf(v.as_bigint(), CellKind::BigInt); return;
        }
    }
    void visit_object(Object* o) override { edge_object(o); }
    void visit_string(String* s) override { edge_string(s); }
    void visit_symbol(Symbol* s) override { leaf(s, CellKind::Symbol); }
    void visit_bigint(BigInt* b) override { leaf(b, CellKind::BigInt); }
    void visit_context(Context* ctx) override { if (ctx) contexts.push_back(ctx); }
    void visit_environment(Environment* env) override { if (env) environments.push_back(env); }
    [[gnu::always_inline]] void edge_environment(Environment* env) { if (env) environments.push_back(env); }
    void visit_weak_map(WeakMap* w) override { if (w) weak_maps.push_back(w); }
    void visit_weak_set(WeakSet* w) override { if (w) weak_sets.push_back(w); }
    void visit_weak_ref(WeakRef* w) override { if (w) weak_refs.push_back(w); }
    void visit_finalization_registry(FinalizationRegistry* r) override { if (r) registries.push_back(r); }

    // The same two-stage prefetch ring the collecting thread's own marking uses (see MarkVisitor::step): without it every cell is a dependent miss to memory.
    static constexpr uint32_t kRing = QUANTA_GC_PREFETCH_RING;
    Heap::ProbeResult ring[kRing];
    uint32_t ring_head = 0, ring_tail = 0, ring_count = 0;

    bool step() {
        while (ring_count < kRing && !local.empty()) {
            Heap::ProbeResult q = local.back();
            local.pop_back();
            __builtin_prefetch(q.cell, 0, 3);
            ring[ring_tail] = q;
            ring_tail = (ring_tail + 1) & (kRing - 1);
            ring_count++;
        }
        if (ring_count > kRing / 2) {
            const Heap::ProbeResult& q = ring[(ring_head + kRing / 2) & (kRing - 1)];
            if (q.kind == CellKind::Object) {
                const void* storage = static_cast<const Object*>(q.cell)->gc_storage_hint();
                if (storage) {
                    __builtin_prefetch(static_cast<const char*>(storage) - 64, 0, 3);
                    __builtin_prefetch(storage, 0, 3);
                }
            }
        }
        if (ring_count == 0) return false;
        const Heap::ProbeResult p = ring[ring_head];
        ring_head = (ring_head + 1) & (kRing - 1);
        ring_count--;
        process(p);
        return true;
    }
    // The cells waiting in the ring are work too: they go back to the list.
    void flush_ring() {
        while (ring_count > 0) {
            local.push_back(ring[ring_head]);
            ring_head = (ring_head + 1) & (kRing - 1);
            ring_count--;
        }
    }

    void process(const Heap::ProbeResult& p) {
        if (clear_remembered) Heap::clear_remembered_par(p);
        if (p.kind == CellKind::Object) {
            Object* o = static_cast<Object*>(p.cell);
            if (__builtin_expect(o->traces_by_default(), 1)) o->trace_fast(*this);
            // A function names its closure's context and environment (queued here for the collecting thread, like any other) and its executable's chunks, which are only read.
            else if (o->get_type() == Object::ObjectType::Function) static_cast<Function*>(o)->trace(*this);
            // A host object's own Visit() only reads its members and marks what they name (that is its contract), so a helper runs it like any other trace.
            else if (o->get_type() == Object::ObjectType::Custom && static_cast<CustomObjectBase*>(o)->get_custom_kind() == CustomObjectBase::CustomKind::Host) static_cast<DOMObject*>(o)->trace(*this);
            else { deferred.push_back(p); deferred_by_type[o->get_type() == Object::ObjectType::Custom ? 32 + (static_cast<size_t>(static_cast<CustomObjectBase*>(o)->get_custom_kind()) & 15) : (static_cast<size_t>(o->get_type()) & 31)]++; }
        } else if (p.kind == CellKind::Environment) {
            static_cast<Environment*>(p.cell)->trace_fast(*this);
        } else {
            static_cast<String*>(p.cell)->gc_trace(*this);
        }
    }
    size_t deferred_by_type[48] = {};

private:
    void leaf(const void* p, CellKind kind) {
        if (Heap::mark_exact_par(p, kind).cell) marked++;
    }
};

class ParallelMarkPool {
public:
    using Clock = std::chrono::steady_clock;

    static ParallelMarkPool& instance() {
        static ParallelMarkPool* pool = new ParallelMarkPool();  // never destroyed: helper threads outlive static destruction
        return *pool;
    }

    // Helper threads besides the collecting one. QUANTA_GC_THREADS=n sets it (0: marking stays on one thread); by default as many as the machine has cores to spare,
    // at most two (on Splay one helper gives +15%, two +26%, five +29%).
    int helpers() const { return helpers_; }

    // Traces `work` on the helpers and the calling thread until it is all traced, or until `deadline` (when there is one), and hands back what is left in `work`
    // and what the helpers have to say in `markers()`. False if the pool was busy (another thread is collecting) and nothing was done.
    bool run(std::vector<Heap::ProbeResult>& work, bool clear_remembered, bool has_deadline, Clock::time_point deadline) {
        std::unique_lock<std::mutex> round(round_mutex_, std::try_to_lock);
        if (!round.owns_lock()) return false;
        ensure_threads();
        finish_trim();
        const int participants = helpers_ + 1;
        for (auto& m : markers_) { m->reset(); m->clear_remembered = clear_remembered; }
        main_heaps_ = Heap::heaps_of_this_thread();
        main_active_ = Heap::active_or_null();
        {
            std::lock_guard<std::mutex> lock(m_);
            shared_.swap(work);
            work.clear();
            shared_count_.store(shared_.size(), std::memory_order_relaxed);
            hungry_.store(0, std::memory_order_relaxed);
            sleepers_.store(0, std::memory_order_relaxed);
            has_deadline_ = has_deadline;
            deadline_ = deadline;
            stop_ = false;
            done_ = false;
            idle_ = 0;
            stop_flag_.store(false, std::memory_order_relaxed);
            idle_waiting_.store(0, std::memory_order_relaxed);
            participants_ = participants;
            finished_ = 0;
            generation_++;
        }
        cv_work_.notify_all();
        worker(*markers_[0]);
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_done_.wait(lock, [&] { return finished_ == helpers_; });
            // What was left when the time was up: the shared list's own buffer is taken over (a copy would hold the whole frontier twice, and it can be hundreds of
            // thousands of cells), and the buffer that comes back in its place is empty.
            work.swap(shared_);
            shared_.clear();
            shared_count_.store(0, std::memory_order_relaxed);
        }
        for (auto& m : markers_) {
            work.insert(work.end(), m->local.begin(), m->local.end());
            m->local.clear();
        }
        return true;
    }

    std::vector<std::unique_ptr<ParMarker>>& markers() { return markers_; }

    // A cycle is over: what capacity a big round grew the helpers' lists to is given back -- by the helpers themselves. A buffer freed by another thread than the one
    // that allocated it is left for its owner to take back, and a helper asleep between rounds never does, so the memory stayed resident however the collecting thread
    // freed it; each helper frees its own lists and hands its idle pages back (mi_collect), in a round of its own that the collecting thread waits for. Only when some list
    // has grown past what is worth keeping.
    void release_excess_capacity() {
        std::unique_lock<std::mutex> round(round_mutex_, std::try_to_lock);
        if (!round.owns_lock() || !started_) return;
        finish_trim();   // (one that was begun at the end of the last major, and nothing has marked since)
        bool big = shared_.capacity() > kWorthTrimming;
        for (auto& m : markers_) big = big || m->local.capacity() > kWorthTrimming || m->deferred.capacity() > kWorthTrimming ||
                                       m->contexts.capacity() > kWorthTrimming || m->environments.capacity() > kWorthTrimming;
        if (!big) return;
        {
            std::lock_guard<std::mutex> lock(m_);
            trim_only_ = true;
            trim_inflight_ = true;
            finished_ = 0;
            generation_++;
        }
        cv_work_.notify_all();
        // The helpers go on by themselves, each in its own thread, and the collecting thread does not wait for them: the end of a major is a pause of the page, and
        // the wait was most of it. The next round (run) or trim (here) finishes this one first. The collecting thread frees its own lists and leaves the allocator's
        // idle pages to Heap::decommit_idle_memory, which hands them back (at most once a second) -- a mi_collect(true) of its own here cost up to 14 ms.
        trim_markers(*markers_[0], /*collect=*/false);
        std::vector<Heap::ProbeResult> fresh;
        fresh.reserve(65536);
        shared_.swap(fresh);
    }

private:
    // Waits for the helpers of a trim that was begun and not waited for (see release_excess_capacity), and ends it. Called with the round mutex held.
    void finish_trim() {
        std::unique_lock<std::mutex> lock(m_);
        if (!trim_inflight_) return;
        cv_done_.wait(lock, [&] { return finished_ == helpers_; });
        trim_only_ = false;
        trim_inflight_ = false;
    }
    bool trim_inflight_ = false;   // (guarded by m_)

    ParallelMarkPool() {
        const char* env = std::getenv("QUANTA_GC_THREADS");
        if (env && *env) {
            helpers_ = std::atoi(env);
        } else {
            const unsigned cores = std::thread::hardware_concurrency();
            helpers_ = cores > 2 ? static_cast<int>(std::min<unsigned>(2, cores - 2)) : 0;
        }
        if (helpers_ < 0) helpers_ = 0;
        if (helpers_ > 15) helpers_ = 15;
        for (int i = 0; i <= helpers_; i++) markers_.push_back(std::make_unique<ParMarker>());
        shared_.reserve(65536);
    }

    void ensure_threads() {
        if (started_) return;
        started_ = true;
        for (int i = 1; i <= helpers_; i++) {
            std::thread([this, i] { helper_main(i); }).detach();
        }
    }

    void helper_main(int id) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_work_.wait(lock, [&] { return generation_ != seen; });
                seen = generation_;
            }
            if (trim_only_) {
                trim_markers(*markers_[static_cast<size_t>(id)]);
            } else {
                Heap::adopt_heaps(main_heaps_, main_active_);
                worker(*markers_[static_cast<size_t>(id)]);
            }
            {
                std::lock_guard<std::mutex> lock(m_);
                finished_++;
            }
            cv_done_.notify_all();
        }
    }

    // One participant: traces what it holds, shares half of a long list with whoever has run dry, takes from the shared list when it has run dry itself, and stops
    // when everyone has (or when the time is up, giving back what it still holds).
    void worker(ParMarker& me) {
        constexpr size_t kBatch = 256;
        unsigned counter = 0;
        for (;;) {
            while (me.step()) {
                ++counter;
                // Someone who has run dry is given the older half of this list (a depth-first list is a stack: its bottom holds the biggest pieces of the graph).
                if ((counter & 7) == 0 && me.local.size() >= 8 && hungry_.load(std::memory_order_relaxed) > 0) {
                    const size_t half = me.local.size() / 2;
                    {
                        std::lock_guard<std::mutex> lock(m_);
                        shared_.insert(shared_.end(), me.local.begin(), me.local.begin() + static_cast<long>(half));
                        shared_count_.store(shared_.size(), std::memory_order_relaxed);
                    }
                    me.local.erase(me.local.begin(), me.local.begin() + static_cast<long>(half));
                    if (sleepers_.load(std::memory_order_relaxed) > 0) cv_work_.notify_all();
                }
                if ((counter & 63) != 0) continue;
                if (stop_flag_.load(std::memory_order_relaxed)) return give_back(me);
                if (has_deadline_ && Clock::now() >= deadline_) { signal_stop(); return give_back(me); }
            }
            // Run dry: look for work for a few microseconds before going to sleep on the lock (waking a sleeper costs more than most of a slice's work).
            hungry_.fetch_add(1, std::memory_order_relaxed);
            for (int spin = 0; spin < 20000; spin++) {
                if (shared_count_.load(std::memory_order_relaxed) > 0 || stop_flag_.load(std::memory_order_relaxed)) break;
                cpu_relax();
            }
            hungry_.fetch_sub(1, std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(m_);
            for (;;) {
                if (stop_) return;
                if (!shared_.empty()) {
                    const size_t take = std::min(shared_.size(), std::max<size_t>(kBatch, shared_.size() / static_cast<size_t>(participants_)));
                    me.local.insert(me.local.end(), shared_.end() - static_cast<long>(take), shared_.end());
                    shared_.resize(shared_.size() - take);
                    shared_count_.store(shared_.size(), std::memory_order_relaxed);
                    break;
                }
                idle_++;
                hungry_.fetch_add(1, std::memory_order_relaxed);
                idle_waiting_.store(idle_, std::memory_order_relaxed);
                if (idle_ == participants_) {
                    done_ = true;
                    stop_ = true;
                    idle_--;
                    hungry_.fetch_sub(1, std::memory_order_relaxed);
                    idle_waiting_.store(idle_, std::memory_order_relaxed);
                    cv_work_.notify_all();
                    return;
                }
                sleepers_.fetch_add(1, std::memory_order_relaxed);
                cv_work_.wait(lock, [&] { return stop_ || !shared_.empty(); });
                sleepers_.fetch_sub(1, std::memory_order_relaxed);
                idle_--;
                hungry_.fetch_sub(1, std::memory_order_relaxed);
                idle_waiting_.store(idle_, std::memory_order_relaxed);
            }
        }
    }

    static constexpr size_t kKeepEntries = 4096;
    // (A trim is a round of its own and walks the allocator's pages: only when a list is past half a megabyte, and only at the end of a major.)
    static constexpr size_t kWorthTrimming = 32768;
    // Run by the thread that owns the lists: frees what is past kKeepEntries, and hands the thread's idle allocator pages back.
    void trim_markers(ParMarker& me, bool collect = true) {
        // Replaced by a list of the size they start with, so that no round has to allocate.
        const auto shed = [](auto& list, size_t keep) {
            if (list.capacity() > keep) {
                std::remove_reference_t<decltype(list)> fresh;
                fresh.reserve(keep);
                list.swap(fresh);
            }
        };
        shed(me.local, 16384); shed(me.deferred, 4096); shed(me.contexts, 0); shed(me.environments, 0);
        if (collect) mi_collect(true);
    }

    void signal_stop() {
        stop_flag_.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_work_.notify_all();
    }

    // The time ran out: whatever this participant still holds goes back to the shared list, for the next slice.
    void give_back(ParMarker& me) {
        me.flush_ring();
        std::lock_guard<std::mutex> lock(m_);
        shared_.insert(shared_.end(), me.local.begin(), me.local.end());
        shared_count_.store(shared_.size(), std::memory_order_relaxed);
        me.local.clear();
    }

    std::mutex round_mutex_;
    std::mutex m_;
    std::condition_variable cv_work_, cv_done_;
    std::vector<std::unique_ptr<ParMarker>> markers_;
    std::vector<Heap::ProbeResult> shared_;
    std::vector<Heap*> main_heaps_;
    Heap* main_active_ = nullptr;
    Clock::time_point deadline_;
    bool has_deadline_ = false;
    bool stop_ = false;
    bool done_ = false;
    bool started_ = false;
    bool trim_only_ = false;
    int helpers_ = 0;
    int idle_ = 0;
    int participants_ = 1;
    int finished_ = 0;
    uint64_t generation_ = 0;
    std::atomic<int> idle_waiting_{0};
    std::atomic<int> hungry_{0};          // participants looking for work (spinning or asleep)
    std::atomic<int> sleepers_{0};        // participants asleep on cv_work_
    std::atomic<size_t> shared_count_{0};
    std::atomic<bool> stop_flag_{false};
};

class MarkVisitor final : public Visitor {
public:
    size_t marked_cells = 0;

    void mark_word(uint64_t word) {
        mark(Heap::probe_word(word));
    }

    // Remembered-set entry: already marked in a previous cycle, but its
    // fields changed since -- re-trace without re-marking.
    void push_remembered(const Heap::ProbeResult& p) {
        if (p.kind == CellKind::Object || p.kind == CellKind::String || p.kind == CellKind::Environment) {
            gray_.push_back(p);
        }
    }

    void visit_object(Object* o) override { edge_object(o); }
    void visit_string(String* s) override { edge_string(s); }
    void visit_symbol(Symbol* s) override { mark_edge(s, CellKind::Symbol); }
    void visit_bigint(BigInt* b) override { mark_edge(b, CellKind::BigInt); }

    // The same visits as plain inline members, for Object::trace_fast, which hands this class
    // its edges without a virtual call between them.
    [[gnu::always_inline]] void edge_object(Object* o) {
        Heap::ProbeResult r = Heap::mark_exact_fast(o, CellKind::Object);
        if (!r.cell) return;
        marked_cells++;
        gray_.push_back(r);
    }
    [[gnu::always_inline]] void edge_environment(Environment* env) { visit_environment(env); }
    [[gnu::always_inline]] void edge_string(String* s) {
        Heap::ProbeResult r = Heap::mark_string_edge_fast(s);
        if (!r.cell) return;
        marked_cells++;
        // A leaf string (CellKind::FlatString) is marked and finished; only a link has children.
        if (r.kind == CellKind::String) gray_.push_back(r);
    }
    [[gnu::always_inline]] void edge_value(const Value& v) {
        switch (v.gc_edge_kind()) {
            case 0: return;
            case 1: edge_object(v.is_function() ? static_cast<Object*>(v.as_function()) : v.as_object()); return;
            case 2: edge_string(v.as_string()); return;
            case 3: mark_edge(v.as_symbol(), CellKind::Symbol); return;
            default: mark_edge(v.as_bigint(), CellKind::BigInt); return;
        }
    }

    // Contexts carry the same stamp an environment does (gc_seen_cycle_): the set that used to answer a repeat visit cost an insert and a node for each survivor in each minor.
    void visit_context(Context* ctx) override {
        if (!ctx) return;
        // Stamped by every real edge, not only the first: a survivor the pool has already
        // queued (root_survivor_context) is in the set without a stamp, and a live closure
        // reaching it afterwards is exactly the edge the prune asks about.
        ctx->gc_stamp_major_epoch(g_major_epoch);
        if (ctx->gc_seen_cycle() != cycle_) {
            ctx->gc_set_seen_cycle(cycle_);
            context_work_.push_back(ctx);
        }
    }
    void visit_environment(Environment* env) override { if (env) mark_edge(env, CellKind::Environment); }

    // Queues a survivor-pool entry without recording it as reached by a real
    // edge. The pool roots every entry it holds, so stamping here would answer
    // "can anything other than the pool still reach this?" with a permanent
    // yes for every entry -- see run_minor_collection.
    void root_survivor_context(Context* ctx) {
        if (ctx && ctx->gc_seen_cycle() != cycle_) {
            ctx->gc_set_seen_cycle(cycle_);
            context_work_.push_back(ctx);
        }
    }

    // True once `ctx` has been visited this cycle, as a root or via tracing
    // (e.g. a live Function's closure_context_) -- see finish_major_cycle's
    // survivor-pruning step.
    bool context_seen(Context* ctx) const { return ctx && ctx->gc_seen_cycle() == cycle_; }

    // Contexts are not cells: no mark bit, and most of their mutable state
    // (pending exception, return value, microtask keep-alives) has no write
    // barrier. Root contexts are therefore re-visited on every root scan --
    // erase-and-revisit forces a fresh trace that picks up anything stored
    // into them since the previous one.
    void revisit_context(Context* ctx) {
        if (!ctx) return;
        ctx->gc_set_seen_cycle(0);
        visit_context(ctx);
    }

    void visit_weak_map(WeakMap* w) override { if (w) pending_weak_maps_.push_back(w); }
    void visit_weak_set(WeakSet* w) override { if (w) pending_weak_sets_.push_back(w); }
    void visit_weak_ref(WeakRef* w) override { if (w) pending_weak_refs_.push_back(w); }
    void visit_finalization_registry(FinalizationRegistry* r) override {
        if (r) pending_fin_registries_.push_back(r);
    }

    // Resolves every currently-known Context/Environment reference down to
    // its constituent cell edges. Uninterruptible by design: Context/
    // Environment are plain C++ objects with no mark bit of their own, so
    // one can never be left "pending" across a mark_step yield boundary --
    // its owning C++ call frame could return and destroy it via completely
    // ordinary control flow (no GC involved at all) before a later slice
    // got around to tracing it. Loops because tracing a context/environment
    // can itself push new gray_ entries (an edge to an Object/String), and
    // tracing a gray cell (e.g. Function::trace visiting closure_context_)
    // can push new context/environment work right back.
    void drain_contexts_and_environments() {
        // A few at a time, their lines asked for before the first is read: an environment (216 bytes) and a context (168) span several cache lines of memory that was last
        // touched a collection ago, and tracing them one by one waited on each in turn.
        constexpr size_t kBatch = 8;
        while (!context_work_.empty()) {
            {
                Context* batch[kBatch];
                size_t n = 0;
                while (n < kBatch && !context_work_.empty()) {
                    Context* c = context_work_.back();
                    context_work_.pop_back();
                    __builtin_prefetch(c, 0, 3);
                    __builtin_prefetch(reinterpret_cast<const char*>(c) + 64, 0, 3);
                    __builtin_prefetch(reinterpret_cast<const char*>(c) + 128, 0, 3);
                    batch[n++] = c;
                }
                for (size_t i = 0; i < n; i++) {
                    if (g_prof_cost) { const auto t = std::chrono::steady_clock::now(); batch[i]->gc_trace(*this); g_ctx_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count(); g_ctx_n++; }
                    else batch[i]->gc_trace(*this);
                }
            }
        }
    }

    // True only at genuine quiescence: nothing left to trace anywhere. The
    // one point where a quiescence-gated step (verify, survivor prune,
    // ephemeron fixpoint) or "cycle complete" may safely run -- a `gray_`
    // that looks empty mid-cycle doesn't mean context/environment work won't
    // repopulate it next.
    bool quiescent() const {
        return gray_.empty() && pf_count_ == 0 &&
               context_work_.empty();
    }

    // One bounded unit of work: quiesce contexts/environments (may push new
    // gray_ entries), then trace at most one gray cell (which may push new
    // context/environment work, hence the next caller's re-quiesce). Returns
    // false once truly nothing is left.
    bool step() {
        if (__builtin_expect(!context_work_.empty(), 0)) drain_contexts_and_environments();
        // Marking is memory-bound, not instruction-bound: a live set of a
        // million cells spread over hundreds of megabytes misses to DRAM on
        // nearly every cell, and popping an entry straight off the worklist
        // leaves no distance at all between learning a cell's address and
        // reading it. The ring in front of the trace supplies that distance.
        //
        // Two stages, because an object's trace reads two lines and the
        // second address is only known once the first has arrived: the cell
        // holds the pointer to the block its elements and shape slots live
        // in. Stage one requests the cell as the entry enters the ring;
        // stage two, half a ring later, reads the now-resident cell and
        // requests its storage block, with the rest of the ring still left
        // to trace before either is needed.
        while (pf_count_ < kPrefetchRing && !gray_.empty()) {
            Heap::ProbeResult q = gray_.back();
            gray_.pop_back();
            __builtin_prefetch(q.cell, 0, 3);
            pf_[pf_tail_] = q;
            pf_tail_ = (pf_tail_ + 1) & (kPrefetchRing - 1);
            pf_count_++;
        }
        if (pf_count_ > kPrefetchRing / 2) {
            uint32_t idx = (pf_head_ + kPrefetchRing / 2) & (kPrefetchRing - 1);
            const Heap::ProbeResult& q = pf_[idx];
            if (q.kind == CellKind::Object) {
                const void* storage = static_cast<const Object*>(q.cell)->gc_storage_hint();
                if (storage) {
                    // The block is walked outward from this address in both
                    // directions (elements below, shape slots above), so the
                    // line before it is wanted too.
                    __builtin_prefetch(static_cast<const char*>(storage) - 64, 0, 3);
                    __builtin_prefetch(storage, 0, 3);
                }
            }
        }
        if (pf_count_ == 0) return false;
        Heap::ProbeResult p = pf_[pf_head_];
        pf_head_ = (pf_head_ + 1) & (kPrefetchRing - 1);
        pf_count_--;
        trace_cell(p);
        return true;
    }

    void drain() {  // full, uninterruptible drain to quiescence
        for (;;) {
            if (should_parallelize() && !parallel_busy_) parallel_round(false, ParallelMarkPool::Clock::time_point());
            if (!step()) break;
        }
    }

    // Marking on the helper threads pays once there is a good deal of it: waking them costs some microseconds.
    // The list of gray cells is a stack, and a depth-first walk of a tree keeps it short whatever the tree's size, so how much is left cannot be read from its length: it is
    // guessed from what the last cycle of the same kind marked, and a few cells are enough to start the helpers from (they take the bottom of each other's lists).
    static constexpr size_t kParallelMinWork = 4096;
    static constexpr size_t kParallelMinExpected = 30000;
    bool should_parallelize() const {
        const size_t have = gray_.size() + pf_count_;
        // QUANTA_GC_PAR_ALWAYS=1: whenever there are two cells to trace, for testing the helpers on small heaps.
        static const bool always = std::getenv("QUANTA_GC_PAR_ALWAYS") != nullptr;
        if (have < (always ? 2u : 8u)) return false;
        if (ParallelMarkPool::instance().helpers() == 0) return false;
        if (always) return true;
        static const size_t min_expected = std::getenv("QUANTA_GC_PAR_MIN") ? static_cast<size_t>(std::atoll(std::getenv("QUANTA_GC_PAR_MIN"))) : kParallelMinExpected;
        return have >= kParallelMinWork || (Collector::major_in_progress_ ? g_expect_major : g_expect_minor) >= min_expected;
    }

    // Traces the gray cells (and those waiting in the prefetch ring) on the helpers and this thread, until they are all traced or `deadline` has come. What the helpers
    // could not trace themselves (functions, maps, host objects...) is traced here, and the contexts and environments they met are queued here.
    void parallel_round(bool has_deadline, ParallelMarkPool::Clock::time_point deadline) {
        std::vector<Heap::ProbeResult> work = std::move(gray_);
        gray_.clear();
        for (uint32_t i = 0; i < pf_count_; i++) work.push_back(pf_[(pf_head_ + i) & (kPrefetchRing - 1)]);
        pf_head_ = pf_tail_ = pf_count_ = 0;
        ParallelMarkPool& pool = ParallelMarkPool::instance();
        if (!pool.run(work, Collector::major_in_progress_, has_deadline, deadline)) {
            gray_ = std::move(work);  // the pool was busy: this thread goes on alone
            parallel_busy_ = true;
            return;
        }
        parallel_busy_ = false;
        if (gray_.empty()) gray_ = std::move(work);
        else gray_.insert(gray_.end(), work.begin(), work.end());
        g_par_rounds++;
        g_par_left += gray_.size();
        if (gray_.size() > g_par_maxfront) g_par_maxfront = gray_.size();
        for (auto& m : pool.markers()) for (size_t t = 0; t < 48; t++) { g_par_deferred_type[t] += m->deferred_by_type[t]; m->deferred_by_type[t] = 0; }
        for (size_t mi = 0; mi < pool.markers().size() && mi < 8; mi++) { g_par_marked[mi] += pool.markers()[mi]->marked; g_par_deferred += pool.markers()[mi]->deferred.size(); }
        for (auto& m : pool.markers()) {
            marked_cells += m->marked;
            for (const Heap::ProbeResult& p : m->deferred) trace_cell(p);
            for (Context* c : m->contexts) visit_context(c);
            for (Environment* e : m->environments) visit_environment(e);
            for (WeakMap* w : m->weak_maps) visit_weak_map(w);
            for (WeakSet* w : m->weak_sets) visit_weak_set(w);
            for (WeakRef* w : m->weak_refs) visit_weak_ref(w);
            for (FinalizationRegistry* r : m->registries) visit_finalization_registry(r);
            m->deferred.clear();
        }
    }
    bool parallel_busy_ = false;

    // A cycle is over and the worklists are empty: what capacity a big cycle grew them to is given back. The visitor lives as long as the thread (one per
    // thread, see mark_visitor()), and clear() keeps capacity, so the gray list that held the two million elements of one array while it was marked stayed at
    // 32 MB for good -- the footprint of a program that had freed everything stood tens of megabytes above where it began. A list that stayed small is kept
    // (growing it again every cycle would cost more than it holds).
    void release_excess_capacity() {
        constexpr size_t kKeepEntries = 64 * 1024;
        const auto shed = [](auto& list, size_t keep) {
            if (list.capacity() > keep) std::remove_reference_t<decltype(list)>().swap(list);
        };
        shed(gray_, kKeepEntries);
        shed(context_work_, kKeepEntries);
        shed(pending_weak_maps_, kKeepEntries);
        shed(pending_weak_sets_, kKeepEntries);
        shed(pending_weak_refs_, kKeepEntries);
        shed(pending_fin_registries_, kKeepEntries);
    }

    // Cycle boundary: called at the start of every collection (minor or
    // major) -- every worklist must start empty.
    void reset_for_new_cycle() {
        // Zero means "never queued", so it is skipped on wrap.
        if (++cycle_ == 0) ++cycle_;
        marked_cells = 0;
        parallel_busy_ = false;
        gray_.clear();
        pf_head_ = pf_tail_ = pf_count_ = 0;
        context_work_.clear();
        pending_weak_maps_.clear();
        pending_weak_sets_.clear();
        pending_weak_refs_.clear();
        pending_fin_registries_.clear();
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
                if (auto* sm = wm->raw_symbol_entries()) {
                    for (auto& e : *sm) {
                        if (!alive(e.first)) continue;
                        size_t before = marked_cells;
                        visit(e.second);
                        if (marked_cells != before) progress = true;
                    }
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
            if (auto* sm = wm->raw_symbol_entries()) {
                for (auto it = sm->begin(); it != sm->end();) {
                    if (!alive(it->first)) it = sm->erase(it); else ++it;
                }
            }
        }
        for (WeakSet* ws : pending_weak_sets_) {
            auto& s = ws->raw_values();
            for (auto it = s.begin(); it != s.end();) {
                if (!alive(*it)) it = s.erase(it); else ++it;
            }
            if (auto* ss = ws->raw_symbol_values()) {
                for (auto it = ss->begin(); it != ss->end();) {
                    if (!alive(*it)) it = ss->erase(it); else ++it;
                }
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
    // Marking from a trace edge, where the kind comes from the edge and the
    // pointer is a cell base -- see Heap::mark_exact.
    void mark_edge(const void* p, CellKind kind) {
        Heap::ProbeResult r = Heap::mark_exact_fast(p, kind);
        if (!r.cell) return;
        marked_cells++;
        if (r.kind == CellKind::Object || r.kind == CellKind::String || r.kind == CellKind::Environment) {
            gray_.push_back(r);
        }
    }

    void mark(const Heap::ProbeResult& p) {
        if (!p.cell || Heap::test_mark(p)) return;
        Heap::set_mark(p);
        marked_cells++;
        // Symbols and BigInts are leaves; their marking is complete here.
        if (p.kind == CellKind::Object || p.kind == CellKind::String || p.kind == CellKind::Environment) {
            gray_.push_back(p);
        }
    }

    void trace_cell(const Heap::ProbeResult& p) {
        // Re-arm Collector::write_barrier's mid-major re-trace path: cleared
        // at trace time, not cycle end, so the same cell can re-dirty.
        if (Collector::major_in_progress_) Heap::clear_remembered_fast(p);
        if (p.kind == CellKind::Object) {
            Object* o = static_cast<Object*>(p.cell);
            if (__builtin_expect(o->traces_by_default(), 1)) o->trace_fast(*this);
            else o->trace(*this);
        } else if (p.kind == CellKind::Environment) {
            static_cast<Environment*>(p.cell)->trace_fast(*this);
        } else {
            static_cast<String*>(p.cell)->gc_trace(*this);
        }
    }

    std::vector<Heap::ProbeResult> gray_;
    // Power of two: the ring index is a mask, not a modulo.
    static constexpr uint32_t kPrefetchRing = QUANTA_GC_PREFETCH_RING;
    Heap::ProbeResult pf_[kPrefetchRing];
    uint32_t pf_head_ = 0, pf_tail_ = 0, pf_count_ = 0;
    std::vector<Context*> context_work_;
    // Which collection this is. An environment queued during it carries the
    // same number, which is what tells a repeat visit from a first one -- the
    // set that used to answer it cost an insert per environment per cycle,
    // plus a rehash whenever a cycle's working set outgrew it.
    uint32_t cycle_ = 0;

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
    void visit_environment(Environment* env) override { if (env && env->is_cell()) check_target(env); }
};

// Conservative stack scanning intentionally reads past individual local
// variables' declared bounds -- that's the whole point. ASAN's stack
// redzones flag this as a false-positive overflow; suppress on this pair.
__attribute__((no_sanitize("address")))
// Words the conservative scan walked in the last collection. A generator-heavy
// program keeps many suspended fibers, each with its own stack, and scanning
// them is most of what its collections cost -- none of which shows in how much
// memory is live. See Heap::retune_budget's caller.
constinit thread_local size_t g_scanned_words = 0;

// A conservative scanner reads whole stack ranges on purpose, redzones and
// all, and ASan flags every such read -- which buried every other report this
// binary could have produced. The reads are in-bounds for the stack itself;
// what ASan objects to is the poisoning it put between frames.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define QUANTA_NO_ASAN __attribute__((no_sanitize("address")))
#  endif
#endif
#if !defined(QUANTA_NO_ASAN) && defined(__SANITIZE_ADDRESS__)
#  define QUANTA_NO_ASAN __attribute__((no_sanitize("address")))
#endif
#ifndef QUANTA_NO_ASAN
#  define QUANTA_NO_ASAN
#endif

QUANTA_NO_ASAN
void scan_range(MarkVisitor& v, const void* lo, const void* hi) {
    auto a = (reinterpret_cast<uintptr_t>(lo) + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1);
    auto b = reinterpret_cast<uintptr_t>(hi) & ~(sizeof(uint64_t) - 1);
    g_scanned_words += (b > a) ? (b - a) / sizeof(uint64_t) : 0;
    for (const uint64_t* p = reinterpret_cast<const uint64_t*>(a);
         p < reinterpret_cast<const uint64_t*>(b); p++) {
        if (*p) v.mark_word(*p);
    }
}

void main_thread_stack_bounds(const char** lo, const char** hi) {
    static thread_local const char* cached_lo = nullptr;
    static thread_local const char* cached_hi = nullptr;
    if (!cached_lo) {
#ifdef _WIN32
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        cached_lo = reinterpret_cast<const char*>(low);
        cached_hi = reinterpret_cast<const char*>(high);
#elif defined(__APPLE__)
        // No pthread_getattr_np on Darwin (a glibc extension) -- these two
        // are the BSD/Darwin-native equivalent. Unlike Linux's addr+size
        // (addr is the low end), pthread_get_stackaddr_np returns the HIGH
        // end (stacks grow down from there).
        cached_hi = static_cast<const char*>(pthread_get_stackaddr_np(pthread_self()));
        cached_lo = cached_hi - pthread_get_stacksize_np(pthread_self());
#else
        pthread_attr_t attr;
        pthread_getattr_np(pthread_self(), &attr);
        void* addr = nullptr;
        size_t size = 0;
        pthread_attr_getstack(&attr, &addr, &size);
        pthread_attr_destroy(&attr);
        cached_lo = static_cast<const char*>(addr);
        cached_hi = cached_lo + size;
#endif
    }
    *lo = cached_lo;
    *hi = cached_hi;
}

__attribute__((no_sanitize("address")))
void scan_stacks(MarkVisitor& v) {
    g_scanned_words = 0;
    // Spill registers into a struct scanned explicitly below -- relying on
    // it falling inside [sp, main_hi] by luck of stack layout missed a
    // register-resident pointer under some compilers/flags (observed with
    // ASan's stack redzones reordering locals enough to exclude it).
#ifdef _WIN32
    CONTEXT spilled_registers;
    RtlCaptureContext(&spilled_registers);
#elif defined(__APPLE__) && defined(__aarch64__)
    // Not getcontext: on Apple silicon it left the caller's callee-saved
    // registers zeroed after returning (a crash report showed x24/x25/x28 = 0
    // and run_minor_collection's MarkVisitor& null right after this call).
    // Storing the callee-saved set by hand touches nothing.
    uint64_t spilled_registers[12];
    __asm__ volatile(
        "stp x19, x20, [%0, #0]\n"
        "stp x21, x22, [%0, #16]\n"
        "stp x23, x24, [%0, #32]\n"
        "stp x25, x26, [%0, #48]\n"
        "stp x27, x28, [%0, #64]\n"
        "stp x29, x30, [%0, #80]\n"
        : : "r"(spilled_registers) : "memory");
#else
    ucontext_t spilled_registers;
    getcontext(&spilled_registers);
#endif
    scan_range(v, &spilled_registers, &spilled_registers + 1);

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

    // A suspended fiber's saved registers live in its mco_coro control block,
    // which sits outside stack_lo/stack_hi -- ahead of the stack in the same
    // allocation, so [co, stack_lo) is exactly it. Not co->coro_size: that
    // measures the whole allocation, stack included, and scanning to it undoes
    // the narrowing below and walks every page of every live fiber on every
    // cycle. The stack's own live region is decided from the suspend point.
    FiberRegistry::for_each([&](const FiberRegistry::Record& rec) {
        // An abandoned fiber is not a root: its stack, its registers and what its owner reports are not held for it any more.
        if (rec.state && rec.state->abandoned()) return;
        // A fiber's live region is [sp, stack_hi] -- stacks grow down, so
        // anything below the deepest point it currently reaches is dead. Take
        // the deepest of the three things that can be holding frames: where it
        // suspended, any fiber it resumed from its own stack, and the scanner
        // itself if this is the fiber we are running on. Under-estimating is
        // safe (a lower start only scans more), so an unknown suspend point
        // falls back to the whole stack.
        const char* from = rec.stack_lo;
        const char* suspended_at = rec.state ? rec.state->suspend_sp : nullptr;
        if (suspended_at) {
            const char* deepest = suspended_at;
            if (sp >= rec.stack_lo && sp < rec.stack_hi && sp < deepest) deepest = sp;
            FiberRegistry::for_each_enter_sp([&](const void* esp) {
                const char* c = static_cast<const char*>(esp);
                if (c >= rec.stack_lo && c < rec.stack_hi && c < deepest) deepest = c;
            });
            // The switch itself writes a little below the recorded point.
            constexpr size_t kSwitchMargin = 1024;
            from = (deepest - rec.stack_lo > static_cast<ptrdiff_t>(kSwitchMargin))
                       ? deepest - kSwitchMargin : rec.stack_lo;
        }
        scan_range(v, from, rec.stack_hi);
        if (rec.state && rec.state->co) {
            mco_coro* co = rec.state->co;
            const char* control_end = rec.stack_lo;
            if (control_end > reinterpret_cast<const char*>(co)) {
                scan_range(v, co, control_end);
            }
        }
        // A fiber of the heap being retired is going with it, and what it holds with it.
        if (rec.extra_roots && !Heap::is_retiring()) rec.extra_roots(v);
    });
}


thread_local Collector::CycleStats g_last_cycle;


// How many requested collections per major. Halved back to the floor by a
// major that reclaimed a worthwhile share of what it marked, doubled by one
// that did not.
static const uint32_t kMajorIntervalFloor = [] { const char* e = std::getenv("QUANTA_GC_INTERVAL"); return e ? static_cast<uint32_t>(std::atoi(e)) : 8u; }();
uint32_t& major_interval_ref() {
    static thread_local uint32_t interval = kMajorIntervalFloor;
    return interval;
}
uint32_t major_interval() { return major_interval_ref(); }

// A major that frees less than this share of the cells it marked did not pay
// for the marking, and the next one is unlikely to either. Set against what
// marking actually costs: it is the single largest phase in the collector, so
// a major has to reclaim a quarter of what it walks to be worth walking. The
// bar used to be a sixteenth, which a bundle-shaped workload clears without
// ever being productive -- majors there marked thirteen cells per cell freed
// and the backoff below reset to the floor every time, so it never engaged.
constexpr size_t kMajorYieldDivisor = 4;
static const uint32_t kMajorIntervalCap = kMajorIntervalFloor * 8;

void note_major_yield(size_t marked, size_t swept) {
    uint32_t& interval = major_interval_ref();
    // A major that freed little while the live set was still growing is not
    // evidence that majors are pointless: the program is building its data
    // (a tree, a parsed bundle), and the next major will find garbage as soon
    // as the building stops. Backing off there let the growth trigger reach
    // several times the live set before anything could be reclaimed. Only a
    // live set that has stopped growing counts as an unproductive major.
    static thread_local size_t previous_marked = 0;
    const bool still_growing = marked > previous_marked + previous_marked / 4;
    previous_marked = marked;
    if (swept * kMajorYieldDivisor >= marked) interval = kMajorIntervalFloor;
    else if (still_growing) return;
    else if (interval < kMajorIntervalCap) interval *= 2;
}


// The one MarkVisitor instance per thread: cycle-lived rather than a
// caller-local stack variable, so its worklists can survive across multiple
// mark_step calls instead of only living for one atomic collection.
MarkVisitor& mark_visitor() {
    static thread_local MarkVisitor v;
    return v;
}

std::vector<const BytecodeChunk*>& chunk_roots() {
    static thread_local std::vector<const BytecodeChunk*> roots;
    return roots;
}

std::vector<const std::vector<Value>*>& value_vector_roots() {
    static thread_local std::vector<const std::vector<Value>*> roots;
    return roots;
}


// The interpreter's side stack of registers for stackless calls (Collector::set_value_stack).
struct ValueStackRange { Value* base = nullptr; Value* const* top = nullptr; };
constinit thread_local ValueStackRange g_value_stack;
constinit thread_local Collector::FrameEnvScanner g_frame_env_scanner = nullptr;
static Collector::FrameEnvScanner g_frame_env_scanner_ptr() { return g_frame_env_scanner; }

std::vector<const FixedArray<Value>*>& value_array_roots() {
    static thread_local std::vector<const FixedArray<Value>*> roots;
    return roots;
}

std::vector<Heap::ProbeResult>& remembered_cells() {
    static thread_local std::vector<Heap::ProbeResult> cells;
    return cells;
}

// The targets of writes into old containers, recorded instead of the
// containers themselves. A minor collection needs old-to-young edges as
// roots; the container is one way to find them and the edge itself is
// another, and the second does not cost a walk of everything the container
// holds. Drained and cleared by every minor, so it only ever holds the
// writes since the last one.
std::vector<Value>& remembered_values() {
    static thread_local std::vector<Value> values;
    return values;
}

bool env_flag(const char* name) {
    const char* val = std::getenv(name);
    return val && *val && *val != '0';
}

size_t run_sweep(bool minor) {
    // Two passes: destructors may consult other cells' memory only through
    // their own backing stores (audited rule), but collecting the dead list
    // first keeps the block bitmaps stable while destructors run.
    // Kept across sweeps for its capacity: a heap that has just filled up
    // hands over millions of cells, and growing the list from nothing again
    // every cycle costs more than the destructors it is holding. Only up to a
    // point, though -- see the end of this function.
    static thread_local std::vector<Heap::DeadCell> dead;
    // The blocks of pure cells (see HeapBlock::set_pure_origin): freed by bitmap after the destructors below have run, with none of their cells looked at.
    static thread_local std::vector<HeapBlock*> pure_blocks;
    // The blocks whose dead cells the loop below has run the destructors of, to be freed by bitmap (a null at the front so that the first compare needs no emptiness test).
    static thread_local std::vector<HeapBlock*> freed_blocks;
    freed_blocks.assign(1, nullptr);
    dead.clear();
    pure_blocks.clear();
    static const bool prof_sweep = env_flag("QUANTA_GC_PROFILE");
    const auto sweep_t0 = std::chrono::steady_clock::now();
    Heap::collect_dead_cells(dead, pure_blocks, minor);
    const auto sweep_t1 = std::chrono::steady_clock::now();
    size_t dead_by_type[32] = {};
    size_t dead_by_kind[kNumCellKinds] = {};
    if (prof_sweep) {
        for (const Heap::DeadCell& d : dead) {
            dead_by_kind[static_cast<size_t>(d.kind)]++;
            if (d.kind == CellKind::Object) {
                const size_t t = static_cast<size_t>(static_cast<Object*>(d.cell)->get_type());
                dead_by_type[t < 32 ? t : 31]++;
            }
        }
    }
    // QUANTA_GC_POISON=1: fill freed cells with a recognizable pattern and
    // leak the slot instead of reusing it -- any use-after-free then crashes
    // deterministically on the poison instead of silently reading a
    // recycled cell. Debug tool for hunting invisible lambda captures.
    static const bool poison = env_flag("QUANTA_GC_POISON");
    // Host objects that asked to hear of their death, and weak handles to what died, before any
    // destructor of this collection runs: every cell is still intact.
    if (DOMObject::finalization_pending()) {
        DOMObject::clear_dead_weak_handles();
        DOMObject::finalize_dead(dead);
    }
    for (const Heap::DeadCell& d : dead) {
        switch (d.kind) {
            case CellKind::Object: {
                Object* obj = static_cast<Object*>(d.cell);
                // Explicit per-type dispatch instead of a virtual ~Object()
                // call -- Object carries no vtable. Function/TypedArrayBase/
                // CustomObjectBase keep their own small vtables, so casting
                // to those three still reaches every further subclass
                // (AsyncFunction/GeneratorFunction/...; 11 numeric
                // TypedArrays; Generator/AsyncGenerator/AsyncIterator/
                // Iterator+4) via ordinary virtual dispatch from here.
                using OT = Object::ObjectType;
                switch (obj->get_type()) {
                    case OT::Function: {
                        // Function itself carries no vtable (see Object.h's note on
                        // why); AsyncFunction/GeneratorFunction/AsyncGeneratorFunction
                        // need their own destructor called explicitly, same reasoning
                        // as the outer switch on get_type().
                        Function* fn = static_cast<Function*>(obj);
                        switch (fn->get_function_kind()) {
                            case Function::FunctionKind::Async: static_cast<AsyncFunction*>(fn)->~AsyncFunction(); break;
                            case Function::FunctionKind::Generator: static_cast<GeneratorFunction*>(fn)->~GeneratorFunction(); break;
                            case Function::FunctionKind::AsyncGenerator: static_cast<AsyncGeneratorFunction*>(fn)->~AsyncGeneratorFunction(); break;
                            default: fn->~Function(); break;
                        }
                        break;
                    }
                    case OT::Custom: {
                        // CustomObjectBase itself carries no vtable either (see
                        // Object.h's note); every concrete kind sharing
                        // ObjectType::Custom needs its own destructor called
                        // explicitly, same reasoning as the outer switch.
                        CustomObjectBase* cob = static_cast<CustomObjectBase*>(obj);
                        using CK = CustomObjectBase::CustomKind;
                        switch (cob->get_custom_kind()) {
                            case CK::Generator: static_cast<Generator*>(cob)->~Generator(); break;
                            case CK::AsyncGenerator: static_cast<AsyncGenerator*>(cob)->~AsyncGenerator(); break;
                            case CK::AsyncIterator: static_cast<AsyncIterator*>(cob)->~AsyncIterator(); break;
                            case CK::ArrayIterator: static_cast<ArrayIterator*>(cob)->~ArrayIterator(); break;
                            case CK::StringIterator: static_cast<StringIterator*>(cob)->~StringIterator(); break;
                            case CK::MapIterator: static_cast<MapIterator*>(cob)->~MapIterator(); break;
                            case CK::SetIterator: static_cast<SetIterator*>(cob)->~SetIterator(); break;
                            case CK::ModuleNamespace: static_cast<ModuleNamespaceObject*>(cob)->~ModuleNamespaceObject(); break;
                            case CK::DeferredNamespace: static_cast<DeferredNamespaceObject*>(cob)->~DeferredNamespaceObject(); break;
                            case CK::Host: static_cast<DOMObject*>(cob)->destroy(); break;
                        }
                        break;
                    }
                    case OT::TypedArray: static_cast<TypedArrayBase*>(obj)->~TypedArrayBase(); break;
                    case OT::Error: static_cast<Error*>(obj)->~Error(); break;
                    case OT::Promise: static_cast<Promise*>(obj)->~Promise(); break;
                    case OT::Proxy: static_cast<Proxy*>(obj)->~Proxy(); break;
                    case OT::Map: static_cast<Map*>(obj)->~Map(); break;
                    case OT::Set: static_cast<Set*>(obj)->~Set(); break;
                    case OT::WeakMap: static_cast<WeakMap*>(obj)->~WeakMap(); break;
                    case OT::WeakSet: static_cast<WeakSet*>(obj)->~WeakSet(); break;
                    case OT::WeakRef: static_cast<WeakRef*>(obj)->~WeakRef(); break;
                    case OT::FinalizationRegistry:
                        static_cast<FinalizationRegistry*>(obj)->~FinalizationRegistry(); break;
                    case OT::ArrayBuffer: static_cast<ArrayBuffer*>(obj)->~ArrayBuffer(); break;
                    case OT::DataView: static_cast<DataView*>(obj)->~DataView(); break;
                    // Holds a shared_ptr<RegExp>, and that RegExp owns the
                    // compiled pattern. Running only ~Object() here left every
                    // regex literal's compiled code behind for good.
                    case OT::RegExp: static_cast<RegExpObject*>(obj)->~RegExpObject(); break;
                    // The rest have no subclass of their own, so ~Object() is
                    // the whole destructor: Ordinary, Array, Arguments, and the
                    // String/Number/Boolean/Date/Symbol/BigInt wrappers.
                    default: obj->~Object(); break;
                }
                break;
            }
            case CellKind::String:
            case CellKind::FlatString: static_cast<String*>(d.cell)->~String(); break;
            case CellKind::Symbol: static_cast<Symbol*>(d.cell)->~Symbol(); break;
            case CellKind::BigInt: static_cast<BigInt*>(d.cell)->~BigInt(); break;
            case CellKind::Environment: static_cast<Environment*>(d.cell)->~Environment(); break;
            default: break;
        }
        if (poison && BlockAllocator::owns_address(d.cell)) {
            HeapBlock* block = HeapBlock::from_cell(d.cell);
            std::memset(d.cell, 0xD9, block->cell_size());
            block->retire_cell(d.cell);  // slot leaked on purpose, never reused
            continue;
        }
        if (poison) {
            Heap::cell_free(d.cell);
            continue;
        }
        // Not freed one by one (each was a lookup of the chunk that owns the address, a slot index and four bit operations): the dead list comes block by block, and a block frees
        // what is allocated and not marked in a pass over its words, which is exactly its dead cells.
        if (d.large) {
            Heap::cell_free(d.cell);
        } else {
            HeapBlock* block = HeapBlock::from_cell(d.cell);
            // A pure block is freed with the other pure blocks below (counted there).
            if (block->pure_origin()) continue;
            if (block != freed_blocks.back()) freed_blocks.push_back(block);
        }
    }
    for (HeapBlock* block : freed_blocks) {
        if (block) block->free_dead_bits();
    }
    const auto sweep_t2 = std::chrono::steady_clock::now();
    const size_t pure_blocks_count = pure_blocks.size();
    const size_t pure_freed = Heap::free_pure_dead(pure_blocks);
    const size_t swept = dead.size() + pure_freed;
    pure_blocks.clear();
    if (prof_sweep) {
        const auto sweep_t3 = std::chrono::steady_clock::now();
        auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
        std::fprintf(stderr, "[gc-prof] %s sweep find_dead=%ldus destructors+free=%ldus pure_free=%ldus: cells needing a destructor %zu (objects %zu, strings %zu, flat %zu), pure cells freed by bitmap %zu in %zu blocks\n",
                     minor ? "minor" : "major", us(sweep_t0, sweep_t1), us(sweep_t1, sweep_t2), us(sweep_t2, sweep_t3), dead.size(), dead_by_kind[0], dead_by_kind[1], dead_by_kind[4], pure_freed, pure_blocks_count);
        std::fprintf(stderr, "[gc-prof] %s sweep object types:", minor ? "minor" : "major");
        for (size_t t = 0; t < 32; t++) if (dead_by_type[t]) std::fprintf(stderr, " type%zu=%zu", t, dead_by_type[t]);
        std::fprintf(stderr, "\n");
    }
    // One major can hand over hundreds of thousands of cells, and a list that
    // keeps that capacity holds it for the rest of the run (4MB on a 1.4M-cell
    // heap) although every sweep after it is small. Minors and ordinary majors
    // fit in what is kept; the rare large one grows the list again.
    constexpr size_t kDeadKeepEntries = 131072;
    if (dead.capacity() > kDeadKeepEntries) std::vector<Heap::DeadCell>().swap(dead);
    return swept;
}

void run_verify(Collector::CycleStats& stats) {
    VerifyVisitor check;
    Heap::for_each_cell([&](void* cell, CellKind kind, bool marked) {
        if (!marked) return;
        if (kind == CellKind::Object) static_cast<Object*>(cell)->trace(check);
        else if (kind == CellKind::String) static_cast<String*>(cell)->gc_trace(check);
        else if (kind == CellKind::Environment) static_cast<Environment*>(cell)->gc_trace(check);
        else if (kind == CellKind::FlatString && static_cast<String*>(cell)->is_link()) {
            std::fprintf(stderr, "[gc-verify] link string allocated as a leaf cell %p\n", cell);
            check.violations++;
        }
    });
    stats.verify_violations = check.violations;
}

}

namespace {

// QUANTA_GC_ROOT_ALL_SURVIVORS=1: every survivor context is rooted in every minor, as it was (for finding out whether a bug is in skipping them).
const bool g_survivors_always_rooted = env_flag("QUANTA_GC_ROOT_ALL_SURVIVORS");

void run_minor_collection() {
    ++Collector::sweep_generation_;
    static const bool verify = env_flag("QUANTA_GC_VERIFY");
    static const bool log = env_flag("QUANTA_GC_LOG");
    static const bool prof = env_flag("QUANTA_GC_PROFILE");

    Heap::clear_gc_request();
    auto t0 = std::chrono::steady_clock::now();

    MarkVisitor& v = mark_visitor();
    v.reset_for_new_cycle();
    // Old cells/environments mutated since the last cycle: their new edges
    // may be the only path to young cells. Re-probe rather than trusting the
    // stored ProbeResult: if the cell was freed since being remembered (e.g.
    // a still-unreleased unique_ptr<Object> staging local that got marked by
    // an intervening collection, then destructed on an early-return path
    // instead of being handed into the graph), its slot may since have been
    // reused -- possibly by a cell of a different kind/size, or even by a
    // whole different (kind, size-class) block if the old one was emptied
    // and reclaimed. Re-probing confirms the address is still a live,
    // same-kind cell before it's dispatched through a virtual trace() call.
    for (const Heap::ProbeResult& p : remembered_cells()) {
        Heap::ProbeResult fresh = Heap::exact_cell(p.cell);
        if (fresh.cell == p.cell && fresh.kind == p.kind) v.push_remembered(fresh);
    }
    // Null entries are environments freed since being remembered, plus ones
    // whose earlier entry a later barrier superseded -- see
    // Collector::release_env. visit_environment already ignores them.
    for (const Value& val : remembered_values()) v.visit(val);
    remembered_values().clear();
    // See FiberRegistry::Record::owner_cell for why these are minor roots.
    FiberRegistry::for_each([&](const FiberRegistry::Record& rec) {
        if (rec.owner_cell && !Heap::is_retiring() && !(rec.state && rec.state->abandoned())) v.visit_object(rec.owner_cell);
    });
    scan_stacks(v);
    auto t1 = std::chrono::steady_clock::now();

    for (Engine* engine : Engine::all_engines()) {
        v.visit_context(engine->get_global_context());
        // Modules are otherwise entirely outside the root set (exports_,
        // thrown_exception_, the cached namespace object) -- see
        // ModuleLoader::gc_trace's doc comment.
        if (ModuleLoader* loader = engine->get_module_loader()) {
            loader->gc_trace(v);
            for (const auto& kv : loader->modules()) {
                if (Context* ctx = kv.second->get_context()) v.visit_context(ctx);
            }
        }
    }
    v.visit_context(Object::current_context_);
    for (Engine* engine : Engine::all_engines())
        for (ExecContextScope* s = engine->exec_top_scope(); s; s = s->prev())
            v.visit_context(s->context());
    for (const std::vector<Value>* vec : value_vector_roots())
        for (const Value& val : *vec) v.visit(val);
    for (const FixedArray<Value>* arr : value_array_roots())
        for (const Value& val : *arr) v.visit(val);
    if (g_value_stack.base)
        for (const Value* p = g_value_stack.base; p < *g_value_stack.top; ++p) v.visit(*p);
    if (g_frame_env_scanner_ptr()) g_frame_env_scanner_ptr()(v);
    for (const BytecodeChunk* chunk : chunk_roots()) chunk->trace(v);
    Symbol::gc_trace_roots(v);
    String::gc_trace_roots(v);
    trace_atomics_gc_roots(v);
    FunctionExecutable::gc_trace_roots(v);
    // The shared job queue -- one instance for the whole thread, not per-engine.
    EventLoop::instance().gc_trace(v);
    auto t2 = std::chrono::steady_clock::now();

    Collector::mark_step(std::chrono::microseconds(-1));
    const auto t2b = std::chrono::steady_clock::now();
    const size_t remembered_n = remembered_cells().size();

    // The Context survivor pool is rooted only now, after every other root has
    // been traced to exhaustion. A minor doesn't retrace old, unmutated cells,
    // so a survivor only reachable through one wouldn't be rediscovered this
    // cycle and still has to be rooted -- but rooting it alongside the real
    // roots also destroys the one thing worth knowing about it, namely whether
    // anything OTHER than the pool can still reach it. Rooted separately, and
    // without the stamp visit_context leaves, that question has an answer: an
    // entry no collection has stamped since the last major cleared every mark
    // bit has no marked cell pointing at it, and no unmarked one either --
    // an unmarked holder is either traced right here or swept by this cycle.
    //
    // Environment survivors are deliberately neither rooted nor pruned by a
    // minor: one is reached only through a real edge or through
    // Collector::write_barrier_env (remembered_envs above), and its pool stays
    // small enough that the major's own prune keeps up with it.
    // Only the survivors a collection has not traced yet, and the ones that only a pending job keeps (no edge has reached those since the last major): a survivor that was traced
    // and that a real edge reached has nothing in it that changed (its call is over; what its environments hold is told by the barrier of environments), so rooting it again was
    // nineteen thousand contexts and their environments walked in every minor of a page.
    size_t rooted_survivors = 0, skipped_survivors = 0;
    for (Engine* engine : Engine::all_engines()) {
        const std::vector<Context*>& pool = engine->get_survivor_contexts();
        const size_t traced = engine->survivor_traced();
        for (size_t i = 0; i < pool.size(); ++i) {
            Context* ctx = pool[i];
            if (i < traced && !g_survivors_always_rooted && ctx->gc_reached_since_major(g_major_epoch)) { ++skipped_survivors; continue; }
            ++rooted_survivors;
            v.root_survivor_context(ctx);
        }
    }
    Collector::mark_step(std::chrono::microseconds(-1));
    // (before the prune: what a WeakMap's live key holds is reached only here, and may name a Context)
    v.drain_ephemerons();

    // Pruned only now, with the whole graph traced. A pool entry kept for any
    // reason can reach a Function whose closure_context_ is a different pool
    // entry, so nothing may be freed until every entry's subgraph has been
    // walked -- deciding entry by entry as they are rooted frees a Context
    // that a later entry then traces.
    for (Engine* engine : Engine::all_engines()) {
        std::vector<Context*>& survivors = engine->mutable_survivor_contexts();
        std::vector<Context*> kept;
        kept.reserve(survivors.size());
        std::vector<Context*> doomed;
        for (Context* ctx : survivors) {
            if (ctx->gc_reached_since_major(g_major_epoch) ||
                EventLoop::instance().is_context_in_use(ctx)) {
                kept.push_back(ctx);
            } else {
                doomed.push_back(ctx);
            }
        }
        survivors = std::move(kept);
        engine->set_survivor_traced(survivors.size());
        // Deleted only once the pool no longer refers to them: ~Context hands
        // an escaped owned_env_ back to the Environment survivor pool and
        // routes the rest through Collector::release_env, so it can append
        // while this runs.
        for (Context* ctx : doomed) delete ctx;
    }
    auto t3 = std::chrono::steady_clock::now();

    v.drain_ephemerons();
    v.finalize_ephemerons();
    auto t4 = std::chrono::steady_clock::now();

    g_last_cycle = Collector::CycleStats{};
    g_last_cycle.minor = true;
    g_last_cycle.marked_cells = v.marked_cells;
    if (verify) run_verify(g_last_cycle);
    auto t5 = std::chrono::steady_clock::now();

    // Must run before sweep below: a fully-dead block can be freed once
    // swept, and a stale entry here pointing into it would then read
    // cell_size back as 0 -- slot_index's offset/cell_size divides by that
    // and traps (SIGFPE).
    for (const Heap::ProbeResult& p : remembered_cells()) Heap::clear_remembered(p);
    remembered_cells().clear();
    // A sweep can free anything this still points at: whatever was reachable
    // is marked and whatever was not is gone, and either way the edges it
    // recorded have been accounted for by the mark that just finished.
    remembered_values().clear();
    // Deliberately NOT freeing pending envs here: a minor traces from the roots
    // only, so its seen_ set cannot tell a dead environment from one this cycle
    // simply never looked at. The next completed major decides.
    (void)0;

    static const bool mark_only = env_flag("QUANTA_GC_MARK_ONLY");
    if (!mark_only) {
        g_last_cycle.swept_cells = run_sweep(/*minor=*/true);
        // Budget the next collection against what this one cost, but the two
        // halves of that cost do not behave alike: marking the live set is
        // amortizable (collecting sooner leaves less floating garbage), while
        // scanning roots is paid in full every time regardless. They are
        // handed over separately. The live figure comes back from the rebuild,
        // which has to touch every block anyway.
        // The blocks the minor touched, not every block of the heap: what this cost grew with the live set (1.8 ms a minor for a million and a half objects,
        // for a minor that marked and swept the same few as with a small heap).
        const size_t live_after = Heap::update_allocation_candidates_after_minor();
        Heap::reset_dirty_blocks();
        Heap::retune_budget(live_after, g_scanned_words * sizeof(uint64_t));
        v.release_excess_capacity();
    }
    g_expect_minor = v.marked_cells;
    auto t6 = std::chrono::steady_clock::now();

    if (prof) {
        auto us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
        if (g_prof_cost) { std::fprintf(stderr, "[gc-prof] minor ctx/env: contexts=%llu (%llu us) envs=%llu (%llu us)\n", (unsigned long long)g_ctx_n, (unsigned long long)g_ctx_ns / 1000, (unsigned long long)g_env_n, (unsigned long long)g_env_ns / 1000); g_ctx_n = g_ctx_ns = g_env_n = g_env_ns = 0; }
        std::fprintf(stderr, "[gc-prof] minor survivors rooted=%zu skipped=%zu\n", rooted_survivors, skipped_survivors);
        std::fprintf(stderr, "[gc-prof] minor drain split: first=%ldus survivors+second=%ldus remembered=%zu rooted_survivors=%zu skipped=%zu\n", static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(t2b - t2).count()), static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2b).count()), remembered_n, rooted_survivors, skipped_survivors);
        std::fprintf(stderr, "[gc-prof] epochs proto=%llu descriptor=%llu\n", static_cast<unsigned long long>(Object::proto_epoch()), static_cast<unsigned long long>(Object::descriptor_epoch()));
        std::fprintf(stderr, "[gc-prof] minor scan_stacks=%ldus roots=%ldus drain=%ldus ephemeron=%ldus verify=%ldus sweep+rebuild=%ldus marked=%zu\n",
                     us(t0,t1), us(t1,t2), us(t2,t3), us(t3,t4), us(t4,t5), us(t5,t6), v.marked_cells);
    }
    if (log) {
        std::fprintf(stderr, "[gc] minor marked=%zu swept=%zu verify_violations=%zu\n",
                     g_last_cycle.marked_cells, g_last_cycle.swept_cells, g_last_cycle.verify_violations);
    }
}

void scan_major_roots(MarkVisitor& v) {
    static const bool prof_roots = env_flag("QUANTA_GC_PROFILE");
    auto pr0 = std::chrono::steady_clock::now();
    auto pr_us = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count(); };
    scan_stacks(v);
    auto pr1 = std::chrono::steady_clock::now();
    // See FiberRegistry::Record::owner_cell for why these are roots here too.
    FiberRegistry::for_each([&](const FiberRegistry::Record& rec) {
        if (rec.owner_cell && !Heap::is_retiring() && !(rec.state && rec.state->abandoned())) v.visit_object(rec.owner_cell);
    });
    // revisit_context, not visit_context: root contexts must be re-traced
    // every slice, not only the first.
    for (Engine* engine : Engine::all_engines()) {
        v.revisit_context(engine->get_global_context());
        // See ModuleLoader::gc_trace's doc comment; module contexts need the
        // same every-slice re-trace as any other root context.
        if (ModuleLoader* loader = engine->get_module_loader()) {
            loader->gc_trace(v);
            for (const auto& kv : loader->modules()) {
                if (Context* ctx = kv.second->get_context()) v.revisit_context(ctx);
            }
        }
    }
    v.revisit_context(Object::current_context_);
    for (Engine* engine : Engine::all_engines())
        for (ExecContextScope* s = engine->exec_top_scope(); s; s = s->prev())
            v.revisit_context(s->context());
    for (const std::vector<Value>* vec : value_vector_roots())
        for (const Value& val : *vec) v.visit(val);
    for (const FixedArray<Value>* arr : value_array_roots())
        for (const Value& val : *arr) v.visit(val);
    if (g_value_stack.base)
        for (const Value* p = g_value_stack.base; p < *g_value_stack.top; ++p) v.visit(*p);
    if (g_frame_env_scanner_ptr()) g_frame_env_scanner_ptr()(v);
    auto pr2 = std::chrono::steady_clock::now();
    for (const BytecodeChunk* chunk : chunk_roots()) chunk->trace(v);
    Symbol::gc_trace_roots(v);
    String::gc_trace_roots(v);
    trace_atomics_gc_roots(v);
    auto pr3 = std::chrono::steady_clock::now();
    FunctionExecutable::gc_trace_roots(v, /*settled_incrementally=*/true);
    auto pr4 = std::chrono::steady_clock::now();
    // The shared job queue -- one instance for the whole thread, not per-engine.
    EventLoop::instance().gc_trace(v);
    if (prof_roots) {
        auto pr5 = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[gc-prof] major roots stacks=%ldus contexts+envs=%ldus chunks+symbols+strings=%ldus executables=%ldus eventloop=%ldus\n", pr_us(pr0, pr1), pr_us(pr1, pr2), pr_us(pr2, pr3), pr_us(pr3, pr4), pr_us(pr4, pr5));
    }
}

thread_local std::chrono::steady_clock::time_point g_major_cycle_start;
constinit thread_local uint32_t g_major_slice_count = 0;

// Only reachable once Collector::mark_step has returned CycleComplete, i.e.
// gray_/context_work_/environment_work_ are all genuinely empty -- the one
// point where "not yet reached" and "unreachable" can't be confused.
void finish_major_cycle(MarkVisitor& v) {
    const auto finish_t0 = std::chrono::steady_clock::now();
    struct FinishTimer {
        std::chrono::steady_clock::time_point t0;
        ~FinishTimer() {
            static const bool p = env_flag("QUANTA_GC_PROFILE");
            if (p) std::fprintf(stderr, "[gc-prof] major finish_major_cycle=%ldus\n", static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()));
        }
    } finish_timer{finish_t0};
    ++Collector::sweep_generation_;
    static const bool verify = env_flag("QUANTA_GC_VERIFY");
    static const bool log = env_flag("QUANTA_GC_LOG");
    static const bool prof = env_flag("QUANTA_GC_PROFILE");

    // Function::call hands every call's Context to the survivor pool
    // unconditionally on the chance a closure captured it; most don't. A
    // survivor already reached above is genuinely still needed; one
    // EventLoop still has a pending timer/Promise/fiber against gets
    // force-traced (nothing else would mark its subgraph) and kept;
    // anything else is unreachable and deleted.
    //
    // The ephemeron fixpoint runs first: a WeakMap value whose key is alive is
    // reached only by it, and may be a closure of a Context the prune below
    // would otherwise free before the fixpoint got to the closure.
    auto fm0 = std::chrono::steady_clock::now();
    v.drain_ephemerons();
    bool any_forced = false;
    std::vector<std::pair<Engine*, std::vector<Context*>>> doomed;
    for (Engine* engine : Engine::all_engines()) {
        std::vector<Context*>& survivors = engine->mutable_survivor_contexts();
        std::vector<Context*> still_alive;
        still_alive.reserve(survivors.size());
        std::vector<Context*> candidates;
        for (Context* ctx : survivors) {
            if (v.context_seen(ctx)) {
                still_alive.push_back(ctx);
            } else if (EventLoop::instance().is_context_in_use(ctx)) {
                v.visit_context(ctx);
                still_alive.push_back(ctx);
                any_forced = true;
            } else {
                candidates.push_back(ctx);
            }
        }
        survivors = std::move(still_alive);
        doomed.emplace_back(engine, std::move(candidates));
    }
    // Trace the force-kept ones' own subgraph, and only then free anything: a
    // force-kept Context can reach a Function whose closure_context_ is one of
    // the candidates, so deciding and deleting in the same pass frees a
    // Context that this very drain then goes on to walk. Whatever the drain
    // reaches goes back into its own engine's pool.
    if (any_forced) {
        Collector::mark_step(std::chrono::microseconds(-1));
        v.drain_ephemerons();
        for (auto& [engine, candidates] : doomed) {
            std::vector<Context*> still_doomed;
            still_doomed.reserve(candidates.size());
            for (Context* ctx : candidates) {
                if (v.context_seen(ctx)) engine->mutable_survivor_contexts().push_back(ctx);
                else still_doomed.push_back(ctx);
            }
            candidates = std::move(still_doomed);
        }
    }
    for (auto& [engine, candidates] : doomed) {
        for (Context* ctx : candidates) delete ctx;
        // What is left was reached or was in use, and this cycle traced it.
        engine->set_survivor_traced(engine->mutable_survivor_contexts().size());
    }

    auto fm1 = std::chrono::steady_clock::now();
    v.drain_ephemerons();
    v.finalize_ephemerons();

    g_last_cycle = Collector::CycleStats{};
    g_last_cycle.minor = false;
    g_last_cycle.marked_cells = v.marked_cells;
    if (verify) run_verify(g_last_cycle);

    // Must run before sweep/decommit below: a fully-dead block can be
    // decommitted (madvise'd, pages zeroed) once swept, and a stale entry
    // here pointing into it would then read cell_size back as 0 --
    // slot_index's offset/cell_size divides by that and traps (SIGFPE).
    for (const Heap::ProbeResult& p : remembered_cells()) Heap::clear_remembered(p);
    remembered_cells().clear();
    // A sweep can free anything this still points at: whatever was reachable
    // is marked and whatever was not is gone, and either way the edges it
    // recorded have been accounted for by the mark that just finished.
    remembered_values().clear();
    auto fm2 = std::chrono::steady_clock::now();
    auto fm3 = std::chrono::steady_clock::now();

    auto fm4 = std::chrono::steady_clock::now();
    static const bool mark_only = env_flag("QUANTA_GC_MARK_ONLY");
    std::chrono::steady_clock::time_point fm5 = fm4, fm6 = fm4, fm7 = fm4;
    if (!mark_only) {
        g_last_cycle.swept_cells = run_sweep(/*minor=*/false);
        fm5 = std::chrono::steady_clock::now();
        // See the minor path for why the two halves of the cost are handed
        // over separately, and why the live figure comes from the rebuild.
        Heap::reset_dirty_blocks();
        const size_t live_after = Heap::rebuild_allocation_candidates();
        fm6 = std::chrono::steady_clock::now();
        const auto fa = std::chrono::steady_clock::now();
        Heap::retune_budget(live_after, g_scanned_words * sizeof(uint64_t));
        Heap::note_major_done(live_after);
        note_major_yield(g_last_cycle.marked_cells, g_last_cycle.swept_cells);
        g_expect_major = v.marked_cells;
        const auto fb = std::chrono::steady_clock::now();
        v.release_excess_capacity();
        const auto fc = std::chrono::steady_clock::now();
        ParallelMarkPool::instance().release_excess_capacity();
        const auto fd = std::chrono::steady_clock::now();
        Heap::decommit_idle_memory();
        fm7 = std::chrono::steady_clock::now();
        if (prof) {
            auto us2 = [](auto a, auto b) { return static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count()); };
            std::fprintf(stderr, "[gc-prof] major tail retune+yield=%ldus visitor_release=%ldus pool_release=%ldus decommit=%ldus\n", us2(fa, fb), us2(fb, fc), us2(fc, fd), us2(fd, fm7));
        }
    }
    if (prof) {
        auto us = [](auto a, auto b) { return static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count()); };
        std::fprintf(stderr, "[gc-prof] major finish phases: survivor_prune=%ldus ephemerons+verify_prelude=%ldus remembered_cleanup=%ldus pending_env_frees=%ldus env_survivor_prune=%ldus sweep=%ldus rebuild=%ldus retune+decommit=%ldus\n",
                     us(fm0, fm1), us(fm1, fm2), us(fm2, fm3), 0L, us(fm3, fm4), us(fm4, fm5), us(fm5, fm6), us(fm6, fm7));
    }

    if (prof) {
        auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - g_major_cycle_start).count();
        std::fprintf(stderr, "[gc-prof] major slices=%u total=%ldus marked=%zu\n",
                     g_major_slice_count, total_us, v.marked_cells);
        std::fprintf(stderr, "[gc-prof] par maxfrontier=%zu rounds=%zu left=%zu deferred=%zu marked per thread: %zu %zu %zu %zu\n", g_par_maxfront, g_par_rounds, g_par_left, g_par_deferred,
                     g_par_marked[0], g_par_marked[1], g_par_marked[2], g_par_marked[3]);
        std::fprintf(stderr, "[gc-prof] par deferred by type:");
        for (size_t t = 0; t < 48; t++) if (g_par_deferred_type[t]) std::fprintf(stderr, " type%zu=%zu", t, g_par_deferred_type[t]);
        std::fprintf(stderr, "\n");
        FunctionExecutable::print_chunk_profile();
        if (Heap::active_or_null()) Heap::active().print_block_profile();
    }
    if (log) {
        std::fprintf(stderr, "[gc] major slices=%u marked=%zu swept=%zu verify_violations=%zu\n",
                     g_major_slice_count, g_last_cycle.marked_cells, g_last_cycle.swept_cells,
                     g_last_cycle.verify_violations);
    }

    Collector::major_in_progress_ = false;
}

// If allocation keeps outpacing an open cycle's marking progress this long,
// the next slice ignores its budget and finishes the cycle in one go instead
// of letting it drag on indefinitely. Measured under QUANTA_GC_PROFILE=1
// against a sustained-allocation, growing-heap workload (heap climbing to
// 1M live objects while churning further short-lived ones): steady-state
// major cycles land around 850-950 slices; the transient cycles that run
// WHILE the heap is still growing (root re-scans keep finding genuinely new
// objects, so a single cycle marks more as it goes) topped out around 2.24s.
// Both thresholds keep roughly 2-3x headroom over that measured ceiling.
constexpr uint32_t kMaxSlicesBeforeForceFinish = 3000;
constexpr auto kMaxCycleWallClockBeforeForceFinish = std::chrono::seconds(5);

// Starts a fresh major cycle if none is open, otherwise continues the one
// already in progress. The roots are scanned when the cycle opens and again
// whenever marking has drained, not once per slice: the root set is the one
// part of the graph with no write barrier on it, so what it needs is a scan
// that no mutator step can follow, and that is exactly the scan taken at
// termination. A per-slice scan gives the same guarantee and also repeats the
// whole conservative stack walk, the context re-trace and the executable
// table for every one of a cycle's hundreds of slices.
Collector::SliceResult run_major_slice(std::chrono::microseconds budget) {
    static const bool prof = env_flag("QUANTA_GC_PROFILE");

    MarkVisitor& v = mark_visitor();
    bool cycle_opened = false;
    if (!Collector::major_in_progress_) {
        Heap::clear_gc_request();
        auto clear_t0 = std::chrono::steady_clock::now();
        Heap::clear_all_marks();
        if (prof) std::fprintf(stderr, "[gc-prof] major clear_all_marks=%ldus\n", static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - clear_t0).count()));
        v.reset_for_new_cycle();
        // Symmetric with clear_all_marks: this cycle re-derives reachability
        // from scratch, so every Context's "a real edge reached me" stamp goes
        // stale at the same instant and is re-set by this cycle's own tracing.
        if (++g_major_epoch == 0) ++g_major_epoch;
        Collector::major_in_progress_ = true;
        g_major_cycle_start = std::chrono::steady_clock::now();
        g_major_slice_count = 0;
        cycle_opened = true;
    }
    g_major_slice_count++;
    // Pacing valve: allocation has outpaced marking progress for too long --
    // stop taking small slices and finish the cycle in one go.
    bool forced_finish = false;
    if (budget.count() >= 0 &&
        (g_major_slice_count > kMaxSlicesBeforeForceFinish ||
         std::chrono::steady_clock::now() - g_major_cycle_start > kMaxCycleWallClockBeforeForceFinish)) {
        budget = std::chrono::microseconds(-1);
        forced_finish = true;
    }

    auto slice_t0 = prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    size_t marked_before = v.marked_cells;
    // Nothing has run between this scan and the drain below, so a cycle that
    // both opens and drains inside this one call needs no second scan.
    bool roots_scanned_here = cycle_opened;
    if (cycle_opened) scan_major_roots(v);
    // The settled executables of a new epoch are traced a slice's share at a time (see FunctionExecutable::gc_trace_roots); an unbounded slice does all that is left.
    if (FunctionExecutable::gc_settled_walk_pending()) {
        FunctionExecutable::gc_settled_walk_step(v, std::chrono::steady_clock::now() + (budget.count() > 0 ? budget / 2 : std::chrono::microseconds(0)), budget.count() >= 0);
    }
    Collector::SliceResult result = Collector::mark_step(budget);
    if (result == Collector::SliceResult::CycleComplete && !roots_scanned_here) {
        // Marking has drained, but the mutator has run since the last root
        // scan and the roots carry no barrier. Re-scan and drain again; only a
        // drain that immediately follows a scan may end the cycle, and if this
        // one runs out of budget the next slice tries again.
        scan_major_roots(v);
        result = Collector::mark_step(budget);
    }
    // A cycle does not end before every executable has been traced.
    if (result == Collector::SliceResult::CycleComplete && FunctionExecutable::gc_settled_walk_pending()) result = Collector::SliceResult::Continuing;
    if (prof) {
        auto took_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - slice_t0).count();
        std::fprintf(stderr, "[gc-prof] major slice=%u took=%ldus marked_delta=%zu%s%s\n",
                     g_major_slice_count, took_us, v.marked_cells - marked_before,
                     result == Collector::SliceResult::Continuing ? " continuing" : " complete",
                     forced_finish ? " forced-finish" : "");
    }
    if (result == Collector::SliceResult::Continuing) return result;
    finish_major_cycle(v);
    return result;
}

// QUANTA_GC_NO_BARRIER=1: perf-diagnosis knob. With barriers off, the
// safepoint policy runs full collections only -- minors would miscollect.
bool barriers_disabled() {
    static const bool disabled = env_flag("QUANTA_GC_NO_BARRIER");
    return disabled;
}

// Measured under QUANTA_GC_PROFILE=1: median/p90 slice time tracks this
// target closely (~510-520us) under sustained heavy allocation. The rare
// (~1%) outlier -- confirmed up to ~20-230ms, not this target being wrong
// -- is one Array whose elements_ holds a very large number of object
// references: Object::trace()'s `for (const auto& val : elements_)` visits
// them all in one uninterruptible trace_cell() call (mark_step only checks
// its deadline between steps, not within one). Strings don't share this
// risk (rope nodes are small, each a separate step); shape-mode named
// properties are capped at kMaxSlots. Deliberately not fixed here: making
// a single array's trace resumable across a slice boundary means its
// elements_ can be mutated by the mutator in between (push/pop/splice)
// before the resume point runs, which needs the same care as this
// session's write-barrier/epoch designs, not a quick chunked-loop patch.
std::chrono::microseconds default_major_slice_budget() {
    return std::chrono::microseconds(500);
}

// QUANTA_GC_SLICE_STRESS=1: an artificially tiny per-slice budget (mark_step
// always does at least one step() before its first deadline check) --
// maximizes how often the mutator runs between trace steps, to surface any
// interleaving bug as early and as deterministically as possible.
std::chrono::microseconds next_slice_budget() {
    static const bool slice_stress = env_flag("QUANTA_GC_SLICE_STRESS");
    if (slice_stress) return std::chrono::microseconds(0);
    return default_major_slice_budget();
}

}

void Collector::retire_heap(Heap* heap) {
    ++sweep_generation_;
    Heap::set_retiring(heap);
    collect();
    Heap::set_retiring(nullptr);
}

void Collector::collect() {
    run_major_slice(std::chrono::microseconds(-1));
}

void Collector::collect_minor() {
    if (barriers_disabled()) {
        run_major_slice(std::chrono::microseconds(-1));
    } else {
        run_minor_collection();
    }
}


Collector::SliceResult Collector::mark_step(std::chrono::microseconds budget) {
    MarkVisitor& v = mark_visitor();
    if (budget.count() < 0) {
        v.drain();
        return SliceResult::CycleComplete;
    }
    auto deadline = std::chrono::steady_clock::now() + budget;
    // step() traces a single cell, so checking the deadline after each one read
    // the clock once per cell and made the read itself a visible share of the
    // mark. A slice budget needs nothing like that resolution: check on a
    // stride, which costs at most a stride's worth of overrun.
    constexpr int kDeadlineStride = 128;
    int until_check = kDeadlineStride;
    for (;;) {
        if (v.should_parallelize() && !v.parallel_busy_) {
            v.parallel_round(true, deadline);
            if (std::chrono::steady_clock::now() >= deadline) {
                v.drain_contexts_and_environments();
                return SliceResult::Continuing;
            }
            continue;
        }
        if (!v.step()) break;
        if (--until_check > 0) continue;
        until_check = kDeadlineStride;
        if (std::chrono::steady_clock::now() >= deadline) {
            // Never yield with context/environment work pending -- see
            // drain_contexts_and_environments on why that's unsafe.
            v.drain_contexts_and_environments();
            return SliceResult::Continuing;
        }
    }
    return SliceResult::CycleComplete;
}

void Collector::write_barrier_slow(const void* cell) {
    if (barriers_disabled() || !cell) return;
    // Every caller hands its own `this` -- a cell base, never a guessed word.
    Heap::ProbeResult p = Heap::exact_cell_base(cell);
    // Young (or non-cell) targets need no record: a minor trace reaches every
    // live young cell from the roots; only marked-old cells go dark.
    if (!p.cell || !Heap::test_mark(p)) return;
    if (Heap::test_and_set_remembered(p)) return;
    remembered_cells().push_back(p);
    // Dijkstra insertion barrier: this container was already marked (so, if
    // an incremental major is in progress, possibly already traced/black)
    // and just gained a new edge -- re-trace it so the edge isn't missed.
    // Same dedup bit as above, safe to share: trace_cell clears it the
    // moment this container is actually re-traced (only while a major is
    // open), so a later mutation can re-arm this path again within the
    // same cycle.
    if (Collector::major_in_progress_) mark_visitor().push_remembered(p);
}

void Collector::write_barrier_value(const void* cell, const Value& value) {
    if (barriers_disabled() || !cell) return;
    // Nothing but a cell can hide a reference, so nothing else is worth
    // recording or shading.
    if (!value.is_object() && !value.is_function() && !value.is_string() &&
        !value.is_symbol() && !value.is_bigint()) {
        return;
    }
    // Shade the target, and on every write rather than only the first one
    // that dirties this container: this is the half that keeps an in-progress
    // mark from losing the edge.
    if (Collector::major_in_progress_) mark_visitor().visit(value);
    // A young container needs no record at all -- a minor reaches every live
    // young cell from the roots.
    Heap::ProbeResult p = Heap::exact_cell_base(cell);
    if (!p.cell || !Heap::test_mark(p)) return;
    // Old container, so the edge has to survive to the next minor. Record the
    // edge rather than the container: handing back the container makes the
    // minor walk every element it holds to find the one that changed, which
    // is the whole array on every append.
    remembered_values().push_back(value);
}

void Collector::write_barrier_env_for(Environment* env, const Value& value) {
    if (value.is_object() || value.is_function() || value.is_string() ||
        value.is_symbol() || value.is_bigint()) {
        write_barrier_env(env);
    }
}

void Collector::write_barrier_env(Environment* env) { write_barrier(env); }

void Collector::release_env(Environment* env) {
    if (!env) return;
    if (env->is_cell()) {
        // A scope nothing outlives gives its cell back at once, hot in the cache for the next one. Never while a major is marking: the conservative scan may have queued it.
        static const bool fast_free = !env_flag("QUANTA_ENV_NO_FAST_FREE");
        if (fast_free && !major_in_progress_ && env->provably_unreachable()) {
            env->~Environment();
            Heap::cell_free(env);
        }
        return;
    }
}


constinit thread_local uint32_t Collector::no_collect_depth_ = 0;
constinit thread_local bool Collector::full_pending_ = false;

bool Collector::collect_or_defer() {
    if (no_collect_depth_ != 0) {
        request_full_collection();
        return false;
    }
    full_pending_ = false;
    collect();
    return true;
}

void Collector::request_full_collection() {
    full_pending_ = true;
    Heap::request_gc();   // arms the safepoint, whose inline test only looks at this
}

bool Collector::collect_if_pending() {
    if (!full_pending_ || no_collect_depth_ != 0) return false;
    full_pending_ = false;
    collect();
    return true;
}

void Collector::safepoint_slow() {
    // Inside a pause nothing is freed, and a request stays asked for until it is over.
    if (no_collect_depth_ != 0) return;
    if (full_pending_) {
        full_pending_ = false;
        run_major_slice(std::chrono::microseconds(-1));
        return;
    }
    // QUANTA_GC_STRESS: "2" = minor at every safepoint (write-barrier soak,
    // full every 64th); any other truthy value = full at every safepoint.
    // Resolved into the shared field so safepoint()'s inline test can read
    // it; -1 until then, which that test treats as armed.
    if (stress_mode_ < 0) {
        const char* v = std::getenv("QUANTA_GC_STRESS");
        stress_mode_ = (!v || !*v || *v == '0') ? 0 : (*v == '2' ? 2 : 1);
    }
    const int stress = stress_mode_;
    static thread_local uint32_t cycle_count = 0;

    // Stress modes always run a collection to completion in one call, same
    // as before incremental slicing existed -- including finishing off an
    // incremental major that happened to already be open, rather than
    // taking another small slice at it. Otherwise a fast-allocating
    // workload could turn GC_STRESS's "aggressive, deterministic" testing
    // mode into an accidental incremental soak that never keeps pace.
    if (stress == 1) {
        run_major_slice(std::chrono::microseconds(-1));
        return;
    }
    if (stress == 2) {
        if (Collector::major_in_progress_ || ++cycle_count % 64 == 0) {
            run_major_slice(std::chrono::microseconds(-1));
        } else {
            run_minor_collection();
        }
        return;
    }

    // An open incremental major always continues before anything else is
    // considered: minor and major must never touch MarkVisitor's shared
    // worklists/mark-bit state in the same window.
    if (Collector::major_in_progress_) {
        run_major_slice(next_slice_budget());
        return;
    }

    if (Heap::gc_requested()) {
        // Majors must keep coming -- minors never reclaim old-generation
        // garbage -- but a fixed one-in-eight charges the same price whether
        // the last one paid for itself or not. A program whose old generation
        // is nearly all live pays a full mark of it to free what a minor
        // frees anyway, so the interval backs off when that is what happened
        // and snaps back the moment a major earns its keep. Backing off cannot
        // strand the survivor pool: a minor prunes it now (see
        // run_minor_collection), so it no longer depends on a major arriving.
        // The interval above backs off when majors come back empty-handed,
        // which is right until the heap starts growing on top of what the last
        // major left live: that growth is old-generation garbage no minor can
        // reclaim, and no amount of poor yield makes it collectable any other
        // way. Ask for one once the heap has added half of the live set again.
        // Scaled by the same backoff the interval uses. Growth on top of the
        // last live set is the signal that old-generation garbage is piling
        // up, but it is only a guess -- and when the majors it forces keep
        // coming back empty, it is a wrong guess. The interval is the
        // collector's own measurement of exactly that, so the threshold rides
        // it: unproductive majors make this trigger progressively harder to
        // reach instead of firing on the same half-a-live-set forever.
        const size_t live = Heap::live_after_major();
        static const size_t growth_percent = [] { const char* e = std::getenv("QUANTA_GC_GROWTH"); return e ? static_cast<size_t>(std::atoi(e)) : size_t(50); }();
        // A heap that is small is not worth a major for every megabyte it adds: marking has a fixed cost of its own (roots, stacks, the setup of the cycle), which a live
        // set of a few hundred kilobytes does not repay. So the growth that asks for one has a floor.
        static const size_t min_growth = [] { const char* e = std::getenv("QUANTA_GC_MIN_GROWTH_KB"); return (e ? static_cast<size_t>(std::atoi(e)) : size_t(4096)) * 1024; }();
        const size_t growth_needed = std::max(live / 100 * growth_percent, min_growth) * (major_interval() / kMajorIntervalFloor);
        const bool grown_enough = (live > 0 && Heap::bytes_since_major() >= growth_needed);
        if (!barriers_disabled() && !grown_enough && ++cycle_count % major_interval() != 0) {
            run_minor_collection();
        } else {
            static const bool why = env_flag("QUANTA_GC_PROFILE");
            if (why) {
                std::fprintf(stderr, "[gc-prof] major because %s: since_major=%zuKB live=%zuKB needed=%zuKB interval=%zu\n",
                             grown_enough ? "growth" : "interval",
                             Heap::bytes_since_major() / 1024, live / 1024, growth_needed / 1024, static_cast<size_t>(major_interval()));
            }
            run_major_slice(next_slice_budget());
        }
    }
}

const Collector::CycleStats& Collector::last_cycle() {
    return g_last_cycle;
}

void Collector::push_chunk(const BytecodeChunk* chunk) {
    chunk_roots().push_back(chunk);
}

void Collector::pop_chunk(const BytecodeChunk* chunk) {
    auto& roots = chunk_roots();
    for (size_t i = roots.size(); i-- > 0;) {
        if (roots[i] == chunk) { roots.erase(roots.begin() + static_cast<long>(i)); return; }
    }
}

void Collector::set_frame_env_scanner(FrameEnvScanner fn) { g_frame_env_scanner = fn; }

void Collector::set_value_stack(Value* base, Value* const* top) {
    g_value_stack.base = base;
    g_value_stack.top = top;
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

void Collector::purge_stack_roots(const void* lo, const void* hi) {
    const auto inside = [lo, hi](const void* p) { return p >= lo && p < hi; };
    auto& vectors = value_vector_roots();
    vectors.erase(std::remove_if(vectors.begin(), vectors.end(), inside), vectors.end());
    auto& arrays = value_array_roots();
    arrays.erase(std::remove_if(arrays.begin(), arrays.end(), inside), arrays.end());
}

void Collector::push_value_array(const FixedArray<Value>* arr) {
    value_array_roots().push_back(arr);
}

void Collector::pop_value_array(const FixedArray<Value>* arr) {
    auto& roots = value_array_roots();
    if (!roots.empty() && roots.back() == arr) { roots.pop_back(); return; }
    for (size_t i = roots.size(); i-- > 0;) {
        if (roots[i] == arr) { roots.erase(roots.begin() + i); return; }
    }
}

}
