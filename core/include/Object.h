/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_OBJECT_H
#define QUANTA_OBJECT_H

#include "Value.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <chrono>

namespace Quanta {

// Forward declarations
class PropertyDescriptor;
class Shape;
class Context;
class ASTNode;
class Parameter;

/**
 * High-performance JavaScript object implementation
 * Features:
 * - Hidden Classes (Shapes) for property access optimization
 * - Inline caching for property lookups
 * - Efficient property storage with backing arrays
 * - Prototype chain management
 * - Property descriptor support
 */
class Object {
public:
    // Object type classification for optimization
    enum class ObjectType : uint8_t {
        Ordinary,       // Regular object
        Array,          // Array object
        Function,       // Function object
        String,         // String object
        Number,         // Number object
        Boolean,        // Boolean object
        Date,           // Date object
        RegExp,         // Regular expression
        Error,          // Error object
        Promise,        // Promise object
        Proxy,          // Proxy object
        Map,            // Map object
        Set,            // Set object
        WeakMap,        // WeakMap object
        WeakSet,        // WeakSet object
        ArrayBuffer,    // ArrayBuffer object
        TypedArray,     // TypedArray variants
        DataView,       // DataView object
        Symbol,         // Symbol object
        BigInt,         // BigInt object
        Custom          // User-defined types
    };


private:
    // Object header for efficient memory layout
    struct ObjectHeader {
        Shape* shape;               // Hidden class for property layout
        Object* prototype;          // Prototype object
        ObjectType type;            // Object type for specialized behavior
        uint8_t flags;              // Various object flags
        uint16_t property_count;    // Number of properties
        uint32_t hash_code;         // Cached hash code
    } header_;

    // Property storage
    std::vector<Value> properties_;         // Property values (indexed by shape)
    std::vector<Value> elements_;           // Array elements for fast indexing
    
    // Overflow map for properties not in shape
    std::unique_ptr<std::unordered_map<std::string, Value>> overflow_properties_;
    
    // Property descriptors for non-default attributes
    std::unique_ptr<std::unordered_map<std::string, PropertyDescriptor>> descriptors_;
    
    // Track property insertion order for enumeration
    std::vector<std::string> property_insertion_order_;

public:
    // Constructors
    Object(ObjectType type = ObjectType::Ordinary);
    explicit Object(Object* prototype, ObjectType type = ObjectType::Ordinary);
    virtual ~Object() = default;

    // Copy and move semantics
    Object(const Object& other) = delete;            // Objects are not copyable
    Object& operator=(const Object& other) = delete;
    Object(Object&& other) noexcept = default;
    Object& operator=(Object&& other) noexcept = default;

    // Type information
    ObjectType get_type() const { return header_.type; }
    bool is_array() const { return header_.type == ObjectType::Array; }
    bool is_function() const { return header_.type == ObjectType::Function; }
    bool is_primitive_wrapper() const {
        return header_.type == ObjectType::String || 
               header_.type == ObjectType::Number || 
               header_.type == ObjectType::Boolean;
    }
    
    // Binary data type checking
    virtual bool is_array_buffer() const { return header_.type == ObjectType::ArrayBuffer; }
    virtual bool is_typed_array() const { return header_.type == ObjectType::TypedArray; }
    virtual bool is_data_view() const { return header_.type == ObjectType::DataView; }
    virtual bool is_shared_array_buffer() const { return false; } // Override in subclass
    
    // WebAssembly type checking
    virtual bool is_wasm_memory() const { return false; }
    virtual bool is_wasm_module() const { return false; }
    virtual bool is_wasm_instance() const { return false; }

    // Prototype chain
    virtual Object* get_prototype() const { return header_.prototype; }
    void set_prototype(Object* prototype);
    bool has_prototype(Object* prototype) const;
    
    // Property operations (optimized)
    bool has_property(const std::string& key) const;
    bool has_own_property(const std::string& key) const;
    
    virtual Value get_property(const std::string& key) const;
    Value get_own_property(const std::string& key) const;
    
    virtual bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    bool delete_property(const std::string& key);
    
    // Array-like element access (optimized for indices)
    Value get_element(uint32_t index) const;
    bool set_element(uint32_t index, const Value& value);
    bool delete_element(uint32_t index);
    
    // Property enumeration
    std::vector<std::string> get_own_property_keys() const;
    std::vector<std::string> get_enumerable_keys() const;
    std::vector<uint32_t> get_element_indices() const;
    
    // Property descriptor operations
    PropertyDescriptor get_property_descriptor(const std::string& key) const;
    bool set_property_descriptor(const std::string& key, const PropertyDescriptor& desc);
    
    // Object operations
    bool is_extensible() const;
    void prevent_extensions();
    void seal();
    void freeze();
    bool is_sealed() const;
    bool is_frozen() const;
    
    // Array operations (for Array objects)
    uint32_t get_length() const;
    void set_length(uint32_t length);
    void push(const Value& value);
    Value pop();
    void unshift(const Value& value);
    Value shift();
    
    // Modern array methods
    std::unique_ptr<Object> map(Function* callback, Context& ctx);
    std::unique_ptr<Object> filter(Function* callback, Context& ctx);
    void forEach(Function* callback, Context& ctx);
    Value reduce(Function* callback, const Value& initial_value, Context& ctx);
    
    // ES2026 Array methods
    Value groupBy(Function* callback, Context& ctx);
    
    // Function operations (for Function objects)
    Value call(Context& ctx, const Value& this_value, const std::vector<Value>& args);
    Value construct(Context& ctx, const std::vector<Value>& args);
    
    // Object conversion
    Value to_primitive(const std::string& hint = "") const;
    std::string to_string() const;
    double to_number() const;
    bool to_boolean() const;
    
    // Utility methods
    size_t property_count() const { return header_.property_count; }
    size_t element_count() const { return elements_.size(); }
    std::string debug_string() const;
    uint32_t hash() const { return header_.hash_code; }
    
    // Memory management
    void mark_references() const;
    size_t memory_usage() const;
    
    // Shape management (internal)
    Shape* get_shape() const { return header_.shape; }
    void transition_shape(const std::string& key, PropertyAttributes attrs);
    
    // Internal property access (bypassing descriptors)
    Value get_internal_property(const std::string& key) const;
    void set_internal_property(const std::string& key, const Value& value);

protected:
    // Virtual methods for specialized objects
    virtual Value internal_get(const std::string& key) const;
    virtual bool internal_set(const std::string& key, const Value& value);
    virtual bool internal_delete(const std::string& key);
    virtual std::vector<std::string> internal_own_keys() const;
    
    // Element storage management
    void ensure_element_capacity(uint32_t capacity);
    void compact_elements();
    
    // Property storage management
    void ensure_property_capacity(size_t capacity);
    bool store_in_shape(const std::string& key, const Value& value, PropertyAttributes attrs);
    bool store_in_overflow(const std::string& key, const Value& value);

public:
    // Clear all properties for object pool reuse
    void clear_properties();
    // Hash function for shape transitions
    struct ShapeTransitionHash {
        std::size_t operator()(const std::pair<Shape*, std::string>& p) const {
            std::size_t h1 = std::hash<void*>{}(p.first);
            std::size_t h2 = std::hash<std::string>{}(p.second);
            return h1 ^ (h2 << 1);
        }
    };
    
    // Shape transition and caching
    static std::unordered_map<std::pair<Shape*, std::string>, Shape*, ShapeTransitionHash> shape_transition_cache_;

private:
    
    // Common property key interning
    static std::unordered_map<std::string, std::string> interned_keys_;
    static const std::string& intern_key(const std::string& key);
    
    // Helper methods
    bool is_array_index(const std::string& key, uint32_t* index = nullptr) const;
    void update_hash_code();
    PropertyDescriptor create_data_descriptor(const Value& value, PropertyAttributes attrs) const;
};

/**
 * Property descriptor for defineProperty operations
 */
class PropertyDescriptor {
public:
    enum Type {
        Data,
        Accessor,
        Generic
    };

private:
    Type type_;
    Value value_;
    Object* getter_;
    Object* setter_;
    PropertyAttributes attributes_;
    bool has_value_ : 1;
    bool has_getter_ : 1;
    bool has_setter_ : 1;
    bool has_writable_ : 1;
    bool has_enumerable_ : 1;
    bool has_configurable_ : 1;

public:
    PropertyDescriptor();
    explicit PropertyDescriptor(const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    PropertyDescriptor(Object* getter, Object* setter, PropertyAttributes attrs = PropertyAttributes::Default);

    // Type checking
    Type get_type() const { return type_; }
    bool is_data_descriptor() const { return type_ == Data; }
    bool is_accessor_descriptor() const { return type_ == Accessor; }
    bool is_generic_descriptor() const { return type_ == Generic; }

    // Value access
    const Value& get_value() const { return value_; }
    void set_value(const Value& value);
    
    Object* get_getter() const { return getter_; }
    void set_getter(Object* getter);
    
    Object* get_setter() const { return setter_; }
    void set_setter(Object* setter);

    // Attributes
    PropertyAttributes get_attributes() const { return attributes_; }
    bool is_writable() const { return attributes_ & PropertyAttributes::Writable; }
    bool is_enumerable() const { return attributes_ & PropertyAttributes::Enumerable; }
    bool is_configurable() const { return attributes_ & PropertyAttributes::Configurable; }
    
    void set_writable(bool writable);
    void set_enumerable(bool enumerable);
    void set_configurable(bool configurable);

    // Presence checks
    bool has_value() const { return has_value_; }
    bool has_getter() const { return has_getter_; }
    bool has_setter() const { return has_setter_; }
    bool has_writable() const { return has_writable_; }
    bool has_enumerable() const { return has_enumerable_; }
    bool has_configurable() const { return has_configurable_; }

    // Operations
    bool is_complete() const;
    void complete_with_defaults();
    PropertyDescriptor merge_with(const PropertyDescriptor& other) const;
    
    std::string to_string() const;
};

/**
 * Hidden Classes (Shapes) for property layout optimization
 */
class Shape {
public:
    struct PropertyInfo {
        uint32_t offset;        // Offset in properties array
        PropertyAttributes attributes;
        uint32_t hash;          // Property name hash for fast lookup
    };

private:
    Shape* parent_;
    std::string transition_key_;
    PropertyAttributes transition_attrs_;
    std::unordered_map<std::string, PropertyInfo> properties_;
    uint32_t property_count_;
    uint32_t id_;
    
    static uint32_t next_shape_id_;

public:
    Shape();
    Shape(Shape* parent, const std::string& key, PropertyAttributes attrs);
    ~Shape() = default;

    // Shape information
    uint32_t get_id() const { return id_; }
    uint32_t get_property_count() const { return property_count_; }
    Shape* get_parent() const { return parent_; }
    
    // Property lookup
    bool has_property(const std::string& key) const;
    PropertyInfo get_property_info(const std::string& key) const;
    
    // Shape transitions
    Shape* add_property(const std::string& key, PropertyAttributes attrs);
    Shape* remove_property(const std::string& key);
    
    // Enumeration
    std::vector<std::string> get_property_keys() const;
    
    // Debugging
    std::string debug_string() const;
    
    // Static root shape
    static Shape* get_root_shape();

private:
    void rebuild_property_map();
};

/**
 * JavaScript Function object implementation
 */
class Function : public Object {
public:
    // Function call types
    enum class CallType {
        Normal,      // Regular function call
        Constructor, // new Function() call
        Method       // obj.method() call
    };

private:
    std::string name_;                                    // Function name
    std::vector<std::string> parameters_;                // Parameter names (for compatibility)
    std::vector<std::unique_ptr<class Parameter>> parameter_objects_; // Parameter objects with defaults
    std::unique_ptr<class ASTNode> body_;                // Function body AST
    class Context* closure_context_;                     // Closure context
    Object* prototype_;                                  // Function prototype
    bool is_native_;                                     // Is native C++ function
    std::function<Value(Context&, const std::vector<Value>&)> native_fn_; // Native function
    
    // Performance optimization tracking
    mutable uint32_t execution_count_;                   // Number of times function was called
    mutable bool is_hot_;                               // Is this a hot function (frequently called)
    mutable std::chrono::high_resolution_clock::time_point last_call_time_; // Last call timestamp

public:
    // Constructors
    Function(const std::string& name, 
             const std::vector<std::string>& params,
             std::unique_ptr<class ASTNode> body,
             class Context* closure_context);
             
    Function(const std::string& name,
             std::vector<std::unique_ptr<class Parameter>> params,
             std::unique_ptr<class ASTNode> body,
             class Context* closure_context);
             
    Function(const std::string& name,
             std::function<Value(Context&, const std::vector<Value>&)> native_fn,
             bool create_prototype = false);

    Function(const std::string& name,
             std::function<Value(Context&, const std::vector<Value>&)> native_fn,
             uint32_t arity,
             bool create_prototype = false);
    
    virtual ~Function() = default;

    // Function properties
    const std::string& get_name() const { return name_; }
    const std::vector<std::string>& get_parameters() const { return parameters_; }
    size_t get_arity() const { return parameters_.size(); }
    bool is_native() const { return is_native_; }
    
    // Performance tracking
    uint32_t get_execution_count() const { return execution_count_; }
    bool is_hot_function() const { return is_hot_; }
    void mark_as_hot() const { is_hot_ = true; }
    void reset_performance_stats() const { execution_count_ = 0; is_hot_ = false; }
    
    // Function execution
    virtual Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value());
    Value construct(Context& ctx, const std::vector<Value>& args);
    
    // Property override to ensure function properties work
    Value get_property(const std::string& key) const override;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::None) override;

    // Function.prototype property (for instances created by this function)
    Object* get_function_prototype() const { return prototype_; }
    void set_function_prototype(Object* proto) { prototype_ = proto; }

    // NOTE: get_prototype() and set_prototype() inherited from Object
    // They handle the internal [[Prototype]] (header_.prototype)
    // Do NOT override them - that's for Object.getPrototypeOf(Function)
    static Function* create_function_prototype();
    
    // Debugging
    std::string to_string() const;

private:
    // Variable hoisting helper
    void scan_for_var_declarations(class ASTNode* node, Context& ctx);
};

// Object factory functions
namespace ObjectFactory {
    // Memory pool management for optimized
    void initialize_memory_pools();
    std::unique_ptr<Object> get_pooled_object();
    std::unique_ptr<Object> get_pooled_array();
    void return_to_pool(std::unique_ptr<Object> obj);
    
    std::unique_ptr<Object> create_object(Object* prototype = nullptr);
    std::unique_ptr<Object> create_array(uint32_t length = 0);
    std::unique_ptr<Object> create_function();
    
    // Array prototype management
    void set_object_prototype(Object* prototype);
    Object* get_object_prototype();
    void set_array_prototype(Object* prototype);
    Object* get_array_prototype();
    std::unique_ptr<Function> create_js_function(const std::string& name,
                                                 const std::vector<std::string>& params,
                                                 std::unique_ptr<class ASTNode> body,
                                                 class Context* closure_context);
    std::unique_ptr<Function> create_js_function(const std::string& name,
                                                 std::vector<std::unique_ptr<class Parameter>> params,
                                                 std::unique_ptr<class ASTNode> body,
                                                 class Context* closure_context);
    std::unique_ptr<Function> create_native_function(const std::string& name,
                                                     std::function<Value(Context&, const std::vector<Value>&)> fn);
    std::unique_ptr<Function> create_native_function(const std::string& name,
                                                     std::function<Value(Context&, const std::vector<Value>&)> fn,
                                                     uint32_t arity);
    std::unique_ptr<Function> create_array_method(const std::string& method_name);
    std::unique_ptr<Object> create_string(const std::string& value);
    std::unique_ptr<Object> create_number(double value);
    std::unique_ptr<Object> create_boolean(bool value);
    std::unique_ptr<Object> create_error(const std::string& message);
    std::unique_ptr<Object> create_promise(Context* ctx = nullptr);
}

} // namespace Quanta

#endif // QUANTA_OBJECT_H