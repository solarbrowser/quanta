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
    bool        is_full() const     { return !h_.free_count && h_.bump_cursor == h_.capacity; }

    HeapBlock*  next() const           { return h_.next; }
    void        set_next(HeapBlock* n) { h_.next = n; }

    // Test-and-set for a cell reached from a trace edge. The probe / test /
    // set sequence resolved the same slot index three times over, once per
    // call, and the index costs a division because the size classes are not
    // powers of two; an edge already names a cell base, so one pass answers
    // all three questions.
    // Inline: every trace edge and every barrier asks one of these, and the
    // callers are in other translation units, so each question was a call for
    // a body that is a multiply, a shift and a bit test.
    size_t slot_index(const void* p) const {
        const char* base = payload_start();
        const char* cp = static_cast<const char*>(p);
        if (cp < base) return SIZE_MAX;
        const size_t offset = static_cast<size_t>(cp - base);
        const size_t idx = static_cast<size_t>(
            (static_cast<uint64_t>(offset) * h_.cell_size_magic) >> 32);
        return idx < h_.capacity ? idx : SIZE_MAX;
    }

    bool mark_if_unmarked(const void* p) {
        const size_t idx = slot_index(p);
        if (idx == SIZE_MAX) return false;
        const size_t word = idx / 64;
        const uint64_t bit = static_cast<uint64_t>(1) << (idx % 64);
        if (!(h_.bm[word].alloc & bit)) return false;
        if (h_.bm[word].mark & bit) return false;
        h_.bm[word].mark |= bit;
        return true;
    }

    bool test_mark(const void* p) const {
        const size_t idx = slot_index(p);
        if (idx == SIZE_MAX) return false;
        return (h_.bm[idx / 64].mark >> (idx % 64)) & 1;
    }
    void set_mark(const void* p);
    void clear_marks();

    // Remembered set membership (write barrier dedup): returns the previous
    // state, setting the bit as a side effect.
    bool test_and_set_remembered(const void* p);
    // Inline: the major collection's trace calls this for every cell it visits.
    void clear_remembered(const void* p) {
        const size_t idx = slot_index(p);
        if (idx == SIZE_MAX) return;
        h_.remembered_bitmap[idx / 64] &= ~(1ULL << (idx % 64));
    }

    // fn(cell_base, marked) for every allocated cell in this block.
    template <typename Fn>
    void for_each_cell(Fn&& fn) {
        for (uint32_t i = 0; i < h_.bump_cursor; i++) {
            if (!((h_.bm[i / 64].alloc >> (i % 64)) & 1)) continue;
            void* cell = payload_start() + static_cast<size_t>(i) * h_.cell_size;
            fn(cell, ((h_.bm[i / 64].mark >> (i % 64)) & 1) != 0);
        }
    }

    // fn(cell_base) for every allocated-but-unmarked (dead) cell -- sweep's
    // only interest. Scans a word (64 slots) at a time and skips it outright
    // when alloc & ~mark is zero (nothing dead there, the common case for a
    // block that survived a cycle mostly or entirely intact), so a fully-live
    // block costs one word-check each rather than one bit-check per slot.
    bool in_dirty_list() const { return h_.in_dirty_list; }
    void set_in_dirty_list(bool v) { h_.in_dirty_list = v; }

    template <typename Fn>
    void for_each_dead_cell(Fn&& fn) {
        uint32_t word_count = (h_.bump_cursor + 63) / 64;
        for (uint32_t w = 0; w < word_count; w++) {
            uint64_t dead_bits = h_.bm[w].alloc & ~h_.bm[w].mark;
            while (dead_bits) {
                uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(dead_bits));
                dead_bits &= dead_bits - 1;
                uint32_t i = w * 64 + bit;
                if (i >= h_.bump_cursor) break;
                fn(payload_start() + static_cast<size_t>(i) * h_.cell_size);
            }
        }
    }

private:
    struct Header {
        // What a trace edge reads first, and nothing else it needs is further away: the slot index (a multiply by the magic and
        // a bound check against the capacity), then the allocated and marked bits of that slot's word, side by side. Marking a
        // cell of 64 bytes or more touches this one cache line; with the two bitmaps apart it took three.
        // ceil(2^32 / cell_size), so the slot index of a cell is a multiply and a shift instead of a divide. The size classes
        // are not powers of two, so the divide was a real one, and it sits on the path every traced edge and every mark bit
        // takes.
        uint32_t   cell_size_magic;
        uint32_t   capacity;
        struct Bits { uint64_t alloc; uint64_t mark; };
        Bits       bm[kBitmapWords];
        Heap*      heap;
        HeapBlock* next;
        // First bitmap word that may hold a free slot below bump_cursor.
        uint32_t   scan_word;
        uint32_t   pad_;
        uint32_t   cell_size;
        uint32_t   bump_cursor;
        uint32_t   free_count;
        CellKind   cell_kind;
        HeapSegment segment;
        // Whether this block is on its heap's dirty list -- see Heap's
        // dirty_blocks_. Sits in padding the two enums already leave, so the
        // header does not grow.
        bool       in_dirty_list;
        // Set once a cell has been retired (poison mode): a freed slot of this block is never handed out
        // again, which is the point of retiring one.
        bool       no_reuse;
        uint64_t   remembered_bitmap[kBitmapWords];
    };

public:
    static constexpr size_t kHeaderSize  = (sizeof(Header) + kCellAlign - 1) & ~(kCellAlign - 1);
    static constexpr size_t kPayloadSize = kBlockSize - kHeaderSize;

private:
    char*  payload_start()            { return reinterpret_cast<char*>(this) + kHeaderSize; }
    const char* payload_start() const { return reinterpret_cast<const char*>(this) + kHeaderSize; }
    // Slot index of p, or SIZE_MAX when p lies outside the payload.

    Header h_;
};

static_assert(HeapBlock::kPayloadSize / 16 <= HeapBlock::kMaxSlots,
              "alloc/mark bitmaps must cover every 16-byte slot");

}

#endif
