/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Object.h"
#include "../../include/Value.h"
#include <string>
#include <vector>
#include <functional>

namespace Quanta {

/**
 * Object Property Management
 * Handles property access, modification, and enumeration
 */
class ObjectProperties {
public:
    // Property access
    static bool has_property(const Object* object, const std::string& key);
    static bool has_own_property(const Object* object, const std::string& key);
    static Value get_property(const Object* object, const std::string& key);
    static Value get_own_property(const Object* object, const std::string& key);

    // Property modification
    static bool set_property(Object* object, const std::string& key, const Value& value,
                           PropertyAttributes attrs = PropertyAttributes::Default);
    static bool delete_property(Object* object, const std::string& key);

    // Property descriptors
    static bool set_property_descriptor(Object* object, const std::string& key,
                                      const PropertyDescriptor& descriptor);
    static PropertyDescriptor get_property_descriptor(const Object* object, const std::string& key);
    static bool has_property_descriptor(const Object* object, const std::string& key);

    // Array element access (optimized for arrays)
    static Value get_element(const Object* array, uint32_t index);
    static bool set_element(Object* array, uint32_t index, const Value& value);
    static bool delete_element(Object* array, uint32_t index);

    // Property enumeration
    static std::vector<std::string> get_own_property_keys(const Object* object);
    static std::vector<std::string> get_enumerable_keys(const Object* object);
    static std::vector<std::string> get_all_property_keys(const Object* object);

    // Property attributes
    static PropertyAttributes get_property_attributes(const Object* object, const std::string& key);
    static bool set_property_attributes(Object* object, const std::string& key, PropertyAttributes attrs);

    // Advanced property operations
    static bool define_property(Object* object, const std::string& key,
                              const PropertyDescriptor& descriptor);
    static bool redefine_property(Object* object, const std::string& key,
                                const PropertyDescriptor& new_descriptor);

    // Property validation
    static bool is_valid_property_key(const std::string& key);
    static bool is_array_index_key(const std::string& key, uint32_t* index = nullptr);

    // Bulk property operations
    static bool copy_properties(Object* target, const Object* source,
                              bool include_non_enumerable = false);
    static bool merge_properties(Object* target, const Object* source,
                                bool overwrite_existing = true);

    // Property access patterns
    static void enable_property_caching(Object* object, bool enabled);
    static void invalidate_property_cache(Object* object);

    // Property change notifications
    using PropertyChangeCallback = std::function<void(Object*, const std::string&, const Value&, const Value&)>;
    static void set_property_change_callback(Object* object, PropertyChangeCallback callback);
    static void remove_property_change_callback(Object* object);

    // Performance optimizations
    static bool optimize_property_storage(Object* object);
    static void compact_property_storage(Object* object);

private:
    // Internal property access helpers
    static Value get_property_internal(const Object* object, const std::string& key, bool own_only);
    static bool set_property_internal(Object* object, const std::string& key, const Value& value,
                                     PropertyAttributes attrs, bool force_own);

    // Property storage optimization
    static bool should_use_fast_properties(const Object* object);
    static void transition_to_slow_properties(Object* object);
    static void transition_to_fast_properties(Object* object);

    // Property lookup optimization
    static Value fast_property_lookup(const Object* object, const std::string& key);
    static bool fast_property_set(Object* object, const std::string& key, const Value& value);

    // Array optimization
    static bool is_dense_array_access(const Object* array, uint32_t index);
    static void ensure_array_capacity(Object* array, uint32_t index);

    // Property descriptor validation
    static bool is_valid_descriptor(const PropertyDescriptor& descriptor);
    static bool is_compatible_descriptor(const PropertyDescriptor& current,
                                       const PropertyDescriptor& new_desc);
};

/**
 * Property Descriptor Utilities
 */
class PropertyDescriptorUtils {
public:
    // Descriptor creation
    static PropertyDescriptor create_data_descriptor(const Value& value,
                                                    bool writable = true,
                                                    bool enumerable = true,
                                                    bool configurable = true);

    static PropertyDescriptor create_accessor_descriptor(const Value& getter,
                                                       const Value& setter,
                                                       bool enumerable = true,
                                                       bool configurable = true);

    // Descriptor validation
    static bool is_data_descriptor(const PropertyDescriptor& desc);
    static bool is_accessor_descriptor(const PropertyDescriptor& desc);
    static bool is_generic_descriptor(const PropertyDescriptor& desc);

    // Descriptor conversion
    static PropertyDescriptor to_data_descriptor(const PropertyDescriptor& desc, const Value& value);
    static PropertyDescriptor to_accessor_descriptor(const PropertyDescriptor& desc,
                                                   const Value& getter, const Value& setter);

    // Descriptor comparison
    static bool descriptors_equal(const PropertyDescriptor& left, const PropertyDescriptor& right);
    static PropertyDescriptor merge_descriptors(const PropertyDescriptor& current,
                                               const PropertyDescriptor& update);

    // Default descriptors
    static PropertyDescriptor get_default_data_descriptor();
    static PropertyDescriptor get_default_accessor_descriptor();

    // Descriptor debugging
    static std::string describe_descriptor(const PropertyDescriptor& desc);
};

/**
 * Property Access Cache
 * Optimizes frequent property access patterns
 */
class PropertyAccessCache {
public:
    struct CacheEntry {
        std::string key;
        Value value;
        uint32_t shape_id;
        bool is_own_property;
        PropertyAttributes attributes;
    };

    PropertyAccessCache(size_t max_size = 64);
    ~PropertyAccessCache();

    // Cache operations
    bool lookup(const Object* object, const std::string& key, Value& value);
    void store(const Object* object, const std::string& key, const Value& value,
              bool is_own, PropertyAttributes attrs);
    void invalidate(const Object* object);
    void clear();

    // Cache statistics
    size_t get_hit_count() const { return hit_count_; }
    size_t get_miss_count() const { return miss_count_; }
    double get_hit_rate() const;

private:
    std::vector<CacheEntry> cache_;
    size_t max_size_;
    size_t current_index_;
    size_t hit_count_;
    size_t miss_count_;

    // Cache management
    size_t find_entry(const Object* object, const std::string& key);
    void evict_entry(size_t index);
    uint32_t get_object_shape_id(const Object* object);
};

} // namespace Quanta