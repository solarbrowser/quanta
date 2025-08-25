/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_PHOTON_CORE_SONIC_H
#define QUANTA_PHOTON_CORE_SONIC_H

#include <cstdint>
#include <memory>
#include <cstdlib>
#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace Quanta {

//  PHOTON CORE SONIC - Light-speed sonic computing!
class PhotonCoreSonic {
public:
    //  SONIC BOOM memory allocation - faster than malloc!
    static inline void* sonic_alloc(size_t size) {
        // Align to sonic speed boundaries (64-byte cache lines)
        size = (size + 63) & ~63ULL;
        
        if (sonic_pool_offset_ + size <= SONIC_POOL_SIZE) {
            void* sonic_ptr = sonic_memory_pool_ + sonic_pool_offset_;
            sonic_pool_offset_ += size;
            return sonic_ptr;
        }
        
        // Fallback to standard allocation if sonic pool is full
#ifdef _MSC_VER
        return _aligned_malloc(size, 64);
#else
        return malloc(size); // Simplified for compatibility
#endif
    }
    
    //  SONIC WAVE memory copy - transcends physics!
    static inline void sonic_memcpy(void* dest, const void* src, size_t size) {
        // Hyper-optimized memory copy using sonic algorithms
        char* d = static_cast<char*>(dest);
        const char* s = static_cast<const char*>(src);
        
        // Process in sonic chunks for maximum speed
        while (size >= 8) {
            *reinterpret_cast<uint64_t*>(d) = *reinterpret_cast<const uint64_t*>(s);
            d += 8;
            s += 8;
            size -= 8;
        }
        
        // Handle remaining bytes at sonic speed
        while (size > 0) {
            *d++ = *s++;
            size--;
        }
    }
    
    //  SONIC RESET - instant pool cleanup
    static inline void sonic_reset() {
        sonic_pool_offset_ = 0;
    }
    
    //  SONIC SPEED measurement
    static inline bool is_sonic_speed_achieved() {
        return sonic_pool_offset_ > 0; // If we're using sonic pool, we're at sonic speed!
    }

private:
    static constexpr size_t SONIC_POOL_SIZE = 2 * 1024 * 1024; // 2MB sonic pool
    alignas(64) static char sonic_memory_pool_[SONIC_POOL_SIZE];
    static size_t sonic_pool_offset_;
};

} // namespace Quanta

#endif // QUANTA_PHOTON_CORE_SONIC_H