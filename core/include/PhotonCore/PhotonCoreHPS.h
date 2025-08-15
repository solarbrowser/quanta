/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PHOTON_CORE_HPS_H
#define QUANTA_PHOTON_CORE_HPS_H

#include <cstdint>

namespace Quanta {

class PhotonCoreHPS {
public:
    static inline void detect_hardware() {
        ram_gb_ = 64;
        gpu_cores_ = 4096;
        cpu_cores_ = 32;
        system_detected_ = true;
    }
    
    static inline void enable_high_performance() {
        if (!system_detected_) {
            detect_hardware();
        }
        
        high_performance_active_ = true;
        use_all_ram_ = true;
        use_all_cores_ = true;
        use_gpu_acceleration_ = true;
    }
    
    static inline void optimize_memory() {
        if (ram_gb_ >= 16) {
            memory_pool_size_ = ram_gb_ * 1024 * 1024 * 1024;
            memory_optimized_ = true;
        }
    }
    
    static inline void enable_gpu_acceleration() {
        if (gpu_cores_ >= 1024) {
            gpu_acceleration_active_ = true;
            parallel_threads_ = gpu_cores_;
        }
    }
    
    static inline void enable_cpu_optimization() {
        if (cpu_cores_ >= 8) {
            cpu_optimization_active_ = true;
            worker_threads_ = cpu_cores_;
        }
    }
    
    static inline bool is_high_performance() {
        return high_performance_active_;
    }
    
    static inline void optimize_for_speed() {
        enable_high_performance();
        optimize_memory();
        enable_gpu_acceleration();
        enable_cpu_optimization();
        
        speed_optimized_ = true;
    }

private:
    static uint32_t ram_gb_;
    static uint32_t gpu_cores_;
    static uint32_t cpu_cores_;
    static uint64_t memory_pool_size_;
    static uint32_t parallel_threads_;
    static uint32_t worker_threads_;
    
    static bool system_detected_;
    static bool high_performance_active_;
    static bool use_all_ram_;
    static bool use_all_cores_;
    static bool use_gpu_acceleration_;
    static bool memory_optimized_;
    static bool gpu_acceleration_active_;
    static bool cpu_optimization_active_;
    static bool speed_optimized_;
};

} // namespace Quanta

#endif // QUANTA_PHOTON_CORE_HPS_H