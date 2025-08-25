/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include "HiddenClass.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <chrono>

namespace Quanta {

//=============================================================================
// Advanced Inline Cache - High-performance Implementation
//=============================================================================

enum class ICState : uint8_t {
    UNINITIALIZED = 0,   // İlk durumu
    PREMONOMORPHIC = 1,  // 1 tip görüldü
    MONOMORPHIC = 2,     // Tek tip optimize - ULTRA FAST
    POLYMORPHIC = 3,     // 2-4 tip - hala hızlı
    MEGAMORPHIC = 4      // 4+ tip - dict mode'a geç
};

struct ICEntry {
    HiddenClass* shape;          // Object shape/hidden class
    size_t property_offset;      // Property memory offset
    Value cached_value;          // Cached değer
    uint64_t hit_count;         // Kaç kez kullanıldı
    
    ICEntry() : shape(nullptr), property_offset(0), hit_count(0) {}
    ICEntry(HiddenClass* s, size_t offset) : shape(s), property_offset(offset), hit_count(0) {}
};

class AdvancedInlineCache {
private:
    ICState state_;
    std::vector<ICEntry> entries_;  // Polymorphic entries
    std::string property_name_;
    uint64_t total_hits_;
    uint64_t total_misses_;
    
    // Performance counters
    std::chrono::high_resolution_clock::time_point last_transition_;
    
    // Optimization thresholds
    static const size_t MAX_POLYMORPHIC_ENTRIES = 4;
    static const uint64_t MEGAMORPHIC_THRESHOLD = 8;
    
public:
    AdvancedInlineCache(const std::string& prop_name);
    ~AdvancedInlineCache();
    
    // � ULTRA-FAST PROPERTY ACCESS
    Value get_property_fast(const Value& object, bool& cache_hit);
    bool set_property_fast(Value& object, const Value& value);
    
    // Cache state management
    void transition_to_polymorphic();
    void transition_to_megamorphic();
    bool should_deoptimize() const;
    
    // Shape prediction
    HiddenClass* predict_shape(const Value& object) const;
    bool is_shape_compatible(HiddenClass* shape) const;
    
    // Performance metrics
    double get_hit_rate() const;
    ICState get_state() const { return state_; }
    void print_stats() const;
    
    // � MONOMORPHIC FAST PATH - INLINE ASM READY
    inline Value monomorphic_get(const Value& object) {
        if (entries_.empty()) return Value::undefined();
        
        ICEntry& entry = entries_[0];
        if (object.get_hidden_class() == entry.shape) {
            entry.hit_count++;
            total_hits_++;
            // DIRECT MEMORY ACCESS - BYPASS ALL CHECKS
            return object.get_property_direct(entry.property_offset);
        }
        
        total_misses_++;
        return Value::undefined();
    }
    
    // � POLYMORPHIC OPTIMIZED PATH
    inline Value polymorphic_get(const Value& object) {
        HiddenClass* obj_shape = object.get_hidden_class();
        
        // Linear search through cached entries (max 4)
        for (ICEntry& entry : entries_) {
            if (entry.shape == obj_shape) {
                entry.hit_count++;
                total_hits_++;
                return object.get_property_direct(entry.property_offset);
            }
        }
        
        total_misses_++;
        return Value::undefined();
    }
};

//=============================================================================
// Global IC Manager - Engine Level Optimization
//=============================================================================

class ICManager {
private:
    std::unordered_map<std::string, std::unique_ptr<AdvancedInlineCache>> caches_;
    uint64_t global_hit_count_;
    uint64_t global_miss_count_;
    
public:
    ICManager();
    ~ICManager();
    
    // IC lifecycle
    AdvancedInlineCache* get_or_create_cache(const std::string& property);
    void optimize_all_caches();
    void cleanup_dead_caches();
    
    // Global statistics
    double get_global_hit_rate() const;
    void print_global_stats() const;
    
    // � MEGA OPTIMIZATION: DIRECT PROPERTY ACCESS BYPASS
    static inline Value ultra_fast_property_access(
        const Value& object, 
        const std::string& property,
        AdvancedInlineCache* cache
    ) {
        // Try monomorphic first - fastest path
        if (cache->get_state() == ICState::MONOMORPHIC) {
            return cache->monomorphic_get(object);
        }
        
        // Polymorphic path - still fast
        if (cache->get_state() == ICState::POLYMORPHIC) {
            return cache->polymorphic_get(object);
        }
        
        // Megamorphic - dictionary mode fallback
        return object.get_property(property);
    }
};

} // namespace Quanta