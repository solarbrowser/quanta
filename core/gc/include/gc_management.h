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
 * GC Management - Object registration and lifecycle management
 * EXTRACTED FROM GC.cpp - Management functionality
 */
class GCManagement {
public:
    // Object registration
    static void register_object(GarbageCollector& gc, Object* obj, size_t size);
    static void unregister_object(GarbageCollector& gc, Object* obj);
    static void register_context(GarbageCollector& gc, Context* ctx);
    static void unregister_context(GarbageCollector& gc, Context* ctx);

    // Root object management
    static void add_root_object(GarbageCollector& gc, Object* obj);
    static void remove_root_object(GarbageCollector& gc, Object* obj);
    static void clear_all_root_objects(GarbageCollector& gc);

    // Weak reference management
    static void add_weak_reference(GarbageCollector& gc, Object* obj);
    static void remove_weak_reference(GarbageCollector& gc, Object* obj);
    static void cleanup_weak_references(GarbageCollector& gc);

    // Thread management
    static void start_gc_thread(GarbageCollector& gc);
    static void stop_gc_thread(GarbageCollector& gc);
    static void gc_thread_main(GarbageCollector& gc);

    // Memory tracking
    static size_t get_total_heap_size(const GarbageCollector& gc);
    static size_t get_young_generation_size(const GarbageCollector& gc);
    static size_t get_old_generation_size(const GarbageCollector& gc);
    static size_t get_allocated_bytes(const GarbageCollector& gc);

    // Configuration
    static void enable_ultra_fast_mode(GarbageCollector& gc, bool enabled);
    static void set_collection_mode(GarbageCollector& gc, GarbageCollector::CollectionMode mode);
    static void set_heap_size_limit(GarbageCollector& gc, size_t limit);
    static void set_gc_trigger_ratio(GarbageCollector& gc, double ratio);

    // Statistics and debugging
    static void reset_statistics(GarbageCollector& gc);
    static void print_statistics(const GarbageCollector& gc);
    static void print_heap_info(const GarbageCollector& gc);
    static void verify_heap_integrity(const GarbageCollector& gc);

    // Emergency operations
    static void emergency_cleanup(GarbageCollector& gc);
    static void force_cleanup_all(GarbageCollector& gc);
    static bool is_memory_pressure_high(const GarbageCollector& gc);

private:
    // Internal helpers
    static bool is_object_registered(const GarbageCollector& gc, Object* obj);
    static bool is_context_registered(const GarbageCollector& gc, Context* ctx);
    static void update_heap_statistics(GarbageCollector& gc);
    static void validate_object_integrity(Object* obj);
};

} // namespace Quanta