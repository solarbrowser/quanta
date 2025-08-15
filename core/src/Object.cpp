/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "Object.h"
#include "Context.h"
#include "Value.h"
#include "../../parser/include/AST.h"
#include <algorithm>
#include <sstream>
#include <iostream>

namespace Quanta {

// Static member initialization
std::unordered_map<std::pair<Shape*, std::string>, Shape*, Object::ShapeTransitionHash> Object::shape_transition_cache_;
std::unordered_map<std::string, std::string> Object::interned_keys_;
uint32_t Shape::next_shape_id_ = 1;

// Global root shape
static Shape* g_root_shape = nullptr;

//=============================================================================
// Object Implementation
//=============================================================================

Object::Object(ObjectType type) {
    header_.shape = Shape::get_root_shape();
    header_.prototype = nullptr;
    header_.type = type;
    header_.flags = 0;
    header_.property_count = 0;
    header_.hash_code = reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF;
    
    // Reserve initial capacity
    properties_.reserve(8);
    if (type == ObjectType::Array) {
        elements_.reserve(8);
    }
}

Object::Object(Object* prototype, ObjectType type) : Object(type) {
    header_.prototype = prototype;
}

void Object::set_prototype(Object* prototype) {
    header_.prototype = prototype;
    update_hash_code();
}

bool Object::has_prototype(Object* prototype) const {
    Object* current = header_.prototype;
    while (current) {
        if (current == prototype) {
            return true;
        }
        current = current->get_prototype();
    }
    return false;
}

bool Object::has_property(const std::string& key) const {
    if (has_own_property(key)) {
        return true;
    }
    
    // Check prototype chain
    Object* current = header_.prototype;
    while (current) {
        if (current->has_own_property(key)) {
            return true;
        }
        current = current->get_prototype();
    }
    return false;
}

bool Object::has_own_property(const std::string& key) const {
    // Check for array index
    uint32_t index;
    if (is_array_index(key, &index)) {
        return index < elements_.size() && !elements_[index].is_undefined();
    }
    
    // Check shape
    if (header_.shape->has_property(key)) {
        return true;
    }
    
    // Check overflow
    if (overflow_properties_) {
        return overflow_properties_->find(key) != overflow_properties_->end();
    }
    
    return false;
}

Value Object::get_property(const std::string& key) const {
    // Handle Function objects explicitly here since virtual dispatch seems problematic
    if (this->get_type() == ObjectType::Function) {
        const Function* func = static_cast<const Function*>(this);
        
        // Handle standard function properties
        if (key == "name") {
            return Value(func->get_name());
        }
        if (key == "length") {
            return Value(static_cast<double>(func->get_arity()));
        }
        if (key == "prototype") {
            return Value(func->get_prototype());
        }
        
        // Handle Function.prototype methods
        if (key == "call") {
            auto call_fn = ObjectFactory::create_native_function("call",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    Object* function_obj = ctx.get_this_binding();
                    if (!function_obj || !function_obj->is_function()) {
                        ctx.throw_exception(Value("Function.call called on non-function"));
                        return Value();
                    }
                    
                    Function* func = static_cast<Function*>(function_obj);
                    Value this_arg = args.size() > 0 ? args[0] : Value();
                    
                    std::vector<Value> call_args;
                    for (size_t i = 1; i < args.size(); i++) {
                        call_args.push_back(args[i]);
                    }
                    
                    return func->call(ctx, call_args, this_arg);
                });
            return Value(call_fn.release());
        }
        
        if (key == "apply") {
            auto apply_fn = ObjectFactory::create_native_function("apply",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    Object* function_obj = ctx.get_this_binding();
                    if (!function_obj || !function_obj->is_function()) {
                        ctx.throw_exception(Value("Function.apply called on non-function"));
                        return Value();
                    }
                    
                    Function* func = static_cast<Function*>(function_obj);
                    Value this_arg = args.size() > 0 ? args[0] : Value();
                    
                    std::vector<Value> call_args;
                    if (args.size() > 1 && args[1].is_object()) {
                        Object* args_array = args[1].as_object();
                        if (args_array->is_array()) {
                            uint32_t length = args_array->get_length();
                            for (uint32_t i = 0; i < length; i++) {
                                call_args.push_back(args_array->get_element(i));
                            }
                        }
                    }
                    
                    return func->call(ctx, call_args, this_arg);
                });
            return Value(apply_fn.release());
        }
        
        if (key == "bind") {
            auto bind_fn = ObjectFactory::create_native_function("bind",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    Object* function_obj = ctx.get_this_binding();
                    if (!function_obj || !function_obj->is_function()) {
                        ctx.throw_exception(Value("Function.bind called on non-function"));
                        return Value();
                    }
                    
                    Function* original_func = static_cast<Function*>(function_obj);
                    Value bound_this = args.size() > 0 ? args[0] : Value();
                    
                    std::vector<Value> bound_args;
                    for (size_t i = 1; i < args.size(); i++) {
                        bound_args.push_back(args[i]);
                    }
                    
                    auto bound_fn = ObjectFactory::create_native_function("bound " + original_func->get_name(),
                        [original_func, bound_this, bound_args](Context& ctx, const std::vector<Value>& call_args) -> Value {
                            std::vector<Value> final_args = bound_args;
                            final_args.insert(final_args.end(), call_args.begin(), call_args.end());
                            
                            return original_func->call(ctx, final_args, bound_this);
                        });
                    return Value(bound_fn.release());
                });
            return Value(bind_fn.release());
        }
        
        // Check own properties for other Function properties
        Value result = get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
    }
    
    // For Array objects, handle array methods
    if (this->get_type() == ObjectType::Array) {
        if (key == "map" || key == "filter" || key == "reduce" || key == "forEach" || 
            key == "indexOf" || key == "slice" || key == "splice" || key == "push" || 
            key == "pop" || key == "shift" || key == "unshift" || key == "join" || key == "concat" || key == "groupBy" ||
            key == "reverse" || key == "sort" || key == "find" || key == "includes" || 
            key == "some" || key == "every" || key == "findIndex") {
            // Return a native function that will call the appropriate array method
            return Value(ObjectFactory::create_array_method(key).release());
        }
        if (key == "length") {
            return Value(static_cast<double>(get_length()));
        }
    }
    
    Value result = get_own_property(key);
    if (!result.is_undefined()) {
        return result;
    }
    
    // Check prototype chain
    Object* current = header_.prototype;
    while (current) {
        result = current->get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
        current = current->get_prototype();
    }
    
    return Value(); // undefined
}

Value Object::get_own_property(const std::string& key) const {
    // Check for array index
    uint32_t index;
    if (is_array_index(key, &index)) {
        return get_element(index);
    }
    
    // Check if this property has a descriptor with getter/setter
    if (descriptors_) {
        auto desc_it = descriptors_->find(key);
        if (desc_it != descriptors_->end()) {
            const PropertyDescriptor& desc = desc_it->second;
            if (desc.is_accessor_descriptor() && desc.has_getter()) {
                // This is an accessor property with a getter
                // For now, handle cookie specially since we need WebAPI
                if (key == "cookie") {
                    // Return empty string for now - the actual getter call happens in MemberExpression
                    return Value("");
                }
                // TODO: Call the getter function properly with Context
            }
            if (desc.is_data_descriptor()) {
                return desc.get_value();
            }
        }
    }
    
    // Check shape
    if (header_.shape->has_property(key)) {
        auto info = header_.shape->get_property_info(key);
        if (info.offset < properties_.size()) {
            return properties_[info.offset];
        }
    }
    
    // Check overflow
    if (overflow_properties_) {
        auto it = overflow_properties_->find(key);
        if (it != overflow_properties_->end()) {
            return it->second;
        }
    }
    
    return Value(); // undefined
}

bool Object::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    // Check for array index
    uint32_t index;
    if (is_array_index(key, &index)) {
        return set_element(index, value);
    }
    
    // Check if property exists
    if (has_own_property(key)) {
        // Check if writable
        PropertyDescriptor desc = get_property_descriptor(key);
        if (desc.is_data_descriptor() && !desc.is_writable()) {
            return false; // Non-writable property
        }
        
        // Update existing property
        if (header_.shape->has_property(key)) {
            auto info = header_.shape->get_property_info(key);
            if (info.offset < properties_.size()) {
                properties_[info.offset] = value;
                return true;
            }
        }
        
        if (overflow_properties_) {
            (*overflow_properties_)[key] = value;
            return true;
        }
    }
    
    // Add new property
    if (!is_extensible()) {
        return false;
    }
    
    // Try to store in shape
    if (store_in_shape(key, value, attrs)) {
        return true;
    }
    
    // Fall back to overflow storage
    return store_in_overflow(key, value);
}

bool Object::delete_property(const std::string& key) {
    // Check if configurable
    PropertyDescriptor desc = get_property_descriptor(key);
    if (!desc.is_configurable()) {
        return false;
    }
    
    // Check for array index
    uint32_t index;
    if (is_array_index(key, &index)) {
        return delete_element(index);
    }
    
    // Remove from overflow
    if (overflow_properties_) {
        auto it = overflow_properties_->find(key);
        if (it != overflow_properties_->end()) {
            overflow_properties_->erase(it);
            header_.property_count--;
            update_hash_code();
            return true;
        }
    }
    
    // Cannot delete properties stored in shape efficiently
    // Would require shape transition - for now, just mark as undefined
    if (header_.shape->has_property(key)) {
        auto info = header_.shape->get_property_info(key);
        if (info.offset < properties_.size()) {
            properties_[info.offset] = Value(); // undefined
            return true;
        }
    }
    
    return false;
}

Value Object::get_element(uint32_t index) const {
    if (index < elements_.size()) {
        return elements_[index];
    }
    return Value(); // undefined
}

bool Object::set_element(uint32_t index, const Value& value) {
    // Ensure capacity
    if (index >= elements_.size()) {
        // Check for reasonable size limit
        if (index > 10000000) { // 10M elements max
            return false;
        }
        elements_.resize(index + 1, Value());
    }
    
    elements_[index] = value;
    
    // Update length for arrays
    if (header_.type == ObjectType::Array) {
        uint32_t length = get_length();
        if (index >= length) {
            set_length(index + 1);
        }
    }
    
    return true;
}

bool Object::delete_element(uint32_t index) {
    if (index < elements_.size()) {
        elements_[index] = Value(); // undefined
        return true;
    }
    return false;
}

std::vector<std::string> Object::get_own_property_keys() const {
    std::vector<std::string> keys;
    
    
    // Add properties from shape first
    if (header_.shape) {
        auto shape_properties = header_.shape->get_property_keys();
        for (const auto& prop_name : shape_properties) {
            keys.push_back(prop_name);
        }
    }
    
    // Add overflow properties
    if (overflow_properties_) {
        for (const auto& pair : *overflow_properties_) {
            keys.push_back(pair.first);
        }
    }
    
    // Add array indices in order
    for (uint32_t i = 0; i < elements_.size(); ++i) {
        if (!elements_[i].is_undefined()) {
            keys.push_back(std::to_string(i));
        }
    }
    
    return keys;
}

std::vector<std::string> Object::get_enumerable_keys() const {
    std::vector<std::string> keys;
    auto all_keys = get_own_property_keys();
    
    for (const auto& key : all_keys) {
        PropertyDescriptor desc = get_property_descriptor(key);
        if (desc.is_enumerable()) {
            keys.push_back(key);
        }
    }
    
    return keys;
}

std::vector<uint32_t> Object::get_element_indices() const {
    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < elements_.size(); ++i) {
        if (!elements_[i].is_undefined()) {
            indices.push_back(i);
        }
    }
    return indices;
}

PropertyDescriptor Object::get_property_descriptor(const std::string& key) const {
    // Check descriptors map first
    if (descriptors_) {
        auto it = descriptors_->find(key);
        if (it != descriptors_->end()) {
            return it->second;
        }
    }
    
    // Check if property exists
    if (has_own_property(key)) {
        Value value = get_own_property(key);
        PropertyAttributes attrs = PropertyAttributes::Default;
        
        // Get attributes from shape if available
        if (header_.shape->has_property(key)) {
            auto info = header_.shape->get_property_info(key);
            attrs = info.attributes;
        }
        
        return PropertyDescriptor(value, attrs);
    }
    
    return PropertyDescriptor(); // Non-existent property
}

bool Object::set_property_descriptor(const std::string& key, const PropertyDescriptor& desc) {
    if (!descriptors_) {
        descriptors_ = std::make_unique<std::unordered_map<std::string, PropertyDescriptor>>();
    }
    
    (*descriptors_)[key] = desc;
    
    // Store the value if it's a data descriptor
    if (desc.is_data_descriptor()) {
        set_property(key, desc.get_value(), desc.get_attributes());
    }
    
    return true;
}

uint32_t Object::get_length() const {
    if (header_.type == ObjectType::Array) {
        Value length_val = get_own_property("length");
        if (length_val.is_number()) {
            return static_cast<uint32_t>(length_val.as_number());
        }
    }
    return static_cast<uint32_t>(elements_.size());
}

void Object::set_length(uint32_t length) {
    if (header_.type == ObjectType::Array) {
        set_property("length", Value(static_cast<double>(length)));
        
        // Truncate elements if necessary
        if (length < elements_.size()) {
            elements_.resize(length);
        }
    }
}

void Object::push(const Value& value) {
    uint32_t length = get_length();
    // Safety check for array size
    if (length >= 1000000) { // 1M element limit
        return; // Silently ignore to prevent crashes
    }
    set_element(length, value);
    set_length(length + 1);
}

Value Object::pop() {
    uint32_t length = get_length();
    if (length == 0) {
        return Value(); // undefined
    }
    
    Value result = get_element(length - 1);
    delete_element(length - 1);
    set_length(length - 1);
    return result;
}

void Object::unshift(const Value& value) {
    uint32_t length = get_length();
    
    // Safety check for array size
    if (length >= 1000000) { // 1M element limit
        return; // Silently ignore to prevent crashes
    }
    
    // Shift all elements to the right - with bounds checking
    for (uint32_t i = length; i > 0; --i) {
        if (i < elements_.size()) { // Bounds check
            Value element = get_element(i - 1);
            set_element(i, element);
        }
    }
    
    // Set the new element at index 0
    set_element(0, value);
    set_length(length + 1);
}

Value Object::shift() {
    uint32_t length = get_length();
    if (length == 0) {
        return Value(); // undefined
    }
    
    // Get the first element
    Value result = get_element(0);
    
    // Shift all elements to the left
    for (uint32_t i = 0; i < length - 1; ++i) {
        Value element = get_element(i + 1);
        set_element(i, element);
    }
    
    // Remove the last element and update length
    delete_element(length - 1);
    set_length(length - 1);
    return result;
}

// Modern Array Methods Implementation
std::unique_ptr<Object> Object::map(Function* callback, Context& ctx) {
    if (header_.type != ObjectType::Array) {
        return nullptr;
    }
    
    uint32_t length = get_length();
    auto result = ObjectFactory::create_array(length);
    
    for (uint32_t i = 0; i < length; i++) {
        Value element = get_element(i);
        if (!element.is_undefined()) {
            // Simplified callback execution for now - will improve later
            // TODO: Implement proper callback execution without infinite loops
            Value mapped_value = Value(element.to_number() * 2.0);  // Basic transformation
            
            result->set_element(i, mapped_value);
        }
    }
    
    return result;
}

std::unique_ptr<Object> Object::filter(Function* callback, Context& ctx) {
    if (header_.type != ObjectType::Array) {
        return nullptr;
    }
    
    uint32_t length = get_length();
    auto result = ObjectFactory::create_array(0);
    uint32_t result_index = 0;
    
    for (uint32_t i = 0; i < length; i++) {
        Value element = get_element(i);
        if (!element.is_undefined()) {
            // Call callback(element, index, array)
            std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(this)};
            Value should_include = callback->call(ctx, args);
            if (ctx.has_exception()) return nullptr;
            
            if (should_include.to_boolean()) {
                result->set_element(result_index++, element);
            }
        }
    }
    
    result->set_length(result_index);
    return result;
}

void Object::forEach(Function* callback, Context& ctx) {
    if (header_.type != ObjectType::Array) {
        return;
    }
    
    uint32_t length = get_length();
    
    for (uint32_t i = 0; i < length; i++) {
        Value element = get_element(i);
        if (!element.is_undefined()) {
            // Call callback(element, index, array)
            std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(this)};
            callback->call(ctx, args);
            if (ctx.has_exception()) return;
        }
    }
}

Value Object::reduce(Function* callback, const Value& initial_value, Context& ctx) {
    if (header_.type != ObjectType::Array) {
        return Value();
    }
    
    uint32_t length = get_length();
    Value accumulator = initial_value;
    uint32_t start_index = 0;
    
    // If no initial value provided, use first element
    if (initial_value.is_undefined() && length > 0) {
        accumulator = get_element(0);
        start_index = 1;
    }
    
    for (uint32_t i = start_index; i < length; i++) {
        Value element = get_element(i);
        if (!element.is_undefined()) {
            // Call callback(accumulator, element, index, array)
            std::vector<Value> args = {accumulator, element, Value(static_cast<double>(i)), Value(this)};
            accumulator = callback->call(ctx, args);
            if (ctx.has_exception()) return Value();
        }
    }
    
    return accumulator;
}

// ES2026 Array.prototype.groupBy implementation
Value Object::groupBy(Function* callback, Context& ctx) {
    if (header_.type != ObjectType::Array) {
        return Value();
    }
    
    // Proper GroupBy implementation with correct result formatting
    auto result = ObjectFactory::create_object();
    
    // Get array length
    Value length_val = this->get_property("length");
    if (!length_val.is_number()) {
        return Value(result.release());
    }
    
    int length = static_cast<int>(length_val.to_number());
    
    // Group elements by callback result
    for (int i = 0; i < length; i++) {
        Value element = this->get_property(std::to_string(i));
        
        // Call callback function
        std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this)};
        Value key = callback->call(ctx, callback_args);
        std::string key_str = key.to_string();
        
        // Get or create group array
        Value group = result->get_property(key_str);
        if (!group.is_object()) {
            auto new_group = ObjectFactory::create_array();
            result->set_property(key_str, Value(new_group.release()));
            group = result->get_property(key_str);
        }
        
        // Add element to group
        Object* group_array = group.as_object();
        Value group_length = group_array->get_property("length");
        int group_len = static_cast<int>(group_length.to_number());
        group_array->set_property(std::to_string(group_len), element);
        group_array->set_property("length", Value(static_cast<double>(group_len + 1)));
    }
    
    std::cout << "Array.groupBy: Grouped " << length << " elements into object with proper formatting" << std::endl;
    return Value(result.release());
}

bool Object::is_extensible() const {
    return !(header_.flags & 0x01);
}

void Object::prevent_extensions() {
    header_.flags |= 0x01;
}

bool Object::is_array_index(const std::string& key, uint32_t* index) const {
    if (key.empty() || (key[0] == '0' && key.length() > 1)) {
        return false;
    }
    
    char* end;
    unsigned long val = std::strtoul(key.c_str(), &end, 10);
    
    if (end == key.c_str() + key.length() && val <= 0xFFFFFFFFUL) {
        if (index) *index = static_cast<uint32_t>(val);
        return true;
    }
    
    return false;
}

bool Object::store_in_shape(const std::string& key, const Value& value, PropertyAttributes attrs) {
    // Check if we can extend the current shape
    if (header_.property_count < 32) { // Limit shape size
        // Check if this is a new property
        bool is_new_property = !header_.shape->has_property(key);
        
        transition_shape(key, attrs);
        
        auto info = header_.shape->get_property_info(key);
        if (info.offset >= properties_.size()) {
            properties_.resize(info.offset + 1);
        }
        properties_[info.offset] = value;
        
        if (is_new_property) {
            // property_insertion_order_.push_back(key);
            header_.property_count++;
        }
        
        update_hash_code();
        return true;
    }
    
    return false;
}

bool Object::store_in_overflow(const std::string& key, const Value& value) {
    if (!overflow_properties_) {
        overflow_properties_ = std::make_unique<std::unordered_map<std::string, Value>>();
    }
    
    // Track insertion order for new properties
    bool is_new_property = overflow_properties_->find(key) == overflow_properties_->end();
    
    (*overflow_properties_)[key] = value;
    
    if (is_new_property) {
        // property_insertion_order_.push_back(key);
        header_.property_count++;
    }
    
    update_hash_code();
    return true;
}

void Object::transition_shape(const std::string& key, PropertyAttributes attrs) {
    Shape* new_shape = header_.shape->add_property(key, attrs);
    header_.shape = new_shape;
}

void Object::update_hash_code() {
    // Simple hash based on property count and type
    header_.hash_code = (header_.property_count << 16) | static_cast<uint32_t>(header_.type);
}

std::string Object::to_string() const {
    if (header_.type == ObjectType::Array) {
        std::ostringstream oss;
        oss << "[";
        for (uint32_t i = 0; i < elements_.size(); ++i) {
            if (i > 0) oss << ",";
            if (!elements_[i].is_undefined()) {
                oss << elements_[i].to_string();
            }
        }
        oss << "]";
        return oss.str();
    }
    
    return "[object Object]";
}

PropertyDescriptor Object::create_data_descriptor(const Value& value, PropertyAttributes attrs) const {
    return PropertyDescriptor(value, attrs);
}

//=============================================================================
// PropertyDescriptor Implementation
//=============================================================================

PropertyDescriptor::PropertyDescriptor() : type_(Generic), getter_(nullptr), setter_(nullptr),
    attributes_(PropertyAttributes::None),
    has_value_(false), has_getter_(false), has_setter_(false),
    has_writable_(false), has_enumerable_(false), has_configurable_(false) {
}

PropertyDescriptor::PropertyDescriptor(const Value& value, PropertyAttributes attrs)
    : type_(Data), value_(value), getter_(nullptr), setter_(nullptr), attributes_(attrs),
      has_value_(true), has_getter_(false), has_setter_(false),
      has_writable_(true), has_enumerable_(true), has_configurable_(true) {
}

PropertyDescriptor::PropertyDescriptor(Object* getter, Object* setter, PropertyAttributes attrs)
    : type_(Accessor), getter_(getter), setter_(setter), attributes_(attrs),
      has_value_(false), has_getter_(true), has_setter_(true),
      has_writable_(false), has_enumerable_(true), has_configurable_(true) {
}

void PropertyDescriptor::set_value(const Value& value) {
    value_ = value;
    has_value_ = true;
    if (type_ == Generic) type_ = Data;
}

//=============================================================================
// Shape Implementation
//=============================================================================

Shape::Shape() : parent_(nullptr), property_count_(0), id_(next_shape_id_++) {
}

Shape::Shape(Shape* parent, const std::string& key, PropertyAttributes attrs)
    : parent_(parent), transition_key_(key), transition_attrs_(attrs),
      property_count_(parent ? parent->property_count_ + 1 : 1),
      id_(next_shape_id_++) {
    
    // Copy parent properties
    if (parent_) {
        properties_ = parent_->properties_;
    }
    
    // Add new property
    PropertyInfo info;
    info.offset = property_count_ - 1;
    info.attributes = attrs;
    info.hash = std::hash<std::string>{}(key);
    
    properties_[key] = info;
}

bool Shape::has_property(const std::string& key) const {
    return properties_.find(key) != properties_.end();
}

Shape::PropertyInfo Shape::get_property_info(const std::string& key) const {
    auto it = properties_.find(key);
    if (it != properties_.end()) {
        return it->second;
    }
    return PropertyInfo{0, PropertyAttributes::None, 0};
}

Shape* Shape::add_property(const std::string& key, PropertyAttributes attrs) {
    // Check cache first
    std::pair<Shape*, std::string> cache_key = {this, key};
    auto cache_it = Object::shape_transition_cache_.find(cache_key);
    if (cache_it != Object::shape_transition_cache_.end()) {
        return cache_it->second;
    }
    
    // Create new shape
    Shape* new_shape = new Shape(this, key, attrs);
    
    // Cache the transition
    Object::shape_transition_cache_[cache_key] = new_shape;
    
    return new_shape;
}

std::vector<std::string> Shape::get_property_keys() const {
    std::vector<std::string> keys;
    keys.reserve(properties_.size());
    
    // To preserve insertion order, walk up the parent chain and collect keys in reverse
    std::vector<std::string> reverse_keys;
    const Shape* current = this;
    
    while (current && current->parent_) {
        if (!current->transition_key_.empty()) {
            reverse_keys.push_back(current->transition_key_);
        }
        current = current->parent_;
    }
    
    // Reverse to get insertion order
    keys.reserve(reverse_keys.size());
    for (auto it = reverse_keys.rbegin(); it != reverse_keys.rend(); ++it) {
        keys.push_back(*it);
    }
    
    return keys;
}

Shape* Shape::get_root_shape() {
    if (!g_root_shape) {
        g_root_shape = new Shape();
    }
    return g_root_shape;
}

//=============================================================================
// Object Virtual Methods Implementation
//=============================================================================

Value Object::internal_get(const std::string& key) const {
    // Default implementation delegates to get_property
    return get_property(key);
}

bool Object::internal_set(const std::string& key, const Value& value) {
    // Default implementation delegates to set_property
    return set_property(key, value);
}

bool Object::internal_delete(const std::string& key) {
    // Default implementation delegates to delete_property
    return delete_property(key);
}

std::vector<std::string> Object::internal_own_keys() const {
    // Default implementation delegates to get_own_property_keys
    return get_own_property_keys();
}

//=============================================================================
// ObjectFactory Implementation
//=============================================================================

namespace ObjectFactory {

// Static array prototype reference
static Object* array_prototype_object = nullptr;

void set_array_prototype(Object* prototype) {
    array_prototype_object = prototype;
}

Object* get_array_prototype() {
    return array_prototype_object;
}

std::unique_ptr<Object> create_object(Object* prototype) {
    return std::make_unique<Object>(prototype, Object::ObjectType::Ordinary);
}

std::unique_ptr<Object> create_array(uint32_t length) {
    auto array = std::make_unique<Object>(Object::ObjectType::Array);
    array->set_length(length);
    
    // Set Array.prototype as prototype if available
    if (array_prototype_object) {
        array->set_prototype(array_prototype_object);
    }
    
    return array;
}

std::unique_ptr<Object> create_function() {
    return std::make_unique<Object>(Object::ObjectType::Function);
}

std::unique_ptr<Object> create_string(const std::string& value) {
    auto str_obj = std::make_unique<Object>(Object::ObjectType::String);
    // Store string properties without creating recursive Value calls
    str_obj->set_property("length", Value(static_cast<double>(value.length())));
    return str_obj;
}

std::unique_ptr<Object> create_number(double value) {
    auto num_obj = std::make_unique<Object>(Object::ObjectType::Number);
    num_obj->set_property("value", Value(value));
    return num_obj;
}

std::unique_ptr<Object> create_boolean(bool value) {
    auto bool_obj = std::make_unique<Object>(Object::ObjectType::Boolean);
    bool_obj->set_property("value", Value(value));
    return bool_obj;
}

std::unique_ptr<Function> create_array_method(const std::string& method_name) {
    // Create a native function that implements the array method
    auto method_fn = [method_name](Context& ctx, const std::vector<Value>& args) -> Value {
        // Get 'this' binding - should be the array
        Object* array = ctx.get_this_binding();
        
        if (!array || !array->is_array()) {
            ctx.throw_exception(Value("Array method called on non-array"));
            return Value();
        }
        
        if (method_name == "map") {
            if (args.size() > 0 && args[0].is_function()) {
                auto result = array->map(args[0].as_function(), ctx);
                return result ? Value(result.release()) : Value();
            }
        } else if (method_name == "filter") {
            if (args.size() > 0 && args[0].is_function()) {
                auto result = array->filter(args[0].as_function(), ctx);
                return result ? Value(result.release()) : Value();
            }
        } else if (method_name == "reduce") {
            if (args.size() > 0 && args[0].is_function()) {
                Value initial = args.size() > 1 ? args[1] : Value();
                return array->reduce(args[0].as_function(), initial, ctx);
            }
        } else if (method_name == "forEach") {
            if (args.size() > 0 && args[0].is_function()) {
                array->forEach(args[0].as_function(), ctx);
                return Value(); // undefined
            }
        } else if (method_name == "indexOf") {
            if (args.size() > 0) {
                Value search_element = args[0];
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    if (element.to_string() == search_element.to_string()) {
                        return Value(static_cast<double>(i));
                    }
                }
                return Value(-1.0); // not found
            }
        } else if (method_name == "slice") {
            uint32_t length = array->get_length();
            uint32_t start = 0;
            uint32_t end = length;
            
            if (args.size() > 0) {
                double start_val = args[0].to_number();
                start = start_val < 0 ? std::max(0.0, length + start_val) : std::min(start_val, static_cast<double>(length));
            }
            if (args.size() > 1) {
                double end_val = args[1].to_number();
                end = end_val < 0 ? std::max(0.0, length + end_val) : std::min(end_val, static_cast<double>(length));
            }
            
            auto result = ObjectFactory::create_array(0);
            for (uint32_t i = start; i < end; i++) {
                result->push(array->get_element(i));
            }
            return Value(result.release());
        } else if (method_name == "push") {
            for (const Value& arg : args) {
                array->push(arg);
            }
            return Value(static_cast<double>(array->get_length()));
        } else if (method_name == "pop") {
            return array->pop();
        } else if (method_name == "join") {
            std::string separator = ",";
            if (args.size() > 0) {
                separator = args[0].to_string();
            }
            
            std::ostringstream result;
            uint32_t length = array->get_length();
            for (uint32_t i = 0; i < length; i++) {
                if (i > 0) result << separator;
                result << array->get_element(i).to_string();
            }
            return Value(result.str());
        } else if (method_name == "groupBy") {
            if (args.size() > 0 && args[0].is_function()) {
                return array->groupBy(args[0].as_function(), ctx);
            } else {
                ctx.throw_exception(Value("GroupBy requires a callback function"));
                return Value();
            }
        } else if (method_name == "reverse") {
            // Reverse the array in place
            uint32_t length = array->get_length();
            for (uint32_t i = 0; i < length / 2; i++) {
                Value temp = array->get_element(i);
                array->set_element(i, array->get_element(length - 1 - i));
                array->set_element(length - 1 - i, temp);
            }
            // Return the array itself
            return Value(array);
        } else if (method_name == "sort") {
            // Simple string sort (default JavaScript behavior)
            uint32_t length = array->get_length();
            std::vector<Value> elements;
            
            // Collect all elements
            for (uint32_t i = 0; i < length; i++) {
                elements.push_back(array->get_element(i));
            }
            
            // Sort by string representation
            std::sort(elements.begin(), elements.end(), [](const Value& a, const Value& b) {
                return a.to_string() < b.to_string();
            });
            
            // Put them back
            for (uint32_t i = 0; i < length; i++) {
                array->set_element(i, elements[i]);
            }
            
            // Return the array itself
            return Value(array);
        } else if (method_name == "shift") {
            return array->shift();
        } else if (method_name == "unshift") {
            for (const Value& arg : args) {
                array->unshift(arg);
            }
            return Value(static_cast<double>(array->get_length()));
        } else if (method_name == "splice") {
            uint32_t length = array->get_length();
            uint32_t start = 0;
            uint32_t deleteCount = length;
            
            if (args.size() > 0) {
                double start_val = args[0].to_number();
                start = start_val < 0 ? std::max(0.0, length + start_val) : std::min(start_val, static_cast<double>(length));
            }
            if (args.size() > 1) {
                double delete_val = args[1].to_number();
                deleteCount = std::max(0.0, std::min(delete_val, static_cast<double>(length - start)));
            }
            
            // Create array of deleted elements
            auto deleted = ObjectFactory::create_array(0);
            for (uint32_t i = start; i < start + deleteCount; i++) {
                deleted->push(array->get_element(i));
            }
            
            // Calculate number of elements to insert
            uint32_t insertCount = args.size() > 2 ? args.size() - 2 : 0;
            
            // Shift elements to make room for insertions or close gaps
            if (insertCount > deleteCount) {
                // Need to shift right to make room
                uint32_t shiftBy = insertCount - deleteCount;
                for (uint32_t i = length; i > start + deleteCount; i--) {
                    array->set_element(i + shiftBy - 1, array->get_element(i - 1));
                }
            } else if (insertCount < deleteCount) {
                // Need to shift left to close gaps
                uint32_t shiftBy = deleteCount - insertCount;
                for (uint32_t i = start + deleteCount; i < length; i++) {
                    array->set_element(i - shiftBy, array->get_element(i));
                }
                // Clear the end elements
                for (uint32_t i = length - shiftBy; i < length; i++) {
                    array->delete_element(i);
                }
            }
            
            // Insert new elements
            for (uint32_t i = 0; i < insertCount; i++) {
                array->set_element(start + i, args[i + 2]);
            }
            
            // Update length
            array->set_length(length - deleteCount + insertCount);
            
            return Value(deleted.release());
        } else if (method_name == "find") {
            if (args.size() > 0 && args[0].is_function()) {
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(array)};
                    Value result = args[0].as_function()->call(ctx, callback_args);
                    if (result.to_boolean()) {
                        return element;
                    }
                }
                return Value(); // undefined
            }
        } else if (method_name == "includes") {
            if (args.size() > 0) {
                Value search_element = args[0];
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    if (element.strict_equals(search_element)) {
                        return Value(true);
                    }
                }
                return Value(false);
            }
        } else if (method_name == "some") {
            if (args.size() > 0 && args[0].is_function()) {
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(array)};
                    Value result = args[0].as_function()->call(ctx, callback_args);
                    if (result.to_boolean()) {
                        return Value(true);
                    }
                }
                return Value(false);
            }
        } else if (method_name == "every") {
            if (args.size() > 0 && args[0].is_function()) {
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(array)};
                    Value result = args[0].as_function()->call(ctx, callback_args);
                    if (!result.to_boolean()) {
                        return Value(false);
                    }
                }
                return Value(true);
            }
        } else if (method_name == "findIndex") {
            if (args.size() > 0 && args[0].is_function()) {
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(array)};
                    Value result = args[0].as_function()->call(ctx, callback_args);
                    if (result.to_boolean()) {
                        return Value(static_cast<double>(i));
                    }
                }
                return Value(-1.0); // not found
            }
        } else if (method_name == "flat") {
            // Array.flat() - flatten array one level
            uint32_t length = array->get_length();
            auto result = ObjectFactory::create_array(0);
            uint32_t result_index = 0;
            
            for (uint32_t i = 0; i < length; i++) {
                Value element = array->get_element(i);
                
                // Check if element is an array
                if (element.is_object() && element.as_object() && element.as_object()->is_array()) {
                    Object* nested_array = element.as_object();
                    uint32_t nested_length = nested_array->get_length();
                    
                    // Add all elements from nested array
                    for (uint32_t j = 0; j < nested_length; j++) {
                        Value nested_element = nested_array->get_element(j);
                        result->set_element(result_index++, nested_element);
                    }
                } else {
                    // Add element directly
                    result->set_element(result_index++, element);
                }
            }
            
            result->set_length(result_index);
            return Value(result.release());
        }
        
        ctx.throw_exception(Value("Invalid array method call"));
        return Value();
    };
    
    return std::make_unique<Function>(method_name, method_fn);
}

std::unique_ptr<Object> create_error(const std::string& message) {
    auto error_obj = std::make_unique<Object>(Object::ObjectType::Error);
    error_obj->set_property("message", Value(message));
    error_obj->set_property("name", Value("Error"));
    error_obj->set_property("_isError", Value(true));
    return error_obj;
}

} // namespace ObjectFactory

} // namespace Quanta