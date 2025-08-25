/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/PolymorphicInlineCache.h"
#include "../include/PhotonCore/PhotonCoreQuantum.h"
#include "../include/PhotonCore/PhotonCoreSonic.h"
#include "../include/PhotonCore/PhotonCorePerformance.h"
#include "../include/PhotonCore/PhotonCoreAcceleration.h"
#include <iostream>
#include <algorithm>
#include <sstream>

namespace Quanta {

//=============================================================================
// PolymorphicInlineCache Implementation - PHASE 2: V8-Level Property Access
//=============================================================================

PolymorphicInlineCache::PolymorphicInlineCache(const std::string& property_name)
    : property_name_(property_name), entry_count_(0), state_(CacheState::UNINITIALIZED),
      total_lookups_(0), cache_hits_(0), cache_misses_(0), state_transitions_(0) {
    
    // Initialize all entries
    for (size_t i = 0; i < MAX_POLYMORPHIC_ENTRIES; ++i) {
        entries_[i] = ICEntry();
    }
}

PolymorphicInlineCache::~PolymorphicInlineCache() {
    // Cleanup
}

uint32_t PolymorphicInlineCache::find_lru_entry() const {
    uint32_t lru_index = 0;
    uint64_t oldest_time = entries_[0].last_access_time;
    
    for (uint32_t i = 1; i < entry_count_; ++i) {
        if (entries_[i].last_access_time < oldest_time) {
            oldest_time = entries_[i].last_access_time;
            lru_index = i;
        }
    }
    
    return lru_index;
}

bool PolymorphicInlineCache::lookup(ShapeID shape_id, PropertyOffset& offset, Function*& method) {
    total_lookups_++;
    
    // Search through cache entries
    for (uint32_t i = 0; i < entry_count_; ++i) {
        ICEntry& entry = entries_[i];
        if (entry.is_valid && entry.shape_id == shape_id) {
            // Cache hit!
            cache_hits_++;
            entry.access_count++;
            entry.last_access_time = std::chrono::steady_clock::now().time_since_epoch().count();
            
            offset = entry.offset;
            method = entry.cached_method;
            
            return true;
        }
    }
    
    // Cache miss
    cache_misses_++;
    return false;
}

void PolymorphicInlineCache::update(ShapeID shape_id, PropertyOffset offset, Function* method) {
    // Check if entry already exists
    for (uint32_t i = 0; i < entry_count_; ++i) {
        if (entries_[i].shape_id == shape_id) {
            // Update existing entry
            entries_[i].offset = offset;
            entries_[i].cached_method = method;
            entries_[i].is_method_cache = (method != nullptr);
            entries_[i].access_count++;
            entries_[i].last_access_time = std::chrono::steady_clock::now().time_since_epoch().count();
            return;
        }
    }
    
    // New entry needed
    uint32_t insert_index;
    
    if (entry_count_ < MAX_POLYMORPHIC_ENTRIES) {
        // Add new entry
        insert_index = entry_count_;
        entry_count_++;
    } else {
        // Replace LRU entry
        insert_index = find_lru_entry();
    }
    
    // Update entry
    entries_[insert_index] = ICEntry(shape_id, offset);
    entries_[insert_index].cached_method = method;
    entries_[insert_index].is_method_cache = (method != nullptr);
    
    // Update cache state
    CacheState old_state = state_;
    
    if (entry_count_ == 1) {
        state_ = CacheState::MONOMORPHIC;
    } else if (entry_count_ <= MAX_POLYMORPHIC_ENTRIES) {
        state_ = CacheState::POLYMORPHIC;
    } else {
        state_ = CacheState::MEGAMORPHIC;
    }
    
    if (old_state != state_) {
        state_transitions_++;
        std::cout << "🔄 CACHE STATE TRANSITION: " << property_name_ 
                 << " -> " << cache_state_string() << std::endl;
    }
}

void PolymorphicInlineCache::invalidate() {
    for (uint32_t i = 0; i < MAX_POLYMORPHIC_ENTRIES; ++i) {
        entries_[i] = ICEntry();
    }
    entry_count_ = 0;
    state_ = CacheState::UNINITIALIZED;
    state_transitions_++;
}

void PolymorphicInlineCache::invalidate_shape(ShapeID shape_id) {
    bool found = false;
    
    for (uint32_t i = 0; i < entry_count_; ++i) {
        if (entries_[i].shape_id == shape_id) {
            // Shift remaining entries
            for (uint32_t j = i; j < entry_count_ - 1; ++j) {
                entries_[j] = entries_[j + 1];
            }
            entries_[entry_count_ - 1] = ICEntry();
            entry_count_--;
            found = true;
            break;
        }
    }
    
    if (found) {
        // Update cache state
        if (entry_count_ == 0) {
            state_ = CacheState::UNINITIALIZED;
        } else if (entry_count_ == 1) {
            state_ = CacheState::MONOMORPHIC;
        } else {
            state_ = CacheState::POLYMORPHIC;
        }
        state_transitions_++;
    }
}

double PolymorphicInlineCache::get_hit_ratio() const {
    return (total_lookups_ > 0) ? static_cast<double>(cache_hits_) / total_lookups_ : 0.0;
}

void PolymorphicInlineCache::print_cache_stats() const {
    std::cout << "📊 Polymorphic IC Stats for '" << property_name_ << "':" << std::endl;
    std::cout << "  State: " << cache_state_string() << std::endl;
    std::cout << "  Entries: " << entry_count_ << "/" << MAX_POLYMORPHIC_ENTRIES << std::endl;
    std::cout << "  Total Lookups: " << total_lookups_ << std::endl;
    std::cout << "  Cache Hits: " << cache_hits_ << std::endl;
    std::cout << "  Cache Misses: " << cache_misses_ << std::endl;
    std::cout << "  Hit Ratio: " << (get_hit_ratio() * 100.0) << "%" << std::endl;
    std::cout << "  State Transitions: " << state_transitions_ << std::endl;
}

std::string PolymorphicInlineCache::cache_state_string() const {
    switch (state_) {
        case CacheState::UNINITIALIZED: return "UNINITIALIZED";
        case CacheState::MONOMORPHIC: return "MONOMORPHIC";
        case CacheState::POLYMORPHIC: return "POLYMORPHIC";
        case CacheState::MEGAMORPHIC: return "MEGAMORPHIC";
        default: return "UNKNOWN";
    }
}

//=============================================================================
// InlineCacheManager Implementation - Manages multiple property caches
//=============================================================================

InlineCacheManager::InlineCacheManager() : next_call_site_id_(1) {
    std::cout << "🚀 POLYMORPHIC INLINE CACHE MANAGER INITIALIZED" << std::endl;
}

InlineCacheManager::~InlineCacheManager() {
    // Cleanup
}

PolymorphicInlineCache* InlineCacheManager::get_cache(uint32_t call_site_id, const std::string& property_name) {
    CacheKey key = {call_site_id, property_name};
    auto it = caches_.find(key);
    
    if (it != caches_.end()) {
        return it->second.get();
    }
    
    return nullptr;
}

PolymorphicInlineCache* InlineCacheManager::create_cache(uint32_t call_site_id, const std::string& property_name) {
    CacheKey key = {call_site_id, property_name};
    
    auto cache = std::make_unique<PolymorphicInlineCache>(property_name);
    PolymorphicInlineCache* cache_ptr = cache.get();
    
    caches_[key] = std::move(cache);
    global_stats_.total_caches_created++;
    
    std::cout << "🆕 CREATED POLYMORPHIC IC: " << property_name 
             << " (CallSite: " << call_site_id << ")" << std::endl;
    
    return cache_ptr;
}

Value InlineCacheManager::cached_property_get(Object* obj, const std::string& property, uint32_t call_site_id) {
    if (!obj) return Value();
    
    // Get or create cache
    PolymorphicInlineCache* cache = get_cache(call_site_id, property);
    if (!cache) {
        cache = create_cache(call_site_id, property);
    }
    
    // Try to get shape-optimized object
    auto* shape_obj = dynamic_cast<ShapeOptimizedObject*>(obj);
    if (shape_obj) {
        ShapeID shape_id = shape_obj->get_shape()->get_id();
        PropertyOffset offset;
        Function* method;
        
        // Try cache lookup
        if (cache->lookup(shape_id, offset, method)) {
            // Cache hit! Use fast property access
            Value result = shape_obj->get_fast_property(offset);
            
            std::cout << " POLYMORPHIC IC HIT: " << property 
                     << " (State: " << cache->cache_state_string() << ")" << std::endl;
            
            return result;
        } else {
            // Cache miss - get property normally and update cache
            Value result = shape_obj->get_property(property);
            
            // Update cache with new shape information
            PropertyOffset offset = shape_obj->get_shape()->get_property_offset(property);
            if (offset != static_cast<PropertyOffset>(-1)) {
                Function* cached_method = result.is_function() ? result.as_function() : nullptr;
                cache->update(shape_id, offset, cached_method);
                
                std::cout << "📝 POLYMORPHIC IC UPDATE: " << property 
                         << " (State: " << cache->cache_state_string() << ")" << std::endl;
            }
            
            return result;
        }
    }
    
    // Fallback to normal property access
    return obj->get_property(property);
}

bool InlineCacheManager::cached_property_set(Object* obj, const std::string& property, const Value& value, uint32_t call_site_id) {
    if (!obj) return false;
    
    // Get or create cache
    PolymorphicInlineCache* cache = get_cache(call_site_id, property);
    if (!cache) {
        cache = create_cache(call_site_id, property);
    }
    
    // Try to get shape-optimized object
    auto* shape_obj = dynamic_cast<ShapeOptimizedObject*>(obj);
    if (shape_obj) {
        bool result = shape_obj->set_property(property, value);
        
        // Update cache after property set (might cause shape transition)
        ShapeID new_shape_id = shape_obj->get_shape()->get_id();
        PropertyOffset offset = shape_obj->get_shape()->get_property_offset(property);
        
        if (offset != static_cast<PropertyOffset>(-1)) {
            Function* cached_method = value.is_function() ? value.as_function() : nullptr;
            cache->update(new_shape_id, offset, cached_method);
        }
        
        return result;
    }
    
    // Fallback to normal property set
    return obj->set_property(property, value);
}

void InlineCacheManager::invalidate_all_caches() {
    for (auto& pair : caches_) {
        pair.second->invalidate();
    }
    global_stats_.cache_invalidations++;
}

void InlineCacheManager::invalidate_property_caches(const std::string& property_name) {
    for (auto& pair : caches_) {
        if (pair.first.property_name == property_name) {
            pair.second->invalidate();
        }
    }
    global_stats_.cache_invalidations++;
}

void InlineCacheManager::invalidate_shape_caches(ShapeID shape_id) {
    for (auto& pair : caches_) {
        pair.second->invalidate_shape(shape_id);
    }
    global_stats_.cache_invalidations++;
}

void InlineCacheManager::analyze_cache_performance() {
    global_stats_.monomorphic_caches = 0;
    global_stats_.polymorphic_caches = 0;
    global_stats_.megamorphic_caches = 0;
    
    for (const auto& pair : caches_) {
        const PolymorphicInlineCache* cache = pair.second.get();
        
        switch (cache->get_state()) {
            case PolymorphicInlineCache::CacheState::MONOMORPHIC:
                global_stats_.monomorphic_caches++;
                break;
            case PolymorphicInlineCache::CacheState::POLYMORPHIC:
                global_stats_.polymorphic_caches++;
                break;
            case PolymorphicInlineCache::CacheState::MEGAMORPHIC:
                global_stats_.megamorphic_caches++;
                break;
            default:
                break;
        }
    }
}

void InlineCacheManager::print_global_statistics() const {
    std::cout << "📊 POLYMORPHIC IC GLOBAL STATISTICS:" << std::endl;
    std::cout << "  Total Caches Created: " << global_stats_.total_caches_created << std::endl;
    std::cout << "  Monomorphic Caches: " << global_stats_.monomorphic_caches << std::endl;
    std::cout << "  Polymorphic Caches: " << global_stats_.polymorphic_caches << std::endl;
    std::cout << "  Megamorphic Caches: " << global_stats_.megamorphic_caches << std::endl;
    std::cout << "  Cache Invalidations: " << global_stats_.cache_invalidations << std::endl;
    std::cout << "  Active Caches: " << caches_.size() << std::endl;
}

void InlineCacheManager::cleanup_unused_caches() {
    // Remove caches with low hit ratios or no recent activity
    auto it = caches_.begin();
    while (it != caches_.end()) {
        const PolymorphicInlineCache* cache = it->second.get();
        
        if (cache->get_total_lookups() > 100 && cache->get_hit_ratio() < 0.1) {
            std::cout << "🧹 CLEANUP: Removing ineffective cache for " << cache->get_property_name() << std::endl;
            it = caches_.erase(it);
        } else {
            ++it;
        }
    }
}

InlineCacheManager& InlineCacheManager::get_instance() {
    static InlineCacheManager instance;
    return instance;
}

//=============================================================================
// CallSiteRegistry Implementation - Tracks property access call sites
//=============================================================================

CallSiteRegistry::CallSiteRegistry() : next_id_(1) {
    // Initialize
}

CallSiteRegistry::~CallSiteRegistry() {
    // Cleanup
}

uint32_t CallSiteRegistry::register_call_site(const std::string& source_location, const std::string& property_name) {
    uint32_t id = next_id_++;
    call_sites_.emplace(id, CallSiteInfo(id, source_location, property_name));
    return id;
}

void CallSiteRegistry::record_access(uint32_t call_site_id) {
    auto it = call_sites_.find(call_site_id);
    if (it != call_sites_.end()) {
        it->second.access_count++;
        it->second.last_access = std::chrono::steady_clock::now();
    }
}

const CallSiteRegistry::CallSiteInfo* CallSiteRegistry::get_call_site_info(uint32_t call_site_id) const {
    auto it = call_sites_.find(call_site_id);
    return (it != call_sites_.end()) ? &it->second : nullptr;
}

std::vector<CallSiteRegistry::CallSiteInfo> CallSiteRegistry::get_hot_call_sites(uint64_t min_access_count) const {
    std::vector<CallSiteInfo> hot_sites;
    
    for (const auto& pair : call_sites_) {
        if (pair.second.access_count >= min_access_count) {
            hot_sites.push_back(pair.second);
        }
    }
    
    // Sort by access count (descending)
    std::sort(hot_sites.begin(), hot_sites.end(),
        [](const CallSiteInfo& a, const CallSiteInfo& b) {
            return a.access_count > b.access_count;
        });
    
    return hot_sites;
}

void CallSiteRegistry::print_call_site_statistics() const {
    std::cout << "📍 CALL SITE REGISTRY STATISTICS:" << std::endl;
    std::cout << "  Total Call Sites: " << call_sites_.size() << std::endl;
    
    auto hot_sites = get_hot_call_sites(10);
    std::cout << "  Hot Call Sites (10+ accesses): " << hot_sites.size() << std::endl;
    
    for (size_t i = 0; i < std::min(hot_sites.size(), size_t(5)); ++i) {
        const auto& site = hot_sites[i];
        std::cout << "    " << site.property_name << " (" << site.access_count << " accesses)" << std::endl;
    }
}

CallSiteRegistry& CallSiteRegistry::get_instance() {
    static CallSiteRegistry instance;
    return instance;
}

//=============================================================================
// PropertyAccessOptimizer Implementation - High-level optimization interface
//=============================================================================

PropertyAccessOptimizer::PropertyAccessOptimizer() 
    : current_strategy_(Strategy::FULL),
      cache_manager_(&InlineCacheManager::get_instance()),
      call_site_registry_(&CallSiteRegistry::get_instance()) {
    
    std::cout << "🚀 PROPERTY ACCESS OPTIMIZER INITIALIZED" << std::endl;
}

PropertyAccessOptimizer::~PropertyAccessOptimizer() {
    // Cleanup
}

Value PropertyAccessOptimizer::optimized_get_property(Object* obj, const std::string& property, 
                                                    const std::string& source_location) {
    if (!obj || current_strategy_ == Strategy::NONE) {
        return obj ? obj->get_property(property) : Value();
    }
    
    // Register call site
    uint32_t call_site_id = call_site_registry_->register_call_site(source_location, property);
    call_site_registry_->record_access(call_site_id);
    
    // Use polymorphic inline cache
    if (current_strategy_ >= Strategy::INLINE_CACHE) {
        return cache_manager_->cached_property_get(obj, property, call_site_id);
    }
    
    // Fallback to normal access
    return obj->get_property(property);
}

bool PropertyAccessOptimizer::optimized_set_property(Object* obj, const std::string& property, 
                                                    const Value& value, const std::string& source_location) {
    if (!obj || current_strategy_ == Strategy::NONE) {
        return obj ? obj->set_property(property, value) : false;
    }
    
    // Register call site
    uint32_t call_site_id = call_site_registry_->register_call_site(source_location, property);
    call_site_registry_->record_access(call_site_id);
    
    // Use polymorphic inline cache
    if (current_strategy_ >= Strategy::INLINE_CACHE) {
        return cache_manager_->cached_property_set(obj, property, value, call_site_id);
    }
    
    // Fallback to normal access
    return obj->set_property(property, value);
}

Value PropertyAccessOptimizer::optimized_method_call(Object* obj, const std::string& method_name,
                                                   const std::vector<Value>& args, Context& ctx,
                                                   const std::string& source_location) {
    if (!obj) return Value();
    
    // Get method with optimization
    Value method_value = optimized_get_property(obj, method_name, source_location);
    
    if (method_value.is_function()) {
        Function* method = method_value.as_function();
        return method->call(ctx, args, Value(obj));
    }
    
    return Value();
}

void PropertyAccessOptimizer::analyze_optimization_effectiveness() {
    std::cout << "🔍 ANALYZING PROPERTY ACCESS OPTIMIZATION:" << std::endl;
    
    cache_manager_->analyze_cache_performance();
    cache_manager_->print_global_statistics();
    
    call_site_registry_->print_call_site_statistics();
}

void PropertyAccessOptimizer::print_optimization_report() const {
    std::cout << "📋 PROPERTY ACCESS OPTIMIZATION REPORT:" << std::endl;
    std::cout << "  Current Strategy: ";
    switch (current_strategy_) {
        case Strategy::NONE: std::cout << "NONE"; break;
        case Strategy::INLINE_CACHE: std::cout << "INLINE_CACHE"; break;
        case Strategy::SHAPE_GUARD: std::cout << "SHAPE_GUARD"; break;
        case Strategy::METHOD_CACHE: std::cout << "METHOD_CACHE"; break;
        case Strategy::FULL: std::cout << "FULL"; break;
    }
    std::cout << std::endl;
    
    std::cout << "  Active Caches: " << cache_manager_->get_cache_count() << std::endl;
    std::cout << "  Call Sites: " << call_site_registry_->get_call_site_count() << std::endl;
}

bool PropertyAccessOptimizer::should_jit_compile_property_access(uint32_t call_site_id) const {
    const auto* call_site_info = call_site_registry_->get_call_site_info(call_site_id);
    return call_site_info && call_site_info->access_count >= MIN_ACCESSES_FOR_OPTIMIZATION;
}

PropertyAccessOptimizer& PropertyAccessOptimizer::get_instance() {
    static PropertyAccessOptimizer instance;
    return instance;
}

//=============================================================================
// AdaptiveInlineCache Implementation - Self-tuning cache system
//=============================================================================

AdaptiveInlineCache::AdaptiveInlineCache() : total_adaptations_(0) {
    // Initialize with default parameters
}

AdaptiveInlineCache::~AdaptiveInlineCache() {
    // Cleanup
}

void AdaptiveInlineCache::adapt_cache_parameters() {
    // Analyze current cache performance and adjust parameters
    InlineCacheManager& cache_manager = InlineCacheManager::get_instance();
    cache_manager.analyze_cache_performance();
    
    const auto& stats = cache_manager.get_global_stats();
    
    // Adapt based on cache distribution
    if (stats.megamorphic_caches > stats.monomorphic_caches + stats.polymorphic_caches) {
        // Too many megamorphic caches - reduce max entries
        if (params_.max_entries > 2) {
            params_.max_entries--;
            total_adaptations_++;
            std::cout << "🔧 ADAPTIVE IC: Reduced max entries to " << params_.max_entries << std::endl;
        }
    } else if (stats.monomorphic_caches > stats.polymorphic_caches * 2) {
        // Mostly monomorphic - can increase max entries
        if (params_.max_entries < 8) {
            params_.max_entries++;
            total_adaptations_++;
            std::cout << "🔧 ADAPTIVE IC: Increased max entries to " << params_.max_entries << std::endl;
        }
    }
}

void AdaptiveInlineCache::monitor_cache_performance() {
    // Periodically check and adapt cache parameters
    static uint64_t last_adaptation = 0;
    static uint64_t check_counter = 0;
    
    check_counter++;
    
    if (check_counter - last_adaptation >= params_.invalidation_interval) {
        adapt_cache_parameters();
        last_adaptation = check_counter;
    }
}

void AdaptiveInlineCache::print_adaptive_stats() const {
    std::cout << "🤖 ADAPTIVE IC STATISTICS:" << std::endl;
    std::cout << "  Total Adaptations: " << total_adaptations_ << std::endl;
    std::cout << "  Hit Ratio Threshold: " << params_.hit_ratio_threshold << std::endl;
    std::cout << "  Max Entries: " << params_.max_entries << std::endl;
    std::cout << "  Method Caching: " << (params_.enable_method_caching ? "ENABLED" : "DISABLED") << std::endl;
}

} // namespace Quanta