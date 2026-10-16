/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "JitLayout.h"

#ifdef QUANTA_JIT_PLATFORM_OK

#include "JitPlatform.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#  if defined(__APPLE__) && defined(__aarch64__)
#    include <pthread.h>
#    include <libkern/OSCacheControl.h>
#  endif
#endif

namespace Quanta::VM::jit::platform {

size_t page_size() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return static_cast<size_t>(info.dwPageSize);
#else
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

#if defined(__APPLE__) && defined(__aarch64__)
namespace {
// Whether the mapping this thread last got from map_writable() is a MAP_JIT one.
bool& last_map_was_jit() {
    static thread_local bool v = false;
    return v;
}
}  // namespace
#endif

void* map_writable(size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
#  if defined(__APPLE__) && defined(__aarch64__)
    void* jit = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit != MAP_FAILED) {
        pthread_jit_write_protect_np(0);
        last_map_was_jit() = true;
        return jit;
    }
    last_map_was_jit() = false;
#  endif
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

bool make_executable(void* mem, size_t size) {
#if defined(_WIN32)
    DWORD previous = 0;
    if (!VirtualProtect(mem, size, PAGE_EXECUTE_READ, &previous)) return false;
    // x86 keeps its instruction cache coherent with stores, so this does nothing
    // there; it is the documented step after writing code and costs one call.
    FlushInstructionCache(GetCurrentProcess(), mem, size);
    return true;
#else
#  if defined(__APPLE__) && defined(__aarch64__)
    if (last_map_was_jit()) {
        // Already executable; only this thread's permission to write it goes back.
        last_map_was_jit() = false;
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(mem, size);
        return true;
    }
    if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0) return false;
    sys_icache_invalidate(mem, size);
    return true;
#  else
    if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0) return false;
#    if defined(__aarch64__)
    __builtin___clear_cache(static_cast<char*>(mem), static_cast<char*>(mem) + size);
#    endif
    return true;
#  endif
#endif
}

void unmap(void* mem, size_t size) {
#if defined(_WIN32)
    (void)size;   // MEM_RELEASE frees the whole reservation and requires a size of 0
    VirtualFree(mem, 0, MEM_RELEASE);
#else
    munmap(mem, size);
#endif
}

}  // namespace Quanta::VM::jit::platform

#endif  // QUANTA_JIT_PLATFORM_OK
