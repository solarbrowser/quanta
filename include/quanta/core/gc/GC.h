/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_GC_H
#define QUANTA_GC_H

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include <unordered_set>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>

namespace Quanta {

class Context;
class Engine;

/**
 * Garbage Collector for Quanta JavaScript Engine
 * Implements mark-and-sweep with generational collection
 */
class GarbageCollector {
public:
    enum class CollectionMode {
        Manual,
        Automatic,
        Incremental
    };
    
    enum class Generation {
        Young,
        Old,
        Permanent
    };
    
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
    CollectionMode collection_mode_;
    size_t young_generation_threshold_;
    size_t old_generation_threshold_;
    size_t heap_size_limit_;
    double gc_trigger_ratio_;
    
    std::unordered_set<ManagedObject*> managed_objects_;
    std::vector<ManagedObject*> young_generation_;
    std::vector<ManagedObject*> old_generation_;
    std::vector<ManagedObject*> permanent_generation_;
    
    std::vector<Context*> root_contexts_;
    std::unordered_set<Object*> root_objects_;
    
    std::mutex gc_mutex_;
    std::thread gc_thread_;
    bool gc_running_;
    bool stop_gc_thread_;
    uint32_t collection_cycles_;
    
    bool ultra_fast_gc_;
    bool parallel_collection_;
    bool zero_copy_optimization_;
    
    bool heavy_operation_mode_;
    size_t emergency_cleanup_threshold_;
    
    Statistics stats_;
    
    std::unordered_set<Object*> weak_references_;
    
public:
    GarbageCollector();
    ~GarbageCollector();
    
    void set_collection_mode(CollectionMode mode) { collection_mode_ = mode; }
    CollectionMode get_collection_mode() const { return collection_mode_; }
    void set_heap_size_limit(size_t limit) { heap_size_limit_ = limit; }
    void set_gc_trigger_ratio(double ratio) { gc_trigger_ratio_ = ratio; }
    
    void register_object(Object* obj, size_t size = 0);
    void unregister_object(Object* obj);
    void register_context(Context* ctx);
    void unregister_context(Context* ctx);
    
    void add_root_object(Object* obj);
    void remove_root_object(Object* obj);
    
    void collect_garbage();
    void collect_young_generation();
    void collect_old_generation();
    void force_full_collection();
    
    void collect_young_generation_ultra_fast();
    void collect_old_generation_ultra_fast();
    void force_ultra_fast_collection();
    
    bool should_trigger_gc() const;
    size_t get_heap_size() const;
    size_t get_available_memory() const;
    
    void add_weak_reference(Object* obj);
    void remove_weak_reference(Object* obj);
    
    const Statistics& get_statistics() const { return stats_; }
    void reset_statistics();
    void print_statistics() const;
    
    void start_gc_thread();
    void stop_gc_thread();
    
    void enable_heavy_operation_mode();
    void disable_heavy_operation_mode();
    void emergency_cleanup();
    void prepare_for_heavy_load(size_t expected_objects);
    bool is_heavy_operation_mode() const { return heavy_operation_mode_; }
    
    void print_heap_info() const;
    void verify_heap_integrity() const;

private:
    void mark_objects();
    void mark_from_context(Context* ctx);
    void mark_from_object(Object* obj);
    void mark_object(Object* obj);
    
    void sweep_objects();
    void sweep_generation(std::vector<ManagedObject*>& generation);
    
    void promote_objects();
    void age_objects();
    
    void detect_cycles();
    void break_cycles();
    
    ManagedObject* find_managed_object(Object* obj);
    void update_statistics(const std::chrono::high_resolution_clock::time_point& start);
    void cleanup_weak_references();
    
    void gc_thread_main();

    void collect_young_generation_photon_core();
    void collect_old_generation_photon_core();
    void force_photon_core_collection();
    void collect_young_generation_parallel();
    void collect_old_generation_parallel();
    
    void mark_objects_ultra_fast();
    void mark_object_ultra_fast(Object* obj);
    void sweep_generation_ultra_fast(std::vector<ManagedObject*>& generation);
    void sweep_objects_ultra_fast();
    void promote_objects_ultra_fast();
    void detect_cycles_ultra_fast();
    void break_cycles_ultra_fast();
    ManagedObject* find_managed_object_ultra_fast(Object* obj);
    
    void mark_objects_parallel_worker();
    void sweep_generation_parallel_worker(std::vector<ManagedObject*>& generation, 
                                         size_t thread_id, size_t thread_count);
};

/**
 * RAII wrapper for GC-managed objects
 */
template<typename T>
class GCPtr {
private:
    T* ptr_;
    GarbageCollector* gc_;

public:
    GCPtr(T* ptr, GarbageCollector* gc) : ptr_(ptr), gc_(gc) {
        if (ptr_ && gc_) {
            gc_->register_object(ptr_);
        }
    }
    
    ~GCPtr() {
        if (ptr_ && gc_) {
            gc_->unregister_object(ptr_);
        }
    }
    
    GCPtr(const GCPtr& other) : ptr_(other.ptr_), gc_(other.gc_) {
        if (ptr_ && gc_) {
            gc_->register_object(ptr_);
        }
    }
    
    GCPtr& operator=(const GCPtr& other) {
        if (this != &other) {
            if (ptr_ && gc_) {
                gc_->unregister_object(ptr_);
            }
            ptr_ = other.ptr_;
            gc_ = other.gc_;
            if (ptr_ && gc_) {
                gc_->register_object(ptr_);
            }
        }
        return *this;
    }
    
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* get() const { return ptr_; }
    
    operator bool() const { return ptr_ != nullptr; }
    
    T* release() {
        T* temp = ptr_;
        ptr_ = nullptr;
        return temp;
    }
};

/**
 * Memory pool for efficient allocation
 */
class MemoryPool {
private:
    struct Block {
        void* memory;
        size_t size;
        bool is_free;
        Block* next;
        
        Block(size_t s) : memory(nullptr), size(s), is_free(true), next(nullptr) {
            memory = std::malloc(s);
        }
        
        ~Block() {
            if (memory) {
                std::free(memory);
            }
        }
    };
    
    Block* head_;
    size_t total_size_;
    size_t used_size_;
    std::mutex pool_mutex_;

public:
    MemoryPool(size_t initial_size = 1024 * 1024);
    ~MemoryPool();
    
    void* allocate(size_t size);
    void deallocate(void* ptr);
    
    size_t get_total_size() const { return total_size_; }
    size_t get_used_size() const { return used_size_; }
    size_t get_free_size() const { return total_size_ - used_size_; }
    
    void defragment();
    
private:
    Block* find_free_block(size_t size);
    void split_block(Block* block, size_t size);
    void merge_free_blocks();
};

}

#endif
