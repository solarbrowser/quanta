/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/wasm/Runtime.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Quanta::Wasm {

namespace {

constexpr uint64_t kMaxPages32 = 65536;
constexpr uint64_t kMaxPages64 = uint64_t(1) << 18;  // (16 GiB: what an implementation sets for the memories that are not 32-bit)
constexpr uint64_t kMaxTableSize = 10000000;
// What a memory reserves of address space to begin with; one that grows past it is moved to a larger place (the data of a memory is read where it is each time).
constexpr uint64_t kInitialReservePages = 256;

}  // namespace

MemoryObject::~MemoryObject() {
    if (!data) return;
#if defined(_WIN32)
    VirtualFree(data, 0, MEM_RELEASE);
#else
    munmap(data, static_cast<size_t>(reserved << 16));
#endif
}

bool MemoryObject::Init(const MemType& t) {
    type = t;
    const uint64_t implementationMax = t.limits.is64 ? kMaxPages64 : kMaxPages32;
    maxPages = t.limits.hasMax ? std::min(t.limits.max, implementationMax) : implementationMax;
    if (t.limits.min > maxPages) return false;
    pages = 0;
    reserved = std::min(maxPages, std::max<uint64_t>(t.limits.min, kInitialReservePages));
    if (reserved == 0) return true;
    // (As much address space as can be had, down from what the memory may grow to, and no less than what it begins with.)
#if defined(_WIN32)
    while (reserved >= t.limits.min && !data) {
        data = static_cast<uint8_t*>(VirtualAlloc(nullptr, static_cast<size_t>(reserved << 16), MEM_RESERVE, PAGE_NOACCESS));
        if (!data) reserved >>= 1;
    }
    if (!data) return false;
    if (t.limits.min && !VirtualAlloc(data, static_cast<size_t>(t.limits.min << 16), MEM_COMMIT, PAGE_READWRITE)) return false;
#else
    void* p = MAP_FAILED;
    while (reserved >= std::max<uint64_t>(t.limits.min, 1)) {
        p = mmap(nullptr, static_cast<size_t>(reserved << 16), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED) break;
        reserved >>= 1;
    }
    if (p == MAP_FAILED) return false;
    data = static_cast<uint8_t*>(p);
    if (t.limits.min && mprotect(data, static_cast<size_t>(t.limits.min << 16), PROT_READ | PROT_WRITE) != 0) return false;
#endif
    if (reserved < t.limits.min) return false;
    pages = t.limits.min;
    return true;
}

int64_t MemoryObject::Grow(uint64_t delta) {
    const uint64_t before = pages;
    if (delta > maxPages || before + delta > maxPages) return -1;
    if (delta == 0) return static_cast<int64_t>(before);
    const uint64_t wanted = before + delta;
    if (wanted > reserved) {
        // Not enough room where it is: a larger range is taken (twice as much, or what it needs), the pages in use go there, and the old range goes.
        uint64_t size = std::min(maxPages, std::max<uint64_t>(wanted, reserved * 2));
        uint8_t* fresh = nullptr;
#if defined(_WIN32)
        while (size >= wanted && !fresh) {
            fresh = static_cast<uint8_t*>(VirtualAlloc(nullptr, static_cast<size_t>(size << 16), MEM_RESERVE, PAGE_NOACCESS));
            if (!fresh) size = size > wanted ? std::max<uint64_t>(wanted, size >> 1) : 0;
        }
        if (!fresh || !VirtualAlloc(fresh, static_cast<size_t>(wanted << 16), MEM_COMMIT, PAGE_READWRITE)) return -1;
        if (before) std::memcpy(fresh, data, static_cast<size_t>(before << 16));
        VirtualFree(data, 0, MEM_RELEASE);
#else
        void* p = MAP_FAILED;
        while (size >= wanted) {
            p = mmap(nullptr, static_cast<size_t>(size << 16), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (p != MAP_FAILED) break;
            size = size > wanted ? std::max<uint64_t>(wanted, size >> 1) : 0;
        }
        if (p == MAP_FAILED) return -1;
        fresh = static_cast<uint8_t*>(p);
        if (mprotect(fresh, static_cast<size_t>(wanted << 16), PROT_READ | PROT_WRITE) != 0) { munmap(fresh, static_cast<size_t>(size << 16)); return -1; }
        if (before) std::memcpy(fresh, data, static_cast<size_t>(before << 16));
        if (data) munmap(data, static_cast<size_t>(reserved << 16));
#endif
        data = fresh;
        reserved = size;
        pages = wanted;
        return static_cast<int64_t>(before);
    }
#if defined(_WIN32)
    if (!VirtualAlloc(data + (before << 16), static_cast<size_t>(delta << 16), MEM_COMMIT, PAGE_READWRITE)) return -1;
#else
    if (mprotect(data + (before << 16), static_cast<size_t>(delta << 16), PROT_READ | PROT_WRITE) != 0) return -1;
#endif
    pages = wanted;
    return static_cast<int64_t>(before);
}

bool TableObject::Init(const TableType& t, const Value& init) {
    type = t;
    maxSize = t.limits.hasMax ? std::min(t.limits.max, kMaxTableSize) : kMaxTableSize;
    if (t.limits.min > maxSize) return false;
    for (uint64_t i = 0; i < t.limits.min; ++i) elems.push_back(init);
    return true;
}

int64_t TableObject::Grow(uint64_t delta, const Value& init) {
    const uint64_t before = elems.size();
    if (delta > maxSize || before + delta > maxSize) return -1;
    for (uint64_t i = 0; i < delta; ++i) elems.push_back(init);
    return static_cast<int64_t>(before);
}

}  // namespace Quanta::Wasm
