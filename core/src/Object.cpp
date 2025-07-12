#include "Object.h"
#include "Value.h"
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
    
    // Add shape properties
    auto shape_keys = header_.shape->get_property_keys();
    keys.insert(keys.end(), shape_keys.begin(), shape_keys.end());
    
    // Add overflow properties
    if (overflow_properties_) {
        for (const auto& pair : *overflow_properties_) {
            keys.push_back(pair.first);
        }
    }
    
    // Add array indices
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
        transition_shape(key, attrs);
        
        auto info = header_.shape->get_property_info(key);
        if (info.offset >= properties_.size()) {
            properties_.resize(info.offset + 1);
        }
        properties_[info.offset] = value;
        header_.property_count++;
        update_hash_code();
        return true;
    }
    
    return false;
}

bool Object::store_in_overflow(const std::string& key, const Value& value) {
    if (!overflow_properties_) {
        overflow_properties_ = std::make_unique<std::unordered_map<std::string, Value>>();
    }
    
    (*overflow_properties_)[key] = value;
    header_.property_count++;
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
    
    for (const auto& pair : properties_) {
        keys.push_back(pair.first);
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

std::unique_ptr<Object> create_object(Object* prototype) {
    return std::make_unique<Object>(prototype, Object::ObjectType::Ordinary);
}

std::unique_ptr<Object> create_array(uint32_t length) {
    auto array = std::make_unique<Object>(Object::ObjectType::Array);
    array->set_length(length);
    return array;
}

std::unique_ptr<Object> create_function() {
    return std::make_unique<Object>(Object::ObjectType::Function);
}

std::unique_ptr<Object> create_string(const std::string& value) {
    auto str_obj = std::make_unique<Object>(Object::ObjectType::String);
    str_obj->set_property("value", Value(value));
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

std::unique_ptr<Object> create_error(const std::string& message) {
    auto error_obj = std::make_unique<Object>(Object::ObjectType::Error);
    error_obj->set_property("message", Value(message));
    error_obj->set_property("name", Value("Error"));
    return error_obj;
}

} // namespace ObjectFactory

} // namespace Quanta