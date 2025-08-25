/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include "Object.h"
#include "ShapeOptimization.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <cstdint>

namespace Quanta {

//=============================================================================
// Polymorphic Inline Cache System - High-performance Property Access
//=============================================================================

// Forward declarations
class Object;
class Function;
class ASTNode;

//=============================================================================
// Inline Cache Entry - Tracks property access patterns
//=============================================================================

struct ICEntry {
    ShapeID shape_id;                   // Object shape identifier
    PropertyOffset offset;              // Property offset within object
    uint32_t access_count;              // Number of times this entry was hit
    uint64_t last_access_time;          // Last access timestamp for LRU
    bool is_valid;                      // Whether this entry is valid
    
    // Method cache for function properties
    Function* cached_method;            // Cached method for fast dispatch
    bool is_method_cache;               // Whether this is a method cache entry
    
    ICEntry() : shape_id(0), offset(0), access_count(0), last_access_time(0), 
                is_valid(false), cached_method(nullptr), is_method_cache(false) {}
    
    ICEntry(ShapeID id, PropertyOffset off) 
        : shape_id(id), offset(off), access_count(1), 
          last_access_time(std::chrono::steady_clock::now().time_since_epoch().count()),
          is_valid(true), cached_method(nullptr), is_method_cache(false) {}
};

//=============================================================================
// Polymorphic Inline Cache - Handles multiple object shapes
//=============================================================================

class PolymorphicInlineCache {
public:
    // Cache states based on number of shapes seen
    enum class CacheState : uint8_t {
        UNINITIALIZED = 0,  // No accesses yet
        MONOMORPHIC = 1,    // Single shape seen
        POLYMORPHIC = 2,    // 2-4 shapes seen
        MEGAMORPHIC = 3     // 5+ shapes seen (cache becomes less effective)
    };
    
    static constexpr size_t MAX_POLYMORPHIC_ENTRIES = 4;
    static constexpr uint32_t MEGAMORPHIC_THRESHOLD = 5;
    
private:
    std::string property_name_;         // Property being cached
    ICEntry entries_[MAX_POLYMORPHIC_ENTRIES];  // Polymorphic cache entries
    uint32_t entry_count_;              // Number of active entries
    CacheState state_;                  // Current cache state
    
    // Performance statistics
    uint64_t total_lookups_;
    uint64_t cache_hits_;
    uint64_t cache_misses_;
    uint64_t state_transitions_;
    
    // Method for finding LRU entry
    uint32_t find_lru_entry() const;
    
public:
    PolymorphicInlineCache(const std::string& property_name);
    ~PolymorphicInlineCache();
    
    // Cache operations
    bool lookup(ShapeID shape_id, PropertyOffset& offset, Function*& method);
    void update(ShapeID shape_id, PropertyOffset offset, Function* method = nullptr);
    void invalidate();
    void invalidate_shape(ShapeID shape_id);
    
    // Cache state management
    CacheState get_state() const { return state_; }
    bool is_monomorphic() const { return state_ == CacheState::MONOMORPHIC; }
    bool is_polymorphic() const { return state_ == CacheState::POLYMORPHIC; }
    bool is_megamorphic() const { return state_ == CacheState::MEGAMORPHIC; }
    
    // Performance metrics
    double get_hit_ratio() const;
    uint64_t get_total_lookups() const { return total_lookups_; }
    uint64_t get_cache_hits() const { return cache_hits_; }
    uint64_t get_cache_misses() const { return cache_misses_; }
    uint32_t get_entry_count() const { return entry_count_; }
    
    // Debug and profiling
    std::string get_property_name() const { return property_name_; }
    void print_cache_stats() const;
    std::string cache_state_string() const;
};

//=============================================================================
// Inline Cache Manager - Manages multiple property caches
//=============================================================================

class InlineCacheManager {
public:
    struct CacheKey {
        uint32_t call_site_id;          // Unique identifier for call site
        std::string property_name;      // Property being accessed
        
        bool operator==(const CacheKey& other) const {
            return call_site_id == other.call_site_id && property_name == other.property_name;
        }
    };
    
    struct CacheKeyHash {
        size_t operator()(const CacheKey& key) const {
            size_t h1 = std::hash<uint32_t>{}(key.call_site_id);
            size_t h2 = std::hash<std::string>{}(key.property_name);
            return h1 ^ (h2 << 1);
        }
    };
    
private:
    std::unordered_map<CacheKey, std::unique_ptr<PolymorphicInlineCache>, CacheKeyHash> caches_;
    uint32_t next_call_site_id_;
    
    // Global statistics
    struct GlobalStats {
        uint64_t total_caches_created;
        uint64_t monomorphic_caches;
        uint64_t polymorphic_caches;
        uint64_t megamorphic_caches;
        uint64_t cache_invalidations;
        
        GlobalStats() : total_caches_created(0), monomorphic_caches(0), 
                       polymorphic_caches(0), megamorphic_caches(0), cache_invalidations(0) {}
    } global_stats_;
    
public:
    InlineCacheManager();
    ~InlineCacheManager();
    
    // Cache management
    uint32_t allocate_call_site_id() { return next_call_site_id_++; }
    PolymorphicInlineCache* get_cache(uint32_t call_site_id, const std::string& property_name);
    PolymorphicInlineCache* create_cache(uint32_t call_site_id, const std::string& property_name);
    
    // Property access with caching
    Value cached_property_get(Object* obj, const std::string& property, uint32_t call_site_id);
    bool cached_property_set(Object* obj, const std::string& property, const Value& value, uint32_t call_site_id);
    
    // Cache invalidation
    void invalidate_all_caches();
    void invalidate_property_caches(const std::string& property_name);
    void invalidate_shape_caches(ShapeID shape_id);
    
    // Performance analysis
    void analyze_cache_performance();
    void print_global_statistics() const;
    const GlobalStats& get_global_stats() const { return global_stats_; }
    
    // Cache cleanup
    void cleanup_unused_caches();
    size_t get_cache_count() const { return caches_.size(); }
    
    // Global instance
    static InlineCacheManager& get_instance();
};

//=============================================================================
// Call Site Registry - Tracks property access call sites
//=============================================================================

class CallSiteRegistry {
public:
    struct CallSiteInfo {
        uint32_t call_site_id;
        std::string source_location;    // File:line for debugging
        std::string property_name;
        uint64_t access_count;
        std::chrono::steady_clock::time_point first_access;
        std::chrono::steady_clock::time_point last_access;
        
        CallSiteInfo(uint32_t id, const std::string& location, const std::string& prop)
            : call_site_id(id), source_location(location), property_name(prop), 
              access_count(0), first_access(std::chrono::steady_clock::now()),
              last_access(std::chrono::steady_clock::now()) {}
    };
    
private:
    std::unordered_map<uint32_t, CallSiteInfo> call_sites_;
    uint32_t next_id_;
    
public:
    CallSiteRegistry();
    ~CallSiteRegistry();
    
    // Call site management
    uint32_t register_call_site(const std::string& source_location, const std::string& property_name);
    void record_access(uint32_t call_site_id);
    
    // Information retrieval
    const CallSiteInfo* get_call_site_info(uint32_t call_site_id) const;
    std::vector<CallSiteInfo> get_hot_call_sites(uint64_t min_access_count = 100) const;
    
    // Statistics
    void print_call_site_statistics() const;
    size_t get_call_site_count() const { return call_sites_.size(); }
    
    // Global instance
    static CallSiteRegistry& get_instance();
};

//=============================================================================
// Property Access Optimizer - High-level optimization interface
//=============================================================================

class PropertyAccessOptimizer {
public:
    // Optimization strategies
    enum class Strategy : uint8_t {
        NONE = 0,           // No optimization
        INLINE_CACHE = 1,   // Use inline caches
        SHAPE_GUARD = 2,    // Use shape guards
        METHOD_CACHE = 3,   // Cache method lookups
        FULL = 4           // All optimizations
    };
    
private:
    Strategy current_strategy_;
    InlineCacheManager* cache_manager_;
    CallSiteRegistry* call_site_registry_;
    
    // Performance thresholds
    static constexpr double MIN_HIT_RATIO_FOR_CACHING = 0.5;
    static constexpr uint64_t MIN_ACCESSES_FOR_OPTIMIZATION = 10;
    
public:
    PropertyAccessOptimizer();
    ~PropertyAccessOptimizer();
    
    // Optimization control
    void set_strategy(Strategy strategy) { current_strategy_ = strategy; }
    Strategy get_strategy() const { return current_strategy_; }
    
    // Optimized property access
    Value optimized_get_property(Object* obj, const std::string& property, 
                                const std::string& source_location = "");
    bool optimized_set_property(Object* obj, const std::string& property, 
                               const Value& value, const std::string& source_location = "");
    
    // Method call optimization
    Value optimized_method_call(Object* obj, const std::string& method_name,
                               const std::vector<Value>& args, Context& ctx,
                               const std::string& source_location = "");
    
    // Performance analysis
    void analyze_optimization_effectiveness();
    void print_optimization_report() const;
    
    // Integration with JIT system
    bool should_jit_compile_property_access(uint32_t call_site_id) const;
    
    // Global instance
    static PropertyAccessOptimizer& get_instance();
};

//=============================================================================
// Adaptive Inline Cache - Self-tuning cache system
//=============================================================================

class AdaptiveInlineCache {
public:
    struct AdaptiveParameters {
        double hit_ratio_threshold;     // Minimum hit ratio to maintain cache
        uint32_t max_entries;           // Maximum cache entries
        uint64_t invalidation_interval; // How often to check for cleanup
        bool enable_method_caching;     // Whether to cache method lookups
        
        AdaptiveParameters() : hit_ratio_threshold(0.7), max_entries(8), 
                             invalidation_interval(1000), enable_method_caching(true) {}
    };
    
private:
    AdaptiveParameters params_;
    std::vector<std::unique_ptr<PolymorphicInlineCache>> adaptive_caches_;
    uint64_t total_adaptations_;
    
public:
    AdaptiveInlineCache();
    ~AdaptiveInlineCache();
    
    // Adaptive cache management
    void adapt_cache_parameters();
    void monitor_cache_performance();
    
    // Configuration
    void set_parameters(const AdaptiveParameters& params) { params_ = params; }
    const AdaptiveParameters& get_parameters() const { return params_; }
    
    // Statistics
    uint64_t get_adaptation_count() const { return total_adaptations_; }
    void print_adaptive_stats() const;
};

} // namespace Quanta