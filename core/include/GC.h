#ifndef QUANTA_GC_H
#define QUANTA_GC_H

#include "Value.h"
#include "Object.h"
#include <unordered_set>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>

namespace Quanta {

// Forward declarations
class Context;
class Engine;

/**
 * Garbage Collector for Quanta JavaScript Engine
 * Implements mark-and-sweep with generational collection
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
    
    // Object management
    std::unordered_set<ManagedObject*> managed_objects_;
    std::vector<ManagedObject*> young_generation_;
    std::vector<ManagedObject*> old_generation_;
    std::vector<ManagedObject*> permanent_generation_;
    
    // Root set for marking
    std::vector<Context*> root_contexts_;
    std::unordered_set<Object*> root_objects_;
    
    // Threading
    std::mutex gc_mutex_;
    std::thread gc_thread_;
    bool gc_running_;
    bool stop_gc_thread_;
    
    // Statistics
    Statistics stats_;
    
    // Weak references
    std::unordered_set<Object*> weak_references_;
    
public:
    GarbageCollector();
    ~GarbageCollector();
    
    // Configuration
    void set_collection_mode(CollectionMode mode) { collection_mode_ = mode; }
    CollectionMode get_collection_mode() const { return collection_mode_; }
    void set_heap_size_limit(size_t limit) { heap_size_limit_ = limit; }
    void set_gc_trigger_ratio(double ratio) { gc_trigger_ratio_ = ratio; }
    
    // Object lifecycle
    void register_object(Object* obj, size_t size = 0);
    void unregister_object(Object* obj);
    void register_context(Context* ctx);
    void unregister_context(Context* ctx);
    
    // Root set management
    void add_root_object(Object* obj);
    void remove_root_object(Object* obj);
    
    // Collection triggers
    void collect_garbage();
    void collect_young_generation();
    void collect_old_generation();
    void force_full_collection();
    
    // Memory management
    bool should_trigger_gc() const;
    size_t get_heap_size() const;
    size_t get_available_memory() const;
    
    // Weak references
    void add_weak_reference(Object* obj);
    void remove_weak_reference(Object* obj);
    
    // Statistics
    const Statistics& get_statistics() const { return stats_; }
    void reset_statistics();
    void print_statistics() const;
    
    // Thread management
    void start_gc_thread();
    void stop_gc_thread();
    
    // Debugging
    void print_heap_info() const;
    void verify_heap_integrity() const;

private:
    // Mark phase
    void mark_objects();
    void mark_from_context(Context* ctx);
    void mark_from_object(Object* obj);
    void mark_object(Object* obj);
    
    // Sweep phase
    void sweep_objects();
    void sweep_generation(std::vector<ManagedObject*>& generation);
    
    // Generational GC
    void promote_objects();
    void age_objects();
    
    // Cycle detection
    void detect_cycles();
    void break_cycles();
    
    // Helper methods
    ManagedObject* find_managed_object(Object* obj);
    void update_statistics(const std::chrono::high_resolution_clock::time_point& start);
    void cleanup_weak_references();
    
    // Background GC thread
    void gc_thread_main();
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
    
    // Copy constructor
    GCPtr(const GCPtr& other) : ptr_(other.ptr_), gc_(other.gc_) {
        if (ptr_ && gc_) {
            gc_->register_object(ptr_);
        }
    }
    
    // Assignment operator
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
    
    // Access operators
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* get() const { return ptr_; }
    
    // Conversion
    operator bool() const { return ptr_ != nullptr; }
    
    // Release ownership
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
    MemoryPool(size_t initial_size = 1024 * 1024); // 1MB default
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

} // namespace Quanta

#endif // QUANTA_GC_H