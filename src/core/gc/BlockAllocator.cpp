/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/gc/BlockAllocator.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

namespace Quanta {

namespace {

// Process-wide sorted chunk ranges. `contains` runs on every explicit
// delete (and per scanned word in conservative root scans), so reads are
// lock-free against
// an immutable snapshot; writers (chunk map/unmap, rare) rebuild the
// snapshot under a mutex. Retired snapshots are kept, not freed: readers
// may still hold them, and their total size is bounded by chunk churn.
struct ChunkRegistry {
    using Ranges = std::vector<std::pair<uintptr_t, uintptr_t>>;  // sorted [base, end)

    std::mutex write_mutex;
    std::atomic<const Ranges*> snapshot{new Ranges()};
    std::vector<const Ranges*> retired;

    void publish(Ranges next) {
        const Ranges* fresh = new Ranges(std::move(next));
        retired.push_back(snapshot.exchange(fresh, std::memory_order_acq_rel));
    }

    void add(void* base) {
        std::lock_guard<std::mutex> lock(write_mutex);
        uintptr_t b = reinterpret_cast<uintptr_t>(base);
        Ranges next = *snapshot.load(std::memory_order_relaxed);
        auto entry = std::make_pair(b, b + BlockAllocator::kChunkSize);
        next.insert(std::lower_bound(next.begin(), next.end(), entry), entry);
        publish(std::move(next));
    }

    void remove(void* base) {
        std::lock_guard<std::mutex> lock(write_mutex);
        uintptr_t b = reinterpret_cast<uintptr_t>(base);
        Ranges next = *snapshot.load(std::memory_order_relaxed);
        for (auto it = next.begin(); it != next.end(); ++it) {
            if (it->first == b) { next.erase(it); break; }
        }
        publish(std::move(next));
    }

    bool contains(const void* p) const {
        const Ranges& ranges = *snapshot.load(std::memory_order_acquire);
        uintptr_t a = reinterpret_cast<uintptr_t>(p);
        auto it = std::upper_bound(ranges.begin(), ranges.end(),
                                   std::make_pair(a, UINTPTR_MAX));
        if (it == ranges.begin()) return false;
        --it;
        return a >= it->first && a < it->second;
    }
};

ChunkRegistry& registry() {
    static ChunkRegistry r;
    return r;
}

}

BlockAllocator::~BlockAllocator() {
    // Chunks deliberately outlive the allocator. Static destructors (e.g.
    // builtin-owned function vectors) delete cells after the engine's heap
    // is gone; keeping chunk memory and the registry alive turns those late
    // deletes into harmless bitmap flips instead of use-after-free. Real
    // reclamation arrives with the collector's shutdown protocol.
}

void BlockAllocator::grow() {
    void* chunk = std::aligned_alloc(HeapBlock::kBlockSize, kChunkSize);
    if (!chunk) std::abort();  // OOM on chunk map: no sane recovery path
    chunks_.push_back(chunk);
    registry().add(chunk);
    char* base = static_cast<char*>(chunk);
    // LIFO order so the first allocation after grow() reuses the coldest end.
    for (size_t i = kBlocksPerChunk; i-- > 0;) {
        free_regions_.push_back(base + i * HeapBlock::kBlockSize);
    }
}

void* BlockAllocator::allocate_block_region() {
    if (free_regions_.empty()) grow();
    void* region = free_regions_.back();
    free_regions_.pop_back();
    return region;
}

void BlockAllocator::release_block_region(void* region) {
    free_regions_.push_back(region);
}

bool BlockAllocator::owns_address(const void* p) {
    return registry().contains(p);
}

}
