/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/gc/Heap.h"
#include "quanta/core/runtime/Value.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Quanta {

thread_local Heap* Heap::active_ = nullptr;
thread_local bool Heap::gc_requested_ = false;

Heap& Heap::active() {
    assert(active_ && "no active Heap -- Engine init must install a HeapScope "
                      "before any GC cell is created");
    return *active_;
}

namespace {

// Heaps this thread owns. A collection only ever touches the calling
// thread's own heaps -- agents never share GC-managed cells, so each
// thread's collector runs independently with no cross-thread locking.
std::vector<Heap*>& thread_heaps() {
    static thread_local std::vector<Heap*> heaps;
    return heaps;
}

// QUANTA_HEAP_STATS=1: dump every heap (any thread) at process exit. Heaps
// are immortal for now, so the raw pointers stay valid until the atexit
// handler runs. This list is process-wide by design (a diagnostic dump,
// not a collection root set), so it is the one registry here that still
// needs a lock.
std::mutex& all_heaps_mutex() {
    static std::mutex m;
    return m;
}
std::vector<Heap*>& all_heaps_for_stats() {
    static std::vector<Heap*> heaps;
    return heaps;
}

void dump_all_heap_stats() {
    static const char* kind_names[kNumCellKinds] = {"Object", "String", "Symbol", "BigInt"};
    std::lock_guard<std::mutex> lock(all_heaps_mutex());
    int i = 0;
    for (Heap* heap : all_heaps_for_stats()) {
        Heap::Stats s = heap->stats();
        std::fprintf(stderr,
            "[heap %d] chunks=%zu blocks=%zu live_cells=%zu live_bytes=%zu large=%zu/%zuB\n",
            i++, s.chunk_count, s.block_count, s.live_cells, s.live_bytes,
            s.large_count, s.large_bytes);
        for (size_t k = 0; k < kNumCellKinds; k++) {
            if (s.live_cells_by_kind[k])
                std::fprintf(stderr, "         %-6s %zu\n", kind_names[k], s.live_cells_by_kind[k]);
        }
    }
}

}

Heap::Heap() {
    thread_heaps().push_back(this);
    {
        std::lock_guard<std::mutex> lock(all_heaps_mutex());
        all_heaps_for_stats().push_back(this);
    }
    static bool stats_hook = [] {
        if (std::getenv("QUANTA_HEAP_STATS")) std::atexit(dump_all_heap_stats);
        return true;
    }();
    (void)stats_hook;
}

Heap::~Heap() {
    // Nothing is finalized or freed here yet. Chunks and large cells
    // outlive the heap on purpose -- static destructors delete cells after
    // the owning engine dies (see ~BlockAllocator). Process exit reclaims.
}

namespace {

// (size+15)/16 -> class index, one byte per 16B step up to the Tier-1 cap.
struct SizeClassTable {
    uint8_t index[Heap::kMaxTier1Size / 16 + 1];
    constexpr SizeClassTable() : index() {
        size_t cls = 0;
        for (size_t step = 0; step <= Heap::kMaxTier1Size / 16; step++) {
            size_t size = step * 16;
            while (cls < Heap::kNumSizeClasses && size > Heap::kSizeClasses[cls]) cls++;
            index[step] = static_cast<uint8_t>(cls);
        }
    }
};
constexpr SizeClassTable kSizeClassTable;

}

size_t Heap::size_class_index(size_t size) {
    if (size > kMaxTier1Size) return kNumSizeClasses;  // large object
    return kSizeClassTable.index[(size + 15) / 16];
}

HeapBlock* Heap::fresh_block(CellKind kind, HeapSegment segment, size_t cls) {
    void* region = block_allocator_.allocate_block_region();
    HeapBlock* block = HeapBlock::init(region, this, kind, segment, kSizeClasses[cls]);
    size_t k = static_cast<size_t>(kind);
    block->set_next(all_blocks_[k][cls]);
    all_blocks_[k][cls] = block;
    block_count_++;
    return block;
}

void* Heap::allocate(size_t size, CellKind kind, HeapSegment segment) {
    if (size == 0) size = 1;
    // ~8MB of new cells between collections; the interpreter safepoint
    // consumes the request (never collect mid-allocation).
    static thread_local size_t bytes_since_gc = 0;
    bytes_since_gc += size;
    if (bytes_since_gc >= 8 * 1024 * 1024) {
        bytes_since_gc = 0;
        gc_requested_ = true;
    }
    size_t cls = size_class_index(size);
    if (cls == kNumSizeClasses) return allocate_large(size, kind);

    size_t k = static_cast<size_t>(kind);
    HeapBlock* block = active_block_[k][cls];
    if (block) {
        if (void* p = block->try_allocate()) return p;
    }
    auto& partial = partial_blocks_[k][cls];
    while (!partial.empty()) {
        block = partial.back();
        partial.pop_back();
        if (void* p = block->try_allocate()) {
            active_block_[k][cls] = block;
            return p;
        }
    }
    block = fresh_block(kind, segment, cls);
    active_block_[k][cls] = block;
    void* p = block->try_allocate();
    assert(p && "fresh block must satisfy a Tier-1 allocation");
    return p;
}

void* Heap::allocate_large(size_t size, CellKind kind) {
    size_t total = kLargeHeaderSize + ((size + 15) & ~size_t(15));
    auto* lc = static_cast<LargeCell*>(std::malloc(total));
    if (!lc) std::abort();
    lc->magic = kLargeMagic;
    lc->heap = this;
    lc->prev = nullptr;
    lc->next = large_cells_;
    lc->size = size;
    lc->kind = kind;
    lc->marked = false;
    lc->remembered = false;
    if (large_cells_) large_cells_->prev = lc;
    large_cells_ = lc;
    return reinterpret_cast<char*>(lc) + kLargeHeaderSize;
}

void Heap::free_large(void* p) {
    auto* lc = reinterpret_cast<LargeCell*>(static_cast<char*>(p) - kLargeHeaderSize);
    assert(lc->magic == kLargeMagic && "free_large on non-large cell");
    Heap* heap = lc->heap;
    if (lc->prev) lc->prev->next = lc->next;
    else heap->large_cells_ = lc->next;
    if (lc->next) lc->next->prev = lc->prev;
    std::free(lc);
}

void Heap::cell_free(void* p) {
    if (!p) return;
    if (BlockAllocator::owns_address(p)) {
        HeapBlock::from_cell(p)->free_cell(p);
    } else {
        free_large(p);
    }
}

bool Heap::contains(const void* p) const {
    return find_cell(p) != nullptr;
}

void* Heap::find_cell(const void* p) const {
    if (!p) return nullptr;
    if (BlockAllocator::owns_address(p)) {
        HeapBlock* block = HeapBlock::from_cell(p);
        if (block->heap() != this) return nullptr;
        return block->cell_containing(p);
    }
    // Large cells: exact payload pointers only. Interior pointers into large
    // cells are not resolvable without a per-heap range index; whether the
    // conservative scanner needs one is an open question (expected: large
    // cells are few, a small sorted vector suffices).
    for (LargeCell* lc = large_cells_; lc; lc = lc->next) {
        if (reinterpret_cast<char*>(lc) + kLargeHeaderSize == p) return const_cast<void*>(p);
    }
    return nullptr;
}

namespace {

bool owned_by_this_thread(const Heap* heap) {
    for (Heap* h : thread_heaps()) {
        if (h == heap) return true;
    }
    return false;
}

// Exact or interior pointer -> live cell of a heap this thread owns.
// owns_address() spans every thread's chunks, so a stale stack word aiming
// into a foreign heap must be rejected here: marking or tracing it would
// touch cells another thread is mutating.
Heap::ProbeResult probe_pointer(void* p) {
    Heap::ProbeResult r;
    if (!p) return r;
    if (BlockAllocator::owns_address(p)) {
        HeapBlock* block = HeapBlock::from_cell(p);
        if (!owned_by_this_thread(block->heap())) return r;
        if (void* base = block->cell_containing(p)) {
            r.cell = base;
            r.kind = block->cell_kind();
        }
        return r;
    }
    for (Heap* heap : thread_heaps()) {
        for (auto* lc = heap->large_cells_head(); lc; lc = lc->next) {
            char* payload = reinterpret_cast<char*>(lc) + Heap::kLargeHeaderSize;
            if (p >= payload && p < payload + lc->size) {
                r.cell = payload;
                r.kind = lc->kind;
                r.is_large = true;
                return r;
            }
        }
    }
    return r;
}

}

Heap::ProbeResult Heap::probe_word(uint64_t word) {
    ProbeResult r = probe_pointer(reinterpret_cast<void*>(word));
    if (r.cell) return r;
    if (void* boxed = Value::gc_payload_of_bits(word)) return probe_pointer(boxed);
    return r;
}

Heap::ProbeResult Heap::exact_cell(const void* p) {
    return probe_pointer(const_cast<void*>(p));
}

bool Heap::test_mark(const ProbeResult& p) {
    if (!p.cell) return true;  // non-cell: nothing to mark
    if (p.is_large) {
        auto* lc = reinterpret_cast<LargeCell*>(static_cast<char*>(p.cell) - kLargeHeaderSize);
        return lc->marked;
    }
    return HeapBlock::from_cell(p.cell)->test_mark(p.cell);
}

void Heap::set_mark(const ProbeResult& p) {
    if (!p.cell) return;
    if (p.is_large) {
        auto* lc = reinterpret_cast<LargeCell*>(static_cast<char*>(p.cell) - kLargeHeaderSize);
        lc->marked = true;
        return;
    }
    HeapBlock::from_cell(p.cell)->set_mark(p.cell);
}

bool Heap::test_and_set_remembered(const ProbeResult& p) {
    if (!p.cell) return true;
    if (p.is_large) {
        auto* lc = reinterpret_cast<LargeCell*>(static_cast<char*>(p.cell) - kLargeHeaderSize);
        bool was = lc->remembered;
        lc->remembered = true;
        return was;
    }
    return HeapBlock::from_cell(p.cell)->test_and_set_remembered(p.cell);
}

void Heap::clear_remembered(const ProbeResult& p) {
    if (!p.cell) return;
    if (p.is_large) {
        auto* lc = reinterpret_cast<LargeCell*>(static_cast<char*>(p.cell) - kLargeHeaderSize);
        lc->remembered = false;
        return;
    }
    HeapBlock::from_cell(p.cell)->clear_remembered(p.cell);
}

void Heap::rebuild_allocation_candidates() {
    for (Heap* heap : thread_heaps()) {
        for (size_t k = 0; k < kNumCellKinds; k++) {
            for (size_t c = 0; c < kNumSizeClasses; c++) {
                auto& partial = heap->partial_blocks_[k][c];
                partial.clear();
                for (HeapBlock* b = heap->all_blocks_[k][c]; b; b = b->next()) {
                    if (b != heap->active_block_[k][c] && !b->is_full()) {
                        partial.push_back(b);
                    }
                }
            }
        }
    }
}

void Heap::clear_all_marks() {
    for (Heap* heap : thread_heaps()) {
        for (size_t k = 0; k < kNumCellKinds; k++) {
            for (size_t c = 0; c < kNumSizeClasses; c++) {
                for (HeapBlock* b = heap->all_blocks_[k][c]; b; b = b->next()) {
                    b->clear_marks();
                }
            }
        }
        for (LargeCell* lc = heap->large_cells_; lc; lc = lc->next) lc->marked = false;
    }
}

void Heap::for_each_cell(const std::function<void(void*, CellKind, bool)>& fn) {
    for (Heap* heap : thread_heaps()) {
        for (size_t k = 0; k < kNumCellKinds; k++) {
            for (size_t c = 0; c < kNumSizeClasses; c++) {
                for (HeapBlock* b = heap->all_blocks_[k][c]; b; b = b->next()) {
                    CellKind kind = b->cell_kind();
                    b->for_each_cell([&](void* cell, bool marked) { fn(cell, kind, marked); });
                }
            }
        }
        for (LargeCell* lc = heap->large_cells_; lc; lc = lc->next) {
            fn(reinterpret_cast<char*>(lc) + kLargeHeaderSize, lc->kind, lc->marked);
        }
    }
}

Heap::Stats Heap::stats() const {
    Stats s;
    s.chunk_count = block_allocator_.chunk_count();
    s.block_count = block_count_;
    for (size_t k = 0; k < kNumCellKinds; k++) {
        for (size_t c = 0; c < kNumSizeClasses; c++) {
            for (HeapBlock* b = all_blocks_[k][c]; b; b = b->next()) {
                s.live_cells += b->live_count();
                s.live_bytes += static_cast<size_t>(b->live_count()) * b->cell_size();
                s.live_cells_by_kind[k] += b->live_count();
            }
        }
    }
    for (LargeCell* lc = large_cells_; lc; lc = lc->next) {
        s.large_count++;
        s.large_bytes += lc->size;
        s.live_cells_by_kind[static_cast<size_t>(lc->kind)]++;
    }
    return s;
}

}
