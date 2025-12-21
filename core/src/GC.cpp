/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/GC.h"
#include "quanta/Context.h"
#include <iostream>
#include <algorithm>
#include <string.h>
#include <atomic>

namespace Quanta {


GarbageCollector::GarbageCollector() 
    : collection_mode_(CollectionMode::Automatic),
      young_generation_threshold_(4 * 1024),
      old_generation_threshold_(4 * 1024 * 1024),
      heap_size_limit_(512 * 1024 * 1024),
      gc_trigger_ratio_(0.3),
      gc_running_(false),
      stop_gc_thread_(false),
      collection_cycles_(0),
      ultra_fast_gc_(true),
      parallel_collection_(true),
      zero_copy_optimization_(true),
      heavy_operation_mode_(false),
      emergency_cleanup_threshold_(400 * 1024 * 1024) {
}

GarbageCollector::~GarbageCollector() {
    stop_gc_thread();
    
    for (auto* managed : managed_objects_) {
        delete managed;
    }
    managed_objects_.clear();
}

void GarbageCollector::register_object(Object* obj, size_t size) {
    if (!obj) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    if (size == 0) {
        size = sizeof(Object) + obj->property_count() * sizeof(Value);
    }
    
    auto* managed = new ManagedObject(obj, Generation::Young, size);
    managed_objects_.insert(managed);
    young_generation_.push_back(managed);
    
    stats_.total_allocations++;
    stats_.bytes_allocated += size;
    
    size_t current_heap_size = get_heap_size();
    if (current_heap_size > stats_.peak_memory_usage) {
        stats_.peak_memory_usage = current_heap_size;
    }
    
    if (collection_mode_ == CollectionMode::Automatic && should_trigger_gc()) {
        static auto last_gc_time = std::chrono::high_resolution_clock::now();
        auto now = std::chrono::high_resolution_clock::now();
        auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_gc_time);

        if (ultra_fast_gc_) {
            if (time_since_last.count() > 100) {
                if (young_generation_.size() > 2000) {
                    if (parallel_collection_) {
                        std::thread([this]() { collect_young_generation_parallel(); }).detach();
                    } else {
                        collect_young_generation_ultra_fast();
                    }
                    last_gc_time = now;
                } else if (young_generation_.size() > 5000) {
                    force_ultra_fast_collection();
                    last_gc_time = now;
                }
            }
        } else {
            if (time_since_last.count() > 200) {
                if (young_generation_.size() > 5000) {
                    collect_young_generation();
                    last_gc_time = now;
                } else if (young_generation_.size() > 10000) {
                    collect_garbage();
                    last_gc_time = now;
                }
            }
        }
    }
}

void GarbageCollector::unregister_object(Object* obj) {
    if (!obj) return;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    auto* managed = find_managed_object(obj);
    if (managed) {
        managed_objects_.erase(managed);
        
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
    
    mark_objects();
    
    sweep_objects();
    
    promote_objects();
    
    cleanup_weak_references();
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::collect_young_generation() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    mark_objects();
    
    sweep_generation(young_generation_);
    
    promote_objects();
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::collect_old_generation() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    mark_objects();
    
    sweep_generation(old_generation_);
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::force_full_collection() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    mark_objects();
    sweep_objects();
    
    detect_cycles();
    break_cycles();
    
    gc_running_ = false;
    update_statistics(start);
}

bool GarbageCollector::should_trigger_gc() const {
    size_t current_heap_size = get_heap_size();
    
    if (ultra_fast_gc_) {

        if (current_heap_size > heap_size_limit_ * gc_trigger_ratio_) {
            return true;
        }

        if (young_generation_.size() > 2000) {
            return true;
        }

        if (managed_objects_.size() > 5000) {
            return true;
        }

        if (stats_.total_allocations % 5000 == 0 && stats_.total_allocations > 0) {
            return true;
        }

        if (current_heap_size > young_generation_threshold_ * 2) {
            return true;
        }

    } else {
        if (current_heap_size > heap_size_limit_ * gc_trigger_ratio_) {
            return true;
        }

        if (young_generation_.size() > 5000) {
            return true;
        }

        if (managed_objects_.size() > 10000) {
            return true;
        }

        if (stats_.total_allocations % 10000 == 0 && stats_.total_allocations > 0) {
            return true;
        }
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


void GarbageCollector::mark_objects() {
    for (auto* managed : managed_objects_) {
        managed->is_marked = false;
    }
    
    for (Context* ctx : root_contexts_) {
        mark_from_context(ctx);
    }
    
    for (Object* obj : root_objects_) {
        mark_object(obj);
    }
}

void GarbageCollector::mark_from_context(Context* ctx) {
    if (!ctx) return;
    
    if (ctx->get_global_object()) {
        mark_object(ctx->get_global_object());
    }
    
}

void GarbageCollector::mark_from_object(Object* obj) {
    if (!obj) return;
    
    mark_object(obj);
    
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
        
        static thread_local int recursion_depth = 0;
        if (recursion_depth < 50) {
            recursion_depth++;
            mark_from_object(obj);
            recursion_depth--;
        }
    }
}

void GarbageCollector::sweep_objects() {
    sweep_generation(young_generation_);
    sweep_generation(old_generation_);
}

void GarbageCollector::sweep_generation(std::vector<ManagedObject*>& generation) {
    auto it = generation.begin();
    while (it != generation.end()) {
        auto* managed = *it;
        if (!managed->is_marked) {
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
    auto it = young_generation_.begin();
    while (it != young_generation_.end()) {
        auto* managed = *it;
        if (managed->access_count > 3) {
            managed->generation = Generation::Old;
            old_generation_.push_back(managed);
            it = young_generation_.erase(it);
        } else {
            ++it;
        }
    }
}

void GarbageCollector::age_objects() {
    for (auto* managed : old_generation_) {
        managed->access_count = std::max(0u, managed->access_count - 1);
    }
}

void GarbageCollector::detect_cycles() {
    for (auto* managed : managed_objects_) {
        if (managed->is_marked) {
            std::unordered_set<Object*> visited;
            std::unordered_set<Object*> stack;
        }
    }
}

void GarbageCollector::break_cycles() {
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
        if (ultra_fast_gc_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            if (should_trigger_gc()) {
                if (parallel_collection_) {
                    if (collection_cycles_ % 5 == 0) {
                        std::thread([this]() { collect_old_generation_parallel(); }).detach();
                    } else {
                        std::thread([this]() { collect_young_generation_parallel(); }).detach();
                    }
                } else {
                    if (collection_cycles_ % 8 == 0) {
                        collect_old_generation_ultra_fast();
                    } else {
                        collect_young_generation_ultra_fast();
                    }
                }
                collection_cycles_++;
            }
            
            if (get_heap_size() > heap_size_limit_ * 0.7) {
                force_ultra_fast_collection();
            }
            
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            
            if (should_trigger_gc()) {
                if (collection_cycles_ % 10 == 0) {
                    collect_old_generation();
                } else {
                    collect_young_generation();
                }
                collection_cycles_++;
            }
            
            if (get_heap_size() > heap_size_limit_ * 0.9) {
                force_full_collection();
            }
        }
    }
}


MemoryPool::MemoryPool(size_t initial_size) 
    : head_(nullptr), total_size_(0), used_size_(0) {
    
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
        block = new Block(std::max(size, size_t(1024)));
        block->next = head_;
        head_ = block;
        total_size_ += block->size;
    }
    
    block->is_free = false;
    used_size_ += size;
    
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


void GarbageCollector::collect_young_generation_ultra_fast() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    mark_objects_ultra_fast();
    
    sweep_generation_ultra_fast(young_generation_);
    
    promote_objects_ultra_fast();
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::collect_old_generation_ultra_fast() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    mark_objects_ultra_fast();
    
    sweep_generation_ultra_fast(old_generation_);
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::force_ultra_fast_collection() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    mark_objects_ultra_fast();
    sweep_objects_ultra_fast();
    
    detect_cycles_ultra_fast();
    break_cycles_ultra_fast();
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::collect_young_generation_parallel() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::unique_lock<std::mutex> lock(gc_mutex_);
    
    std::vector<std::thread> threads;
    const size_t thread_count = std::min(4u, std::thread::hardware_concurrency());
    
    std::atomic<bool> marking_complete{false};
    for (size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back([this]() {
            mark_objects_parallel_worker();
        });
    }
    
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
    threads.clear();
    
    for (size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back([this, thread_count, i]() {
            sweep_generation_parallel_worker(young_generation_, i, thread_count);
        });
    }
    
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
    
    promote_objects_ultra_fast();
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::collect_old_generation_parallel() {
    if (gc_running_) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    gc_running_ = true;
    
    std::unique_lock<std::mutex> lock(gc_mutex_);
    
    std::vector<std::thread> threads;
    const size_t thread_count = std::min(4u, std::thread::hardware_concurrency());
    
    for (size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back([this]() {
            mark_objects_parallel_worker();
        });
    }
    
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
    threads.clear();
    
    for (size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back([this, thread_count, i]() {
            sweep_generation_parallel_worker(old_generation_, i, thread_count);
        });
    }
    
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
    
    gc_running_ = false;
    update_statistics(start);
}

void GarbageCollector::mark_objects_ultra_fast() {
    for (auto* managed : managed_objects_) {
        managed->is_marked = false;
    }

    for (Object* obj : root_objects_) {
        mark_object_ultra_fast(obj);
    }
    
    for (Context* ctx : root_contexts_) {
        if (ctx->get_global_object()) {
            mark_object_ultra_fast(ctx->get_global_object());
        }
    }
}

void GarbageCollector::mark_object_ultra_fast(Object* obj) {
    if (!obj) return;
    
    auto* managed = find_managed_object_ultra_fast(obj);
    if (managed && !managed->is_marked) {
        managed->is_marked = true;
        managed->access_count += 2;
        
        std::vector<std::string> keys = obj->get_enumerable_keys();
        for (const std::string& key : keys) {
            Value prop = obj->get_property(key);
            if (prop.is_object()) {
                mark_object_ultra_fast(prop.as_object());
            }
        }
    }
}

void GarbageCollector::sweep_generation_ultra_fast(std::vector<ManagedObject*>& generation) {
    auto it = generation.begin();
    while (it != generation.end()) {
        auto* managed = *it;
        if (!managed->is_marked) {
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

void GarbageCollector::sweep_objects_ultra_fast() {
    sweep_generation_ultra_fast(young_generation_);
    sweep_generation_ultra_fast(old_generation_);
}

void GarbageCollector::promote_objects_ultra_fast() {
    auto it = young_generation_.begin();
    while (it != young_generation_.end()) {
        auto* managed = *it;
        if (managed->access_count > 2) {
            managed->generation = Generation::Old;
            old_generation_.push_back(managed);
            it = young_generation_.erase(it);
        } else {
            ++it;
        }
    }
}

void GarbageCollector::detect_cycles_ultra_fast() {
}

void GarbageCollector::break_cycles_ultra_fast() {
}

GarbageCollector::ManagedObject* GarbageCollector::find_managed_object_ultra_fast(Object* obj) {
    for (auto* managed : managed_objects_) {
        if (managed->object == obj) {
            return managed;
        }
    }
    return nullptr;
}

void GarbageCollector::mark_objects_parallel_worker() {
    mark_objects_ultra_fast();
}

void GarbageCollector::sweep_generation_parallel_worker(std::vector<ManagedObject*>& generation, 
                                                       size_t thread_id, size_t thread_count) {
    size_t start_idx = (generation.size() * thread_id) / thread_count;
    size_t end_idx = (generation.size() * (thread_id + 1)) / thread_count;
    
    for (size_t i = start_idx; i < end_idx && i < generation.size(); ++i) {
        auto* managed = generation[i];
        if (!managed->is_marked) {
            managed->is_marked = false;
        }
    }
}


void GarbageCollector::enable_heavy_operation_mode() {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    heavy_operation_mode_ = true;
    
    heap_size_limit_ = 1024 * 1024 * 1024;
    gc_trigger_ratio_ = 0.8;
    
    young_generation_.reserve(100000);
    old_generation_.reserve(500000);
    
}

void GarbageCollector::disable_heavy_operation_mode() {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    force_ultra_fast_collection();
    
    heavy_operation_mode_ = false;
    
    heap_size_limit_ = 512 * 1024 * 1024;
    gc_trigger_ratio_ = 0.3;
    
}

void GarbageCollector::emergency_cleanup() {
    
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    force_ultra_fast_collection();
    
    size_t objects_freed = 0;
    
    auto it = young_generation_.begin();
    while (it != young_generation_.end()) {
        auto* managed = *it;
        if (managed->access_count < 2) {
            managed_objects_.erase(managed);
            delete managed->object;
            delete managed;
            it = young_generation_.erase(it);
            objects_freed++;
        } else {
            ++it;
        }
    }
    
    stats_.total_deallocations += objects_freed;
}

void GarbageCollector::prepare_for_heavy_load(size_t expected_objects) {
    std::lock_guard<std::mutex> lock(gc_mutex_);
    
    enable_heavy_operation_mode();
    
    young_generation_.reserve(expected_objects);
    if (expected_objects > 100000) {
        old_generation_.reserve(expected_objects / 2);
    }
    
    if (expected_objects > 1000000) {
        heap_size_limit_ = 2048UL * 1024 * 1024;
        emergency_cleanup_threshold_ = 1800UL * 1024 * 1024;
    }
    
}

}
