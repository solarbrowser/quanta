/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Value.h"
#include "quanta/Object.h"
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace Quanta {


class Object;
class Context;


enum class Generation : uint8_t {
    YOUNG = 0,
    OLD = 1,
    PERMANENT = 2
};


struct GCObjectHeader {
    Object* object;
    Generation generation;
    uint32_t age;
    bool is_marked;
    bool is_remembered;
    uint64_t allocation_time;
    size_t size;
    
    GCObjectHeader(Object* obj, size_t obj_size)
        : object(obj), generation(Generation::YOUNG), age(0), 
          is_marked(false), is_remembered(false), 
          allocation_time(std::chrono::steady_clock::now().time_since_epoch().count()),
          size(obj_size) {}
};


class MemoryRegion {
public:
    static constexpr size_t DEFAULT_YOUNG_SIZE = 8 * 1024 * 1024;
    static constexpr size_t DEFAULT_OLD_SIZE = 64 * 1024 * 1024;
    static constexpr size_t DEFAULT_PERMANENT_SIZE = 16 * 1024 * 1024;
    
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
    
    GCObjectHeader* allocate(size_t size);
    bool can_allocate(size_t size) const;
    
    Generation get_generation() const { return generation_; }
    size_t get_total_size() const { return total_size_; }
    size_t get_used_size() const { return used_size_; }
    size_t get_free_size() const { return total_size_ - used_size_; }
    double get_utilization() const { return static_cast<double>(used_size_) / total_size_; }
    
    const std::vector<GCObjectHeader*>& get_objects() const { return objects_; }
    void add_object(GCObjectHeader* header);
    void remove_object(GCObjectHeader* header);
    
    void mark_objects();
    size_t sweep_objects();
    void compact_memory();
    
    size_t get_object_count() const { return objects_.size(); }
    void print_statistics() const;
};


class RememberedSet {
private:
    std::unordered_set<GCObjectHeader*> old_to_young_refs_;
    std::unordered_set<GCObjectHeader*> permanent_to_young_refs_;
    std::unordered_set<GCObjectHeader*> permanent_to_old_refs_;
    
public:
    RememberedSet();
    ~RememberedSet();
    
    void add_reference(GCObjectHeader* from, GCObjectHeader* to);
    void remove_reference(GCObjectHeader* from, GCObjectHeader* to);
    void clear();
    
    std::vector<GCObjectHeader*> get_young_roots() const;
    std::vector<GCObjectHeader*> get_old_roots() const;
    
    size_t get_old_to_young_count() const { return old_to_young_refs_.size(); }
    size_t get_permanent_to_young_count() const { return permanent_to_young_refs_.size(); }
    size_t get_permanent_to_old_count() const { return permanent_to_old_refs_.size(); }
    
    void print_statistics() const;
};


class GenerationalGC {
public:
    struct GCConfig {
        size_t young_generation_size;
        size_t old_generation_size;
        size_t permanent_generation_size;
        
        uint32_t promotion_age_threshold;
        double young_gc_trigger_ratio;
        double old_gc_trigger_ratio;
        
        bool enable_concurrent_gc;
        bool enable_parallel_gc;
        size_t gc_thread_count;
        
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
    
    struct GCStats {
        uint64_t minor_gc_count;
        uint64_t major_gc_count;
        uint64_t total_allocation_bytes;
        uint64_t total_collection_time_ms;
        uint64_t objects_promoted;
        uint64_t objects_collected;
        
        double average_minor_gc_time_ms;
        double average_major_gc_time_ms;
        double allocation_rate_mb_per_sec;
        
        GCStats() : minor_gc_count(0), major_gc_count(0), total_allocation_bytes(0),
                   total_collection_time_ms(0), objects_promoted(0), objects_collected(0),
                   average_minor_gc_time_ms(0.0), average_major_gc_time_ms(0.0),
                   allocation_rate_mb_per_sec(0.0) {}
    };
    
private:
    GCConfig config_;
    GCStats stats_;
    
    std::unique_ptr<MemoryRegion> young_generation_;
    std::unique_ptr<MemoryRegion> old_generation_;
    std::unique_ptr<MemoryRegion> permanent_generation_;
    
    std::unique_ptr<RememberedSet> remembered_set_;
    
    std::vector<Object**> root_pointers_;
    std::unordered_set<Context*> active_contexts_;
    
    std::atomic<bool> gc_in_progress_;
    std::mutex gc_mutex_;
    std::chrono::steady_clock::time_point last_gc_time_;
    
    std::atomic<bool> write_barrier_enabled_;
    
public:
    GenerationalGC();
    explicit GenerationalGC(const GCConfig& config);
    ~GenerationalGC();
    
    void set_config(const GCConfig& config) { config_ = config; }
    const GCConfig& get_config() const { return config_; }
    
    GCObjectHeader* allocate_object(size_t size, Generation preferred_gen = Generation::YOUNG);
    void deallocate_object(GCObjectHeader* header);
    
    void add_root(Object** root_ptr);
    void remove_root(Object** root_ptr);
    void add_context(Context* ctx);
    void remove_context(Context* ctx);
    
    void collect_minor();
    void collect_major();
    void collect_auto();
    
    void write_barrier(Object* from, Object* to);
    void enable_write_barrier(bool enabled = true) { write_barrier_enabled_ = enabled; }
    
    bool should_trigger_minor_gc() const;
    bool should_trigger_major_gc() const;
    void promote_object(GCObjectHeader* header);
    
    const GCStats& get_statistics() const { return stats_; }
    void print_statistics() const;
    void print_memory_usage() const;
    void analyze_allocation_patterns() const;
    
    void tune_gc_parameters();
    void adaptive_heap_sizing();
    
    static GenerationalGC& get_instance();
    
private:
    void mark_phase(Generation max_generation);
    void sweep_phase(Generation max_generation);
    void compact_phase(Generation generation);
    void promotion_phase();
    
    void scan_roots(Generation max_generation);
    void scan_contexts(Generation max_generation);
    void scan_remembered_set(Generation target_generation);
    
    void update_statistics();
    bool is_gc_needed() const;
    Generation get_object_generation(Object* obj) const;
    
public:
    GCObjectHeader* get_object_header(Object* obj) const;
};


class GCObjectAllocator {
private:
    GenerationalGC* gc_;
    
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
    
    template<typename T, typename... Args>
    T* allocate_object(Args&&... args);
    
    template<typename T>
    T* allocate_array(size_t count);
    
    template<typename T, typename... Args>
    T* allocate_in_generation(Generation gen, Args&&... args);
    
    void deallocate_object(Object* obj);
    
    const AllocationStats& get_allocation_stats() const { return alloc_stats_; }
    void print_allocation_statistics() const;
    
    static GCObjectAllocator& get_instance();
};


class GCIntegration {
public:
    static void initialize_gc();
    static void shutdown_gc();
    
    static void on_object_allocation(Object* obj);
    static void on_context_creation(Context* ctx);
    static void on_context_destruction(Context* ctx);
    static void on_function_call_enter();
    static void on_function_call_exit();
    
    static void monitor_allocation_rate();
    static void monitor_gc_pressure();
    
    static void adapt_gc_frequency();
    static void optimize_gc_timing();
    
    static void force_gc(bool major = false);
    static void disable_gc_temporarily();
    static void enable_gc();
};

}
