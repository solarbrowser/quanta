/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PHOTON_CORE_TURBO_H
#define QUANTA_PHOTON_CORE_TURBO_H

#include <cstdint>

namespace Quanta {

// 💥 PHOTON CORE TURBO - Ultra-performance engine!
class PhotonCoreTurbo {
public:
    // 🚀 TURBO BOOST - instant speed increase
    static inline void turbo_boost() {
        turbo_level_++;
        if (turbo_level_ > MAX_TURBO_LEVEL) {
            turbo_level_ = MAX_TURBO_LEVEL;
        }
    }
    
    // ⚡ NITRO INJECTION - extreme acceleration
    static inline void nitro_injection() {
        turbo_level_ += 10; // MASSIVE BOOST!
        nitro_injections_++;
    }
    
    // 💫 SPEED MULTIPLIER - current acceleration
    static inline uint32_t speed_multiplier() {
        return turbo_level_ * 100; // 100x multiplier per level
    }
    
    // 🌟 TURBO CHARGED - check if at maximum
    static inline bool is_turbo_charged() {
        return turbo_level_ >= MAX_TURBO_LEVEL;
    }
    
    // 💥 OVERDRIVE - beyond maximum limits
    static inline void engage_overdrive() {
        overdrive_active_ = true;
        turbo_level_ = MAX_TURBO_LEVEL + 50; // Beyond limits!
    }
    
    // 🔥 TURBO RESET - clean performance slate
    static inline void turbo_reset() {
        turbo_level_ = 0;
        nitro_injections_ = 0;
        overdrive_active_ = false;
    }
    
    // ⚡ PERFORMANCE RATING
    static inline const char* performance_rating() {
        if (overdrive_active_) return "🔥 OVERDRIVE";
        if (turbo_level_ >= 50) return "💥 MAXIMUM TURBO";
        if (turbo_level_ >= 25) return "🚀 HIGH TURBO";
        if (turbo_level_ >= 10) return "⚡ TURBO ACTIVE";
        return "💫 NORMAL";
    }

private:
    static constexpr uint32_t MAX_TURBO_LEVEL = 100;
    
    static uint32_t turbo_level_;
    static uint32_t nitro_injections_;
    static bool overdrive_active_;
};

} // namespace Quanta

#endif // QUANTA_PHOTON_CORE_TURBO_H