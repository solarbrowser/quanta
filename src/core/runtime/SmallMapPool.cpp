/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/SmallMapPool.h"
#include <algorithm>
#include <utility>
#include <vector>
#include <cstdint>

namespace Quanta {

namespace {
// A class's free blocks, chained through their own first word: pushing one is a load and a store, and giving a block back never allocates (deallocate() is
// noexcept, and a sweep gives back thousands at once). The list used to be a vector of pointers that grew only when a take found it full, one doubling per
// burst: a collection frees a whole generation of closures' caches in a row, the list that was there held a few dozen of them, the rest went back to the
// allocator, and the program took them again one at a time -- nine takes in ten missed. How much a class keeps is bounded in bytes instead.
struct FreeList {
    void* head = nullptr;
    size_t count = 0;
    size_t cap = 0;
    size_t taken = 0;   // what was asked of the class since trim() last looked: the demand it has to keep up with
};

struct SizeClassPool {
    static constexpr size_t kPerClassCap = 16384;
    // The most one class holds on to, whatever its block size is.
    static constexpr size_t kClassBytesCap = 512 * 1024;
    std::vector<std::pair<size_t, FreeList>> classes;

    // Which free list a byte size belongs to. This used to be found by
    // walking `classes`, which was enough while only a handful of distinct
    // sizes were ever asked for -- but an object's butterfly grows in two
    // independent dimensions, so a real program produces dozens of sizes and
    // every allocation and every free walked the list looking for its own.
    // Open addressing, keyed by the size itself; a zero key means the slot is
    // free, and no entry is ever removed, so a probe chain never breaks.
    static constexpr uint32_t kIndexSlots = 512;
    static constexpr uint32_t kNoClass = 0xFFFFFFFFu;
    size_t index_key[kIndexSlots] = {};
    uint32_t index_val[kIndexSlots] = {};

    // Null when the size has no class and none can be made: either the caller
    // only wanted to look, or the table is at the load factor past which
    // probing stops being cheap, and the plain allocator takes over from here.
    // One size is asked for over and over -- a loop building objects of the
    // same shape frees each and takes the next -- so the last answer stands in
    // front of the table. index_val never moves, so the pointer stays good.
    size_t recent_key = 0;
    uint32_t* recent_slot = nullptr;

    uint32_t* slot_for(size_t bytes, bool create) {
        if (bytes == recent_key) return recent_slot;
        uint32_t h = static_cast<uint32_t>((bytes * 0x9E3779B97F4A7C15ull) >> 52) & (kIndexSlots - 1);
        while (index_key[h] != 0) {
            if (index_key[h] == bytes) {
                recent_key = bytes;
                recent_slot = &index_val[h];
                return recent_slot;
            }
            h = (h + 1) & (kIndexSlots - 1);
        }
        if (!create || classes.size() >= kIndexSlots / 2) return nullptr;
        index_key[h] = bytes;
        index_val[h] = kNoClass;
        recent_key = bytes;
        recent_slot = &index_val[h];
        return recent_slot;
    }

    void* take(size_t bytes) {
        if (bytes < sizeof(void*)) return nullptr;   // (a block has to be able to hold the link)
        uint32_t* slot = slot_for(bytes, true);
        if (!slot) return nullptr;
        if (*slot == kNoClass) {
            *slot = static_cast<uint32_t>(classes.size());
            FreeList list;
            list.cap = std::min(kPerClassCap, std::max<size_t>(4, kClassBytesCap / bytes));
            classes.push_back({bytes, list});
            return nullptr;
        }
        FreeList& list = classes[*slot].second;
        list.taken++;
        void* p = list.head;
        if (!p) return nullptr;
        list.head = *static_cast<void**>(p);
        list.count--;
        return p;
    }

    void give(size_t bytes, void* p) {
        uint32_t* slot = bytes >= sizeof(void*) ? slot_for(bytes, false) : nullptr;
        if (!slot || *slot == kNoClass) {
            ::operator delete(p);  // take() registers the class first, so this is the unpooled tail
            return;
        }
        FreeList& list = classes[*slot].second;
        if (list.count < list.cap) {
            *static_cast<void**>(p) = list.head;
            list.head = p;
            list.count++;
        } else {
            ::operator delete(p);
        }
    }
};
// Deliberately never destructed: some pool clients (Shape::slots_/
// transitions_, backing Shape::root()'s own thread_local static instance)
// have their teardown deferred to thread-exit too, with no defined order
// relative to this pool's own thread_local destructor -- a plain
// `thread_local SizeClassPool` here would risk the pool being torn down
// first and every subsequent give()/take() call from the shape tree's
// cascading destruction running on freed memory. A leaked-on-purpose
// pointer sidesteps the ordering question entirely: nothing ever runs this
// pool's destructor, so there's no race to have. Matches this codebase's
// existing "process exit reclaims" policy for the GC heap itself (see
// Heap::~Heap()/BlockAllocator::~BlockAllocator()).
thread_local SizeClassPool* g_pool = nullptr;

SizeClassPool& pool() {
    if (!g_pool) g_pool = new SizeClassPool();
    return *g_pool;
}
}

void* SmallMapPool::take(size_t bytes) {
    if (void* p = pool().take(bytes)) return p;
    return ::operator new(bytes);
}
void SmallMapPool::give(size_t bytes, void* p) {
    pool().give(bytes, p);
}

void SmallMapPool::trim() {
    if (!g_pool) return;
    // What a class keeps is what was asked of it since the last time: a collection gives back a generation of blocks in one go and the program takes them again
    // one at a time until the next, so a pool cut to a few dozen at every major (as this once did) lost nearly all of them before they could be taken. A
    // class nothing asked for since falls to a small reserve of a few kilobytes, which is what gives the memory back.
    constexpr size_t kKeepBytesPerClass = 8192;
    for (auto& entry : g_pool->classes) {
        const size_t bytes = entry.first;
        FreeList& list = entry.second;
        const size_t floor = bytes ? std::max<size_t>(4, kKeepBytesPerClass / bytes) : 4;
        const size_t keep = std::max(floor, std::min(list.cap, list.taken));
        list.taken = 0;
        while (list.count > keep) {
            void* p = list.head;
            list.head = *static_cast<void**>(p);
            list.count--;
            ::operator delete(p);
        }
    }
}

}
