/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include "../../include/Object.h"
#include <unordered_set>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>

namespace Quanta {

class Context;
class Engine;

/**
 * High-performance Garbage Collector for Quanta JavaScript Engine
 * Implements mark-and-sweep with generational collection and advanced optimizations
 */
class GarbageCollector {
public:
    // GC modes
    enum class CollectionMode {
        Manual,         // Manual collection only
        Automatic,      // Automatic collection based on thresholds
        Incremental     // Incremental collection
    };

    // Object generation for generational GC
    enum class Generation {
        Young,          // Newly allocated objects
        Old,            // Long-lived objects
        Permanent       // Permanent objects (built-ins)
    };

    // GC statistics
    struct Statistics {
        uint64_t total_allocations;
        uint64_t total_deallocations;
        uint64_t total_collections;
        uint64_t bytes_allocated;
        uint64_t bytes_freed;
        uint64_t peak_memory_usage;
        std::chrono::duration<double> total_gc_time;
        std::chrono::duration<double> average_gc_time;

        Statistics() : total_allocations(0), total_deallocations(0),
                      total_collections(0), bytes_allocated(0), bytes_freed(0),
                      peak_memory_usage(0), total_gc_time(0), average_gc_time(0) {}
    };

    // Managed object wrapper
    struct ManagedObject {
        Object* object;
        Generation generation;
        bool is_marked;
        size_t size;
        std::chrono::high_resolution_clock::time_point allocation_time;
        uint32_t access_count;

        ManagedObject(Object* obj, Generation gen, size_t obj_size)
            : object(obj), generation(gen), is_marked(false), size(obj_size),
              allocation_time(std::chrono::high_resolution_clock::now()),
              access_count(0) {}
    };

private:
    // Configuration
    CollectionMode collection_mode_;
    size_t young_generation_threshold_;
    size_t old_generation_threshold_;
    size_t heap_size_limit_;
    double gc_trigger_ratio_;

    // State
    std::atomic<bool> gc_running_;
    std::atomic<bool> stop_gc_thread_;
    std::atomic<uint64_t> collection_cycles_;

    // Optimizations
    std::atomic<bool> ultra_fast_gc_;
    std::atomic<bool> parallel_collection_;
    std::atomic<bool> zero_copy_optimization_;
    std::atomic<bool> heavy_operation_mode_;
    size_t emergency_cleanup_threshold_;

    // Object tracking
    std::vector<ManagedObject*> managed_objects_;
    std::unordered_set<Object*> root_objects_;
    std::unordered_set<Object*> permanent_objects_;

    // Statistics and monitoring
    Statistics stats_;
    std::chrono::high_resolution_clock::time_point last_collection_time_;

    // Threading
    std::unique_ptr<std::thread> gc_thread_;
    mutable std::mutex gc_mutex_;
    mutable std::mutex stats_mutex_;

public:
    GarbageCollector();
    ~GarbageCollector();

    // Object lifecycle management
    void register_object(Object* obj, size_t size = 0);
    void unregister_object(Object* obj);
    void add_root(Object* obj);
    void remove_root(Object* obj);
    void add_permanent(Object* obj);

    // Collection operations
    void collect();
    void force_collect();
    void incremental_collect();
    void collect_young_generation();
    void collect_old_generation();

    // Configuration
    void set_collection_mode(CollectionMode mode);
    void set_thresholds(size_t young_threshold, size_t old_threshold);
    void enable_ultra_fast_mode(bool enable);
    void enable_parallel_collection(bool enable);
    void enable_heavy_operation_mode(bool enable);

    // Statistics and monitoring
    Statistics get_statistics() const;
    size_t get_managed_object_count() const;
    size_t get_total_memory_usage() const;
    double get_memory_pressure() const;
    bool should_collect() const;

    // Advanced features
    void start_background_collection();
    void stop_background_collection();
    void emergency_cleanup();
    void defragment_heap();

    // Thread safety
    void pause_collection();
    void resume_collection();

private:
    // Collection phases
    void mark_phase();
    void sweep_phase();
    void mark_object(Object* obj);
    void mark_from_roots();
    void sweep_young_generation();
    void sweep_old_generation();

    // Object management
    ManagedObject* find_managed_object(Object* obj);
    ManagedObject* find_managed_object_ultra_fast(Object* obj);
    void promote_to_old_generation(ManagedObject* managed);
    bool is_reachable(Object* obj);

    // Optimization algorithms
    void update_statistics();
    void optimize_collection_strategy();
    bool should_promote_object(ManagedObject* managed);

    // Threading
    void background_collection_loop();
    void stop_gc_thread();
};

} // namespace Quanta