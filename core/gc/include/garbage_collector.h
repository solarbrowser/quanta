/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "memory_pool.h"
#include "../../include/Object.h"
#include <vector>
#include <unordered_set>
#include <memory>
#include <atomic>
#include <mutex>

namespace Quanta {

class Context;

/**
 * Garbage Collector Implementation
 * High-performance mark-and-sweep collector with memory pool management
 */
class GarbageCollector {
public:
    struct ManagedObject {
        Object* object;
        size_t size;
        bool marked;
        bool is_root;
        ManagedObject* next;

        ManagedObject(Object* obj, size_t obj_size)
            : object(obj), size(obj_size), marked(false), is_root(false), next(nullptr) {}
    };

private:
    std::vector<ManagedObject*> managed_objects_;
    std::unordered_set<Object*> root_objects_;
    MemoryPool memory_pool_;

    // Statistics
    std::atomic<size_t> total_allocations_{0};
    std::atomic<size_t> total_deallocations_{0};
    std::atomic<size_t> bytes_allocated_{0};
    std::atomic<size_t> bytes_freed_{0};

    // Collection control
    std::atomic<bool> collection_enabled_{true};
    std::atomic<size_t> collection_threshold_{1024 * 1024}; // 1MB default

    mutable std::mutex gc_mutex_;

public:
    GarbageCollector();
    ~GarbageCollector();

    // Object management
    void register_object(Object* obj, size_t size = 0);
    void unregister_object(Object* obj);
    void add_root(Object* obj);
    void remove_root(Object* obj);

    // Collection operations
    void collect();
    void force_collect();
    void enable_collection(bool enabled);

    // Memory management
    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size = 0);

    // Statistics and introspection
    size_t get_managed_object_count() const;
    size_t get_total_memory_usage() const;
    size_t get_collection_count() const;
    double get_collection_efficiency() const;

    // Configuration
    void set_collection_threshold(size_t threshold);
    size_t get_collection_threshold() const;

private:
    // Collection phases
    void mark_phase();
    void sweep_phase();
    void mark_object(Object* obj);
    void mark_from_roots();

    // Object finding
    ManagedObject* find_managed_object(Object* obj);
    ManagedObject* find_managed_object_ultra_fast(Object* obj);

    // Cleanup
    void cleanup_managed_objects();
    bool should_collect() const;
};

} // namespace Quanta