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
    // 16B steps to 256, coarser above; Tier-1 cap fits AsyncGenerator (2272B).
    static constexpr uint32_t kSizeClasses[] = {
        16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256,
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

    // Write-barrier dedup bit: previous state, set as a side effect.
    static bool test_and_set_remembered(const ProbeResult& p);
    static void clear_remembered(const ProbeResult& p);

    static void clear_all_marks();   // every heap, every block, every large
    // Walks every live cell of every heap: fn(cell, kind, marked).
    static void for_each_cell(const std::function<void(void*, CellKind, bool)>& fn);

    // Post-sweep: re-queue every block with free slots as an allocation
    // candidate, so reclaimed cells actually get reused.
    static void rebuild_allocation_candidates();

    // Allocation-triggered GC request; the interpreter's safepoint consumes it.
    static bool gc_requested() { return gc_requested_; }
    static void request_gc()   { gc_requested_ = true; }
    static void clear_gc_request() { gc_requested_ = false; }
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
