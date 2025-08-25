/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include "Object.h"
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace Quanta {

//=============================================================================
// Generational Garbage Collection - PHASE 2: V8-Level Memory Management
//=============================================================================

// Forward declarations
class Object;
class Context;

//=============================================================================
// Memory Generation - Represents different generations of objects
//=============================================================================

enum class Generation : uint8_t {
    YOUNG = 0,      // Recently allocated objects (high mortality rate)
    OLD = 1,        // Long-lived objects (low mortality rate)
    PERMANENT = 2   // Immortal objects (never collected)
};

//=============================================================================
// GC Object Header - Extended object header for generational GC
//=============================================================================

struct GCObjectHeader {
    Object* object;                     // Pointer to the actual object
    Generation generation;              // Current generation
    uint32_t age;                       // Number of GC cycles survived
    bool is_marked;                     // Mark bit for mark-and-sweep
    bool is_remembered;                 // In remembered set (points to younger gen)
    uint64_t allocation_time;           // When object was allocated
    size_t size;                        // Object size in bytes
    
    GCObjectHeader(Object* obj, size_t obj_size)
        : object(obj), generation(Generation::YOUNG), age(0), 
          is_marked(false), is_remembered(false), 
          allocation_time(std::chrono::steady_clock::now().time_since_epoch().count()),
          size(obj_size) {}
};

//=============================================================================
// Memory Region - Represents a contiguous memory region for a generation
//=============================================================================

class MemoryRegion {
public:
    static constexpr size_t DEFAULT_YOUNG_SIZE = 8 * 1024 * 1024;   // 8MB
    static constexpr size_t DEFAULT_OLD_SIZE = 64 * 1024 * 1024;    // 64MB
    static constexpr size_t DEFAULT_PERMANENT_SIZE = 16 * 1024 * 1024; // 16MB
    
private:
    Generation generation_;
    void* memory_start_;
    void* memory_end_;
    void* allocation_pointer_;
    size_t total_size_;
    size_t used_size_;
    std::vector<GCObjectHeader*> objects_;
    
public:
    MemoryRegion(Generation gen, size_t size);
    ~MemoryRegion();
    
    // Memory allocation
    GCObjectHeader* allocate(size_t size);
    bool can_allocate(size_t size) const;
    
    // Region management
    Generation get_generation() const { return generation_; }
    size_t get_total_size() const { return total_size_; }
    size_t get_used_size() const { return used_size_; }
    size_t get_free_size() const { return total_size_ - used_size_; }
    double get_utilization() const { return static_cast<double>(used_size_) / total_size_; }
    
    // Object management
    const std::vector<GCObjectHeader*>& get_objects() const { return objects_; }
    void add_object(GCObjectHeader* header);
    void remove_object(GCObjectHeader* header);
    
    // GC operations
    void mark_objects();
    size_t sweep_objects();
    void compact_memory();
    
    // Statistics
    size_t get_object_count() const { return objects_.size(); }
    void print_statistics() const;
};

//=============================================================================
// Remembered Set - Tracks inter-generational references
//=============================================================================

class RememberedSet {
private:
    std::unordered_set<GCObjectHeader*> old_to_young_refs_;
    std::unordered_set<GCObjectHeader*> permanent_to_young_refs_;
    std::unordered_set<GCObjectHeader*> permanent_to_old_refs_;
    
public:
    RememberedSet();
    ~RememberedSet();
    
    // Reference tracking
    void add_reference(GCObjectHeader* from, GCObjectHeader* to);
    void remove_reference(GCObjectHeader* from, GCObjectHeader* to);
    void clear();
    
    // Root scanning for minor GC
    std::vector<GCObjectHeader*> get_young_roots() const;
    std::vector<GCObjectHeader*> get_old_roots() const;
    
    // Statistics
    size_t get_old_to_young_count() const { return old_to_young_refs_.size(); }
    size_t get_permanent_to_young_count() const { return permanent_to_young_refs_.size(); }
    size_t get_permanent_to_old_count() const { return permanent_to_old_refs_.size(); }
    
    void print_statistics() const;
};

//=============================================================================
// Generational Garbage Collector - Main GC implementation
//=============================================================================

class GenerationalGC {
public:
    // GC configuration parameters
    struct GCConfig {
        size_t young_generation_size;       // Size of young generation
        size_t old_generation_size;         // Size of old generation
        size_t permanent_generation_size;   // Size of permanent generation
        
        uint32_t promotion_age_threshold;   // Age to promote to old generation
        double young_gc_trigger_ratio;      // Trigger minor GC when young gen is X% full
        double old_gc_trigger_ratio;        // Trigger major GC when old gen is X% full
        
        bool enable_concurrent_gc;          // Enable concurrent garbage collection
        bool enable_parallel_gc;            // Enable parallel garbage collection
        size_t gc_thread_count;             // Number of GC threads
        
        GCConfig()
            : young_generation_size(MemoryRegion::DEFAULT_YOUNG_SIZE),
              old_generation_size(MemoryRegion::DEFAULT_OLD_SIZE),
              permanent_generation_size(MemoryRegion::DEFAULT_PERMANENT_SIZE),
              promotion_age_threshold(3),
              young_gc_trigger_ratio(0.8),
              old_gc_trigger_ratio(0.9),
              enable_concurrent_gc(true),
              enable_parallel_gc(true),
              gc_thread_count(4) {}
    };
    
    // GC statistics
    struct GCStats {
        uint64_t minor_gc_count;            // Number of minor (young) GCs
        uint64_t major_gc_count;            // Number of major (full) GCs
        uint64_t total_allocation_bytes;    // Total bytes allocated
        uint64_t total_collection_time_ms;  // Total time spent in GC
        uint64_t objects_promoted;          // Objects promoted to old generation
        uint64_t objects_collected;         // Total objects collected
        
        double average_minor_gc_time_ms;    // Average minor GC time
        double average_major_gc_time_ms;    // Average major GC time
        double allocation_rate_mb_per_sec;  // Allocation rate
        
        GCStats() : minor_gc_count(0), major_gc_count(0), total_allocation_bytes(0),
                   total_collection_time_ms(0), objects_promoted(0), objects_collected(0),
                   average_minor_gc_time_ms(0.0), average_major_gc_time_ms(0.0),
                   allocation_rate_mb_per_sec(0.0) {}
    };
    
private:
    GCConfig config_;
    GCStats stats_;
    
    // Memory regions
    std::unique_ptr<MemoryRegion> young_generation_;
    std::unique_ptr<MemoryRegion> old_generation_;
    std::unique_ptr<MemoryRegion> permanent_generation_;
    
    // Inter-generational reference tracking
    std::unique_ptr<RememberedSet> remembered_set_;
    
    // Root set management
    std::vector<Object**> root_pointers_;
    std::unordered_set<Context*> active_contexts_;
    
    // GC state
    std::atomic<bool> gc_in_progress_;
    std::mutex gc_mutex_;
    std::chrono::steady_clock::time_point last_gc_time_;
    
    // Write barrier state
    std::atomic<bool> write_barrier_enabled_;
    
public:
    GenerationalGC();
    explicit GenerationalGC(const GCConfig& config);
    ~GenerationalGC();
    
    // Configuration
    void set_config(const GCConfig& config) { config_ = config; }
    const GCConfig& get_config() const { return config_; }
    
    // Object allocation
    GCObjectHeader* allocate_object(size_t size, Generation preferred_gen = Generation::YOUNG);
    void deallocate_object(GCObjectHeader* header);
    
    // Root management
    void add_root(Object** root_ptr);
    void remove_root(Object** root_ptr);
    void add_context(Context* ctx);
    void remove_context(Context* ctx);
    
    // Garbage collection
    void collect_minor();               // Collect young generation only
    void collect_major();               // Collect all generations
    void collect_auto();                // Automatic collection based on thresholds
    
    // Write barriers (for inter-generational reference tracking)
    void write_barrier(Object* from, Object* to);
    void enable_write_barrier(bool enabled = true) { write_barrier_enabled_ = enabled; }
    
    // Memory management
    bool should_trigger_minor_gc() const;
    bool should_trigger_major_gc() const;
    void promote_object(GCObjectHeader* header);
    
    // Statistics and monitoring
    const GCStats& get_statistics() const { return stats_; }
    void print_statistics() const;
    void print_memory_usage() const;
    void analyze_allocation_patterns() const;
    
    // Tuning and optimization
    void tune_gc_parameters();
    void adaptive_heap_sizing();
    
    // Global instance
    static GenerationalGC& get_instance();
    
private:
    // Internal GC operations
    void mark_phase(Generation max_generation);
    void sweep_phase(Generation max_generation);
    void compact_phase(Generation generation);
    void promotion_phase();
    
    // Root scanning
    void scan_roots(Generation max_generation);
    void scan_contexts(Generation max_generation);
    void scan_remembered_set(Generation target_generation);
    
    // Utilities
    void update_statistics();
    bool is_gc_needed() const;
    Generation get_object_generation(Object* obj) const;
    
public:
    // Make this accessible for allocator
    GCObjectHeader* get_object_header(Object* obj) const;
};

//=============================================================================
// GC-aware Object Allocator - Integrates with generational GC
//=============================================================================

class GCObjectAllocator {
private:
    GenerationalGC* gc_;
    
    // Allocation statistics
    struct AllocationStats {
        uint64_t young_allocations;
        uint64_t old_allocations;
        uint64_t permanent_allocations;
        uint64_t total_bytes_allocated;
        double allocation_rate;
        
        AllocationStats() : young_allocations(0), old_allocations(0),
                           permanent_allocations(0), total_bytes_allocated(0),
                           allocation_rate(0.0) {}
    } alloc_stats_;
    
public:
    GCObjectAllocator();
    ~GCObjectAllocator();
    
    // Object allocation with GC integration
    template<typename T, typename... Args>
    T* allocate_object(Args&&... args);
    
    template<typename T>
    T* allocate_array(size_t count);
    
    // Manual generation specification
    template<typename T, typename... Args>
    T* allocate_in_generation(Generation gen, Args&&... args);
    
    // Deallocation
    void deallocate_object(Object* obj);
    
    // Statistics
    const AllocationStats& get_allocation_stats() const { return alloc_stats_; }
    void print_allocation_statistics() const;
    
    // Global instance
    static GCObjectAllocator& get_instance();
};

//=============================================================================
// GC Integration with Engine - Hooks for automatic GC
//=============================================================================

class GCIntegration {
public:
    // Integration points
    static void initialize_gc();
    static void shutdown_gc();
    
    // Automatic GC triggers
    static void on_object_allocation(Object* obj);
    static void on_context_creation(Context* ctx);
    static void on_context_destruction(Context* ctx);
    static void on_function_call_enter();
    static void on_function_call_exit();
    
    // Performance monitoring
    static void monitor_allocation_rate();
    static void monitor_gc_pressure();
    
    // Adaptive behavior
    static void adapt_gc_frequency();
    static void optimize_gc_timing();
    
    // Manual control
    static void force_gc(bool major = false);
    static void disable_gc_temporarily();
    static void enable_gc();
};

} // namespace Quanta