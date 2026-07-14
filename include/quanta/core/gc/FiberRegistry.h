/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_GC_FIBERREGISTRY_H
#define QUANTA_GC_FIBERREGISTRY_H

#include <cstddef>
#include <functional>

namespace Quanta {

struct FiberState;
class Object;
class Visitor;

// Live fibers (Generator / AsyncGenerator / AsyncExecutor). A suspended
// fiber's stack is full of live cell references the conservative scanner
// must see. Stacks come from FiberStackPool and may hold a previous
// fiber's remains below the live region -- fine, since the scanner
// validates every candidate word against the heap (stale words cost at
// most temporary false retention). The FiberState pair carries the saved
// register file, scanned as a raw word range.
class FiberRegistry {
public:
    struct Record {
        const void* owner;
        const char* stack_lo;
        const char* stack_hi;
        const FiberState* state;
        // Owner as a GC cell (Generator/AsyncGenerator), null for non-cell
        // owners. A suspended fiber's owner mutates its traced fields on
        // every resume, so minor collections treat it as a root instead of
        // requiring a barrier at each of those writes.
        Object* owner_cell;
        // Non-cell fiber owners (AsyncExecutor) report the cells their C++
        // fields reference here; cell owners are traced like any object.
        std::function<void(Visitor&)> extra_roots;
    };

    static void register_fiber(const void* owner, const char* stack, size_t size,
                               const FiberState* state, Object* owner_cell,
                               std::function<void(Visitor&)> extra_roots = {});
    static void unregister_fiber(const void* owner);
    static void for_each(const std::function<void(const Record&)>& fn);

    // Caller-side suspension points: pushed right before swapcontext into a
    // fiber, popped when it returns. Tells the scanner how deep the
    // suspended host stack's live region reaches.
    static void push_enter_sp(const void* sp);
    static void pop_enter_sp();
    static void for_each_enter_sp(const std::function<void(const void*)>& fn);
};

// RAII for the caller side of a fiber switch: the address of this object is
// (a lower bound of) the host stack's live top while the fiber runs.
class FiberEnterScope {
public:
    FiberEnterScope() { FiberRegistry::push_enter_sp(this); }
    ~FiberEnterScope() { FiberRegistry::pop_enter_sp(); }

    FiberEnterScope(const FiberEnterScope&) = delete;
    FiberEnterScope& operator=(const FiberEnterScope&) = delete;
};

}

#endif
