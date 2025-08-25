/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include "Object.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

namespace Quanta {

//=============================================================================
// Shape-Based Optimization System - PHASE 2: V8-Level Performance
//=============================================================================

// Forward declarations
class Object;

// Type definitions
using ShapeID = uint32_t;
using PropertyOffset = uint32_t;

//=============================================================================
// Object Shape - Tracks the structure/layout of objects
//=============================================================================

class ObjectShape {
public:
    using ShapeID = ::Quanta::ShapeID;
    using PropertyOffset = ::Quanta::PropertyOffset;
    
    struct PropertyDescriptor {
        std::string name;
        PropertyOffset offset;          // Offset in object's property array
        bool is_configurable;
        bool is_enumerable;
        bool is_writable;
        
        PropertyDescriptor(const std::string& n, PropertyOffset off)
            : name(n), offset(off), is_configurable(true), is_enumerable(true), is_writable(true) {}
    };
    
private:
    ShapeID shape_id_;
    std::vector<PropertyDescriptor> properties_;
    std::unordered_map<std::string, PropertyOffset> property_map_;
    ObjectShape* parent_shape_;        // For shape transitions
    uint32_t property_count_;
    uint32_t transition_count_;        // How many objects transitioned from this shape
    
    static ShapeID next_shape_id_;
    static std::unordered_map<ShapeID, std::shared_ptr<ObjectShape>> global_shapes_;
    
public:
    ObjectShape();
    ObjectShape(ObjectShape* parent, const std::string& property_name);
    ~ObjectShape();
    
    // Shape identification
    ShapeID get_id() const { return shape_id_; }
    uint32_t get_property_count() const { return property_count_; }
    
    // Property access
    bool has_property(const std::string& name) const;
    PropertyOffset get_property_offset(const std::string& name) const;
    const PropertyDescriptor* get_property_descriptor(const std::string& name) const;
    const std::vector<PropertyDescriptor>& get_properties() const { return properties_; }
    
    // Shape transitions
    ObjectShape* transition_add_property(const std::string& property_name);
    ObjectShape* transition_delete_property(const std::string& property_name);
    ObjectShape* get_parent() const { return parent_shape_; }
    
    // Optimization metrics
    uint32_t get_transition_count() const { return transition_count_; }
    void increment_transition_count() { transition_count_++; }
    
    // Shape cache management
    static std::shared_ptr<ObjectShape> get_root_shape();
    static std::shared_ptr<ObjectShape> get_shape_by_id(ShapeID id);
    static void cleanup_unused_shapes();
    
    // Debug and profiling
    std::string to_string() const;
    bool is_stable() const { return transition_count_ < 100; } // Stable if < 100 transitions
};

//=============================================================================
// Shape Cache - Fast property access based on object shapes
//=============================================================================

class ShapeCache {
public:
    struct CacheEntry {
        ShapeID shape_id;
        PropertyOffset offset;
        uint64_t access_count;
        uint64_t hit_count;
        
        CacheEntry() : shape_id(0), offset(0), access_count(0), hit_count(0) {}
        CacheEntry(ShapeID id, PropertyOffset off) 
            : shape_id(id), offset(off), access_count(1), hit_count(0) {}
    };
    
private:
    static constexpr size_t CACHE_SIZE = 1024;
    static constexpr size_t CACHE_MASK = CACHE_SIZE - 1;
    
    CacheEntry cache_[CACHE_SIZE];
    uint64_t total_lookups_;
    uint64_t cache_hits_;
    uint64_t cache_misses_;
    
    uint32_t hash_property_access(const std::string& property, ShapeID shape_id) const;
    
public:
    ShapeCache();
    ~ShapeCache();
    
    // Cache operations
    bool lookup(const std::string& property, ShapeID shape_id, PropertyOffset& offset);
    void insert(const std::string& property, ShapeID shape_id, PropertyOffset offset);
    void invalidate_shape(ShapeID shape_id);
    
    // Performance metrics
    double get_hit_ratio() const;
    uint64_t get_total_lookups() const { return total_lookups_; }
    uint64_t get_cache_hits() const { return cache_hits_; }
    uint64_t get_cache_misses() const { return cache_misses_; }
    
    // Cache management
    void clear();
    void print_stats() const;
};

//=============================================================================
// Shape-Optimized Object - Objects that use shape-based optimization
//=============================================================================

class ShapeOptimizedObject : public Object {
private:
    std::shared_ptr<ObjectShape> shape_;
    std::vector<Value> fast_properties_;  // Properties stored in shape order
    
    static ShapeCache global_shape_cache_;
    
public:
    ShapeOptimizedObject();
    ShapeOptimizedObject(std::shared_ptr<ObjectShape> shape);
    virtual ~ShapeOptimizedObject();
    
    // Shape-optimized property access
    Value get_property(const std::string& key) const override;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attributes = PropertyAttributes::Default);
    bool has_property(const std::string& key) const;
    bool delete_property(const std::string& key);
    
    // Shape management
    std::shared_ptr<ObjectShape> get_shape() const { return shape_; }
    void transition_shape(std::shared_ptr<ObjectShape> new_shape);
    
    // Fast property access (bypasses normal property lookup)
    Value get_fast_property(PropertyOffset offset) const;
    void set_fast_property(PropertyOffset offset, const Value& value);
    
    // Object factory integration
    static std::unique_ptr<ShapeOptimizedObject> create_with_shape(std::shared_ptr<ObjectShape> shape);
    
    // Performance monitoring
    static ShapeCache& get_global_cache() { return global_shape_cache_; }
};

//=============================================================================
// Shape Transition Manager - Manages object shape transitions
//=============================================================================

class ShapeTransitionManager {
public:
    struct TransitionStats {
        uint64_t total_transitions;
        uint64_t add_property_transitions;
        uint64_t delete_property_transitions;
        uint64_t shape_cache_hits;
        uint64_t shape_cache_misses;
        
        TransitionStats() : total_transitions(0), add_property_transitions(0), 
                           delete_property_transitions(0), shape_cache_hits(0), shape_cache_misses(0) {}
    };
    
private:
    TransitionStats stats_;
    std::unordered_map<std::string, std::shared_ptr<ObjectShape>> transition_cache_;
    
public:
    ShapeTransitionManager();
    ~ShapeTransitionManager();
    
    // Shape transition operations
    std::shared_ptr<ObjectShape> add_property_transition(std::shared_ptr<ObjectShape> current_shape, 
                                                        const std::string& property_name);
    std::shared_ptr<ObjectShape> delete_property_transition(std::shared_ptr<ObjectShape> current_shape, 
                                                           const std::string& property_name);
    
    // Statistics and monitoring
    const TransitionStats& get_stats() const { return stats_; }
    void print_transition_stats() const;
    
    // Cache management
    void clear_transition_cache();
    
    // Global instance
    static ShapeTransitionManager& get_instance();
};

//=============================================================================
// Shape-Based Optimization Integration
//=============================================================================

class ShapeOptimizer {
public:
    // Enable shape optimization for existing objects
    static void optimize_object(Object* obj);
    
    // Create shape-optimized objects
    static std::unique_ptr<ShapeOptimizedObject> create_optimized_object();
    static std::unique_ptr<ShapeOptimizedObject> create_optimized_object_with_properties(
        const std::vector<std::string>& property_names);
    
    // Performance analysis
    static void analyze_object_shapes();
    static void print_shape_statistics();
    
    // Integration with JIT system
    static bool should_optimize_property_access(const std::string& property, Object* obj);
    static PropertyOffset get_optimized_offset(const std::string& property, Object* obj);
    
    // Global optimization control
    static void enable_shape_optimization(bool enabled = true);
    static bool is_shape_optimization_enabled();
    
private:
    static bool optimization_enabled_;
    static uint64_t objects_optimized_;
    static uint64_t fast_property_accesses_;
};

//=============================================================================
// Shape-Based Inline Cache - V8-style property access optimization
//=============================================================================

class ShapeInlineCache {
public:
    struct ICEntry {
        ShapeID shape_id;
        PropertyOffset offset;
        uint32_t access_count;
        bool is_valid;
        
        ICEntry() : shape_id(0), offset(0), access_count(0), is_valid(false) {}
    };
    
    static constexpr size_t IC_SIZE = 4; // Polymorphic inline cache with 4 entries
    
private:
    ICEntry entries_[IC_SIZE];
    uint32_t current_size_;
    uint64_t total_accesses_;
    uint64_t ic_hits_;
    
public:
    ShapeInlineCache();
    ~ShapeInlineCache();
    
    // Inline cache operations
    bool lookup(ShapeID shape_id, PropertyOffset& offset);
    void update(ShapeID shape_id, PropertyOffset offset);
    void invalidate();
    
    // Performance metrics
    double get_hit_ratio() const;
    bool is_monomorphic() const { return current_size_ == 1; }
    bool is_polymorphic() const { return current_size_ > 1 && current_size_ <= IC_SIZE; }
    bool is_megamorphic() const { return current_size_ > IC_SIZE; }
    
    // Cache state
    uint32_t get_cache_size() const { return current_size_; }
    void print_cache_state() const;
};

} // namespace Quanta