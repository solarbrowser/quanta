/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Object.h"
#include "../../include/Value.h"
#include <vector>
#include <memory>

namespace Quanta {

class Shape;
class Context;

/**
 * Object Construction and Prototype Management
 */
class ObjectCore {
public:
    // Object construction
    static std::unique_ptr<Object> create_object(ObjectType type = ObjectType::Object);
    static std::unique_ptr<Object> create_object_with_prototype(Object* prototype, ObjectType type = ObjectType::Object);

    // Array creation
    static std::unique_ptr<Object> create_array(uint32_t length = 0);
    static std::unique_ptr<Object> create_array_from_values(const std::vector<Value>& values);

    // Function creation
    static std::unique_ptr<Object> create_function(const std::string& name = "");
    static std::unique_ptr<Object> create_native_function(const std::string& name,
                                                         NativeFunction native_func,
                                                         int arity = 0);

    // Prototype chain management
    static void set_prototype(Object* object, Object* prototype);
    static Object* get_prototype(const Object* object);
    static bool has_prototype(const Object* object, Object* prototype);

    // Prototype chain traversal
    static std::vector<Object*> get_prototype_chain(const Object* object);
    static Object* find_in_prototype_chain(const Object* object,
                                          const std::function<bool(Object*)>& predicate);

    // Object type checks and conversions
    static bool is_array_like(const Object* object);
    static bool is_callable(const Object* object);
    static bool is_constructor(const Object* object);

    // Object copying and cloning
    static std::unique_ptr<Object> shallow_copy(const Object* source);
    static std::unique_ptr<Object> deep_copy(const Object* source);

    // Object comparison
    static bool objects_equal(const Object* left, const Object* right);
    static bool objects_same_value(const Object* left, const Object* right);

    // Object validation
    static bool is_valid_object(const Object* object);
    static bool validate_object_integrity(const Object* object);

    // Memory and lifecycle
    static void register_object_with_gc(Object* object);
    static void unregister_object_from_gc(Object* object);

    // Object introspection
    static std::string get_object_type_name(const Object* object);
    static size_t get_object_size(const Object* object);
    static uint32_t get_object_hash(const Object* object);

    // Built-in object creation
    static std::unique_ptr<Object> create_global_object();
    static std::unique_ptr<Object> create_error_object(const std::string& message,
                                                      const std::string& type = "Error");

private:
    // Internal helpers
    static void initialize_object_header(Object* object, ObjectType type);
    static void setup_default_properties(Object* object, ObjectType type);
    static Shape* get_default_shape_for_type(ObjectType type);
};

/**
 * Object Factory for common object patterns
 */
class ObjectFactory {
public:
    // Common object patterns
    static Value create_plain_object();
    static Value create_array_object(uint32_t length = 0);
    static Value create_function_object(const std::string& name = "");

    // Object with predefined properties
    static Value create_object_with_properties(const std::vector<std::pair<std::string, Value>>& props);

    // Built-in constructor objects
    static Value create_object_constructor();
    static Value create_array_constructor();
    static Value create_function_constructor();

    // Error objects
    static Value create_type_error(const std::string& message);
    static Value create_reference_error(const std::string& message);
    static Value create_syntax_error(const std::string& message);
    static Value create_range_error(const std::string& message);

    // Special objects
    static Value create_arguments_object(const std::vector<Value>& args);
    static Value create_regex_object(const std::string& pattern, const std::string& flags = "");

    // Object pool management
    static void initialize_object_pools();
    static void cleanup_object_pools();

    // Performance optimization
    static Value create_fast_object(); // For frequently created objects
    static void enable_object_pooling(bool enabled);

private:
    static bool object_pooling_enabled_;
    static std::vector<std::unique_ptr<Object>> object_pool_;
    static std::mutex pool_mutex_;

    // Pool management
    static Object* get_pooled_object();
    static void return_to_pool(Object* object);
};

/**
 * Object Utilities
 */
namespace ObjectUtils {
    // Object inspection
    std::string describe_object(const Object* object);
    void print_object_debug_info(const Object* object);

    // Property enumeration helpers
    std::vector<std::string> get_all_property_names(const Object* object, bool include_non_enumerable = false);
    std::vector<std::string> get_own_property_names(const Object* object, bool include_non_enumerable = false);

    // Prototype utilities
    bool is_prototype_of(const Object* prototype, const Object* object);
    Object* get_common_prototype(const Object* obj1, const Object* obj2);

    // Object transformation
    std::unique_ptr<Object> merge_objects(const Object* obj1, const Object* obj2);
    std::unique_ptr<Object> pick_properties(const Object* source, const std::vector<std::string>& keys);
    std::unique_ptr<Object> omit_properties(const Object* source, const std::vector<std::string>& keys);

    // Array utilities
    bool is_sparse_array(const Object* array);
    uint32_t get_sparse_array_density(const Object* array);
    void compact_sparse_array(Object* array);

    // Performance analysis
    struct ObjectStats {
        size_t property_count;
        size_t element_count;
        size_t memory_usage;
        uint32_t prototype_depth;
        bool has_hidden_properties;
    };

    ObjectStats analyze_object(const Object* object);
}

} // namespace Quanta