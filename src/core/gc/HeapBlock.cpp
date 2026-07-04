/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/gc/HeapBlock.h"
#include <cassert>
#include <cstring>

namespace Quanta {

HeapBlock* HeapBlock::init(void* region, Heap* heap, CellKind kind,
                           HeapSegment segment, uint32_t cell_size) {
    assert((reinterpret_cast<uintptr_t>(region) & (kBlockSize - 1)) == 0 &&
           "block region must be 16KB aligned");
    assert(cell_size % kCellAlign == 0 && cell_size >= kCellAlign);

    auto* block = static_cast<HeapBlock*>(region);
    Header& h = block->h_;
    h.heap        = heap;
    h.next        = nullptr;
    h.free_list   = nullptr;
    h.cell_size   = cell_size;
    h.capacity    = static_cast<uint32_t>(kPayloadSize / cell_size);
    h.bump_cursor = 0;
    h.free_count  = 0;
    h.cell_kind   = kind;
    h.segment     = segment;
    std::memset(h.alloc_bitmap, 0, sizeof(h.alloc_bitmap));
    std::memset(h.mark_bitmap, 0, sizeof(h.mark_bitmap));
    return block;
}

void* HeapBlock::try_allocate() {
    void* p;
    if (h_.free_list) {
        p = h_.free_list;
        h_.free_list = h_.free_list->next;
        h_.free_count--;
    } else if (h_.bump_cursor < h_.capacity) {
        p = payload_start() + static_cast<size_t>(h_.bump_cursor) * h_.cell_size;
        h_.bump_cursor++;
    } else {
        return nullptr;
    }
    size_t idx = slot_index(p);
    h_.alloc_bitmap[idx / 64] |= (1ULL << (idx % 64));
    return p;
}

void HeapBlock::free_cell(void* p) {
    size_t idx = slot_index(p);
    assert(idx != SIZE_MAX && "free of pointer outside block payload");
    uint64_t bit = 1ULL << (idx % 64);
    assert((h_.alloc_bitmap[idx / 64] & bit) && "double free of heap cell");
    h_.alloc_bitmap[idx / 64] &= ~bit;
    auto* cell = static_cast<FreeCell*>(p);
    cell->next = h_.free_list;
    h_.free_list = cell;
    h_.free_count++;
}

size_t HeapBlock::slot_index(const void* p) const {
    const char* base = payload_start();
    const char* cp = static_cast<const char*>(p);
    if (cp < base) return SIZE_MAX;
    size_t offset = static_cast<size_t>(cp - base);
    size_t idx = offset / h_.cell_size;
    return idx < h_.capacity ? idx : SIZE_MAX;
}

bool HeapBlock::is_allocated(const void* p) const {
    size_t idx = slot_index(p);
    if (idx == SIZE_MAX) return false;
    return (h_.alloc_bitmap[idx / 64] >> (idx % 64)) & 1;
}

void* HeapBlock::cell_containing(const void* p) {
    size_t idx = slot_index(p);
    if (idx == SIZE_MAX) return nullptr;
    if (!((h_.alloc_bitmap[idx / 64] >> (idx % 64)) & 1)) return nullptr;
    return payload_start() + idx * h_.cell_size;
}

bool HeapBlock::test_mark(const void* p) const {
    size_t idx = slot_index(p);
    if (idx == SIZE_MAX) return false;
    return (h_.mark_bitmap[idx / 64] >> (idx % 64)) & 1;
}

void HeapBlock::set_mark(const void* p) {
    size_t idx = slot_index(p);
    if (idx == SIZE_MAX) return;
    h_.mark_bitmap[idx / 64] |= (1ULL << (idx % 64));
}

void HeapBlock::clear_marks() {
    std::memset(h_.mark_bitmap, 0, sizeof(h_.mark_bitmap));
}

}
