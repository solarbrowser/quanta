/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_GC_HEAP_H
#define QUANTA_GC_HEAP_H

#include "quanta/core/gc/BlockAllocator.h"
#include "quanta/core/gc/CellKind.h"
#include "quanta/core/gc/HeapBlock.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace Quanta {

// One Heap per Engine (each trust domain gets its own heap). Single-writer:
// all allocation happens on the main thread, so the fast path takes no locks.
class Heap {
public:
    static constexpr uint32_t kSizeClasses[] = {
        16, 24, 32, 48, 64, 80, 88, 96, 112, 128, 160, 192, 208, 224, 232, 240, 256,
        320, 384, 448, 512, 640, 768, 1024, 1280, 1536,
        2048, 2560, 3072, 4096
    };
    static constexpr size_t kNumSizeClasses =
        sizeof(kSizeClasses) / sizeof(kSizeClasses[0]);
    static constexpr size_t kMaxTier1Size = 4096;

    struct Stats {
        size_t chunk_count = 0;
        size_t block_count = 0;
        size_t live_cells = 0;
        size_t live_bytes = 0;          // cell_size * live cells
        size_t large_count = 0;
        size_t large_bytes = 0;
        size_t live_cells_by_kind[kNumCellKinds] = {};
    };

    Heap();
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    // Active heap of this thread; set by Engine init (HeapScope), read by
    // the cell operator new hooks.
    static Heap* active_or_null() { return active_; }
    static Heap& active();
    static void  set_active(Heap* heap) { active_ = heap; }

    void* allocate(size_t size, CellKind kind,
                   HeapSegment segment = HeapSegment::Core);

    struct ProbeResult {
        void* cell = nullptr;
        CellKind kind = CellKind::Object;
        bool is_large = false;
    };
    // Conservative word probe across ALL heaps: raw pointer or NaN-boxed
    // Value bits -> live cell base, or nullptr. Interior pointers resolve
    // for block cells; large cells match on their payload range.
    static ProbeResult probe_word(uint64_t word);

    static bool test_mark(const ProbeResult& p);
    static void set_mark(const ProbeResult& p);
    // Exact, known-live cell (from a trace edge, not a guess).
    static ProbeResult exact_cell(const void* p);
    // Marks a cell named by a trace edge, where the pointer is a cell base
    // and the kind is known from the edge itself. Returns the cell when this
    // call is the one that marked it, and nothing when it was already marked
    // or does not belong to this thread. Unlike exact_cell it never resolves
    // an interior pointer or reads the kind back out of the block, neither of
    // which an edge needs.
    static ProbeResult mark_exact(const void* p, CellKind kind);
    // The cell a pointer names, where the pointer is known to be a cell base
    // -- the write barrier's container, not a guessed stack word. Skips the
    // interior-pointer resolution the conservative probe has to do; the kind
    // still comes from the block, since the barrier's callers do not carry it.
    static ProbeResult exact_cell_base(const void* p);

    // Write-barrier dedup bit: previous state, set as a side effect.
    static bool test_and_set_remembered(const ProbeResult& p);
    static void clear_remembered(const ProbeResult& p);

    static void clear_all_marks();   // every heap, every block, every large
    // Walks every live cell of every heap: fn(cell, kind, marked).
    static void for_each_cell(const std::function<void(void*, CellKind, bool)>& fn);
    // Sweep's fast path: fn(cell, kind) for every dead cell only (see
    // HeapBlock::for_each_dead_cell for the word-level skip).
    static void for_each_dead_cell(const std::function<void(void*, CellKind)>& fn);

    // Post-sweep: re-queue every block with free slots as an allocation
    // candidate, so reclaimed cells actually get reused.
    static void rebuild_allocation_candidates();

    // Full idle-chunk scan (see BlockAllocator::decommit_idle_chunks) --
    // major-collection-only housekeeping, not part of the minor pause budget.
    static void decommit_idle_memory();

    // Allocation-triggered GC request; the interpreter's safepoint consumes it.
    static bool gc_requested() { return gc_requested_; }
    static void request_gc()   { gc_requested_ = true; }
    static void clear_gc_request() { gc_requested_ = false; }
    // Bytes allocated since the last major finished, and the live set it left
    // behind. A major is due once the heap has grown by a share of what was
    // live: that is the signal that old-generation garbage is piling up, and
    // it does not depend on how many contexts happened to survive.
    static size_t bytes_since_major() { return bytes_since_major_; }
    static void note_bytes_since_major(size_t bytes);
    static void note_major_done(size_t live_bytes) {
        bytes_since_major_ = 0;
        live_after_major_ = live_bytes;
    }
    static size_t live_after_major() { return live_after_major_; }
    // Charges `bytes` toward gc_requested()'s budget for memory the cell heap
    // doesn't see directly (survivor Contexts). An ordinary charge: a minor
    // reclaims that pool now, so its growth no longer has to buy a major.
    static void note_extra_bytes(size_t bytes);
    // Sets the allocation budget from what a collection just cost: the live
    // set it marked and the roots it had to scan, which are budgeted for
    // differently (see the definition).
    static void retune_budget(size_t live_bytes, size_t root_scan_bytes);
    // Explicit-free path for `delete` (unique_ptr interop). Static: the
    // owning heap is recovered from the memory itself, so a cell created in
    // one realm and deleted while another realm's heap is active stays safe.
    static void cell_free(void* p);

    // True when p points into (or at) a live cell of THIS heap.
    bool contains(const void* p) const;
    // Cell base address for p (interior pointers OK), nullptr when not a
    // live cell of this heap -- the conservative root scanner's query.
    void* find_cell(const void* p) const;

    Stats stats() const;

    struct LargeCell {
        uint64_t   magic;
        Heap*      heap;
        LargeCell* prev;
        LargeCell* next;
        size_t     size;
        CellKind   kind;
        bool       marked;
        bool       remembered;
        // payload follows, 16B aligned
    };
    static constexpr size_t kLargeHeaderSize =
        (sizeof(LargeCell) + HeapBlock::kCellAlign - 1) & ~(HeapBlock::kCellAlign - 1);
    LargeCell* large_cells_head() const { return large_cells_; }

private:
    static constexpr uint64_t kLargeMagic = 0x514C41524745ULL;  // "QLARGE"

    static size_t size_class_index(size_t size);
    HeapBlock* fresh_block(CellKind kind, HeapSegment segment, size_t cls);
    void* allocate_large(size_t size, CellKind kind);
    static void free_large(void* p);

    static thread_local Heap* active_;
    static thread_local bool gc_requested_;
    static thread_local size_t bytes_since_major_;
    static thread_local size_t live_after_major_;

    BlockAllocator block_allocator_;
    // Current allocation target per (kind, class); full blocks rotate into
    // all_blocks_ chains and come back via partial_blocks_ after a sweep.
    HeapBlock* active_block_[kNumCellKinds][kNumSizeClasses] = {};
    HeapBlock* all_blocks_[kNumCellKinds][kNumSizeClasses] = {};
    std::vector<HeapBlock*> partial_blocks_[kNumCellKinds][kNumSizeClasses];
    LargeCell* large_cells_ = nullptr;
    size_t block_count_ = 0;
};

// RAII: makes a heap the thread's active heap for its lifetime.
class HeapScope {
public:
    explicit HeapScope(Heap* heap) : previous_(Heap::active_or_null()) {
        Heap::set_active(heap);
    }
    ~HeapScope() { Heap::set_active(previous_); }

    HeapScope(const HeapScope&) = delete;
    HeapScope& operator=(const HeapScope&) = delete;

private:
    Heap* previous_;
};

}

#endif
