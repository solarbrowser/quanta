/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>

namespace Quanta {

/**
 * High-performance memory pool for object allocation
 * Provides fast allocation/deallocation with minimal fragmentation
 */
class MemoryPool {
public:
    struct Block {
        void* data;
        size_t size;
        bool is_free;
        Block* next;
        Block* prev;

        Block(size_t block_size)
            : data(nullptr), size(block_size), is_free(true), next(nullptr), prev(nullptr) {}
    };

private:
    std::vector<std::unique_ptr<char[]>> chunks_;
    Block* free_list_head_;
    Block* allocated_list_head_;

    size_t chunk_size_;
    size_t total_allocated_;
    size_t total_chunks_;

    std::atomic<size_t> allocation_count_{0};
    std::atomic<size_t> deallocation_count_{0};

    mutable std::mutex pool_mutex_;

public:
    explicit MemoryPool(size_t initial_size = 1024 * 1024); // 1MB default
    ~MemoryPool();

    // Memory operations
    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size = 0);

    // Pool management
    void grow_pool(size_t additional_size = 0);
    void compact();
    void clear();

    // Statistics
    size_t get_total_allocated() const;
    size_t get_total_chunks() const;
    size_t get_allocation_count() const;
    size_t get_deallocation_count() const;
    size_t get_free_memory() const;
    size_t get_used_memory() const;

    // Configuration
    void set_chunk_size(size_t size);
    size_t get_chunk_size() const;

    // Introspection
    void print_statistics() const;
    bool validate_integrity() const;

private:
    // Block management
    Block* find_free_block(size_t size);
    Block* split_block(Block* block, size_t size);
    void merge_adjacent_blocks(Block* block);
    void add_to_free_list(Block* block);
    void remove_from_free_list(Block* block);
    void add_to_allocated_list(Block* block);
    void remove_from_allocated_list(Block* block);

    // Memory management
    void allocate_new_chunk(size_t min_size = 0);
    void initialize_chunk(char* chunk_data, size_t chunk_size);

    // Utilities
    bool is_valid_pointer(void* ptr) const;
    Block* find_block_for_pointer(void* ptr) const;
};

} // namespace Quanta