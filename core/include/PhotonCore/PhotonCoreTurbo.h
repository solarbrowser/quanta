/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PHOTON_CORE_TURBO_H
#define QUANTA_PHOTON_CORE_TURBO_H

#include <cstdint>

namespace Quanta {

// CPU optimization and performance acceleration
class PhotonCoreTurbo {
public:
    static inline void optimization_boost() {
        optimization_level_++;
        if (optimization_level_ > MAX_OPTIMIZATION_LEVEL) {
            optimization_level_ = MAX_OPTIMIZATION_LEVEL;
        }
    }
    
    static inline void acceleration_injection() {
        optimization_level_ += 10;
        acceleration_cycles_++;
    }
    
    static inline uint32_t get_performance_multiplier() {
        return optimization_level_ * 100;
    }
    
    static inline bool is_fully_optimized() {
        return optimization_level_ >= MAX_OPTIMIZATION_LEVEL;
    }
    
    static inline void enable_advanced_optimization() {
        advanced_optimization_active_ = true;
        optimization_level_ = MAX_OPTIMIZATION_LEVEL + 50;
    }
    
    static inline void reset_optimization() {
        optimization_level_ = 0;
        acceleration_cycles_ = 0;
        advanced_optimization_active_ = false;
    }
    
    static inline const char* get_performance_rating() {
        if (maximum_performance_active_) return "HIGH";
        if (advanced_optimization_active_) return "ADVANCED";
        if (optimization_level_ >= 50) return "HIGH";
        if (optimization_level_ >= 25) return "MEDIUM";
        if (optimization_level_ >= 10) return "ACTIVE";
        return "NORMAL";
    }
    
    static void enable_maximum_optimization();
    static void cascade_optimization();
    static double calculate_performance_factor();
    static void enable_peak_performance();
    static void initialize_cpu_optimization();
    
    static inline uint64_t get_cpu_cycles() { return cpu_cycles_; }
    static inline uint32_t get_performance_boosts() { return performance_boosts_; }
    static inline bool is_maximum_performance_active() { return maximum_performance_active_; }
    
    static inline void apply_performance_boost() {
        optimization_level_ += 100;
        performance_boosts_++;
        cpu_cycles_ += 1000000;
    }

private:
    static constexpr uint32_t MAX_OPTIMIZATION_LEVEL = 100;
    static constexpr uint32_t PERFORMANCE_MULTIPLIER = 1000;
    static constexpr double MAXIMUM_PERFORMANCE_MULTIPLIER = 1000.0;
    static constexpr double ADVANCED_OPTIMIZATION_MULTIPLIER = 10.0;
    
    static uint32_t optimization_level_;
    static uint32_t acceleration_cycles_;
    static bool advanced_optimization_active_;
    static uint64_t cpu_cycles_;
    static uint32_t performance_boosts_;
    static bool maximum_performance_active_;
};

} // namespace Quanta

#endif // QUANTA_PHOTON_CORE_TURBO_H