/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_OBJECT_H
#define QUANTA_OBJECT_H

#include "quanta/core/runtime/Value.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <chrono>

namespace Quanta {

class PropertyDescriptor;
class Context;
class Environment;
class ASTNode;
class Parameter;

class Object {
public:
    enum class ObjectType : uint8_t {
        Ordinary,
        Array,
        Arguments,
        Function,
        String,
        Number,
        Boolean,
        Date,
        RegExp,
        Error,
        Promise,
        Proxy,
        Map,
        Set,
        WeakMap,
        WeakSet,
        ArrayBuffer,
        TypedArray,
        DataView,
        Symbol,
        BigInt,
        Custom
    };

    static thread_local Context* current_context_;

private:
    struct ObjectHeader {
        Object* prototype;
        ObjectType type;
        uint8_t flags;
        uint16_t property_count;
        uint32_t hash_code;
    } header_;

    std::vector<Value> elements_;

    std::unique_ptr<std::unordered_map<std::string, Value>> overflow_properties_;

    std::unique_ptr<std::unordered_map<std::string, PropertyDescriptor>> descriptors_;

    std::unique_ptr<std::unordered_set<uint32_t>> deleted_elements_;

    // Creation order for overflow/descriptor-tracked properties; overflow_properties_ is
    // an unordered_map and has no enumeration order of its own (see get_own_property_keys).
    std::vector<std::string> property_insertion_order_;

public:
    Object(ObjectType type = ObjectType::Ordinary);
    explicit Object(Object* prototype, ObjectType type = ObjectType::Ordinary);
    virtual ~Object() = default;

    friend class Function;
    Object(const Object& other) = delete;
    Object& operator=(const Object& other) = delete;
    Object(Object&& other) noexcept = default;
    Object& operator=(Object&& other) noexcept = default;

    ObjectType get_type() const { return header_.type; }
    void set_type(ObjectType type) { header_.type = type; }
    bool is_array() const { return header_.type == ObjectType::Array; }
    bool is_function() const { return header_.type == ObjectType::Function; }
    bool is_primitive_wrapper() const {
        return header_.type == ObjectType::String || 
               header_.type == ObjectType::Number || 
               header_.type == ObjectType::Boolean;
    }
    
    virtual bool is_array_buffer() const { return header_.type == ObjectType::ArrayBuffer; }
    virtual bool is_typed_array() const { return header_.type == ObjectType::TypedArray; }
    virtual bool is_data_view() const { return header_.type == ObjectType::DataView; }
    virtual bool is_shared_array_buffer() const { return false; }
    
    virtual bool is_wasm_memory() const { return false; }
    virtual bool is_wasm_module() const { return false; }
    virtual bool is_wasm_instance() const { return false; }

    virtual Object* get_prototype() const { return header_.prototype; }
    // Non-virtual: reads the internal [[Prototype]] slot directly, bypassing Proxy's getPrototypeOf trap.
    // For internal bookkeeping (e.g. checking whether a freshly-constructed object already has a prototype) where invoking a user trap would be observably wrong.
    Object* get_prototype_raw() const { return header_.prototype; }
    void set_prototype(Object* prototype);
    bool has_prototype(Object* prototype) const;
    
    virtual bool has_property(const std::string& key) const;
    virtual bool has_own_property(const std::string& key) const;
    bool has_private_slot(const std::string& key) const;
    void add_private_field(const std::string& key, const Value& value = Value());

    virtual Value get_property(const std::string& key) const;
    Value get_property(const Value& key) const;
    Value get_own_property(const std::string& key) const;

    virtual bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    bool set_property(const Value& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    bool ordinary_set(const std::string& key, const Value& value);
    virtual bool delete_property(const std::string& key);
    
    virtual Value get_element(uint32_t index) const;

    // fp: unchecked array access
    inline Value get_element_unchecked(uint32_t index) const {
        return elements_[index];
    }

    bool set_element(uint32_t index, const Value& value);
    bool delete_element(uint32_t index);
    
    virtual std::vector<std::string> get_own_property_keys() const;
    virtual std::vector<std::string> get_enumerable_keys() const;
    virtual std::vector<std::string> get_internal_property_keys() const;
    std::vector<uint32_t> get_element_indices() const;
    
    virtual PropertyDescriptor get_property_descriptor(const std::string& key) const;
    virtual bool set_property_descriptor(const std::string& key, const PropertyDescriptor& desc);
    
    bool is_extensible() const;
    void prevent_extensions();
    void seal();
    void freeze();
    bool is_sealed() const;
    bool is_frozen() const;
    
    virtual uint32_t get_length() const;
    void set_length(uint32_t length);
    void push(const Value& value);
    Value pop();
    void unshift(const Value& value);
    Value shift();
    
    std::unique_ptr<Object> map(Function* callback, Context& ctx, const Value& thisArg = Value());
    std::unique_ptr<Object> filter(Function* callback, Context& ctx, const Value& thisArg = Value());
    void forEach(Function* callback, Context& ctx, const Value& thisArg = Value());
    Value reduce(Function* callback, const Value& initial_value, Context& ctx);
    Value reduceRight(Function* callback, const Value& initial_value, Context& ctx);
    std::unique_ptr<Object> flat(uint32_t depth = 1);
    std::unique_ptr<Object> flatMap(Function* callback, Context& ctx, const Value& thisArg = Value());
    Object* copyWithin(int32_t target, int32_t start, int32_t end = -1);
    Value findLast(Function* callback, Context& ctx, const Value& thisArg = Value());
    Value findLastIndex(Function* callback, Context& ctx, const Value& thisArg = Value());
    std::unique_ptr<Object> toSpliced(uint32_t start, uint32_t deleteCount, const std::vector<Value>& items);
    Object* fill(const Value& value, int32_t start = 0, int32_t end = -1);
    std::unique_ptr<Object> toSorted(Function* compareFn, Context& ctx);
    std::unique_ptr<Object> with_method(uint32_t index, const Value& value);
    Value at(int32_t index);
    std::unique_ptr<Object> toReversed();

    Value groupBy(Function* callback, Context& ctx);
    
    Value call(Context& ctx, const Value& this_value, const std::vector<Value>& args);
    Value construct(Context& ctx, const std::vector<Value>& args);
    
    Value to_primitive(const std::string& hint = "") const;
    std::string to_string() const;
    double to_number() const;
    bool to_boolean() const;
    
    size_t property_count() const { return header_.property_count; }
    size_t element_count() const { return elements_.size(); }
    std::string debug_string() const;
    uint32_t hash() const { return header_.hash_code; }
    
    void mark_references() const;
    size_t memory_usage() const;
    
    const std::unordered_map<std::string, PropertyDescriptor>* get_descriptors() const { return descriptors_.get(); }
    const std::unordered_map<std::string, Value>* get_overflow_properties() const { return overflow_properties_.get(); }

    Value get_internal_property(const std::string& key) const;
    void set_internal_property(const std::string& key, const Value& value);

protected:
    virtual Value internal_get(const std::string& key) const;
    virtual bool internal_set(const std::string& key, const Value& value);
    virtual bool internal_delete(const std::string& key);
    virtual std::vector<std::string> internal_own_keys() const;
    
    void ensure_element_capacity(uint32_t capacity);
    void compact_elements();
    
    void ensure_property_capacity(size_t capacity);
    bool store_in_overflow(const std::string& key, const Value& value);

public:
    void clear_properties();

private:

    static std::unordered_map<std::string, std::string> interned_keys_;
    static const std::string& intern_key(const std::string& key);
    
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

    Type get_type() const { return type_; }
    bool is_data_descriptor() const { return type_ == Data; }
    bool is_accessor_descriptor() const { return type_ == Accessor; }
    bool is_generic_descriptor() const { return type_ == Generic; }

    const Value& get_value() const { return value_; }
    void set_value(const Value& value);
    
    Object* get_getter() const { return getter_; }
    void set_getter(Object* getter);
    
    Object* get_setter() const { return setter_; }
    void set_setter(Object* setter);

    PropertyAttributes get_attributes() const { return attributes_; }
    bool is_writable() const { return attributes_ & PropertyAttributes::Writable; }
    bool is_enumerable() const { return attributes_ & PropertyAttributes::Enumerable; }
    bool is_configurable() const { return attributes_ & PropertyAttributes::Configurable; }
    
    void set_writable(bool writable);
    void set_enumerable(bool enumerable);
    void set_configurable(bool configurable);

    bool has_value() const { return has_value_; }
    bool has_getter() const { return has_getter_; }
    bool has_setter() const { return has_setter_; }
    bool has_writable() const { return has_writable_; }
    bool has_enumerable() const { return has_enumerable_; }
    bool has_configurable() const { return has_configurable_; }

    bool is_complete() const;
    void complete_with_defaults();
    PropertyDescriptor merge_with(const PropertyDescriptor& other) const;
    
    std::string to_string() const;
};

/**
 * JavaScript Function object implementation
 */
class Function : public Object {
public:
    enum class CallType {
        Normal,
        Constructor,
        Method
    };

private:
    std::string name_;
    std::vector<std::string> parameters_;
    std::vector<std::unique_ptr<class Parameter>> parameter_objects_;
    std::unique_ptr<class ASTNode> body_;
    class Context* closure_context_;
    class Environment* closure_environment_;  // lexical environment captured at creation time
    mutable Object* prototype_;  // Mutable to allow lazy initialization in get_property
    bool is_native_;
    bool is_constructor_;  // Whether this function has [[Construct]] internal method
    bool is_arrow_;        // Arrow functions have lexical this binding
    bool is_class_constructor_;  // Class constructors must be called with new
    bool is_strict_;       // Function runs in strict mode (e.g. class methods)
    bool is_param_default_;  // Created as a default param expression; uses param scope as outer env
    std::string source_text_;
    std::function<Value(Context&, const std::vector<Value>&)> native_fn_;

    mutable uint32_t execution_count_;
    mutable bool is_hot_;
    mutable std::chrono::high_resolution_clock::time_point last_call_time_;

public:
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

    const std::string& get_name() const { return name_; }
    void set_name(const std::string& name);
    const std::vector<std::string>& get_parameters() const { return parameters_; }
    const std::vector<std::unique_ptr<class Parameter>>& get_parameter_objects() const { return parameter_objects_; }
    size_t get_arity() const { return parameters_.size(); }
    bool is_native() const { return is_native_; }
    bool is_constructor() const { return is_constructor_; }
    void set_is_constructor(bool value) { is_constructor_ = value; }
    bool is_arrow() const { return is_arrow_; }
    class Context* get_closure_context() const { return closure_context_; }
    class Environment* get_closure_environment() const { return closure_environment_; }
    void set_is_arrow(bool value) { is_arrow_ = value; }
    bool is_class_constructor() const { return is_class_constructor_; }
    void set_is_class_constructor(bool value) { is_class_constructor_ = value; }
    bool is_strict() const { return is_strict_; }
    void set_is_strict(bool value) { is_strict_ = value; }
    bool is_param_default() const { return is_param_default_; }
    void set_is_param_default(bool v) { is_param_default_ = v; }
    const std::string& get_source_text() const { return source_text_; }
    void set_source_text(const std::string& s) { source_text_ = s; }
    
    uint32_t get_execution_count() const { return execution_count_; }
    bool is_hot_function() const { return is_hot_; }
    void mark_as_hot() const { is_hot_ = true; }
    void reset_performance_stats() const { execution_count_ = 0; is_hot_ = false; }
    
    virtual Value call(Context& ctx, const std::vector<Value>& args, Value this_value = Value());
    Value construct(Context& ctx, const std::vector<Value>& args);
    
    Value get_property(const std::string& key) const override;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default) override;
    std::vector<std::string> get_own_property_keys() const override;
    std::vector<std::string> get_internal_property_keys() const override;
    bool has_own_property(const std::string& key) const {
        if (key == "prototype" && prototype_ != nullptr) return true;
        return Object::has_own_property(key);
    }

    Object* get_function_prototype() const { return prototype_; }
    void set_function_prototype(Object* proto) { prototype_ = proto; }

    static Function* create_function_prototype();
    
    std::string to_string() const;

protected:
    void scan_for_var_declarations(class ASTNode* node, Context& ctx);
};

namespace ObjectFactory {
    void initialize_memory_pools();
    std::unique_ptr<Object> get_pooled_object();
    std::unique_ptr<Object> get_pooled_array();
    void return_to_pool(std::unique_ptr<Object> obj);
    
    std::unique_ptr<Object> create_object(Object* prototype = nullptr);
    std::unique_ptr<Object> create_array(uint32_t length = 0);
    std::unique_ptr<Object> create_function();
    
    void set_object_prototype(Object* prototype);
    Object* get_object_prototype();
    void set_array_prototype(Object* prototype);
    Object* get_array_prototype();
    void set_function_prototype(Object* prototype);
    Object* get_function_prototype();
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
    std::unique_ptr<Function> create_native_constructor(const std::string& name,
                                                        std::function<Value(Context&, const std::vector<Value>&)> fn,
                                                        uint32_t arity = 1);
    std::unique_ptr<Function> create_array_method(const std::string& method_name);
    std::unique_ptr<Object> create_string(const std::string& value);
    std::unique_ptr<Object> create_number(double value);
    std::unique_ptr<Object> create_boolean(bool value);
    std::unique_ptr<Object> create_error(const std::string& message);
    std::unique_ptr<Object> create_promise(Context* ctx = nullptr);
}

}

#endif
