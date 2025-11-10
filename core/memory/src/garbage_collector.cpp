/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/garbage_collector.h"
#include "../../include/Object.h"
#include "../../include/Context.h"
#include <iostream>
#include <algorithm>

namespace Quanta {

GarbageCollector::GarbageCollector()
    : collection_mode_(CollectionMode::Automatic),
      young_generation_threshold_(1024 * 1024), // 1MB
      old_generation_threshold_(16 * 1024 * 1024), // 16MB
      heap_size_limit_(256 * 1024 * 1024), // 256MB
      gc_trigger_ratio_(0.75),
      gc_running_(false),
      stop_gc_thread_(false),
      collection_cycles_(0),
      ultra_fast_gc_(false),
      parallel_collection_(false),
      zero_copy_optimization_(false),
      heavy_operation_mode_(false),
      emergency_cleanup_threshold_(512 * 1024 * 1024), // 512MB
      last_collection_time_(std::chrono::high_resolution_clock::now()) {
}

GarbageCollector::~GarbageCollector() {
    stop_background_collection();

    // Clean up all managed objects
    for (auto* managed : managed_objects_) {
        delete managed;
    }
    managed_objects_.clear();
    root_objects_.clear();
    permanent_objects_.clear();
}

// Object lifecycle management
void GarbageCollector::register_object(Object* obj, size_t size) {
    std::lock_guard<std::mutex> lock(gc_mutex_);

    auto* managed = new ManagedObject(obj, Generation::Young, size);
    managed_objects_.push_back(managed);

    stats_.total_allocations++;
    stats_.bytes_allocated += size;
}

void GarbageCollector::unregister_object(Object* obj) {
    std::lock_guard<std::mutex> lock(gc_mutex_);

    auto it = std::find_if(managed_objects_.begin(), managed_objects_.end(),
                          [obj](const ManagedObject* managed) {
                              return managed->object == obj;
                          });

    if (it != managed_objects_.end()) {
        auto* managed = *it;
        stats_.total_deallocations++;
        stats_.bytes_freed += managed->size;

        delete managed;
        managed_objects_.erase(it);
    }
}

void GarbageCollector::add_root(Object* obj) {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    root_objects_.insert(obj);
}

void GarbageCollector::remove_root(Object* obj) {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    root_objects_.erase(obj);
}

void GarbageCollector::add_permanent(Object* obj) {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    permanent_objects_.insert(obj);
}

// Collection operations
void GarbageCollector::collect() {
    if (gc_running_.exchange(true)) {
        return; // Already collecting
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        mark_phase();
        sweep_phase();
        update_statistics();

        collection_cycles_++;
        last_collection_time_ = std::chrono::high_resolution_clock::now();

        auto duration = last_collection_time_ - start_time;
        stats_.total_gc_time += duration;
        stats_.total_collections++;
        stats_.average_gc_time = stats_.total_gc_time / stats_.total_collections;

    } catch (const std::exception& e) {
        std::cerr << "GC Error: " << e.what() << std::endl;
    }

    gc_running_ = false;
}

void GarbageCollector::force_collect() {
    collect();
}

void GarbageCollector::incremental_collect() {
    // Simplified incremental collection
    collect();
}

void GarbageCollector::collect_young_generation() {
    // Simplified young generation collection
    collect();
}

void GarbageCollector::collect_old_generation() {
    // Simplified old generation collection
    collect();
}

// Configuration
void GarbageCollector::set_collection_mode(CollectionMode mode) {
    collection_mode_ = mode;
}

void GarbageCollector::set_thresholds(size_t young_threshold, size_t old_threshold) {
    young_generation_threshold_ = young_threshold;
    old_generation_threshold_ = old_threshold;
}

void GarbageCollector::enable_ultra_fast_mode(bool enable) {
    ultra_fast_gc_ = enable;
}

void GarbageCollector::enable_parallel_collection(bool enable) {
    parallel_collection_ = enable;
}

void GarbageCollector::enable_heavy_operation_mode(bool enable) {
    heavy_operation_mode_ = enable;
}

// Statistics and monitoring
GarbageCollector::Statistics GarbageCollector::get_statistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

size_t GarbageCollector::get_managed_object_count() const {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    return managed_objects_.size();
}

size_t GarbageCollector::get_total_memory_usage() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_.bytes_allocated - stats_.bytes_freed;
}

double GarbageCollector::get_memory_pressure() const {
    size_t usage = get_total_memory_usage();
    return static_cast<double>(usage) / heap_size_limit_;
}

bool GarbageCollector::should_collect() const {
    if (collection_mode_ == CollectionMode::Manual) {
        return false;
    }

    return get_memory_pressure() > gc_trigger_ratio_;
}

// Advanced features
void GarbageCollector::start_background_collection() {
    if (!gc_thread_) {
        stop_gc_thread_ = false;
        gc_thread_ = std::make_unique<std::thread>(&GarbageCollector::background_collection_loop, this);
    }
}

void GarbageCollector::stop_background_collection() {
    if (gc_thread_) {
        stop_gc_thread_ = true;
        if (gc_thread_->joinable()) {
            gc_thread_->join();
        }
        gc_thread_.reset();
    }
}

void GarbageCollector::emergency_cleanup() {
    // Force immediate collection
    force_collect();
}

void GarbageCollector::defragment_heap() {
    // Simplified heap defragmentation
    collect();
}

// Thread safety
void GarbageCollector::pause_collection() {
    gc_mutex_.lock();
}

void GarbageCollector::resume_collection() {
    gc_mutex_.unlock();
}

// Private methods
void GarbageCollector::mark_phase() {
    // Clear all marks
    for (auto* managed : managed_objects_) {
        managed->is_marked = false;
    }

    // Mark from roots
    mark_from_roots();
}

void GarbageCollector::sweep_phase() {
    auto it = managed_objects_.begin();
    while (it != managed_objects_.end()) {
        auto* managed = *it;

        if (!managed->is_marked && permanent_objects_.find(managed->object) == permanent_objects_.end()) {
            // Object is unreachable and not permanent - can be collected
            stats_.bytes_freed += managed->size;
            delete managed;
            it = managed_objects_.erase(it);
        } else {
            ++it;
        }
    }
}

void GarbageCollector::mark_object(Object* obj) {
    if (!obj) return;

    auto* managed = find_managed_object(obj);
    if (managed && !managed->is_marked) {
        managed->is_marked = true;
        managed->access_count++;

        // In a real implementation, we would traverse object references
        // For now, this is simplified
    }
}

void GarbageCollector::mark_from_roots() {
    for (Object* root : root_objects_) {
        mark_object(root);
    }

    for (Object* permanent : permanent_objects_) {
        mark_object(permanent);
    }
}

void GarbageCollector::sweep_young_generation() {
    // Simplified young generation sweep
    sweep_phase();
}

void GarbageCollector::sweep_old_generation() {
    // Simplified old generation sweep
    sweep_phase();
}

GarbageCollector::ManagedObject* GarbageCollector::find_managed_object(Object* obj) {
    for (auto* managed : managed_objects_) {
        if (managed->object == obj) {
            return managed;
        }
    }
    return nullptr;
}

GarbageCollector::ManagedObject* GarbageCollector::find_managed_object_ultra_fast(Object* obj) {
    // For now, same as regular find - could be optimized with hash table
    return find_managed_object(obj);
}

void GarbageCollector::promote_to_old_generation(ManagedObject* managed) {
    if (managed->generation == Generation::Young) {
        managed->generation = Generation::Old;
    }
}

bool GarbageCollector::is_reachable(Object* obj) {
    auto* managed = find_managed_object(obj);
    return managed && managed->is_marked;
}

void GarbageCollector::update_statistics() {
    auto now = std::chrono::high_resolution_clock::now();
    size_t current_usage = get_total_memory_usage();

    if (current_usage > stats_.peak_memory_usage) {
        stats_.peak_memory_usage = current_usage;
    }
}

void GarbageCollector::optimize_collection_strategy() {
    // Could implement adaptive thresholds based on collection efficiency
}

bool GarbageCollector::should_promote_object(ManagedObject* managed) {
    auto age = std::chrono::high_resolution_clock::now() - managed->allocation_time;
    return age > std::chrono::milliseconds(1000) || managed->access_count > 10;
}

void GarbageCollector::background_collection_loop() {
    while (!stop_gc_thread_) {
        if (should_collect()) {
            collect();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void GarbageCollector::stop_gc_thread() {
    stop_gc_thread_ = true;
}

} // namespace Quanta