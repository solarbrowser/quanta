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
};

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
