/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/GC.h"
#include "../../include/Object.h"
#include "../../include/Context.h"
#include <vector>
#include <set>
#include <mutex>

namespace Quanta {

/**
 * GC Collection - Collection algorithms and strategies
 * EXTRACTED FROM GC.cpp - Collection functionality
 */
class GCCollection {
public:
    // Main collection methods
    static void collect_garbage(GarbageCollector& gc);
    static void collect_young_generation(GarbageCollector& gc);
    static void collect_old_generation(GarbageCollector& gc);
    static void force_full_collection(GarbageCollector& gc);

    // Collection decision logic
    static bool should_trigger_gc(const GarbageCollector& gc);
    static bool should_trigger_young_gc(const GarbageCollector& gc);
    static bool should_trigger_old_gc(const GarbageCollector& gc);

    // Mark and sweep algorithms
    static void mark_objects(GarbageCollector& gc);
    static void mark_from_roots(GarbageCollector& gc);
    static void mark_reachable_objects(GarbageCollector& gc);
    static void sweep_unmarked_objects(GarbageCollector& gc);

    // Advanced collection strategies
    static void incremental_collection(GarbageCollector& gc);
    static void concurrent_collection(GarbageCollector& gc);
    static void parallel_collection(GarbageCollector& gc);

    // Generation management
    static void promote_to_old_generation(GarbageCollector& gc, Object* obj);
    static void demote_to_young_generation(GarbageCollector& gc, Object* obj);
    static void update_generation_thresholds(GarbageCollector& gc);

    // Collection optimization
    static void optimize_collection_frequency(GarbageCollector& gc);
    static void emergency_collection(GarbageCollector& gc);
    static void ultra_fast_collection(GarbageCollector& gc);

private:
    // Internal marking helpers
    static void mark_object_recursive(Object* obj, std::set<Object*>& marked);
    static void mark_context_objects(Context* ctx, std::set<Object*>& marked);
    static void mark_weak_references(GarbageCollector& gc, std::set<Object*>& marked);

    // Internal sweep helpers
    static size_t sweep_generation(GarbageCollector& gc, bool young_generation);
    static void finalize_objects(const std::vector<Object*>& objects);
    static void compact_heap(GarbageCollector& gc);

    // Collection metrics
    static void update_collection_statistics(GarbageCollector& gc, size_t freed_bytes);
    static void record_collection_time(GarbageCollector& gc, double elapsed_ms);
};

} // namespace Quanta