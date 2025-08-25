/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>

namespace Quanta {

// Forward declarations
class Object;
class Value;

//=============================================================================
// Standard Hidden Classes for Ultra-Fast Property Access
// 
// Hidden Classes are a fundamental optimization that
// enables high-performance JavaScript object property access.
// This implementation provides:
// - Fast property access through inline caching
// - Efficient object layout optimization  
// - Dynamic property addition/deletion handling
// - Shape transitions for polymorphic objects
// - Memory-efficient object representation
//=============================================================================

using PropertyIndex = uint32_t;
using HiddenClassID = uint64_t;

//=============================================================================
// PropertyDescriptor - Describes object properties with metadata
//=============================================================================

enum class PropertyType : uint8_t {
    DATA = 0,           // Regular data property
    ACCESSOR = 1,       // Getter/setter property
    METHOD = 2,         // Function property (optimized)
    CONSTANT = 3        // Compile-time constant
};

enum class HiddenClassPropertyAttributes : uint8_t {
    NONE = 0,
    WRITABLE = 1 << 0,
    ENUMERABLE = 1 << 1, 
    CONFIGURABLE = 1 << 2,
    DEFAULT = WRITABLE | ENUMERABLE | CONFIGURABLE
};

struct HiddenClassPropertyDescriptor {
    std::string name;
    PropertyIndex index;
    PropertyType type;
    HiddenClassPropertyAttributes attributes;
    
    // For optimization
    bool is_fast_access;     // Can use fast inline cache
    bool is_frequently_accessed;  // Hot property
    
    HiddenClassPropertyDescriptor(const std::string& prop_name, PropertyIndex idx)
        : name(prop_name), index(idx), type(PropertyType::DATA), 
          attributes(HiddenClassPropertyAttributes::DEFAULT), 
          is_fast_access(true), is_frequently_accessed(false) {}
};

//=============================================================================
// HiddenClass - Standard object shape representation
//=============================================================================

class HiddenClass : public std::enable_shared_from_this<HiddenClass> {
    friend class HiddenClassCache;
    
public:
    HiddenClassID class_id_;
    std::vector<HiddenClassPropertyDescriptor> properties_;
    std::unordered_map<std::string, PropertyIndex> property_map_;

private:
    
    // Transitions to other hidden classes
    std::unordered_map<std::string, std::shared_ptr<HiddenClass>> transitions_;
    std::shared_ptr<HiddenClass> parent_;
    
    // Optimization data
    uint32_t instance_count_;        // How many objects use this class
    uint32_t access_count_;          // Property access frequency
    bool is_stable_;                 // No recent transitions
    bool is_deprecated_;             // Old, replaced class
    
    // Fast lookup tables
    mutable std::vector<PropertyIndex> fast_property_indices_;
    mutable bool fast_indices_valid_;
    
    static std::atomic<HiddenClassID> next_class_id_;

public:
    explicit HiddenClass();
    explicit HiddenClass(std::shared_ptr<HiddenClass> parent);
    ~HiddenClass();
    
    // Core functionality
    HiddenClassID get_class_id() const { return class_id_; }
    size_t get_property_count() const { return properties_.size(); }
    const std::vector<HiddenClassPropertyDescriptor>& get_properties() const { return properties_; }
    
    // Property management
    bool has_property(const std::string& name) const;
    PropertyIndex get_property_index(const std::string& name) const;
    const HiddenClassPropertyDescriptor* get_property_descriptor(const std::string& name) const;
    const HiddenClassPropertyDescriptor* get_property_descriptor(PropertyIndex index) const;
    
    // Hidden class transitions
    std::shared_ptr<HiddenClass> add_property(const std::string& name, PropertyType type = PropertyType::DATA);
    std::shared_ptr<HiddenClass> remove_property(const std::string& name);
    std::shared_ptr<HiddenClass> change_property_type(const std::string& name, PropertyType new_type);
    
    // Performance optimization
    void mark_property_hot(const std::string& name);
    void update_access_frequency();
    bool is_monomorphic() const { return instance_count_ > 10 && transitions_.empty(); }
    bool should_optimize() const { return access_count_ > 100 && is_stable_; }
    
    // Object layout optimization
    void optimize_property_layout();
    std::vector<PropertyIndex> get_optimized_layout() const;
    
    // Fast property access
    const std::vector<PropertyIndex>& get_fast_indices() const;
    void invalidate_fast_indices();
    
    // Statistics and debugging
    void print_class_info() const;
    void print_transitions() const;
    
    // Instance tracking
    void add_instance() { instance_count_++; }
    void remove_instance() { if (instance_count_ > 0) instance_count_--; }
    uint32_t get_instance_count() const { return instance_count_; }
    
    // Stability tracking
    void mark_stable() { is_stable_ = true; }
    void mark_unstable() { is_stable_ = false; }
    bool is_stable() const { return is_stable_; }
    
    // Deprecation for GC
    void mark_deprecated() { is_deprecated_ = true; }
    bool is_deprecated() const { return is_deprecated_; }
};

//=============================================================================
// HiddenClassCache - Global cache for hidden class reuse
//=============================================================================

class HiddenClassCache {
    friend class HiddenClass;
public:
    std::unordered_map<HiddenClassID, std::weak_ptr<HiddenClass>> class_cache_;
    std::unordered_map<std::string, std::shared_ptr<HiddenClass>> common_classes_;
    
    // Performance tracking
    uint64_t cache_hits_;
    uint64_t cache_misses_;
    uint64_t total_lookups_;
    
    mutable std::mutex cache_mutex_;

private:

public:
    HiddenClassCache();
    ~HiddenClassCache();
    
    // Cache management
    std::shared_ptr<HiddenClass> get_or_create_class(const std::vector<std::string>& property_names);
    std::shared_ptr<HiddenClass> find_class(HiddenClassID class_id);
    void cache_class(std::shared_ptr<HiddenClass> hidden_class);
    
    // Common class patterns
    std::shared_ptr<HiddenClass> get_empty_class();
    std::shared_ptr<HiddenClass> get_array_class();
    std::shared_ptr<HiddenClass> get_function_class();
    
    // Cache optimization
    void cleanup_deprecated_classes();
    void optimize_cache();
    
    // Statistics
    double get_cache_hit_ratio() const;
    void print_cache_statistics() const;
    void reset_statistics();
    
    // Singleton access
    static HiddenClassCache& get_instance();
};

//=============================================================================
// HiddenClassObject - Objects that use hidden classes
//=============================================================================

class HiddenClassObject {
private:
    std::shared_ptr<HiddenClass> hidden_class_;
    std::vector<Value> property_values_;  // Parallel to property descriptors
    
    // Fast access cache
    mutable PropertyIndex last_accessed_index_;
    mutable std::string last_accessed_name_;

public:
    explicit HiddenClassObject(std::shared_ptr<HiddenClass> hidden_class);
    ~HiddenClassObject();
    
    // Property access
    bool get_property(const std::string& name, Value& out_value) const;
    bool set_property(const std::string& name, const Value& value);
    bool has_property(const std::string& name) const;
    bool delete_property(const std::string& name);
    
    // Fast indexed access
    Value get_property_by_index(PropertyIndex index) const;
    void set_property_by_index(PropertyIndex index, const Value& value);
    
    // Hidden class management
    std::shared_ptr<HiddenClass> get_hidden_class() const { return hidden_class_; }
    void transition_to_class(std::shared_ptr<HiddenClass> new_class);
    
    // Object layout
    size_t get_property_count() const;
    std::vector<std::string> get_property_names() const;
    
    // Performance optimization
    void optimize_for_access_pattern();
    bool is_optimized() const;
    
    // Debugging
    void print_object_layout() const;
};

//=============================================================================
// HiddenClassOptimizer - Analyzes and optimizes hidden class usage
//=============================================================================

class HiddenClassOptimizer {
private:
    struct OptimizationStats {
        uint64_t total_objects_created;
        uint64_t hidden_class_transitions;
        uint64_t cache_hits;
        uint64_t property_accesses;
        uint64_t optimized_accesses;
        double average_properties_per_object;
        double transition_rate;
    };
    
    OptimizationStats stats_;
    std::vector<std::weak_ptr<HiddenClass>> tracked_classes_;
    
    mutable std::mutex optimizer_mutex_;

public:
    HiddenClassOptimizer();
    ~HiddenClassOptimizer();
    
    // Optimization analysis
    void analyze_object_patterns();
    void optimize_hot_classes();
    void consolidate_similar_classes();
    
    // Performance monitoring
    void track_object_creation(std::shared_ptr<HiddenClass> hidden_class);
    void track_property_access(const std::string& property_name);
    void track_class_transition(std::shared_ptr<HiddenClass> from, std::shared_ptr<HiddenClass> to);
    
    // Recommendations
    std::vector<std::string> get_optimization_recommendations() const;
    void print_optimization_report() const;
    
    // Statistics
    const OptimizationStats& get_statistics() const { return stats_; }
    void reset_statistics();
    
    // Singleton access
    static HiddenClassOptimizer& get_instance();
};

//=============================================================================
// HiddenClassIntegration - Integration with Quanta engine
//=============================================================================

namespace HiddenClassIntegration {
    // Engine hooks
    void initialize_hidden_classes();
    void shutdown_hidden_classes();
    
    // Object creation hooks
    std::shared_ptr<HiddenClass> create_class_for_object(Object* obj);
    void optimize_object_layout(Object* obj);
    
    // Property access hooks
    bool fast_property_get(Object* obj, const std::string& name, Value& out_value);
    bool fast_property_set(Object* obj, const std::string& name, const Value& value);
    
    // Performance monitoring
    void monitor_class_usage();
    void print_hidden_class_statistics();
    
    // Advanced optimization
    void enable_adaptive_optimization();
    void tune_optimization_thresholds();
}

} // namespace Quanta