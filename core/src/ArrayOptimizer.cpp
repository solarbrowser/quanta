#include "ArrayOptimizer.h"
#include <iostream>
#include <chrono>
#include <sstream>

namespace Quanta {

void ArrayOptimizer::initialize_array(const std::string& var_name) {
    if (fast_arrays_.find(var_name) == fast_arrays_.end()) {
        fast_arrays_[var_name] = std::make_unique<OptimizedArray>();
    }
}

bool ArrayOptimizer::has_fast_array(const std::string& var_name) const {
    return fast_arrays_.find(var_name) != fast_arrays_.end();
}

bool ArrayOptimizer::optimized_push(const std::string& var_name, double value) {
    auto it = fast_arrays_.find(var_name);
    if (it != fast_arrays_.end()) {
        auto start = std::chrono::high_resolution_clock::now();
        it->second->push(value);
        auto end = std::chrono::high_resolution_clock::now();
        
        operations_count_++;
        operations_time_ += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        return true;
    }
    return false;
}

double ArrayOptimizer::optimized_pop(const std::string& var_name) {
    auto it = fast_arrays_.find(var_name);
    if (it != fast_arrays_.end()) {
        auto start = std::chrono::high_resolution_clock::now();
        double result = it->second->pop();
        auto end = std::chrono::high_resolution_clock::now();
        
        operations_count_++;
        operations_time_ += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        return result;
    }
    return 0.0;
}

double ArrayOptimizer::optimized_get(const std::string& var_name, size_t index) {
    auto it = fast_arrays_.find(var_name);
    if (it != fast_arrays_.end()) {
        auto start = std::chrono::high_resolution_clock::now();
        double result = it->second->get(index);
        auto end = std::chrono::high_resolution_clock::now();
        
        operations_count_++;
        operations_time_ += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        return result;
    }
    return 0.0;
}

size_t ArrayOptimizer::optimized_length(const std::string& var_name) {
    auto it = fast_arrays_.find(var_name);
    if (it != fast_arrays_.end()) {
        return it->second->length();
    }
    return 0;
}

std::string ArrayOptimizer::to_string_format(const std::string& var_name) {
    auto it = fast_arrays_.find(var_name);
    if (it != fast_arrays_.end()) {
        std::ostringstream oss;
        oss << "ARRAY:[";
        
        size_t length = it->second->length();
        for (size_t i = 0; i < length; ++i) {
            if (i > 0) oss << ",";
            double value = it->second->get(i);
            
            // Format number appropriately
            if (value == static_cast<int64_t>(value)) {
                oss << static_cast<int64_t>(value);
            } else {
                oss << value;
            }
        }
        
        oss << "]";
        return oss.str();
    }
    return "ARRAY:[]";
}

void ArrayOptimizer::optimized_bulk_push(const std::string& var_name, const std::vector<double>& values) {
    auto it = fast_arrays_.find(var_name);
    if (it != fast_arrays_.end()) {
        auto start = std::chrono::high_resolution_clock::now();
        it->second->bulk_push(values.data(), values.size());
        auto end = std::chrono::high_resolution_clock::now();
        
        operations_count_ += values.size();
        operations_time_ += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    }
}

void ArrayOptimizer::optimized_clear(const std::string& var_name) {
    auto it = fast_arrays_.find(var_name);
    if (it != fast_arrays_.end()) {
        it->second->clear();
    }
}

uint64_t ArrayOptimizer::get_operations_per_second() const {
    if (operations_time_ == 0) return 0;
    
    // Convert nanoseconds to seconds and calculate ops/sec
    double seconds = operations_time_.load() / 1000000000.0;
    return static_cast<uint64_t>(operations_count_.load() / seconds);
}

void ArrayOptimizer::reset_performance_metrics() {
    operations_count_ = 0;
    operations_time_ = 0;
}

void ArrayOptimizer::print_performance_report() const {
    uint64_t ops_per_sec = get_operations_per_second();
    
    std::cout << "\nARRAY OPTIMIZER PERFORMANCE REPORT" << std::endl;
    std::cout << "═══════════════════════════════════════════" << std::endl;
    std::cout << "Total Operations:    " << operations_count_.load() << std::endl;
    std::cout << "Total Time:          " << (operations_time_.load() / 1000000.0) << " ms" << std::endl;
    std::cout << "Performance:         " << ops_per_sec << " ops/sec" << std::endl;
    std::cout << "Active Arrays:       " << fast_arrays_.size() << std::endl;
    
    if (ops_per_sec > 100000000) {
        std::cout << "STATUS: 100+ MILLION OPS/SEC ACHIEVED!" << std::endl;
    } else if (ops_per_sec > 10000000) {
        std::cout << "STATUS: HIGH PERFORMANCE ACHIEVED!" << std::endl;
    } else if (ops_per_sec > 1000000) {
        std::cout << "STATUS: 1+ MILLION OPS/SEC ACHIEVED!" << std::endl;
    }
    
    std::cout << "═══════════════════════════════════════════" << std::endl;
}

// Static helper methods for detection
bool ArrayOptimizer::is_array_creation(const std::string& expression) {
    // Detect patterns like "[]", "new Array()", etc.
    return expression.find("[]") != std::string::npos || 
           expression.find("new Array") != std::string::npos;
}

bool ArrayOptimizer::is_array_push_call(const std::string& expression) {
    // Detect patterns like "arr.push("
    return expression.find(".push(") != std::string::npos;
}

bool ArrayOptimizer::is_array_access(const std::string& expression) {
    // Detect patterns like "arr[0]", "arr[index]"
    size_t bracket_pos = expression.find('[');
    return bracket_pos != std::string::npos && 
           expression.find(']', bracket_pos) != std::string::npos;
}

void ArrayOptimizer::initialize_optimizer() {
    OptimizedArray::initialize_pools();
    // Array optimizer initialized
}

void ArrayOptimizer::cleanup_optimizer() {
    OptimizedArray::cleanup_pools();
    std::cout << "� ARRAY OPTIMIZER CLEANED UP" << std::endl;
}

} // namespace Quanta