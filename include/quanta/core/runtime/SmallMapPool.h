/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_RUNTIME_SMALLMAPPOOL_H
#define QUANTA_RUNTIME_SMALLMAPPOOL_H

#include <cstddef>

namespace Quanta {

// Backs SmallMapAllocator: pools the malloc/free calls that small,
// short-lived std::unordered_map/unordered_set instances make for their own
// node and bucket-array storage (Environment::slots_, Shape::slots_/
// transitions_, HybridDescriptorMap::overflow_ -- each rebuilt fresh, many
// times a second, on objects/scopes that usually hold only a handful of
// entries). _Hashtable only ever requests a handful of FIXED sizes (one
// node size per map instantiation, plus whichever prime bucket-count tiers
// get hit), so a linear-scanned, size-keyed free list is enough -- the
// number of distinct sizes seen in practice stays in the single digits.
// Real state lives in exactly one .cpp (SmallMapPool.cpp), not here, so
// every TU that includes this header doesn't get its own separate copy --
// these maps are routinely constructed in one TU and destroyed in another
// (e.g. Collector.cpp), so there must be a single pool per thread or freed
// blocks never come back.
class SmallMapPool {
public:
    static void* take(size_t bytes);
    static void give(size_t bytes, void* p);
};

// Generic STL Allocator that routes through SmallMapPool. Stateless (always
// compares equal), so it's safe as the 5th template parameter of any
// std::unordered_map/unordered_set whose values never have their address
// taken and cached across calls -- the allocator only changes how nodes/
// buckets are malloc'd/freed, never the container's own pointer-stability
// guarantee (rehash only reallocates the bucket-pointer array, per the
// standard; existing elements are never relocated). Do NOT apply this to a
// map whose returned pointer/iterator gets cached across calls unless that
// pattern is independently verified safe -- see Environment::
// stable_binding_slot's own comment for the one map in this codebase that
// needed that extra scrutiny before its allocator could be swapped.
template <typename T>
struct SmallMapAllocator {
    using value_type = T;
    SmallMapAllocator() noexcept = default;
    template <typename U> SmallMapAllocator(const SmallMapAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) { return static_cast<T*>(SmallMapPool::take(n * sizeof(T))); }
    void deallocate(T* p, std::size_t n) noexcept { SmallMapPool::give(n * sizeof(T), p); }
    template <typename U> bool operator==(const SmallMapAllocator<U>&) const noexcept { return true; }
    template <typename U> bool operator!=(const SmallMapAllocator<U>&) const noexcept { return false; }
};

}

#endif
