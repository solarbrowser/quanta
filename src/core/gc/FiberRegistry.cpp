/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/gc/FiberRegistry.h"
#include <vector>

namespace Quanta {

namespace {

// Single-threaded engine world: plain statics, no locking.
std::vector<FiberRegistry::Record>& records() {
    static std::vector<FiberRegistry::Record> r;
    return r;
}

std::vector<const void*>& enter_sps() {
    static std::vector<const void*> s;
    return s;
}

}

void FiberRegistry::register_fiber(const void* owner, const char* stack, size_t size,
                                   const FiberState* state,
                                   std::function<void(Visitor&)> extra_roots) {
    records().push_back({owner, stack, stack + size, state, std::move(extra_roots)});
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
