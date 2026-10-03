/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Isolate.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/gc/Heap.h"
#include <algorithm>

namespace Quanta {

Isolate::Isolate() : heap_(new Heap()) {
    Heap::set_active(heap_);
}

Isolate::~Isolate() {
    // A collection that is already half done would trace into what is about to go.
    if (Collector::major_in_progress()) Collector::collect();
    Collector::retire_heap(heap_);
}

void Isolate::add_engine(Engine* engine) {
    engines_.push_back(engine);
}

void Isolate::remove_engine(Engine* engine) {
    engines_.erase(std::remove(engines_.begin(), engines_.end(), engine), engines_.end());
}

}
