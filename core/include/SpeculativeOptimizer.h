/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include <unordered_map>
#include <vector>
#include <chrono>
#include <atomic>

namespace Quanta {

//=============================================================================
// Hot Loop Detection & Speculative Optimization - High-performance
//=============================================================================

enum class OptimizationLevel : uint8_t {
    NONE = 0,
    BASIC = 1,        // Type guards + inline cache
    ADVANCED = 2,     // Native specialization
    AGGRESSIVE = 3    // Full SIMD + parallel
};

struct HotSpot {
    std::string code_signature;     // Hash of loop body
    uint64_t execution_count;       // How many times executed
    uint64_t total_iterations;      // Total loop iterations
    double average_time_us;         // Average execution time
    OptimizationLevel current_level; // Current optimization level
    bool has_deoptimized;          // Has this hotspot deoptimized before
    std::chrono::high_resolution_clock::time_point last_execution;
    
    // Type predictions
    std::unordered_map<std::string, std::string> predicted_types;
    std::vector<std::string> failed_assumptions;
    
    HotSpot() : execution_count(0), total_iterations(0), average_time_us(0.0),
               current_level(OptimizationLevel::NONE), has_deoptimized(false) {}
};

class SpeculativeOptimizer {
private:
    std::unordered_map<std::string, HotSpot> hotspots_;
    std::atomic<uint64_t> total_optimizations_;
    std::atomic<uint64_t> total_deoptimizations_;
    
    // Optimization thresholds
    static const uint64_t HOT_THRESHOLD = 100;        // Executions before optimization
    static const uint64_t MEGA_HOT_THRESHOLD = 1000;  // Executions for aggressive opt
    static const double DEOPT_THRESHOLD = 0.1;        // 10% failure rate triggers deopt
    
public:
    SpeculativeOptimizer();
    ~SpeculativeOptimizer();
    
    // € HOT LOOP DETECTION
    void record_execution(const std::string& code, uint64_t iterations, double time_us);
    bool is_hot_loop(const std::string& code) const;
    OptimizationLevel get_optimization_level(const std::string& code) const;
    
    // ¥ SPECULATIVE OPTIMIZATION
    Value execute_with_speculation(const std::string& code, const std::vector<Value>& context);
    void record_type_assumption(const std::string& code, const std::string& variable, const std::string& type);
    bool verify_type_assumptions(const std::string& code, const std::vector<Value>& context);
    
    // DEOPTIMIZATION
    void deoptimize_hotspot(const std::string& code, const std::string& reason);
    bool should_deoptimize(const std::string& code) const;
    
    // NATIVE SPECIALIZATION
    Value execute_native_specialized(const std::string& code, const std::vector<Value>& context);
    std::string generate_native_code(const std::string& js_code, const HotSpot& hotspot);
    
    // STATISTICS
    void print_hotspot_stats() const;
    double get_optimization_success_rate() const;
    size_t get_active_hotspots() const;
    
    // € ULTRA-FAST MATHEMATICAL SPECIALIZATION
    inline Value ultra_fast_math_loop(int64_t start, int64_t end, const std::string& operation) {
        int64_t iterations = end - start;
        
        // Specialize for common mathematical patterns
        if (operation == "sum") {
            // Gauss formula - O(1) instead of O(n)
            int64_t result = (iterations * (start + end - 1)) / 2;
            return Value(static_cast<double>(result));
        }
        
        if (operation == "multiply") {
            // Product series optimization
            double result = 1.0;
            for (int64_t i = start; i < end && i < start + 1000; i++) {
                result *= i;
            }
            return Value(result);
        }
        
        return Value(0.0);
    }
    
private:
    std::string compute_code_signature(const std::string& code) const;
    bool can_specialize_numerically(const std::string& code) const;
    bool can_use_simd_optimization(const HotSpot& hotspot) const;
    Value execute_simd_optimized(const std::string& code, const std::vector<Value>& context);
};

//=============================================================================
// Deoptimization Engine - Handle Failed Assumptions
//=============================================================================

class DeoptimizationEngine {
public:
    enum class DeoptReason {
        TYPE_GUARD_FAILED,
        BRANCH_MISPREDICTION,
        IC_MISS_RATE_HIGH,
        EXCEPTION_THROWN,
        ASSUMPTION_VIOLATED
    };
    
    struct DeoptInfo {
        DeoptReason reason;
        std::string code_location;
        std::string description;
        uint64_t frequency;
        std::chrono::high_resolution_clock::time_point timestamp;
        
        DeoptInfo(DeoptReason r, const std::string& loc, const std::string& desc) 
            : reason(r), code_location(loc), description(desc), frequency(1),
              timestamp(std::chrono::high_resolution_clock::now()) {}
    };
    
private:
    std::vector<DeoptInfo> deoptimization_log_;
    std::unordered_map<std::string, uint64_t> deopt_frequency_;
    
public:
    // ” DEOPTIMIZATION HANDLING
    void trigger_deoptimization(DeoptReason reason, const std::string& location, const std::string& description);
    bool should_prevent_reoptimization(const std::string& code) const;
    
    // Š DEOPT ANALYSIS
    std::vector<DeoptInfo> get_frequent_deopts() const;
    void print_deopt_summary() const;
    double get_stability_score(const std::string& code) const;
};

} // namespace Quanta