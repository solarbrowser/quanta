/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_GC_HEAPBLOCK_H
#define QUANTA_GC_HEAPBLOCK_H

#include "quanta/core/gc/CellKind.h"
#include <cstddef>
#include <cstdint>

namespace Quanta {

class Heap;

// 16 KB aligned block holding cells of a single (kind, size class).
// Cell address -> block: mask the low 14 bits. All GC metadata (alloc/mark
// bitmaps, kind, size) lives in the block header; cells stay headerless.
class HeapBlock {
public:
    static constexpr size_t kBlockSize   = 16 * 1024;
    static constexpr uintptr_t kBlockMask = ~(static_cast<uintptr_t>(kBlockSize) - 1);
    static constexpr size_t kMaxSlots    = 1024;   // 16B min cell over ~16KB payload
    static constexpr size_t kBitmapWords = kMaxSlots / 64;
    static constexpr size_t kCellAlign   = 16;

    static HeapBlock* from_cell(const void* p) {
        return reinterpret_cast<HeapBlock*>(reinterpret_cast<uintptr_t>(p) & kBlockMask);
    }

    // Constructs a block in place over a 16KB aligned region.
    static HeapBlock* init(void* region, Heap* heap, CellKind kind,
                           HeapSegment segment, uint32_t cell_size);

    void* try_allocate();          // nullptr when full
    void  free_cell(void* p);      // explicit delete path (unique_ptr interop)
    // Poison mode: drop the cell from the alloc bitmap WITHOUT recycling the
    // slot, so a use-after-free hits poisoned bytes instead of a fresh cell.
    void  retire_cell(void* p);

    bool  is_allocated(const void* p) const;
    // First payload byte of the cell containing p, or nullptr when p does not
    // point into a live cell. Interior pointers resolve to their cell base --
    // required by the conservative root scanner.
    void* cell_containing(const void* p);

    Heap*       heap() const        { return h_.heap; }
    CellKind    cell_kind() const   { return h_.cell_kind; }
    HeapSegment segment() const     { return h_.segment; }
    uint32_t    cell_size() const   { return h_.cell_size; }
    uint32_t    capacity() const    { return h_.capacity; }
    uint32_t    live_count() const  { return h_.bump_cursor - h_.free_count; }
    bool        is_empty() const    { return live_count() == 0; }
    bool        is_full() const     { return !h_.free_list && h_.bump_cursor == h_.capacity; }

    HeapBlock*  next() const           { return h_.next; }
    void        set_next(HeapBlock* n) { h_.next = n; }

    bool test_mark(const void* p) const;
    void set_mark(const void* p);
    void clear_marks();

    // Remembered set membership (write barrier dedup): returns the previous
    // state, setting the bit as a side effect.
    bool test_and_set_remembered(const void* p);
    void clear_remembered(const void* p);

    // fn(cell_base, marked) for every allocated cell in this block.
    template <typename Fn>
    void for_each_cell(Fn&& fn) {
        for (uint32_t i = 0; i < h_.bump_cursor; i++) {
            if (!((h_.alloc_bitmap[i / 64] >> (i % 64)) & 1)) continue;
            void* cell = payload_start() + static_cast<size_t>(i) * h_.cell_size;
            fn(cell, ((h_.mark_bitmap[i / 64] >> (i % 64)) & 1) != 0);
        }
    }

private:
    struct FreeCell { FreeCell* next; };

    struct Header {
        Heap*      heap;
        HeapBlock* next;
        FreeCell*  free_list;
        uint32_t   cell_size;
        uint32_t   capacity;
        uint32_t   bump_cursor;
        uint32_t   free_count;
        CellKind   cell_kind;
        HeapSegment segment;
        uint64_t   alloc_bitmap[kBitmapWords];
        uint64_t   mark_bitmap[kBitmapWords];
        uint64_t   remembered_bitmap[kBitmapWords];
    };

public:
    static constexpr size_t kHeaderSize  = (sizeof(Header) + kCellAlign - 1) & ~(kCellAlign - 1);
    static constexpr size_t kPayloadSize = kBlockSize - kHeaderSize;

private:
    char*  payload_start()            { return reinterpret_cast<char*>(this) + kHeaderSize; }
    const char* payload_start() const { return reinterpret_cast<const char*>(this) + kHeaderSize; }
    // Slot index of p, or SIZE_MAX when p lies outside the payload.
    size_t slot_index(const void* p) const;

    Header h_;
};

static_assert(HeapBlock::kPayloadSize / 16 <= HeapBlock::kMaxSlots,
              "alloc/mark bitmaps must cover every 16-byte slot");

}

#endif
