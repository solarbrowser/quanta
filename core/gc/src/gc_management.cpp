/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/gc_management.h"
#include "../../include/GC.h"
#include "../../include/Object.h"
#include "../../include/Context.h"
#include <iostream>
#include <algorithm>
#include <thread>

namespace Quanta {

// EXTRACTED FROM GC.cpp - Management functionality (lines 46-158, 300+)

//=============================================================================
// Object Registration (originally lines 46-129)
//=============================================================================

void GCManagement::register_object(GarbageCollector& gc, Object* obj, size_t size) {
    if (!obj) return;

    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    // Check if object is already registered
    if (is_object_registered(gc, obj)) {
        return;
    }

    auto* managed = new GarbageCollector::ManagedObject();
    managed->object = obj;
    managed->size = size;
    managed->marked = false;
    managed->mark_count = 0;
    managed->generation = 0; // Start in young generation
    managed->allocation_time = std::chrono::high_resolution_clock::now();

    gc.managed_objects_.insert(managed);

    // Add to young generation by default
    gc.young_generation_.push_back(managed);

    // Update statistics
    gc.stats_.total_allocations++;
    gc.stats_.bytes_allocated += size;

    // Check if we need to trigger GC
    if (gc.ultra_fast_gc_) {
        // In ultra-fast mode, check for immediate collection needs
        size_t young_size = get_young_generation_size(gc);
        if (young_size > gc.young_generation_threshold_) {
            // Schedule a quick young generation collection
        }
    }
}

void GCManagement::unregister_object(GarbageCollector& gc, Object* obj) {
    if (!obj) return;

    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    auto* managed = gc.find_managed_object(obj);
    if (managed) {
        gc.managed_objects_.erase(managed);

        // Remove from generation vectors
        auto remove_from_vector = [managed](std::vector<GarbageCollector::ManagedObject*>& vec) {
            vec.erase(std::remove(vec.begin(), vec.end(), managed), vec.end());
        };

        remove_from_vector(gc.young_generation_);
        remove_from_vector(gc.old_generation_);
        remove_from_vector(gc.permanent_generation_);

        gc.stats_.total_deallocations++;
        gc.stats_.bytes_freed += managed->size;

        delete managed;
    }
}

void GCManagement::register_context(GarbageCollector& gc, Context* ctx) {
    if (!ctx) return;

    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    if (!is_context_registered(gc, ctx)) {
        gc.root_contexts_.push_back(ctx);
    }
}

void GCManagement::unregister_context(GarbageCollector& gc, Context* ctx) {
    if (!ctx) return;

    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.root_contexts_.erase(std::remove(gc.root_contexts_.begin(), gc.root_contexts_.end(), ctx),
                           gc.root_contexts_.end());
}

//=============================================================================
// Root Object Management (originally lines 146-158)
//=============================================================================

void GCManagement::add_root_object(GarbageCollector& gc, Object* obj) {
    if (!obj) return;

    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.root_objects_.insert(obj);
}

void GCManagement::remove_root_object(GarbageCollector& gc, Object* obj) {
    if (!obj) return;

    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.root_objects_.erase(obj);
}

void GCManagement::clear_all_root_objects(GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.root_objects_.clear();
}

//=============================================================================
// Weak Reference Management
//=============================================================================

void GCManagement::add_weak_reference(GarbageCollector& gc, Object* obj) {
    if (!obj) return;

    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.weak_references_.insert(obj);
}

void GCManagement::remove_weak_reference(GarbageCollector& gc, Object* obj) {
    if (!obj) return;

    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.weak_references_.erase(obj);
}

void GCManagement::cleanup_weak_references(GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    // Remove weak references to objects that no longer exist
    auto it = gc.weak_references_.begin();
    while (it != gc.weak_references_.end()) {
        Object* obj = *it;
        if (!obj || !is_object_registered(gc, obj)) {
            it = gc.weak_references_.erase(it);
        } else {
            ++it;
        }
    }
}

//=============================================================================
// Thread Management
//=============================================================================

void GCManagement::start_gc_thread(GarbageCollector& gc) {
    if (gc.gc_thread_.joinable()) {
        return; // Thread already running
    }

    gc.stop_gc_thread_ = false;
    gc.gc_thread_ = std::thread([&gc]() {
        gc_thread_main(gc);
    });
}

void GCManagement::stop_gc_thread(GarbageCollector& gc) {
    gc.stop_gc_thread_ = true;
    gc.gc_condition_.notify_all();

    if (gc.gc_thread_.joinable()) {
        gc.gc_thread_.join();
    }
}

void GCManagement::gc_thread_main(GarbageCollector& gc) {
    while (!gc.stop_gc_thread_) {
        std::unique_lock<std::mutex> lock(gc.gc_mutex_);

        // Wait for collection trigger or timeout
        gc.gc_condition_.wait_for(lock, std::chrono::milliseconds(100), [&gc]() {
            return gc.stop_gc_thread_ || should_trigger_gc_internal(gc);
        });

        if (gc.stop_gc_thread_) {
            break;
        }

        // Trigger collection if needed
        if (should_trigger_gc_internal(gc)) {
            lock.unlock();

            if (get_young_generation_size(gc) > gc.young_generation_threshold_) {
                // Use collection module
                // GCCollection::collect_young_generation(gc);
            } else if (get_old_generation_size(gc) > gc.old_generation_threshold_) {
                // GCCollection::collect_old_generation(gc);
            }

            lock.lock();
        }
    }
}

//=============================================================================
// Memory Tracking
//=============================================================================

size_t GCManagement::get_total_heap_size(const GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    size_t total_size = 0;
    for (const auto* managed : gc.managed_objects_) {
        if (managed) {
            total_size += managed->size;
        }
    }
    return total_size;
}

size_t GCManagement::get_young_generation_size(const GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    size_t young_size = 0;
    for (const auto* managed : gc.young_generation_) {
        if (managed) {
            young_size += managed->size;
        }
    }
    return young_size;
}

size_t GCManagement::get_old_generation_size(const GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    size_t old_size = 0;
    for (const auto* managed : gc.old_generation_) {
        if (managed) {
            old_size += managed->size;
        }
    }
    return old_size;
}

size_t GCManagement::get_allocated_bytes(const GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    return gc.stats_.bytes_allocated;
}

//=============================================================================
// Configuration
//=============================================================================

void GCManagement::enable_ultra_fast_mode(GarbageCollector& gc, bool enabled) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.ultra_fast_gc_ = enabled;

    if (enabled) {
        // Optimize thresholds for speed
        gc.young_generation_threshold_ = std::min(gc.young_generation_threshold_, size_t(2 * 1024)); // 2KB
        gc.gc_trigger_ratio_ = 0.2; // More aggressive collection
    } else {
        // Reset to normal thresholds
        gc.young_generation_threshold_ = 4 * 1024; // 4KB
        gc.gc_trigger_ratio_ = 0.3;
    }
}

void GCManagement::set_collection_mode(GarbageCollector& gc, GarbageCollector::CollectionMode mode) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.collection_mode_ = mode;
}

void GCManagement::set_heap_size_limit(GarbageCollector& gc, size_t limit) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.heap_size_limit_ = limit;
}

void GCManagement::set_gc_trigger_ratio(GarbageCollector& gc, double ratio) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.gc_trigger_ratio_ = std::max(0.1, std::min(0.9, ratio)); // Clamp between 0.1 and 0.9
}

//=============================================================================
// Statistics and Debugging
//=============================================================================

void GCManagement::reset_statistics(GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);
    gc.stats_ = GarbageCollector::Statistics{};
}

void GCManagement::print_statistics(const GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    std::cout << "=== Garbage Collection Statistics ===" << std::endl;
    std::cout << "Total Allocations: " << gc.stats_.total_allocations << std::endl;
    std::cout << "Total Deallocations: " << gc.stats_.total_deallocations << std::endl;
    std::cout << "Bytes Allocated: " << gc.stats_.bytes_allocated << std::endl;
    std::cout << "Bytes Freed: " << gc.stats_.bytes_freed << std::endl;
    std::cout << "Collection Cycles: " << gc.stats_.collection_cycles << std::endl;
    std::cout << "Total Collection Time: " << gc.stats_.total_collection_time << "ms" << std::endl;
    std::cout << "Average Collection Time: "
              << (gc.stats_.collection_cycles > 0 ? gc.stats_.total_collection_time / gc.stats_.collection_cycles : 0)
              << "ms" << std::endl;
    std::cout << "Min Collection Time: " << gc.stats_.min_collection_time << "ms" << std::endl;
    std::cout << "Max Collection Time: " << gc.stats_.max_collection_time << "ms" << std::endl;

    size_t current_heap = get_total_heap_size(gc);
    std::cout << "Current Heap Size: " << current_heap << " bytes" << std::endl;
    std::cout << "Young Generation Size: " << get_young_generation_size(gc) << " bytes" << std::endl;
    std::cout << "Old Generation Size: " << get_old_generation_size(gc) << " bytes" << std::endl;
    std::cout << "Heap Utilization: "
              << (gc.heap_size_limit_ > 0 ? (double)current_heap / gc.heap_size_limit_ * 100.0 : 0)
              << "%" << std::endl;
}

void GCManagement::print_heap_info(const GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    std::cout << "=== Heap Information ===" << std::endl;
    std::cout << "Total Objects: " << gc.managed_objects_.size() << std::endl;
    std::cout << "Young Generation Objects: " << gc.young_generation_.size() << std::endl;
    std::cout << "Old Generation Objects: " << gc.old_generation_.size() << std::endl;
    std::cout << "Permanent Generation Objects: " << gc.permanent_generation_.size() << std::endl;
    std::cout << "Root Objects: " << gc.root_objects_.size() << std::endl;
    std::cout << "Root Contexts: " << gc.root_contexts_.size() << std::endl;
    std::cout << "Weak References: " << gc.weak_references_.size() << std::endl;

    std::cout << "Heap Size Limit: " << gc.heap_size_limit_ << " bytes" << std::endl;
    std::cout << "Young Gen Threshold: " << gc.young_generation_threshold_ << " bytes" << std::endl;
    std::cout << "Old Gen Threshold: " << gc.old_generation_threshold_ << " bytes" << std::endl;
    std::cout << "GC Trigger Ratio: " << gc.gc_trigger_ratio_ << std::endl;

    std::cout << "Collection Mode: ";
    switch (gc.collection_mode_) {
        case GarbageCollector::CollectionMode::Automatic:
            std::cout << "Automatic";
            break;
        case GarbageCollector::CollectionMode::Manual:
            std::cout << "Manual";
            break;
        default:
            std::cout << "Unknown";
            break;
    }
    std::cout << std::endl;

    std::cout << "Ultra Fast Mode: " << (gc.ultra_fast_gc_ ? "Enabled" : "Disabled") << std::endl;
    std::cout << "Parallel Collection: " << (gc.parallel_collection_ ? "Enabled" : "Disabled") << std::endl;
    std::cout << "Zero Copy Optimization: " << (gc.zero_copy_optimization_ ? "Enabled" : "Disabled") << std::endl;
}

void GCManagement::verify_heap_integrity(const GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    std::cout << "[GC] Verifying heap integrity..." << std::endl;

    size_t total_objects = 0;
    size_t young_objects = 0;
    size_t old_objects = 0;
    size_t permanent_objects = 0;

    // Count objects in generations
    for (const auto* managed : gc.managed_objects_) {
        if (managed) {
            validate_object_integrity(managed->object);
            total_objects++;
        }
    }

    young_objects = gc.young_generation_.size();
    old_objects = gc.old_generation_.size();
    permanent_objects = gc.permanent_generation_.size();

    std::cout << "[GC] Heap integrity check completed:" << std::endl;
    std::cout << "  Total objects in managed_objects_: " << total_objects << std::endl;
    std::cout << "  Objects in young generation: " << young_objects << std::endl;
    std::cout << "  Objects in old generation: " << old_objects << std::endl;
    std::cout << "  Objects in permanent generation: " << permanent_objects << std::endl;

    // Check for consistency
    size_t generation_total = young_objects + old_objects + permanent_objects;
    if (generation_total != total_objects) {
        std::cout << "[GC] WARNING: Generation count mismatch! Total: "
                  << total_objects << ", Generation sum: " << generation_total << std::endl;
    } else {
        std::cout << "[GC] Heap integrity verified successfully." << std::endl;
    }
}

//=============================================================================
// Emergency Operations
//=============================================================================

void GCManagement::emergency_cleanup(GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    std::cout << "[GC] EMERGENCY CLEANUP INITIATED" << std::endl;

    // Clean up weak references
    cleanup_weak_references(gc);

    // Force immediate collection of all generations
    // This would use the collection module
    // GCCollection::emergency_collection(gc);

    // Reset emergency flags
    gc.heavy_operation_mode_ = false;

    std::cout << "[GC] Emergency cleanup completed." << std::endl;
}

void GCManagement::force_cleanup_all(GarbageCollector& gc) {
    std::lock_guard<std::mutex> lock(gc.gc_mutex_);

    // Clean up all managed objects
    for (auto* managed : gc.managed_objects_) {
        if (managed) {
            delete managed;
        }
    }
    gc.managed_objects_.clear();
    gc.young_generation_.clear();
    gc.old_generation_.clear();
    gc.permanent_generation_.clear();

    // Clear roots and weak references
    gc.root_objects_.clear();
    gc.root_contexts_.clear();
    gc.weak_references_.clear();

    // Reset statistics
    gc.stats_ = GarbageCollector::Statistics{};

    std::cout << "[GC] Complete cleanup performed." << std::endl;
}

bool GCManagement::is_memory_pressure_high(const GarbageCollector& gc) {
    size_t total_heap = get_total_heap_size(gc);
    return total_heap > gc.emergency_cleanup_threshold_;
}

//=============================================================================
// Private Helper Methods
//=============================================================================

bool GCManagement::is_object_registered(const GarbageCollector& gc, Object* obj) {
    if (!obj) return false;

    auto* managed = gc.find_managed_object(obj);
    return managed != nullptr;
}

bool GCManagement::is_context_registered(const GarbageCollector& gc, Context* ctx) {
    if (!ctx) return false;

    return std::find(gc.root_contexts_.begin(), gc.root_contexts_.end(), ctx)
           != gc.root_contexts_.end();
}

void GCManagement::update_heap_statistics(GarbageCollector& gc) {
    // Update heap statistics periodically
    gc.stats_.current_heap_size = get_total_heap_size(gc);
    gc.stats_.last_update_time = std::chrono::high_resolution_clock::now();
}

void GCManagement::validate_object_integrity(Object* obj) {
    if (!obj) return;

    // Basic object validation
    try {
        // Check if object memory is accessible
        volatile auto test = obj->get_type();
        (void)test; // Suppress unused variable warning
    } catch (...) {
        std::cout << "[GC] WARNING: Invalid object detected at " << obj << std::endl;
    }
}

// Internal helper function for thread management
bool should_trigger_gc_internal(const GarbageCollector& gc) {
    if (gc.collection_mode_ == GarbageCollector::CollectionMode::Manual) {
        return false;
    }

    size_t total_heap_size = 0;
    for (const auto* managed : gc.managed_objects_) {
        if (managed) {
            total_heap_size += managed->size;
        }
    }

    return total_heap_size > gc.heap_size_limit_ * gc.gc_trigger_ratio_;
}

} // namespace Quanta