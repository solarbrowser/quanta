/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/SmallMapPool.h"
#include <utility>
#include <vector>
#include <cstdint>

namespace Quanta {

namespace {
struct SizeClassPool {
    static constexpr size_t kPerClassCap = 16384;
    std::vector<std::pair<size_t, std::vector<void*>>> classes;

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
        if (bytes == 0) return nullptr;
        uint32_t* slot = slot_for(bytes, true);
        if (!slot) return nullptr;
        if (*slot == kNoClass) {
            *slot = static_cast<uint32_t>(classes.size());
            // Reserve now: so the push_back in give() can NEVER reallocate/throw
            // -- deallocate() is noexcept, and a bad_alloc during that realloc
            // would call std::terminate() (not far-fetched during a GC sweep,
            // when many maps get torn down in a batch).
            classes.push_back({bytes, {}});
            classes.back().second.reserve(kPerClassCap);
            return nullptr;
        }
        std::vector<void*>& free_list = classes[*slot].second;
        if (free_list.empty()) return nullptr;
        void* p = free_list.back();
        free_list.pop_back();
        return p;
    }

    void give(size_t bytes, void* p) {
        uint32_t* slot = bytes ? slot_for(bytes, false) : nullptr;
        if (!slot || *slot == kNoClass) {
            ::operator delete(p);  // take() registers the class first, so this is the unpooled tail
            return;
        }
        std::vector<void*>& free_list = classes[*slot].second;
        if (free_list.size() < kPerClassCap) free_list.push_back(p);
        else ::operator delete(p);
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

}
