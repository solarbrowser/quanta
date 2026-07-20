/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/SmallMapPool.h"
#include <utility>
#include <vector>

namespace Quanta {

namespace {
struct SizeClassPool {
    static constexpr size_t kPerClassCap = 16384;
    std::vector<std::pair<size_t, std::vector<void*>>> classes;

    void* take(size_t bytes) {
        for (auto& c : classes) {
            if (c.first == bytes) {
                if (c.second.empty()) return nullptr;
                void* p = c.second.back();
                c.second.pop_back();
                return p;
            }
        }
        // Reserve now: so the push_back in give() can NEVER reallocate/throw
        // -- deallocate() is noexcept, and a bad_alloc during that realloc
        // would call std::terminate() (not far-fetched during a GC sweep,
        // when many maps get torn down in a batch).
        classes.push_back({bytes, {}});
        classes.back().second.reserve(kPerClassCap);
        return nullptr;
    }
    void give(size_t bytes, void* p) {
        for (auto& c : classes) {
            if (c.first == bytes) {
                if (c.second.size() < kPerClassCap) c.second.push_back(p);
                else ::operator delete(p);
                return;
            }
        }
        ::operator delete(p);  // take() always registers the class first, shouldn't reach here
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
