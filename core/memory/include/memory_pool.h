/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <atomic>
#include <vector>

namespace Quanta {

/**
 * High-performance memory pool for fast object allocation
 * Provides efficient memory management with block-based allocation
 */
class MemoryPool {
private:
    struct Block {
        void* memory;
        size_t size;
        bool is_free;
        Block* next;
        Block* prev;

        Block(size_t s) : memory(nullptr), size(s), is_free(true), next(nullptr), prev(nullptr) {
            memory = malloc(s);
        }

        ~Block() {
            if (memory) {
                free(memory);
            }
        }
    };

    // Pool management
    Block* head_;
    Block* free_list_head_;
    size_t total_size_;
    std::atomic<size_t> used_size_;
    std::atomic<size_t> allocation_count_;
    std::atomic<size_t> deallocation_count_;

    // Configuration
    size_t min_block_size_;
    size_t max_block_size_;
    bool auto_defragment_;

    // Thread safety
    mutable std::mutex pool_mutex_;

public:
    MemoryPool(size_t initial_size = 1024 * 1024); // 1MB default
    ~MemoryPool();

    // Memory operations
    void* allocate(size_t size);
    void* allocate_aligned(size_t size, size_t alignment);
    void deallocate(void* ptr);
    void deallocate(void* ptr, size_t size);

    // Pool management
    void grow(size_t additional_size);
    void shrink();
    void defragment();
    void reset();

    // Statistics
    size_t get_total_size() const { return total_size_; }
    size_t get_used_size() const { return used_size_.load(); }
    size_t get_free_size() const { return total_size_ - used_size_.load(); }
    size_t get_allocation_count() const { return allocation_count_.load(); }
    size_t get_deallocation_count() const { return deallocation_count_.load(); }
    double get_fragmentation_ratio() const;

    // Configuration
    void set_auto_defragment(bool enable) { auto_defragment_ = enable; }
    void set_block_size_limits(size_t min_size, size_t max_size);

    // Debugging and validation
    bool validate_pool() const;
    void print_statistics() const;
    void print_block_info() const;

private:
    // Block management
    Block* find_free_block(size_t size);
    Block* find_best_fit_block(size_t size);
    void split_block(Block* block, size_t size);
    void merge_free_blocks();
    void merge_adjacent_blocks(Block* block);

    // Free list management
    void add_to_free_list(Block* block);
    void remove_from_free_list(Block* block);

    // Memory operations
    void* allocate_from_block(Block* block, size_t size);
    bool is_valid_pointer(void* ptr) const;
    Block* find_block_for_pointer(void* ptr) const;

    // Optimization
    void optimize_free_list();
    bool should_defragment() const;
};

} // namespace Quanta