#include "UltraFastArray.h"
#include <iostream>
#include <algorithm>
#include <cstdlib>

// Direct C function declarations
extern "C" {
    void* memset(void* s, int c, size_t n);
    void* memcpy(void* dest, const void* src, size_t n);
}

namespace Quanta {

// Static member initialization
std::vector<double*> UltraFastArray::small_pools_;
std::vector<double*> UltraFastArray::medium_pools_;
std::vector<double*> UltraFastArray::large_pools_;

std::atomic<size_t> UltraFastArray::small_pool_index_{0};
std::atomic<size_t> UltraFastArray::medium_pool_index_{0};
std::atomic<size_t> UltraFastArray::large_pool_index_{0};

void UltraFastArray::initialize_pools() {
    // Initializing ultra-fast array pools
    
    // Pre-allocate small arrays (1K elements each)
    small_pools_.reserve(SMALL_POOL_SIZE);
    for (size_t i = 0; i < SMALL_POOL_SIZE; ++i) {
        double* array = static_cast<double*>(std::malloc(SMALL_CAPACITY * sizeof(double)));
        if (array) {
            memset(array, 0, SMALL_CAPACITY * sizeof(double));
            small_pools_.push_back(array);
        }
    }
    
    // Pre-allocate medium arrays (100K elements each)
    medium_pools_.reserve(MEDIUM_POOL_SIZE);
    for (size_t i = 0; i < MEDIUM_POOL_SIZE; ++i) {
        double* array = static_cast<double*>(std::malloc(MEDIUM_CAPACITY * sizeof(double)));
        if (array) {
            memset(array, 0, MEDIUM_CAPACITY * sizeof(double));
            medium_pools_.push_back(array);
        }
    }
    
    // Pre-allocate large arrays (10M elements each)
    large_pools_.reserve(LARGE_POOL_SIZE);
    for (size_t i = 0; i < LARGE_POOL_SIZE; ++i) {
        double* array = static_cast<double*>(std::malloc(LARGE_CAPACITY * sizeof(double)));
        if (array) {
            memset(array, 0, LARGE_CAPACITY * sizeof(double));
            large_pools_.push_back(array);
        }
    }
    
    // Ultra-fast array pools initialized
}

void UltraFastArray::cleanup_pools() {
    // Free all pre-allocated memory
    for (double* array : small_pools_) {
        if (array) std::free(array);
    }
    for (double* array : medium_pools_) {
        if (array) std::free(array);
    }
    for (double* array : large_pools_) {
        if (array) std::free(array);
    }
    
    small_pools_.clear();
    medium_pools_.clear();
    large_pools_.clear();
}

void UltraFastArray::allocate_from_pool() {
    // Default to small array for maximum speed
    data_ = get_small_pool_array();
    if (data_) {
        capacity_ = SMALL_CAPACITY;
        owns_memory_ = true;
    } else {
        // Fallback allocation
        data_ = static_cast<double*>(std::malloc(SMALL_CAPACITY * sizeof(double)));
        capacity_ = SMALL_CAPACITY;
        owns_memory_ = true;
    }
}

double* UltraFastArray::get_small_pool_array() {
    size_t index = small_pool_index_.fetch_add(1, std::memory_order_relaxed);
    if (index < small_pools_.size()) {
        return small_pools_[index];
    }
    return nullptr;
}

double* UltraFastArray::get_medium_pool_array() {
    size_t index = medium_pool_index_.fetch_add(1, std::memory_order_relaxed);
    if (index < medium_pools_.size()) {
        return medium_pools_[index];
    }
    return nullptr;
}

double* UltraFastArray::get_large_pool_array() {
    size_t index = large_pool_index_.fetch_add(1, std::memory_order_relaxed);
    if (index < large_pools_.size()) {
        return large_pools_[index];
    }
    return nullptr;
}

void UltraFastArray::return_to_pool() {
    // For now, don't return to pool to avoid complexity
    // In production, implement proper pool return mechanism
    if (owns_memory_ && data_) {
        // For pooled arrays, we don't free - they're managed by the pool
        // For malloc'd arrays, free them
        bool is_from_pool = false;
        
        // Check if array is from pools (simplified check)
        for (double* pool_array : small_pools_) {
            if (data_ == pool_array) {
                is_from_pool = true;
                break;
            }
        }
        
        if (!is_from_pool) {
            std::free(data_);
        }
    }
}

void UltraFastArray::expand_capacity() {
    size_t new_capacity = capacity_ * 2;
    
    // Try to get a larger pool array
    double* new_data = nullptr;
    
    if (new_capacity <= SMALL_CAPACITY && capacity_ < SMALL_CAPACITY) {
        new_data = get_small_pool_array();
        new_capacity = SMALL_CAPACITY;
    } else if (new_capacity <= MEDIUM_CAPACITY) {
        new_data = get_medium_pool_array();
        new_capacity = MEDIUM_CAPACITY;
    } else if (new_capacity <= LARGE_CAPACITY) {
        new_data = get_large_pool_array();
        new_capacity = LARGE_CAPACITY;
    }
    
    if (!new_data) {
        // Fallback: regular allocation
        new_data = static_cast<double*>(std::realloc(data_, new_capacity * sizeof(double)));
        if (!new_data) {
            // Allocation failed - use malloc
            new_data = static_cast<double*>(std::malloc(new_capacity * sizeof(double)));
            if (new_data && data_) {
                memcpy(new_data, data_, length_ * sizeof(double));
                if (owns_memory_) std::free(data_);
            }
        }
    } else {
        // Copy existing data to pool array
        if (data_) {
            memcpy(new_data, data_, length_ * sizeof(double));
            if (owns_memory_) return_to_pool();
        }
    }
    
    data_ = new_data;
    capacity_ = new_capacity;
    owns_memory_ = true;
}

void UltraFastArray::bulk_push(const double* values, size_t count) {
    // Ensure capacity
    while (length_ + count > capacity_) {
        expand_capacity();
    }
    
    // Ultra-fast memory copy
    memcpy(data_ + length_, values, count * sizeof(double));
    length_ += count;
}

void UltraFastArray::bulk_copy_from(const UltraFastArray& other) {
    clear();
    bulk_push(other.data_, other.length_);
}

size_t UltraFastArray::get_pool_stats() {
    return small_pools_.size() + medium_pools_.size() + large_pools_.size();
}

} // namespace Quanta