#include "../include/GC.h"
#include "Context.h"
#include <iostream>
#include <algorithm>
#include <string.h>

namespace Quanta {

//=============================================================================
// GarbageCollector Implementation
//=============================================================================

GarbageCollector::GarbageCollector() 
    : collection_mode_(CollectionMode::Automatic),
      young_generation_threshold_(1024 * 1024),      // 1MB
      old_generation_threshold_(10 * 1024 * 1024),   // 10MB
      heap_size_limit_(100 * 1024 * 1024),           // 100MB
      gc_trigger_ratio_(0.8),
      gc_running_(false),
      stop_gc_thread_(false) {
}

GarbageCollector::~GarbageCollector() {
    stop_gc_thread();
    
    // Clean up all managed objects
    for (auto* managed : managed_objects_) {
        delete managed;
    }
    managed_objects_.clear();
}

void GarbageCollector::register_object(Object* obj, size_t size) {
    if (!obj) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    // Estimate size if not provided
    if (size == 0) {
        size = sizeof(Object) + obj->property_count() * sizeof(Value);
    }
    
    auto* managed = new ManagedObject(obj, Generation::Young, size);
    managed_objects_.insert(managed);
    young_generation_.push_back(managed);
    
    stats_.total_allocations++;
    stats_.bytes_allocated += size;
    
    // Update peak memory usage
    size_t current_heap_size = get_heap_size();
    if (current_heap_size > stats_.peak_memory_usage) {
        stats_.peak_memory_usage = current_heap_size;
    }
    
    // Trigger GC if needed
    if (collection_mode_ == CollectionMode::Automatic && should_trigger_gc()) {
        collect_garbage();
    }
}

void GarbageCollector::unregister_object(Object* obj) {
    if (!obj) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    auto* managed = find_managed_object(obj);
    if (managed) {
        managed_objects_.erase(managed);
        
        // Remove from generation vectors
        auto remove_from_vector = [managed](std::vector<ManagedObject*>& vec) {
            vec.erase(std::remove(vec.begin(), vec.end(), managed), vec.end());
        };
        
        remove_from_vector(young_generation_);
        remove_from_vector(old_generation_);
        remove_from_vector(permanent_generation_);
        
        stats_.total_deallocations++;
        stats_.bytes_freed += managed->size;
        
        delete managed;
    }
}

void GarbageCollector::register_context(Context* ctx) {
    if (!ctx) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    root_contexts_.push_back(ctx);
}

void GarbageCollector::unregister_context(Context* ctx) {
    if (!ctx) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    root_contexts_.erase(std::remove(root_contexts_.begin(), root_contexts_.end(), ctx), 
                        root_contexts_.end());
}

void GarbageCollector::add_root_object(Object* obj) {
    if (!obj) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    root_objects_.insert(obj);
}

void GarbageCollector::remove_root_object(Object* obj) {
    if (!obj) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    root_objects_.erase(obj);
}

void GarbageCollector::collect_garbage() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    // Mark phase
    mark_objects();
    
    // Sweep phase
    sweep_objects();
    
    // Promote objects between generations
    promote_objects();
    
    // Clean up weak references
    cleanup_weak_references();
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::collect_young_generation() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    // Mark from roots
    mark_objects();
    
    // Sweep only young generation
    sweep_generation(young_generation_);
    
    // Promote surviving objects
    promote_objects();
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::collect_old_generation() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    // Full mark phase
    mark_objects();
    
    // Sweep old generation
    sweep_generation(old_generation_);
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::force_full_collection() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    // Full mark and sweep
    mark_objects();
    sweep_objects();
    
    // Cycle detection and breaking
    detect_cycles();
    break_cycles();
    
    gc_running_ = false;
    update_statistics(start);
}

bool GarbageCollector::should_trigger_gc() const {
    size_t current_heap_size = get_heap_size();
    
    // Trigger if heap size exceeds threshold
    if (current_heap_size > heap_size_limit_ * gc_trigger_ratio_) {
        return true;
    }
    
    // Trigger if young generation is full
    if (young_generation_.size() * sizeof(ManagedObject) > young_generation_threshold_) {
        return true;
    }
    
    return false;
}

size_t GarbageCollector::get_heap_size() const {
    size_t total = 0;
    for (const auto* managed : managed_objects_) {
        total += managed->size;
    }
    return total;
}

size_t GarbageCollector::get_available_memory() const {
    return heap_size_limit_ - get_heap_size();
}

void GarbageCollector::add_weak_reference(Object* obj) {
    if (!obj) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    weak_references_.insert(obj);
}

void GarbageCollector::remove_weak_reference(Object* obj) {
    if (!obj) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    weak_references_.erase(obj);
}

void GarbageCollector::reset_statistics() {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    stats_ = Statistics();
}

void GarbageCollector::print_statistics() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(gc_mutex_));
    
    std::cout << "=== Garbage Collector Statistics ===" << std::endl;
    std::cout << "Total Allocations: " << stats_.total_allocations << std::endl;
    std::cout << "Total Deallocations: " << stats_.total_deallocations << std::endl;
    std::cout << "Total Collections: " << stats_.total_collections << std::endl;
    std::cout << "Bytes Allocated: " << stats_.bytes_allocated << std::endl;
    std::cout << "Bytes Freed: " << stats_.bytes_freed << std::endl;
    std::cout << "Peak Memory Usage: " << stats_.peak_memory_usage << " bytes" << std::endl;
    std::cout << "Current Heap Size: " << get_heap_size() << " bytes" << std::endl;
    std::cout << "Average GC Time: " << stats_.average_gc_time.count() << "ms" << std::endl;
    std::cout << "Young Generation Objects: " << young_generation_.size() << std::endl;
    std::cout << "Old Generation Objects: " << old_generation_.size() << std::endl;
    std::cout << "Permanent Generation Objects: " << permanent_generation_.size() << std::endl;
}

void GarbageCollector::start_gc_thread() {
    if (collection_mode_ != CollectionMode::Automatic) return;
    
    stop_gc_thread_ = false;
    gc_thread_ = std::thread(&GarbageCollector::gc_thread_main, this);
}

void GarbageCollector::stop_gc_thread() {
    stop_gc_thread_ = true;
    if (gc_thread_.joinable()) {
        gc_thread_.join();
    }
}

void GarbageCollector::print_heap_info() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(gc_mutex_));
    
    std::cout << "=== Heap Information ===" << std::endl;
    std::cout << "Total Objects: " << managed_objects_.size() << std::endl;
    std::cout << "Young Generation: " << young_generation_.size() << std::endl;
    std::cout << "Old Generation: " << old_generation_.size() << std::endl;
    std::cout << "Permanent Generation: " << permanent_generation_.size() << std::endl;
    std::cout << "Root Objects: " << root_objects_.size() << std::endl;
    std::cout << "Weak References: " << weak_references_.size() << std::endl;
    std::cout << "Heap Size: " << get_heap_size() << " bytes" << std::endl;
    std::cout << "Heap Limit: " << heap_size_limit_ << " bytes" << std::endl;
}

void GarbageCollector::verify_heap_integrity() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(gc_mutex_));
    
    // Verify all objects in generations are in managed_objects_
    auto verify_generation = [this](const std::vector<ManagedObject*>& gen, const std::string& name) {
        for (const auto* managed : gen) {
            if (managed_objects_.find(const_cast<ManagedObject*>(managed)) == managed_objects_.end()) {
                std::cerr << "ERROR: " << name << " object not in managed_objects_" << std::endl;
            }
        }
    };
    
    verify_generation(young_generation_, "Young generation");
    verify_generation(old_generation_, "Old generation");
    verify_generation(permanent_generation_, "Permanent generation");
    
    std::cout << "Heap integrity verification complete." << std::endl;
}

// Private methods

void GarbageCollector::mark_objects() {
    // Clear all marks
    for (auto* managed : managed_objects_) {
        managed->is_marked = false;
    }
    
    // Mark from root contexts
    for (Context* ctx : root_contexts_) {
        mark_from_context(ctx);
    }
    
    // Mark from root objects
    for (Object* obj : root_objects_) {
        mark_object(obj);
    }
}

void GarbageCollector::mark_from_context(Context* ctx) {
    if (!ctx) return;
    
    // Mark global object
    if (ctx->get_global_object()) {
        mark_object(ctx->get_global_object());
    }
    
    // Mark all bindings in the context
    // This would require access to context's internal state
    // For now, we'll mark the global object
}

void GarbageCollector::mark_from_object(Object* obj) {
    if (!obj) return;
    
    mark_object(obj);
    
    // Mark all referenced objects
    std::vector<std::string> keys = obj->get_enumerable_keys();
    for (const std::string& key : keys) {
        Value prop = obj->get_property(key);
        if (prop.is_object()) {
            mark_object(prop.as_object());
        }
    }
}

void GarbageCollector::mark_object(Object* obj) {
    if (!obj) return;
    
    auto* managed = find_managed_object(obj);
    if (managed && !managed->is_marked) {
        managed->is_marked = true;
        managed->access_count++;
        
        // Recursively mark referenced objects
        mark_from_object(obj);
    }
}

void GarbageCollector::sweep_objects() {
    sweep_generation(young_generation_);
    sweep_generation(old_generation_);
    // Don't sweep permanent generation
}

void GarbageCollector::sweep_generation(std::vector<ManagedObject*>& generation) {
    auto it = generation.begin();
    while (it != generation.end()) {
        auto* managed = *it;
        if (!managed->is_marked) {
            // Object is not reachable, delete it
            managed_objects_.erase(managed);
            stats_.total_deallocations++;
            stats_.bytes_freed += managed->size;
            
            delete managed->object;
            delete managed;
            it = generation.erase(it);
        } else {
            ++it;
        }
    }
}

void GarbageCollector::promote_objects() {
    // Promote young objects that have survived enough collections
    auto it = young_generation_.begin();
    while (it != young_generation_.end()) {
        auto* managed = *it;
        if (managed->access_count > 3) { // Arbitrary threshold
            managed->generation = Generation::Old;
            old_generation_.push_back(managed);
            it = young_generation_.erase(it);
        } else {
            ++it;
        }
    }
}

void GarbageCollector::age_objects() {
    // Age objects in old generation
    for (auto* managed : old_generation_) {
        managed->access_count = std::max(0u, managed->access_count - 1);
    }
}

void GarbageCollector::detect_cycles() {
    // Simple cycle detection algorithm
    // In a production system, this would be more sophisticated
    for (auto* managed : managed_objects_) {
        if (managed->is_marked) {
            // Check for cycles using DFS
            std::unordered_set<Object*> visited;
            std::unordered_set<Object*> stack;
            // Implementation would go here
        }
    }
}

void GarbageCollector::break_cycles() {
    // Break detected cycles
    // This would involve careful handling of circular references
}

GarbageCollector::ManagedObject* GarbageCollector::find_managed_object(Object* obj) {
    for (auto* managed : managed_objects_) {
        if (managed->object == obj) {
            return managed;
        }
    }
    return nullptr;
}

void GarbageCollector::update_statistics(const std::chrono::high_resolution_clock::time_point& start) {
    stats_.total_collections++;
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::duration<double>>(end - start);
    
    stats_.total_gc_time += duration;
    stats_.average_gc_time = stats_.total_gc_time / stats_.total_collections;
}

void GarbageCollector::cleanup_weak_references() {
    auto it = weak_references_.begin();
    while (it != weak_references_.end()) {
        auto* managed = find_managed_object(*it);
        if (!managed || !managed->is_marked) {
            it = weak_references_.erase(it);
        } else {
            ++it;
        }
    }
}

void GarbageCollector::gc_thread_main() {
    while (!stop_gc_thread_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (should_trigger_gc()) {
            collect_young_generation();
        }
        
        // Periodic full collection
        static int counter = 0;
        if (++counter % 100 == 0) {
            collect_old_generation();
        }
    }
}

//=============================================================================
// MemoryPool Implementation
//=============================================================================

MemoryPool::MemoryPool(size_t initial_size) 
    : head_(nullptr), total_size_(0), used_size_(0) {
    
    // Allocate initial block
    head_ = new Block(initial_size);
    total_size_ = initial_size;
}

MemoryPool::~MemoryPool() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    Block* current = head_;
    while (current) {
        Block* next = current->next;
        delete current;
        current = next;
    }
}

void* MemoryPool::allocate(size_t size) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    Block* block = find_free_block(size);
    if (!block) {
        // Allocate new block
        block = new Block(std::max(size, size_t(1024)));
        block->next = head_;
        head_ = block;
        total_size_ += block->size;
    }
    
    block->is_free = false;
    used_size_ += size;
    
    // Split block if it's too large
    if (block->size > size + sizeof(Block)) {
        split_block(block, size);
    }
    
    return block->memory;
}

void MemoryPool::deallocate(void* ptr) {
    if (!ptr) return;
    
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    Block* current = head_;
    while (current) {
        if (current->memory == ptr) {
            current->is_free = true;
            used_size_ -= current->size;
            break;
        }
        current = current->next;
    }
    
    // Merge adjacent free blocks
    merge_free_blocks();
}

void MemoryPool::defragment() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    merge_free_blocks();
}

MemoryPool::Block* MemoryPool::find_free_block(size_t size) {
    Block* current = head_;
    while (current) {
        if (current->is_free && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    return nullptr;
}

void MemoryPool::split_block(Block* block, size_t size) {
    if (block->size <= size + sizeof(Block)) return;
    
    Block* new_block = new Block(block->size - size);
    new_block->next = block->next;
    block->next = new_block;
    block->size = size;
    
    // The new block starts as free
    new_block->is_free = true;
}

void MemoryPool::merge_free_blocks() {
    Block* current = head_;
    while (current && current->next) {
        if (current->is_free && current->next->is_free) {
            Block* next = current->next;
            current->size += next->size;
            current->next = next->next;
            delete next;
        } else {
            current = current->next;
        }
    }
}

} // namespace Quanta