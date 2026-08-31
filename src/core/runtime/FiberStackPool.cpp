/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Its own translation unit because it is the only part of the runtime that
// asks the platform for memory directly, and <windows.h> cannot be let
// anywhere near the parser: it defines IN, CONST, DELETE and VOID as macros,
// and the AST names tokens after several of them. Nothing here includes an
// engine header beyond the pool's own.
#include "quanta/core/runtime/FiberStackPool.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <cstdlib>

namespace Quanta {

namespace {

// Straight from the OS, not from the C allocator. A fiber's stack is sized
// for the deepest recursion it must survive -- two megabytes -- while the
// work a fiber actually does touches a few kilobytes of it, so all but the
// first page or two is address space that is never read or written. An
// allocator that hands back memory it has already committed makes the engine
// pay for the whole reservation; mapped directly, the pages that are never
// touched never become resident, and what a fiber costs follows what it uses.
// Which allocator the process happens to run on stops being the thing that
// decides that.
char* map_stack(size_t size) {
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return static_cast<char*>(p);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<char*>(p);
#endif
}

void unmap_stack(char* p, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

}

thread_local std::vector<FiberStackPool::Bucket> FiberStackPool::buckets_;

char* FiberStackPool::acquire(size_t size) {
    for (auto& b : buckets_) {
        if (b.size == size && !b.free.empty()) {
            char* p = b.free.back();
            b.free.pop_back();
            return p;
        }
    }
    char* p = map_stack(size);
    if (!p) std::abort();  // OOM on a stack map: no sane recovery path
    return p;
}

void FiberStackPool::release(char* p, size_t size) {
    for (auto& b : buckets_) {
        if (b.size == size) {
            if (b.free.size() >= kMaxPerBucket) { unmap_stack(p, size); return; }
            b.free.push_back(p);
            return;
        }
    }
    buckets_.push_back({size, {p}});
}

}
