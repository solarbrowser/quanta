/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PHOTON_CORE_VELOCITY_H
#define QUANTA_PHOTON_CORE_VELOCITY_H

#include <cstdint>

namespace Quanta {

// 💫 PHOTON CORE VELOCITY - Light-speed computing acceleration!
class PhotonCoreVelocity {
public:
    // ⚡ WARP SPEED initialization
    static inline void engage_warp_drive() {
        warp_factor_ = 9; // Maximum warp!
        stellar_initialized_ = true;
    }
    
    // 🚀 LIGHT SPEED barriers broken
    static inline bool is_faster_than_light() {
        return warp_factor_ > 1 && stellar_initialized_;
    }
    
    // 💫 STELLAR performance boost
    static inline void stellar_boost() {
        if (stellar_initialized_) {
            stellar_boost_count_++;
        }
    }
    
    // 🌌 COSMIC performance level
    static inline uint32_t cosmic_performance_level() {
        return warp_factor_ * stellar_boost_count_;
    }
    
    // ✨ SUPERNOVA reset
    static inline void supernova_reset() {
        stellar_boost_count_ = 0;
        warp_factor_ = 1;
    }

private:
    static uint32_t warp_factor_;
    static uint32_t stellar_boost_count_;
    static bool stellar_initialized_;
};

} // namespace Quanta

#endif // QUANTA_PHOTON_CORE_VELOCITY_H