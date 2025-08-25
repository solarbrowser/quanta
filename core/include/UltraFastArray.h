#ifndef QUANTA_ULTRA_FAST_ARRAY_H
#define QUANTA_ULTRA_FAST_ARRAY_H

#include <vector>
#include <memory>
#include <atomic>
#include <cstdlib>
#include <cstdint>

namespace Quanta {

/**
 * Ultra-Fast Array Implementation
 * Designed for 100+ million operations per second
 * Direct memory operations, no string encoding overhead
 */
class UltraFastArray {
private:
    // Pre-allocated memory pools for different sizes
    static std::vector<double*> small_pools_;    // 0-1K elements
    static std::vector<double*> medium_pools_;   // 1K-100K elements  
    static std::vector<double*> large_pools_;    // 100K+ elements
    
    // Lock-free pool management
    static std::atomic<size_t> small_pool_index_;
    static std::atomic<size_t> medium_pool_index_;
    static std::atomic<size_t> large_pool_index_;
    
    // Array data
    double* data_;
    size_t length_;
    size_t capacity_;
    bool owns_memory_;
    
    // Pool sizes
    static constexpr size_t SMALL_POOL_SIZE = 10000;   // 10K pre-allocated small arrays
    static constexpr size_t MEDIUM_POOL_SIZE = 1000;   // 1K pre-allocated medium arrays
    static constexpr size_t LARGE_POOL_SIZE = 100;     // 100 pre-allocated large arrays
    
    static constexpr size_t SMALL_CAPACITY = 1024;     // 1K elements per small array
    static constexpr size_t MEDIUM_CAPACITY = 102400;  // 100K elements per medium array
    static constexpr size_t LARGE_CAPACITY = 10240000; // 10M elements per large array

public:
    // Constructor
    UltraFastArray() : data_(nullptr), length_(0), capacity_(0), owns_memory_(false) {
        allocate_from_pool();
    }
    
    // Destructor
    ~UltraFastArray() {
        if (owns_memory_ && data_) {
            return_to_pool();
        }
    }
    
    // Ultra-fast push operation - O(1) amortized
    inline void push(double value) {
        if (length_ >= capacity_) {
            expand_capacity();
        }
        data_[length_++] = value;
    }
    
    // Ultra-fast access - O(1)
    inline double get(size_t index) const {
        return (index < length_) ? data_[index] : 0.0;
    }
    
    // Ultra-fast length - O(1)
    inline size_t length() const { return length_; }
    
    // Ultra-fast clear - O(1)
    inline void clear() { length_ = 0; }
    
    // Pop operation - O(1)
    inline double pop() {
        return (length_ > 0) ? data_[--length_] : 0.0;
    }
    
    // Bulk operations for maximum performance
    void bulk_push(const double* values, size_t count);
    void bulk_copy_from(const UltraFastArray& other);
    
    // Memory pool management
    static void initialize_pools();
    static void cleanup_pools();
    static size_t get_pool_stats();
    
    // Direct memory access for ultimate speed
    inline double* get_data_ptr() { return data_; }
    inline const double* get_data_ptr() const { return data_; }

private:
    void allocate_from_pool();
    void return_to_pool();
    void expand_capacity();
    
    // Pool allocation helpers
    double* get_small_pool_array();
    double* get_medium_pool_array(); 
    double* get_large_pool_array();
};

} // namespace Quanta

#endif // QUANTA_ULTRA_FAST_ARRAY_H