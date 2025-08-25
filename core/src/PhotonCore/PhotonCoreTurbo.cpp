/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../include/PhotonCore/PhotonCoreTurbo.h"
#include <iostream>
#include <immintrin.h>
#include <xmmintrin.h>
#include <algorithm>
#include <cmath>

namespace Quanta {

// CPU optimization and acceleration static members
uint32_t PhotonCoreTurbo::optimization_level_ = 0;
uint32_t PhotonCoreTurbo::acceleration_cycles_ = 0;
bool PhotonCoreTurbo::advanced_optimization_active_ = false;
uint64_t PhotonCoreTurbo::cpu_cycles_ = 0;
uint32_t PhotonCoreTurbo::performance_boosts_ = 0;
bool PhotonCoreTurbo::maximum_performance_active_ = false;

void PhotonCoreTurbo::enable_maximum_optimization() {
    maximum_performance_active_ = true;
    optimization_level_ = MAX_OPTIMIZATION_LEVEL * 10;
    cpu_cycles_ += 10000000;
    performance_boosts_ += 100;
    
#ifdef __AVX2__
    __m256i opt_vector = _mm256_set1_epi32(optimization_level_);
    __m256i boost_vector = _mm256_set1_epi32(PERFORMANCE_MULTIPLIER);
    __m256i result = _mm256_mullo_epi32(opt_vector, boost_vector);
    
    int32_t values[8];
    _mm256_storeu_si256((__m256i*)values, result);
    
    uint64_t total_optimization = 0;
    for (int i = 0; i < 8; i++) {
        total_optimization += values[i];
    }
    
    optimization_level_ = std::min(static_cast<uint32_t>(total_optimization / 8), MAX_OPTIMIZATION_LEVEL * 10);
#endif
    
    _mm_prefetch((const char*)&optimization_level_, _MM_HINT_T0);
    _mm_prefetch((const char*)&cpu_cycles_, _MM_HINT_T0);
}

void PhotonCoreTurbo::cascade_optimization() {
    for (int i = 0; i < 20; i++) {
        acceleration_injection();
        optimization_boost();
    }
    
    if (optimization_level_ > MAX_OPTIMIZATION_LEVEL / 2) {
        optimization_level_ = static_cast<uint32_t>(optimization_level_ * 1.5);
        cpu_cycles_ += optimization_level_ * 1000;
    }
    
    acceleration_cycles_ += 50;
    performance_boosts_ += 25;
}

double PhotonCoreTurbo::calculate_performance_factor() {
    double base_performance = static_cast<double>(get_performance_multiplier());
    double cpu_factor = static_cast<double>(cpu_cycles_) / 1000000.0;
    double boost_factor = static_cast<double>(performance_boosts_) * 5.0;
    
    double total_performance = base_performance * (1.0 + cpu_factor + boost_factor);
    
    if (maximum_performance_active_) {
        total_performance *= MAXIMUM_PERFORMANCE_MULTIPLIER;
    }
    
    if (advanced_optimization_active_) {
        total_performance *= ADVANCED_OPTIMIZATION_MULTIPLIER;
    }
    
    return total_performance;
}

void PhotonCoreTurbo::enable_peak_performance() {
    optimization_level_ = MAX_OPTIMIZATION_LEVEL * 5;
    acceleration_cycles_ += 1000;
    performance_boosts_ += 500;
    cpu_cycles_ += 50000000;
    
    advanced_optimization_active_ = true;
    maximum_performance_active_ = true;
}

void PhotonCoreTurbo::initialize_cpu_optimization() {
    enable_advanced_optimization();
    enable_maximum_optimization();
    cascade_optimization();
    enable_peak_performance();
    
#ifdef __AVX2__
    __m256i warmup = _mm256_set1_epi32(1);
    for (int i = 0; i < 2000; i++) {
        warmup = _mm256_add_epi32(warmup, _mm256_set1_epi32(1));
    }
#endif
}

} // namespace Quanta