/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ENGINE_ISOLATE_H
#define QUANTA_ENGINE_ISOLATE_H

#include <vector>

namespace Quanta {

class Engine;
class Heap;

// One GC heap and the realms (Engines) that live in it. A browser makes one
// Isolate per thread and one Engine in it per document; $262.createRealm and
// ShadowRealm add realms to the Isolate of the one that made them. The realms
// share every cell, so a reference from one into another needs nothing beyond
// the ordinary trace.
//
// A realm destroyed while others remain (Engine::~Engine) hands what lingers of it --
// its global Context, its Realm, the Contexts and Environments its closures
// escaped into -- to a surviving realm's pools, where the collector frees each once
// nothing alive can reach it.
//
// Destroying an Isolate retires its heap -- one full collection that does not let
// stale stack words keep the heap's cells alive (Collector::retire_heap) -- which
// is only right once no realm that anything still uses is left in it.
class Isolate {
public:
    // Makes the heap and, like an Engine always did, the thread's active one.
    Isolate();
    ~Isolate();

    Isolate(const Isolate&) = delete;
    Isolate& operator=(const Isolate&) = delete;

    Heap* heap() const { return heap_; }
    const std::vector<Engine*>& engines() const { return engines_; }
    // True while the Isolate is destroying the realms it still holds.
    bool closing() const { return closing_; }

private:
    friend class Engine;
    void add_engine(Engine* engine);
    void remove_engine(Engine* engine);

    Heap* heap_;
    std::vector<Engine*> engines_;
    bool closing_ = false;
};

}

#endif
