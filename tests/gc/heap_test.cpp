/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Standalone unit tests for the GC heap (make heap-test).
 */

#include "quanta/core/gc/Heap.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

using namespace Quanta;

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_alignment_and_block_mapping() {
    Heap heap;
    for (size_t sz : {1ul, 16ul, 17ul, 96ul, 256ul, 2272ul, 4096ul}) {
        void* p = heap.allocate(sz, CellKind::Object);
        CHECK(p != nullptr);
        CHECK((reinterpret_cast<uintptr_t>(p) & (HeapBlock::kCellAlign - 1)) == 0);
        HeapBlock* b = HeapBlock::from_cell(p);
        CHECK(b->heap() == &heap);
        CHECK(b->cell_kind() == CellKind::Object);
        CHECK(b->cell_size() >= sz);
        CHECK(heap.contains(p));
    }
}

static void test_size_class_boundaries() {
    Heap heap;
    // One byte over a class boundary must land in the next class.
    void* a = heap.allocate(96, CellKind::Object);
    void* b = heap.allocate(97, CellKind::Object);
    CHECK(HeapBlock::from_cell(a)->cell_size() == 96);
    CHECK(HeapBlock::from_cell(b)->cell_size() == 112);
    // Tier-1 cap goes to blocks, one past it to the LOS.
    void* c = heap.allocate(4096, CellKind::Object);
    void* d = heap.allocate(4097, CellKind::Object);
    CHECK(BlockAllocator::owns_address(c));
    CHECK(!BlockAllocator::owns_address(d));
    CHECK(heap.contains(d));
    Heap::cell_free(d);
    CHECK(!heap.contains(d));
}

static void test_bump_freelist_and_reuse() {
    Heap heap;
    void* p1 = heap.allocate(64, CellKind::Object);
    void* p2 = heap.allocate(64, CellKind::Object);
    CHECK(p1 != p2);
    Heap::cell_free(p1);
    CHECK(!heap.contains(p1));
    void* p3 = heap.allocate(64, CellKind::Object);
    CHECK(p3 == p1);  // LIFO free-list reuse
    CHECK(heap.contains(p3));
    CHECK(heap.contains(p2));
}

static void test_block_overflow_multi_block() {
    Heap heap;
    // 64B cells -> a block holds kPayloadSize/64 cells; force several blocks.
    size_t per_block = HeapBlock::kPayloadSize / 64;
    std::vector<void*> cells;
    std::set<HeapBlock*> blocks;
    for (size_t i = 0; i < per_block * 3 + 5; i++) {
        void* p = heap.allocate(64, CellKind::Object);
        cells.push_back(p);
        blocks.insert(HeapBlock::from_cell(p));
    }
    CHECK(blocks.size() >= 3);
    for (void* p : cells) CHECK(heap.contains(p));
    // Distinct addresses.
    std::set<void*> unique(cells.begin(), cells.end());
    CHECK(unique.size() == cells.size());
}

static void test_interior_pointers() {
    Heap heap;
    char* p = static_cast<char*>(heap.allocate(128, CellKind::Object));
    CHECK(heap.find_cell(p) == p);
    CHECK(heap.find_cell(p + 1) == p);
    CHECK(heap.find_cell(p + 127) == p);
    // A never-allocated slot in the same block is not a cell.
    HeapBlock* b = HeapBlock::from_cell(p);
    CHECK(b->cell_containing(p + 128) == nullptr);
    // Stack/garbage addresses are not cells.
    int local = 0;
    CHECK(heap.find_cell(&local) == nullptr);
}

static void test_kind_segregation() {
    Heap heap;
    void* o = heap.allocate(48, CellKind::Object);
    void* s = heap.allocate(48, CellKind::String);
    CHECK(HeapBlock::from_cell(o) != HeapBlock::from_cell(s));
    CHECK(HeapBlock::from_cell(o)->cell_kind() == CellKind::Object);
    CHECK(HeapBlock::from_cell(s)->cell_kind() == CellKind::String);
}

static void test_two_heaps_isolated() {
    Heap h1, h2;
    void* p1 = h1.allocate(64, CellKind::Object);
    void* p2 = h2.allocate(64, CellKind::Object);
    CHECK(h1.contains(p1) && !h1.contains(p2));
    CHECK(h2.contains(p2) && !h2.contains(p1));
    // cell_free recovers the owner from memory, active heap irrelevant.
    HeapScope scope(&h1);
    Heap::cell_free(p2);
    CHECK(!h2.contains(p2));
}

static void test_active_heap_scope() {
    CHECK(Heap::active_or_null() == nullptr);
    Heap h1;
    {
        HeapScope s1(&h1);
        CHECK(Heap::active_or_null() == &h1);
        Heap h2;
        {
            HeapScope s2(&h2);
            CHECK(Heap::active_or_null() == &h2);
        }
        CHECK(Heap::active_or_null() == &h1);
    }
    CHECK(Heap::active_or_null() == nullptr);
}

static void test_stats() {
    Heap heap;
    heap.allocate(64, CellKind::Object);
    heap.allocate(64, CellKind::String);
    void* big = heap.allocate(10000, CellKind::Object);
    Heap::Stats s = heap.stats();
    CHECK(s.live_cells == 2);
    CHECK(s.large_count == 1);
    CHECK(s.large_bytes == 10000);
    CHECK(s.live_cells_by_kind[static_cast<size_t>(CellKind::Object)] == 2);  // 1 block + 1 large
    CHECK(s.live_cells_by_kind[static_cast<size_t>(CellKind::String)] == 1);
    Heap::cell_free(big);
    s = heap.stats();
    CHECK(s.large_count == 0);
}

static void test_mark_bitmap_reserved() {
    Heap heap;
    void* p = heap.allocate(64, CellKind::Object);
    HeapBlock* b = HeapBlock::from_cell(p);
    CHECK(!b->test_mark(p));
    b->set_mark(p);
    CHECK(b->test_mark(p));
    b->clear_marks();
    CHECK(!b->test_mark(p));
}

static void test_churn() {
    // Allocation/free churn across classes; catches free-list corruption.
    Heap heap;
    std::vector<void*> live;
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 200000; i++) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t sz = 16 + (rng >> 33) % 512;
        if (live.size() > 1000 && (rng & 1)) {
            size_t idx = (rng >> 8) % live.size();
            Heap::cell_free(live[idx]);
            live[idx] = live.back();
            live.pop_back();
        } else {
            void* p = heap.allocate(sz, CellKind::Object);
            std::memset(p, 0xAB, sz);  // scribble: catches overlapping slots
            live.push_back(p);
        }
    }
    for (void* p : live) CHECK(heap.contains(p));
    Heap::Stats s = heap.stats();
    CHECK(s.live_cells == live.size());
}

int main() {
    test_alignment_and_block_mapping();
    test_size_class_boundaries();
    test_bump_freelist_and_reuse();
    test_block_overflow_multi_block();
    test_interior_pointers();
    test_kind_segregation();
    test_two_heaps_isolated();
    test_active_heap_scope();
    test_stats();
    test_mark_bitmap_reserved();
    test_churn();

    if (failures == 0) {
        std::printf("heap-test: ALL PASS\n");
        return 0;
    }
    std::printf("heap-test: %d FAILURE(S)\n", failures);
    return 1;
}
