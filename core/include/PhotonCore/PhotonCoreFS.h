/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PHOTON_CORE_FS_H
#define QUANTA_PHOTON_CORE_FS_H

#include <cstdint>

namespace Quanta {

class PhotonCoreFS {
public:
    static inline void enable_fast_mode() {
        fast_mode_active_ = true;
        skip_validation_ = true;
        minimal_init_ = true;
    }
    
    static inline bool quick_start() {
        if (!fast_mode_active_) {
            enable_fast_mode();
        }
        
        startup_count_++;
        return true;
    }
    
    static inline void optimize_boot() {
        boot_optimizations_++;
    }
    
    static inline bool is_fast_ready() {
        return fast_mode_active_ && minimal_init_;
    }
    
    static inline uint32_t get_startup_count() {
        return startup_count_;
    }
    
    static inline void reset_stats() {
        startup_count_ = 0;
        boot_optimizations_ = 0;
    }
    
    static inline void enable_minimal_startup() {
        enable_fast_mode();
        optimize_boot();
    }

private:
    static bool fast_mode_active_;
    static bool skip_validation_;
    static bool minimal_init_;
    static uint32_t startup_count_;
    static uint32_t boot_optimizations_;
};

} // namespace Quanta

#endif // QUANTA_PHOTON_CORE_FS_H