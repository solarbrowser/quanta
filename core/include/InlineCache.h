/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_INLINE_CACHE_H
#define QUANTA_INLINE_CACHE_H

#include "Value.h"
#include "Object.h"
#include <unordered_map>
#include <string>
#include <memory>
#include <chrono>

namespace Quanta {

/**
 * Inline Cache for property access optimization
 * Caches property lookups to avoid repeated hash table searches
 */
class InlineCache {
public:
    struct CacheEntry {
        std::string property_name;
        Value cached_value;
        Object* cached_object;
        std::chrono::high_resolution_clock::time_point timestamp;
        uint32_t access_count;
        bool is_valid;
        
        CacheEntry() : cached_object(nullptr), access_count(0), is_valid(false) {}
    };
    
    struct PropertyCache {
        std::unordered_map<std::string, CacheEntry> entries;
        uint32_t hit_count;
        uint32_t miss_count;
        
        PropertyCache() : hit_count(0), miss_count(0) {}
    };

private:
    std::unordered_map<void*, PropertyCache> object_caches_;
    uint32_t max_cache_entries_;
    uint32_t total_hits_;
    uint32_t total_misses_;
    
public:
    InlineCache(uint32_t max_entries = 1000) 
        : max_cache_entries_(max_entries), total_hits_(0), total_misses_(0) {}
    
    // Cache operations
    bool try_get_property(Object* obj, const std::string& property, Value& result);
    void cache_property(Object* obj, const std::string& property, const Value& value);
    void invalidate_cache(Object* obj);
    void invalidate_property(Object* obj, const std::string& property);
    void clear_cache();
    
    // Statistics
    double get_hit_ratio() const;
    uint32_t get_total_hits() const { return total_hits_; }
    uint32_t get_total_misses() const { return total_misses_; }
    
    // Cache management
    void cleanup_expired_entries();
    void set_max_entries(uint32_t max_entries) { max_cache_entries_ = max_entries; }
    
    // Debugging
    void print_cache_stats() const;
    void print_cache_contents() const;

private:
    bool is_cache_entry_valid(const CacheEntry& entry, Object* obj) const;
    void evict_oldest_entries(PropertyCache& cache);
};

/**
 * String Interning for memory optimization
 * Ensures identical strings share the same memory
 */
class StringInterning {
public:
    struct InternedString {
        std::string value;
        uint32_t reference_count;
        std::chrono::high_resolution_clock::time_point creation_time;
        
        InternedString(const std::string& str) 
            : value(str), reference_count(1), 
              creation_time(std::chrono::high_resolution_clock::now()) {}
    };

private:
    std::unordered_map<std::string, std::shared_ptr<InternedString>> interned_strings_;
    uint32_t total_strings_;
    uint32_t interned_count_;
    size_t memory_saved_;
    
public:
    StringInterning() : total_strings_(0), interned_count_(0), memory_saved_(0) {}
    
    // String operations
    std::shared_ptr<InternedString> intern_string(const std::string& str);
    bool is_interned(const std::string& str) const;
    void release_string(const std::string& str);
    
    // Memory management
    void cleanup_unused_strings();
    size_t get_memory_saved() const { return memory_saved_; }
    size_t get_total_interned_strings() const { return interned_strings_.size(); }
    
    // Statistics
    double get_interning_ratio() const;
    void print_interning_stats() const;
    
    // Optimization
    std::string optimize_string_concatenation(const std::vector<std::string>& strings);
    
private:
    void update_memory_savings();
};

/**
 * Method Call Cache for function optimization
 */
class MethodCallCache {
public:
    struct MethodEntry {
        Value method;
        Object* receiver;
        std::string method_name;
        uint32_t call_count;
        std::chrono::high_resolution_clock::time_point last_access;
        
        MethodEntry() : receiver(nullptr), call_count(0) {}
    };

private:
    std::unordered_map<std::string, MethodEntry> method_cache_;
    uint32_t cache_hits_;
    uint32_t cache_misses_;
    uint32_t max_cache_size_;
    
public:
    MethodCallCache(uint32_t max_size = 500) 
        : cache_hits_(0), cache_misses_(0), max_cache_size_(max_size) {}
    
    // Method caching
    bool try_get_method(Object* receiver, const std::string& method_name, Value& method);
    void cache_method(Object* receiver, const std::string& method_name, const Value& method);
    void invalidate_method(Object* receiver, const std::string& method_name);
    
    // Performance
    double get_hit_ratio() const;
    void cleanup_old_entries();
    void print_method_cache_stats() const;

private:
    std::string make_cache_key(Object* receiver, const std::string& method_name) const;
};

/**
 *  Performance Cache Manager
 */
class PerformanceCache {
private:
    std::unique_ptr<InlineCache> inline_cache_;
    std::unique_ptr<StringInterning> string_interning_;
    std::unique_ptr<MethodCallCache> method_cache_;
    bool optimization_enabled_;
    
public:
    PerformanceCache(bool enabled = true);
    ~PerformanceCache() = default;
    
    // Component access
    InlineCache* get_inline_cache() const { return inline_cache_.get(); }
    StringInterning* get_string_interning() const { return string_interning_.get(); }
    MethodCallCache* get_method_cache() const { return method_cache_.get(); }
    
    // Global control
    void enable_optimization(bool enabled) { optimization_enabled_ = enabled; }
    bool is_optimization_enabled() const { return optimization_enabled_; }
    
    // Maintenance
    void perform_maintenance();
    void clear_all_caches();
    
    // Statistics
    void print_performance_stats() const;
    double get_overall_performance_gain() const;
    
    // optimized mode
    void enable_maximum_performance_mode();
    void cleanup_optimized_caches();
};

} // namespace Quanta

#endif // QUANTA_INLINE_CACHE_H