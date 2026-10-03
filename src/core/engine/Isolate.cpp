/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/Isolate.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/engine/Context.h"
#include "quanta/parser/FunctionExecutable.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/core/gc/Heap.h"
#include <algorithm>

namespace Quanta {

Isolate::Isolate() : heap_(new Heap()) {
    Heap::set_active(heap_);
}

Isolate::~Isolate() {
    // Realms made inside the Isolate (createRealm, ShadowRealm) have no other owner.
    // They go with it, each tearing down fully: there is no one left to inherit.
    closing_ = true;
    for (Engine* engine : std::vector<Engine*>(engines_)) delete engine;
    // The collections below allocate nothing but do consult the active heap.
    HeapScope heap_scope(heap_);
    // A collection that is already half done would trace into what is about to go.
    if (Collector::major_in_progress()) Collector::collect();
    // What the scripts that ran here learned names the heap's cells and would keep
    // them (and their realms) alive past it.
    FunctionExecutable::drop_feedback_of(nullptr);
    Collector::retire_heap(heap_);
    // Only now: until the heap was collected its cells named these as closure contexts.
    retired_contexts_.clear();
}

void Isolate::add_engine(Engine* engine) {
    engines_.push_back(engine);
    // What the last realm left behind goes to the next one, whose pools the
    // collector already prunes by reachability.
    for (std::unique_ptr<Context>& context : retired_contexts_) engine->add_survivor_context(context.release());
    retired_contexts_.clear();
}

void Isolate::remove_engine(Engine* engine) {
    engines_.erase(std::remove(engines_.begin(), engines_.end(), engine), engines_.end());
}

}
