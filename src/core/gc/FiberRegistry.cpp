/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/gc/FiberRegistry.h"
#include "quanta/core/engine/Realm.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/runtime/FiberState.h"
#include <vector>

namespace Quanta {

namespace {

// Thread-local: a fiber only ever runs on the thread that created it, so
// each thread's registry is independent and needs no locking.
std::vector<FiberRegistry::Record>& records() {
    static thread_local std::vector<FiberRegistry::Record> r;
    return r;
}

std::vector<const void*>& enter_sps() {
    static thread_local std::vector<const void*> s;
    return s;
}

}

void FiberRegistry::register_fiber(const void* owner, const char* stack, size_t size,
                                   const FiberState* state, Object* owner_cell,
                                   std::function<void(Visitor&)> extra_roots, std::function<void()> on_abandon) {
    records().push_back({owner, stack, stack + size, state, owner_cell, std::move(extra_roots), g_current_realm ? g_current_realm->id() : 0, std::move(on_abandon)});
}

void FiberRegistry::unregister_fiber(const void* owner) {
    auto& r = records();
    for (size_t i = 0; i < r.size(); i++) {
        if (r[i].owner == owner) {
            r[i] = r.back();
            r.pop_back();
            return;
        }
    }
}

size_t FiberRegistry::abandon_realm(uint64_t realm_id) {
    size_t n = 0;
    // (A copy of the records: giving back what an owner holds may reach the registry.)
    std::vector<Record> mine;
    for (const Record& rec : records()) {
        if (rec.realm_id != realm_id || !rec.state || rec.state->abandoned()) continue;
        // Suspended: it yielded, so it knows where. A fiber that is running (or has resumed another, or has not started) has no suspend point.
        if (!rec.state->suspend_sp || !rec.state->co) continue;
        mine.push_back(rec);
    }
    for (Record& rec : mine) {
        FiberState* state = const_cast<FiberState*>(rec.state);
        state->abandon();
        // What the stack registered as roots (environments, argument vectors): the fiber never runs again, so nothing there is held for it.
        Collector::purge_stack_roots(rec.stack_lo, rec.stack_hi);
        if (rec.on_abandon) rec.on_abandon();
        n++;
    }
    return n;
}

void FiberRegistry::for_each(const std::function<void(const Record&)>& fn) {
    for (const auto& rec : records()) fn(rec);
}

void FiberRegistry::push_enter_sp(const void* sp) {
    enter_sps().push_back(sp);
}

void FiberRegistry::pop_enter_sp() {
    enter_sps().pop_back();
}

void FiberRegistry::for_each_enter_sp(const std::function<void(const void*)>& fn) {
    for (const void* sp : enter_sps()) fn(sp);
}

}
