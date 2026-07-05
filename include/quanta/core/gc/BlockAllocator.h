/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_GC_BLOCKALLOCATOR_H
#define QUANTA_GC_BLOCKALLOCATOR_H

#include "quanta/core/gc/HeapBlock.h"
#include <cstddef>
#include <vector>

namespace Quanta {

// Carves 16KB aligned block regions out of 1MB chunks. Also maintains the
// process-wide chunk registry: `owns_address` is how `operator delete` and
// the conservative root scanner decide whether an arbitrary word points
// into ANY heap's block space (as opposed to malloc/large-object memory).
class BlockAllocator {
public:
    static constexpr size_t kChunkSize      = 1024 * 1024;
    static constexpr size_t kBlocksPerChunk = kChunkSize / HeapBlock::kBlockSize;

    BlockAllocator() = default;
    ~BlockAllocator();

    BlockAllocator(const BlockAllocator&) = delete;
    BlockAllocator& operator=(const BlockAllocator&) = delete;

    // A fresh 16KB aligned region (not yet a HeapBlock; caller runs init).
    void* allocate_block_region();
    void release_block_region(void* region);

    // Scans every chunk for one whose 64 regions are all currently free --
    // including a chunk that was carved out by grow() but never had all of
    // its regions drawn, since LIFO consumption can leave an older chunk's
    // regions untouched underneath newer ones indefinitely -- and hands its
    // physical pages back to the OS. Call once per major collection, not
    // per allocation: it is a full scan, not an incremental check.
    void decommit_idle_chunks() const;

    size_t chunk_count() const { return chunks_.size(); }
    size_t block_capacity() const { return chunks_.size() * kBlocksPerChunk; }

    // True when p lies inside any live chunk of any allocator (process-wide).
    static bool owns_address(const void* p);

private:
    void grow();

    std::vector<void*> chunks_;
    std::vector<void*> free_regions_;
};

}

#endif
