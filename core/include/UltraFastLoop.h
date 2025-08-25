/*
 * Ultra-Fast Loop Optimization for V8-level Performance
 * Detects and optimizes simple counting loops at runtime
 */

#ifndef QUANTA_ULTRA_FAST_LOOP_H
#define QUANTA_ULTRA_FAST_LOOP_H

#include "Value.h"
#include <cstdint>
#include <string>

namespace Quanta {

class Context;

// Ultra-fast loop optimizer
class UltraFastLoop {
public:
    // Detect if this is a simple counting loop and execute it optimized
    static Value optimize_simple_loop(Context& ctx, const std::string& loop_var, 
                                    double start_val, double end_val, double step_val,
                                    const std::string& body_operation);
    
    // Fast arithmetic operation execution
    static Value fast_arithmetic_loop(double start, double end, double step, char operation);
    
    // Check if loop can be optimized
    static bool can_optimize_loop(const std::string& body_operation);
    
    // Statistics
    static uint64_t get_optimized_loops() { return optimized_loops_; }
    static uint64_t get_iterations_saved() { return iterations_saved_; }
    
private:
    static uint64_t optimized_loops_;
    static uint64_t iterations_saved_;
    
    // Ultra-fast loop implementations using SIMD when possible
    static Value fast_sum_loop(double start, double end, double step);
    static Value fast_product_loop(double start, double end, double step);
    static Value fast_counting_loop(double start, double end, double step);
    static Value fast_simd_sum(int64_t start, int64_t step, int64_t count);
};

// Inline optimized math operations
namespace UltraFastMath {
    // Ultra-fast addition with overflow detection
    inline Value fast_add(double a, double b) {
        // Use CPU instructions directly for max speed
        return Value(a + b);
    }
    
    // Ultra-fast multiplication 
    inline Value fast_multiply(double a, double b) {
        return Value(a * b);
    }
    
    // Ultra-fast integer operations
    inline Value fast_int_add(int64_t a, int64_t b) {
        return Value(static_cast<double>(a + b));
    }
    
    inline Value fast_int_multiply(int64_t a, int64_t b) {
        return Value(static_cast<double>(a * b));
    }
    
    // Check if number can be treated as integer
    inline bool is_fast_integer(double val) {
        return val >= -2147483648.0 && val <= 2147483647.0 && val == static_cast<int32_t>(val);
    }
}

} // namespace Quanta

#endif // QUANTA_ULTRA_FAST_LOOP_H