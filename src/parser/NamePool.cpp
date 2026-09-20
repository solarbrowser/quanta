/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/parser/NamePool.h"

#include <cstring>
#include <deque>
#include <string_view>
#include <vector>

namespace Quanta {

namespace {

// Names are short and hashed once per identifier occurrence -- hundreds of
// thousands of times a parse -- so this reads eight bytes at a time and mixes
// once per word, where std::hash walks the bytes through a general-purpose
// routine. Only the low bits are used to pick a slot and the high half is kept
// as a tag, so the avalanche at the end matters and nothing else does.
inline uint64_t load64(const char* p) {
    uint64_t word;
    std::memcpy(&word, p, 8);
    return word;
}
inline uint32_t load32(const char* p) {
    uint32_t word;
    std::memcpy(&word, p, 4);
    return word;
}

// The last one to seven bytes as one word, by overlapping fixed-size loads.
// A variable-length copy here was an out-of-line memcpy call per name, which
// was the single hottest instruction in the pool; the length itself is mixed
// into the hash, so overlapping bytes cannot make two different names agree.
inline uint64_t tail_word(const char* p, size_t n) {
    if (n >= 4) return load32(p) | (static_cast<uint64_t>(load32(p + n - 4)) << 32);
    return static_cast<uint64_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint64_t>(static_cast<unsigned char>(p[n >> 1])) << 8) |
           (static_cast<uint64_t>(static_cast<unsigned char>(p[n - 1])) << 16);
}

inline uint64_t hash_name(std::string_view text) {
    constexpr uint64_t kMul = 0x9FB21C651E98DF25ull;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ text.size();
    const char* p = text.data();
    size_t n = text.size();
    while (n >= 8) {
        h = (h ^ load64(p)) * kMul;
        h ^= h >> 32;
        p += 8;
        n -= 8;
    }
    if (n > 0) {
        h = (h ^ tail_word(p, n)) * kMul;
        h ^= h >> 32;
    }
    h *= kMul;
    return h ^ (h >> 29);
}

// Equality of two runs of the same length, without a call for the short names
// that almost every identifier is.
inline bool same_bytes(const char* a, const char* b, size_t n) {
    if (n > 16) return std::memcmp(a, b, n) == 0;
    if (n >= 8) return load64(a) == load64(b) && load64(a + n - 8) == load64(b + n - 8);
    if (n >= 4) return load32(a) == load32(b) && load32(a + n - 4) == load32(b + n - 4);
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// A deque, not a vector: an entry's address has to stay put for the reference
// text() hands back.
//
// The index is open addressing over a flat array instead of a node-based map:
// a lookup is one probe into contiguous memory, and the stored tag settles
// nearly every non-match without touching the name's text.
struct Pool {
    struct Slot {
        uint32_t tag;
        uint32_t id_plus_one;  // 0 marks an empty slot
    };

    std::deque<std::string> texts;
    // By id, so a probe reaches a name's bytes through one flat array instead
    // of the deque's chunk arithmetic and its map of chunks.
    std::vector<const std::string*> by_id;
    std::vector<Slot> slots;
    size_t mask = 0;

    Pool() {
        // Id zero is the empty name, which the parser uses for anonymous
        // things, so it costs no lookup.
        texts.emplace_back();
        by_id.push_back(&texts.back());
        slots.assign(1024, Slot{0, 0});
        mask = slots.size() - 1;
    }

    void grow() {
        std::vector<Slot> old = std::move(slots);
        slots.assign(old.size() * 2, Slot{0, 0});
        mask = slots.size() - 1;
        for (const Slot& slot : old) {
            if (slot.id_plus_one == 0) continue;
            const uint64_t h = hash_name(*by_id[slot.id_plus_one - 1]);
            size_t at = static_cast<size_t>(h) & mask;
            while (slots[at].id_plus_one != 0) at = (at + 1) & mask;
            slots[at] = slot;
        }
    }
};

Pool& pool() {
    static thread_local Pool p;
    return p;
}

}  // namespace

uint32_t NamePool::intern(std::string_view text) {
    if (text.empty()) return 0;
    Pool& p = pool();
    const uint64_t h = hash_name(text);
    const uint32_t tag = static_cast<uint32_t>(h >> 32);
    size_t at = static_cast<size_t>(h) & p.mask;
    while (p.slots[at].id_plus_one != 0) {
        const Pool::Slot& slot = p.slots[at];
        if (slot.tag == tag) {
            const std::string& candidate = *p.by_id[slot.id_plus_one - 1];
            if (candidate.size() == text.size() &&
                same_bytes(candidate.data(), text.data(), text.size())) {
                return slot.id_plus_one - 1;
            }
        }
        at = (at + 1) & p.mask;
    }
    const uint32_t id = static_cast<uint32_t>(p.texts.size());
    p.texts.emplace_back(text);
    p.by_id.push_back(&p.texts.back());
    p.slots[at] = Pool::Slot{tag, id + 1};
    // Kept under half full so a probe run stays short.
    if (p.texts.size() * 2 > p.slots.size()) p.grow();
    return id;
}

const std::string& NamePool::text(uint32_t id) {
    Pool& p = pool();
    if (id >= p.by_id.size()) return *p.by_id.front();
    return *p.by_id[id];
}

}  // namespace Quanta
