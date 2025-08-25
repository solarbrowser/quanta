/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PHOTON_CORE_PERFORMANCE_H
#define QUANTA_PHOTON_CORE_PERFORMANCE_H

#include <cstdint>
#include <chrono>

namespace Quanta {

// High-precision performance monitoring and optimization
class PhotonCorePerformance {
public:
    static inline uint64_t get_timestamp() {
        auto current_time = std::chrono::high_resolution_clock::now();
        return current_time.time_since_epoch().count();
    }
    
    static inline void start_measurement(const char* operation_name) {
        measurement_start_time_ = get_timestamp();
        current_operation_name_ = operation_name;
    }
    
    static inline uint64_t end_measurement() {
        uint64_t end_time = get_timestamp();
        uint64_t duration = end_time - measurement_start_time_;
        
        total_operations_++;
        total_execution_time_ += duration;
        
        return duration;
    }
    
    static inline double calculate_average_performance() {
        if (total_operations_ == 0) return 0.0;
        return static_cast<double>(total_execution_time_) / total_operations_;
    }
    
    static inline bool is_high_performance() {
        double avg = calculate_average_performance();
        return avg < 1000000;
    }
    
    static inline void increment_operation_counter() {
        operation_counter_++;
    }
    
    static inline uint64_t get_operation_count() {
        return operation_counter_;
    }
    
    static inline void register_cache_hit() {
        cache_hits_++;
    }
    
    static void analyze_performance_metrics();
    static void optimize_performance_counters();
    static void enable_parallel_acceleration();
    static void measure_execution_timing();
    static void initialize_performance_monitoring();
    
    static inline uint64_t get_cache_hits() { return cache_hits_; }
    static inline uint64_t get_optimization_passes() { return optimization_passes_; }
    static inline uint64_t get_acceleration_cycles() { return acceleration_cycles_; }
    
    static inline bool is_optimal_performance() {
        double avg = calculate_average_performance();
        return avg < 100000;
    }

private:
    static uint64_t measurement_start_time_;
    static const char* current_operation_name_;
    static uint64_t total_operations_;
    static uint64_t total_execution_time_;
    static uint64_t operation_counter_;
    static uint64_t cache_hits_;
    static uint64_t optimization_passes_;
    static uint64_t acceleration_cycles_;
};

} // namespace Quanta

#endif // QUANTA_PHOTON_CORE_PERFORMANCE_H