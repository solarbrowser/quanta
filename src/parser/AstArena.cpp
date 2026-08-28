/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/AstArena.h"

#include <cstdint>
#include <cstdlib>
#include <new>
#include <unordered_set>

namespace Quanta {

namespace {

// Big enough that the per-chunk bookkeeping disappears against what it holds
// (a chunk fits roughly four thousand of the commonest node), small enough
// that one going empty is worth handing back. Also the alignment, so a node's
// chunk is its address with the low bits cleared.
constexpr size_t kChunkSize = 256 * 1024;
constexpr uintptr_t kChunkMask = ~(static_cast<uintptr_t>(kChunkSize) - 1);
// Node sizes are 40 to 192 bytes and cluster on a few values; 16-byte steps
// cover them in twelve classes with at most fifteen bytes lost to rounding.
constexpr size_t kGranularity = 16;
constexpr size_t kNumClasses = AstArena::kMaxNodeSize / kGranularity;  // 1..192

struct Chunk {
    Chunk* next_in_class;
    Chunk** prev_link;      // where the pointer to this chunk lives, so unlinking is O(1)
    void* free_list;        // freed nodes, threaded through their own first word
    uint32_t bump;          // bytes of payload handed out and never reclaimed
    uint32_t live;          // nodes currently allocated out of this chunk
    uint32_t node_size;
    uint32_t class_index;
};

constexpr size_t kHeaderSize = (sizeof(Chunk) + 15) & ~size_t{15};
constexpr size_t kPayload = kChunkSize - kHeaderSize;

// One partially-used chunk list per size class. A chunk leaves the list only
// when it is released, so allocation always looks at the head first.
thread_local Chunk* g_open[kNumClasses] = {};
thread_local size_t g_live_nodes = 0;

// Which addresses are chunks of ours. A pointer that did not come from here --
// a node too large for a class, or one made before this thread had a chunk at
// all -- must be recognised WITHOUT reading it: masking an arbitrary address
// and dereferencing the result is undefined, and would read whatever happens
// to sit at that alignment.
std::unordered_set<uintptr_t>& registry() {
    static thread_local std::unordered_set<uintptr_t> r;
    return r;
}

inline Chunk* chunk_of(void* p) {
    return reinterpret_cast<Chunk*>(reinterpret_cast<uintptr_t>(p) & kChunkMask);
}

inline uint8_t* payload_of(Chunk* c) {
    return reinterpret_cast<uint8_t*>(c) + kHeaderSize;
}

Chunk* new_chunk(size_t class_index, size_t node_size) {
    void* raw = std::aligned_alloc(kChunkSize, kChunkSize);
    if (!raw) return nullptr;
    auto* c = static_cast<Chunk*>(raw);
    c->next_in_class = g_open[class_index];
    c->prev_link = &g_open[class_index];
    if (c->next_in_class) c->next_in_class->prev_link = &c->next_in_class;
    g_open[class_index] = c;
    registry().insert(reinterpret_cast<uintptr_t>(c));
    c->free_list = nullptr;
    c->bump = 0;
    c->live = 0;
    c->node_size = static_cast<uint32_t>(node_size);
    c->class_index = static_cast<uint32_t>(class_index);
    return c;
}

void release(Chunk* c) {
    *c->prev_link = c->next_in_class;
    if (c->next_in_class) c->next_in_class->prev_link = c->prev_link;
    registry().erase(reinterpret_cast<uintptr_t>(c));
    std::free(c);
}

}  // namespace

void* AstArena::take(size_t bytes) {
    if (bytes == 0 || bytes > kMaxNodeSize) return ::operator new(bytes);
    const size_t node_size = (bytes + kGranularity - 1) & ~(kGranularity - 1);
    const size_t ci = node_size / kGranularity - 1;

    for (Chunk* c = g_open[ci]; c; c = c->next_in_class) {
        if (c->free_list) {
            void* p = c->free_list;
            c->free_list = *reinterpret_cast<void**>(p);
            ++c->live;
            ++g_live_nodes;
            return p;
        }
        if (c->bump + node_size <= kPayload) {
            void* p = payload_of(c) + c->bump;
            c->bump += static_cast<uint32_t>(node_size);
            ++c->live;
            ++g_live_nodes;
            return p;
        }
        // Full and with nothing freed yet: it will come back when something is
        // freed out of it, so leave it linked and try the next one.
    }
    Chunk* c = new_chunk(ci, node_size);
    // Out of memory is out of memory: falling back to the general allocator
    // here would put a small node outside the registry, and every later free
    // would have to guess which side it came from.
    if (!c) throw std::bad_alloc();
    void* p = payload_of(c);
    c->bump = static_cast<uint32_t>(node_size);
    c->live = 1;
    ++g_live_nodes;
    return p;
}

void AstArena::give(void* p) noexcept {
    if (!p) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(p) & kChunkMask;
    if (!registry().count(base)) {
        // Not ours: a node too large for any class went to the general
        // allocator, and goes back to it.
        ::operator delete(p);
        return;
    }
    Chunk* c = reinterpret_cast<Chunk*>(base);
    *reinterpret_cast<void**>(p) = c->free_list;
    c->free_list = p;
    --c->live;
    --g_live_nodes;
    if (c->live == 0) release(c);
}

AstArena::Stats AstArena::stats() {
    const size_t chunks = registry().size();
    return Stats{chunks, g_live_nodes, chunks * kChunkSize};
}

}  // namespace Quanta
