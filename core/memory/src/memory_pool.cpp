/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/memory_pool.h"
#include <iostream>
#include <algorithm>

namespace Quanta {

MemoryPool::MemoryPool(size_t initial_size)
    : head_(nullptr), free_list_head_(nullptr), total_size_(initial_size),
      used_size_(0), allocation_count_(0), deallocation_count_(0),
      min_block_size_(32), max_block_size_(initial_size / 4), auto_defragment_(true) {

    // Create initial block
    head_ = new Block(initial_size);
    free_list_head_ = head_;
}

MemoryPool::~MemoryPool() {
    Block* current = head_;
    while (current) {
        Block* next = current->next;
        delete current;
        current = next;
    }
}

void* MemoryPool::allocate(size_t size) {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    // Find suitable block
    Block* block = find_best_fit_block(size);
    if (!block) {
        // Grow pool if needed
        grow(std::max(size * 2, min_block_size_));
        block = find_best_fit_block(size);
    }

    if (block) {
        void* ptr = allocate_from_block(block, size);
        if (ptr) {
            allocation_count_++;
            used_size_ += size;

            if (auto_defragment_ && should_defragment()) {
                optimize_free_list();
            }
        }
        return ptr;
    }

    return nullptr;
}

void* MemoryPool::allocate_aligned(size_t size, size_t alignment) {
    // Allocate extra space for alignment
    size_t aligned_size = size + alignment - 1;
    void* ptr = allocate(aligned_size);

    if (ptr) {
        // Align the pointer
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t aligned_addr = (addr + alignment - 1) & ~(alignment - 1);
        return reinterpret_cast<void*>(aligned_addr);
    }

    return nullptr;
}

void MemoryPool::deallocate(void* ptr) {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    if (!is_valid_pointer(ptr)) {
        return;
    }

    Block* block = find_block_for_pointer(ptr);
    if (block) {
        block->is_free = true;
        add_to_free_list(block);
        deallocation_count_++;
        used_size_ -= block->size;

        // Try to merge adjacent free blocks
        merge_adjacent_blocks(block);
    }
}

void MemoryPool::deallocate(void* ptr, size_t size) {
    // Size-aware deallocation
    used_size_ -= size;
    deallocate(ptr);
}

void MemoryPool::grow(size_t additional_size) {
    Block* new_block = new Block(additional_size);

    // Add to end of list
    Block* current = head_;
    while (current && current->next) {
        current = current->next;
    }

    if (current) {
        current->next = new_block;
        new_block->prev = current;
    } else {
        head_ = new_block;
    }

    total_size_ += additional_size;
    add_to_free_list(new_block);
}

void MemoryPool::shrink() {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    // Remove unused blocks at the end
    Block* current = head_;
    Block* last_used = nullptr;

    while (current) {
        if (!current->is_free) {
            last_used = current;
        }
        current = current->next;
    }

    if (last_used && last_used->next) {
        // Free blocks after last used block
        current = last_used->next;
        last_used->next = nullptr;

        while (current) {
            Block* next = current->next;
            total_size_ -= current->size;
            remove_from_free_list(current);
            delete current;
            current = next;
        }
    }
}

void MemoryPool::defragment() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    merge_free_blocks();
    optimize_free_list();
}

void MemoryPool::reset() {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    Block* current = head_;
    while (current) {
        current->is_free = true;
        current = current->next;
    }

    used_size_ = 0;
    allocation_count_ = 0;
    deallocation_count_ = 0;

    // Rebuild free list
    free_list_head_ = head_;
    current = head_;
    while (current && current->next) {
        current->next = current->next;
        current = current->next;
    }
}

MemoryPool::Block* MemoryPool::find_free_block(size_t size) {
    Block* current = free_list_head_;

    while (current) {
        if (current->is_free && current->size >= size) {
            return current;
        }
        current = current->next;
    }

    return nullptr;
}

MemoryPool::Block* MemoryPool::find_best_fit_block(size_t size) {
    Block* best_fit = nullptr;
    size_t smallest_suitable = SIZE_MAX;

    Block* current = free_list_head_;
    while (current) {
        if (current->is_free && current->size >= size && current->size < smallest_suitable) {
            best_fit = current;
            smallest_suitable = current->size;

            if (current->size == size) {
                break; // Perfect fit
            }
        }
        current = current->next;
    }

    return best_fit;
}

void* MemoryPool::allocate_from_block(Block* block, size_t size) {
    if (!block || !block->is_free || block->size < size) {
        return nullptr;
    }

    // Split block if it's significantly larger than needed
    if (block->size > size + min_block_size_) {
        split_block(block, size);
    }

    block->is_free = false;
    remove_from_free_list(block);

    return block->memory;
}

void MemoryPool::split_block(Block* block, size_t size) {
    if (block->size <= size + sizeof(Block)) {
        return; // Not worth splitting
    }

    Block* new_block = new Block(block->size - size);
    new_block->memory = static_cast<char*>(block->memory) + size;

    // Insert new block after current
    new_block->next = block->next;
    new_block->prev = block;

    if (block->next) {
        block->next->prev = new_block;
    }
    block->next = new_block;

    block->size = size;
    add_to_free_list(new_block);
}

void MemoryPool::merge_free_blocks() {
    Block* current = head_;

    while (current && current->next) {
        if (current->is_free && current->next->is_free) {
            merge_adjacent_blocks(current);
        }
        current = current->next;
    }
}

void MemoryPool::merge_adjacent_blocks(Block* block) {
    if (!block || !block->is_free) return;

    // Merge with next block if possible
    Block* next = block->next;
    if (next && next->is_free &&
        static_cast<char*>(block->memory) + block->size == next->memory) {

        block->size += next->size;
        block->next = next->next;

        if (next->next) {
            next->next->prev = block;
        }

        remove_from_free_list(next);
        delete next;
    }

    // Merge with previous block if possible
    Block* prev = block->prev;
    if (prev && prev->is_free &&
        static_cast<char*>(prev->memory) + prev->size == block->memory) {

        prev->size += block->size;
        prev->next = block->next;

        if (block->next) {
            block->next->prev = prev;
        }

        remove_from_free_list(block);
        delete block;
    }
}

void MemoryPool::add_to_free_list(Block* block) {
    if (!block) return;

    block->next = free_list_head_;
    if (free_list_head_) {
        free_list_head_->prev = block;
    }
    free_list_head_ = block;
    block->prev = nullptr;
}

void MemoryPool::remove_from_free_list(Block* block) {
    if (!block) return;

    if (block == free_list_head_) {
        free_list_head_ = block->next;
    }

    if (block->prev) {
        block->prev->next = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
}

bool MemoryPool::is_valid_pointer(void* ptr) const {
    Block* current = head_;

    while (current) {
        if (current->memory == ptr) {
            return true;
        }
        current = current->next;
    }

    return false;
}

MemoryPool::Block* MemoryPool::find_block_for_pointer(void* ptr) const {
    Block* current = head_;

    while (current) {
        if (current->memory == ptr) {
            return current;
        }
        current = current->next;
    }

    return nullptr;
}

void MemoryPool::optimize_free_list() {
    // Sort free list by size for better allocation performance
    // This is a simplified implementation
    merge_free_blocks();
}

bool MemoryPool::should_defragment() const {
    return get_fragmentation_ratio() > 0.3; // 30% fragmentation threshold
}

double MemoryPool::get_fragmentation_ratio() const {
    if (total_size_ == 0) return 0.0;

    size_t free_blocks = 0;
    Block* current = free_list_head_;

    while (current) {
        if (current->is_free) {
            free_blocks++;
        }
        current = current->next;
    }

    size_t free_memory = total_size_ - used_size_;
    if (free_memory == 0) return 0.0;

    return static_cast<double>(free_blocks) / (free_memory / min_block_size_);
}

void MemoryPool::set_block_size_limits(size_t min_size, size_t max_size) {
    min_block_size_ = min_size;
    max_block_size_ = max_size;
}

bool MemoryPool::validate_pool() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    size_t calculated_used = 0;
    Block* current = head_;

    while (current) {
        if (!current->is_free) {
            calculated_used += current->size;
        }
        current = current->next;
    }

    return calculated_used == used_size_;
}

void MemoryPool::print_statistics() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    std::cout << "=== Memory Pool Statistics ===" << std::endl;
    std::cout << "Total Size: " << total_size_ << " bytes" << std::endl;
    std::cout << "Used Size: " << used_size_ << " bytes" << std::endl;
    std::cout << "Free Size: " << get_free_size() << " bytes" << std::endl;
    std::cout << "Allocations: " << allocation_count_ << std::endl;
    std::cout << "Deallocations: " << deallocation_count_ << std::endl;
    std::cout << "Fragmentation: " << (get_fragmentation_ratio() * 100) << "%" << std::endl;
}

void MemoryPool::print_block_info() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    std::cout << "=== Memory Pool Blocks ===" << std::endl;
    Block* current = head_;
    int block_num = 0;

    while (current) {
        std::cout << "Block " << block_num++ << ": "
                  << "Size=" << current->size
                  << ", Free=" << (current->is_free ? "Yes" : "No")
                  << ", Address=" << current->memory << std::endl;
        current = current->next;
    }
}

} // namespace Quanta