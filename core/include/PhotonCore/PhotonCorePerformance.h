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

// 💫 PHOTON CORE PERFORMANCE - Light-speed performance measurement!
class PhotonCorePerformance {
public:
    // 💎 DIAMOND PRECISION execution timing
    static inline uint64_t diamond_timestamp() {
        // Ultra-precise diamond-grade timing
        auto diamond_time = std::chrono::high_resolution_clock::now();
        return diamond_time.time_since_epoch().count();
    }
    
    // ✨ CRYSTAL CLEAR performance measurement
    static inline void diamond_start_measurement(const char* operation_name) {
        diamond_start_time_ = diamond_timestamp();
        diamond_operation_name_ = operation_name;
    }
    
    // 💎 DIAMOND RESULTS
    static inline uint64_t diamond_end_measurement() {
        uint64_t diamond_end_time = diamond_timestamp();
        uint64_t diamond_duration = diamond_end_time - diamond_start_time_;
        
        // Update diamond statistics
        diamond_total_operations_++;
        diamond_total_time_ += diamond_duration;
        
        return diamond_duration;
    }
    
    // 🌟 DIAMOND GRADE statistics
    static inline double diamond_average_performance() {
        if (diamond_total_operations_ == 0) return 0.0;
        return static_cast<double>(diamond_total_time_) / diamond_total_operations_;
    }
    
    // 💫 DIAMOND CLARITY - check if performance is crystal clear
    static inline bool is_diamond_grade_performance() {
        double avg = diamond_average_performance();
        return avg < 1000000; // Less than 1ms average = diamond grade!
    }
    
    // ⚡ DIAMOND FLASH - instant operation counter
    static inline void diamond_flash_increment() {
        diamond_flash_counter_++;
    }
    
    // 🔥 DIAMOND FIRE - get flash counter
    static inline uint64_t diamond_fire_count() {
        return diamond_flash_counter_;
    }

private:
    static uint64_t diamond_start_time_;
    static const char* diamond_operation_name_;
    static uint64_t diamond_total_operations_;
    static uint64_t diamond_total_time_;
    static uint64_t diamond_flash_counter_;
};

} // namespace Quanta

#endif // QUANTA_PHOTON_CORE_PERFORMANCE_H