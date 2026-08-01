/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_FIBERSTATE_H
#define QUANTA_FIBERSTATE_H

#include "minicoro.h"
#include "quanta/core/runtime/FiberStackPool.h"

namespace Quanta {

// A fiber's minicoro handle, kept out of the owning GC cell: mco_coro's
// control block + saved-register area would bloat Generator/AsyncGenerator
// cells into a larger size class. Plain-malloc backing store, owned via
// unique_ptr. co->stack_base/stack_size is the fiber's own stack (scanned
// directly by FiberRegistry); the control block/register area pointed to by
// `co` itself is a separate scanned range -- see FiberRegistry::Record.
struct FiberState {
    mco_coro* co = nullptr;
    // Stack pointer this fiber last suspended at; null while it is running.
    // The collector scans [suspend_sp, stack_hi] instead of the whole stack --
    // a fiber's stack is mostly untouched, and everything below where it
    // stopped is dead. Null reads as "unknown", which scans all of it, so a
    // suspend that does not go through quanta_fiber_yield is merely slower.
    const char* suspend_sp = nullptr;
};

// The only two ways to suspend or resume a fiber. Going through mco_yield or
// mco_resume directly leaves suspend_sp stale, which would have the collector
// start above live frames.
inline void quanta_fiber_yield(FiberState* fs) {
    char sp;
    fs->suspend_sp = &sp;
    mco_yield(fs->co);
    fs->suspend_sp = nullptr;
}
inline void quanta_fiber_resume(FiberState* fs) {
    fs->suspend_sp = nullptr;
    mco_resume(fs->co);
}

// mco_desc alloc_cb/dealloc_cb: routes minicoro's single combined allocation
// (control block + register area + stack) through the existing stack pool
// instead of plain malloc/free.
inline void* fiber_alloc_cb(size_t size, void*) {
    return FiberStackPool::acquire(size);
}
inline void fiber_dealloc_cb(void* ptr, size_t size, void*) {
    FiberStackPool::release(static_cast<char*>(ptr), size);
}

}

#endif
