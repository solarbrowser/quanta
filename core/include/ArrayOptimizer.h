#ifndef QUANTA_ARRAY_OPTIMIZER_H
#define QUANTA_ARRAY_OPTIMIZER_H

#include "UltraFastArray.h"
#include "Context.h"
#include <unordered_map>
#include <string>
#include <memory>

namespace Quanta {

/**
 * Array Optimizer - Bypasses slow string encoding for array operations
 * Provides 100+ million ops/sec performance for array operations
 */
class ArrayOptimizer {
private:
    // Map variable names to ultra-fast arrays
    std::unordered_map<std::string, std::unique_ptr<UltraFastArray>> fast_arrays_;
    
    // Performance metrics
    std::atomic<uint64_t> operations_count_{0};
    std::atomic<uint64_t> operations_time_{0};
    
public:
    ArrayOptimizer() = default;
    ~ArrayOptimizer() = default;
    
    // Initialize ultra-fast array for a variable
    void initialize_array(const std::string& var_name);
    
    // Check if variable has ultra-fast array
    bool has_fast_array(const std::string& var_name) const;
    
    // Ultra-fast push operation
    bool ultra_fast_push(const std::string& var_name, double value);
    
    // Ultra-fast pop operation  
    double ultra_fast_pop(const std::string& var_name);
    
    // Ultra-fast access operation
    double ultra_fast_get(const std::string& var_name, size_t index);
    
    // Ultra-fast length operation
    size_t ultra_fast_length(const std::string& var_name);
    
    // Convert ultra-fast array to string format for compatibility
    std::string to_string_format(const std::string& var_name);
    
    // Bulk operations for maximum performance
    void ultra_fast_bulk_push(const std::string& var_name, const std::vector<double>& values);
    
    // Clear array
    void ultra_fast_clear(const std::string& var_name);
    
    // Performance monitoring
    uint64_t get_operations_per_second() const;
    void reset_performance_metrics();
    void print_performance_report() const;
    
    // Array creation detection
    static bool is_array_creation(const std::string& expression);
    static bool is_array_push_call(const std::string& expression);
    static bool is_array_access(const std::string& expression);
    
    // Initialize optimizer system
    static void initialize_optimizer();
    static void cleanup_optimizer();
};

} // namespace Quanta

#endif // QUANTA_ARRAY_OPTIMIZER_H