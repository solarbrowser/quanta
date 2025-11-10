/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/object_properties.h"
#include "../../include/Object.h"
#include "../../include/Value.h"
#include <algorithm>
#include <iostream>

namespace Quanta {

// EXTRACTED FROM Object.cpp - Property management implementation

bool ObjectProperties::has_property(const Object* object, const std::string& key) {
    if (!object) return false;

    // Check own properties first
    if (has_own_property(object, key)) {
        return true;
    }

    // Check prototype chain
    Object* prototype = object->header_.prototype;
    while (prototype) {
        if (prototype->has_own_property(key)) {
            return true;
        }
        prototype = prototype->header_.prototype;
    }

    return false;
}

bool ObjectProperties::has_own_property(const Object* object, const std::string& key) {
    if (!object) return false;

    // Check if it's an array index
    uint32_t index;
    if (object->is_array_index(key, &index)) {
        // For arrays, check elements array
        if (object->header_.type == Object::ObjectType::Array) {
            return index < object->elements_.size();
        }
    }

    // Check in shape-based properties
    if (object->header_.shape && object->header_.shape->has_property(key)) {
        return true;
    }

    // Check overflow properties
    if (object->overflow_properties_) {
        return object->overflow_properties_->count(key) > 0;
    }

    return false;
}

Value ObjectProperties::get_property(const Object* object, const std::string& key) {
    if (!object) return Value();

    Value result = get_own_property(object, key);
    if (!result.is_undefined()) {
        return result;
    }

    // Search prototype chain
    Object* prototype = object->header_.prototype;
    while (prototype) {
        result = get_own_property(prototype, key);
        if (!result.is_undefined()) {
            return result;
        }
        prototype = prototype->header_.prototype;
    }

    return Value(); // undefined
}

Value ObjectProperties::get_own_property(const Object* object, const std::string& key) {
    if (!object) return Value();

    // Check if it's an array index for optimization
    uint32_t index;
    if (object->is_array_index(key, &index)) {
        return get_element(object, index);
    }

    // Check shape-based properties
    if (object->header_.shape && object->header_.shape->has_property(key)) {
        auto info = object->header_.shape->get_property_info(key);
        if (info.offset < object->properties_.size()) {
            return object->properties_[info.offset].value;
        }
    }

    // Check overflow properties
    if (object->overflow_properties_) {
        auto it = object->overflow_properties_->find(key);
        if (it != object->overflow_properties_->end()) {
            return it->second.value;
        }
    }

    return Value(); // undefined
}

bool ObjectProperties::set_property(Object* object, const std::string& key, const Value& value, PropertyAttributes attrs) {
    if (!object) return false;

    // Special handling for array length
    if (object->header_.type == Object::ObjectType::Array && key == "length") {
        return set_array_length(object, value);
    }

    // Check if it's an array index
    uint32_t index;
    if (object->is_array_index(key, &index)) {
        return set_element(object, index, value);
    }

    // Check if property exists in shape
    if (object->header_.shape && object->header_.shape->has_property(key)) {
        auto info = object->header_.shape->get_property_info(key);
        if (info.writable && info.offset < object->properties_.size()) {
            object->properties_[info.offset].value = value;
            return true;
        }
        return false; // Property exists but not writable
    }

    // Add new property - transition shape or use overflow
    return add_new_property(object, key, value, attrs);
}

bool ObjectProperties::delete_property(Object* object, const std::string& key) {
    if (!object) return false;

    // Check if property exists and is configurable
    if (!has_own_property(object, key)) {
        return true; // Deleting non-existent property returns true
    }

    // Check if it's an array index
    uint32_t index;
    if (object->is_array_index(key, &index)) {
        return delete_element(object, index);
    }

    // Check shape-based properties
    if (object->header_.shape && object->header_.shape->has_property(key)) {
        auto info = object->header_.shape->get_property_info(key);
        if (!info.configurable) {
            return false; // Property is not configurable
        }
        // For simplicity, transition to a shape without this property
        // In a real implementation, this would be more complex
        return remove_shape_property(object, key);
    }

    // Remove from overflow properties
    if (object->overflow_properties_) {
        auto it = object->overflow_properties_->find(key);
        if (it != object->overflow_properties_->end()) {
            if (!it->second.attributes.configurable) {
                return false;
            }
            object->overflow_properties_->erase(it);
            return true;
        }
    }

    return true;
}

Value ObjectProperties::get_element(const Object* array, uint32_t index) {
    if (!array) return Value();

    // Check dense array storage
    if (index < array->elements_.size()) {
        return array->elements_[index];
    }

    // Check overflow properties for sparse arrays
    std::string key = std::to_string(index);
    if (array->overflow_properties_) {
        auto it = array->overflow_properties_->find(key);
        if (it != array->overflow_properties_->end()) {
            return it->second.value;
        }
    }

    return Value(); // undefined
}

bool ObjectProperties::set_element(Object* array, uint32_t index, const Value& value) {
    if (!array) return false;

    // Extend elements array if necessary
    if (index >= array->elements_.size()) {
        // Check if we should use dense or sparse storage
        const uint32_t SPARSE_THRESHOLD = 1000;
        if (index > array->elements_.size() + SPARSE_THRESHOLD) {
            // Use sparse storage
            return set_sparse_element(array, index, value);
        } else {
            // Extend dense storage
            array->elements_.resize(index + 1);
        }
    }

    array->elements_[index] = value;

    // Update array length if necessary
    if (array->header_.type == Object::ObjectType::Array) {
        update_array_length(array, index + 1);
    }

    return true;
}

bool ObjectProperties::delete_element(Object* array, uint32_t index) {
    if (!array) return false;

    // Remove from dense storage
    if (index < array->elements_.size()) {
        array->elements_[index] = Value(); // Set to undefined
    }

    // Remove from overflow properties
    std::string key = std::to_string(index);
    if (array->overflow_properties_) {
        array->overflow_properties_->erase(key);
    }

    return true;
}

std::vector<std::string> ObjectProperties::get_own_property_keys(const Object* object) {
    if (!object) return {};

    std::vector<std::string> keys;

    // Add array indices first (if it's an array)
    if (object->header_.type == Object::ObjectType::Array) {
        for (uint32_t i = 0; i < object->elements_.size(); ++i) {
            if (!object->elements_[i].is_undefined()) {
                keys.push_back(std::to_string(i));
            }
        }
    }

    // Add shape-based property keys
    if (object->header_.shape) {
        auto shape_keys = object->header_.shape->get_property_keys();
        keys.insert(keys.end(), shape_keys.begin(), shape_keys.end());
    }

    // Add overflow property keys
    if (object->overflow_properties_) {
        for (const auto& [key, prop] : *object->overflow_properties_) {
            keys.push_back(key);
        }
    }

    return keys;
}

std::vector<std::string> ObjectProperties::get_enumerable_keys(const Object* object) {
    if (!object) return {};

    std::vector<std::string> keys;

    // Get all own property keys
    auto all_keys = get_own_property_keys(object);

    // Filter enumerable properties
    for (const auto& key : all_keys) {
        PropertyAttributes attrs = get_property_attributes(object, key);
        if (attrs.enumerable) {
            keys.push_back(key);
        }
    }

    return keys;
}

PropertyAttributes ObjectProperties::get_property_attributes(const Object* object, const std::string& key) {
    if (!object) return PropertyAttributes::Default;

    // Check shape-based properties
    if (object->header_.shape && object->header_.shape->has_property(key)) {
        auto info = object->header_.shape->get_property_info(key);
        return info.attributes;
    }

    // Check overflow properties
    if (object->overflow_properties_) {
        auto it = object->overflow_properties_->find(key);
        if (it != object->overflow_properties_->end()) {
            return it->second.attributes;
        }
    }

    return PropertyAttributes::Default;
}

// Private helper methods
bool ObjectProperties::add_new_property(Object* object, const std::string& key, const Value& value, PropertyAttributes attrs) {
    if (!object) return false;

    // Try to transition shape first
    Shape* new_shape = object->header_.shape->add_property(key, attrs);
    if (new_shape && object->properties_.size() < Object::MAX_INLINE_PROPERTIES) {
        object->header_.shape = new_shape;
        object->properties_.push_back({value, attrs});
        object->header_.property_count++;
        return true;
    }

    // Fall back to overflow properties
    if (!object->overflow_properties_) {
        object->overflow_properties_ = std::make_unique<std::unordered_map<std::string, Object::Property>>();
    }

    (*object->overflow_properties_)[key] = {value, attrs};
    return true;
}

bool ObjectProperties::set_array_length(Object* array, const Value& value) {
    if (!array || array->header_.type != Object::ObjectType::Array) return false;

    double length_double = value.to_number();

    // Validate length value
    if (length_double < 0 || length_double != std::floor(length_double) || length_double > 4294967295.0) {
        return false; // Invalid length
    }

    uint32_t new_length = static_cast<uint32_t>(length_double);
    uint32_t old_length = static_cast<uint32_t>(array->elements_.size());

    if (new_length < old_length) {
        // Truncate array
        array->elements_.resize(new_length);

        // Remove overflow properties with indices >= new_length
        if (array->overflow_properties_) {
            auto it = array->overflow_properties_->begin();
            while (it != array->overflow_properties_->end()) {
                uint32_t idx;
                if (array->is_array_index(it->first, &idx) && idx >= new_length) {
                    it = array->overflow_properties_->erase(it);
                } else {
                    ++it;
                }
            }
        }
    } else if (new_length > old_length) {
        // Extend array
        array->elements_.resize(new_length);
    }

    return true;
}

bool ObjectProperties::set_sparse_element(Object* array, uint32_t index, const Value& value) {
    if (!array->overflow_properties_) {
        array->overflow_properties_ = std::make_unique<std::unordered_map<std::string, Object::Property>>();
    }

    std::string key = std::to_string(index);
    (*array->overflow_properties_)[key] = {value, PropertyAttributes::Default};

    update_array_length(array, index + 1);
    return true;
}

void ObjectProperties::update_array_length(Object* array, uint32_t min_length) {
    if (!array || array->header_.type != Object::ObjectType::Array) return;

    // Find current length property and update if necessary
    // This is simplified - real implementation would be more complex
    uint32_t current_length = static_cast<uint32_t>(array->elements_.size());
    if (min_length > current_length) {
        // Update length property in shape or overflow
        Value new_length(static_cast<double>(min_length));
        // set_property(array, "length", new_length, PropertyAttributes::Writable);
    }
}

bool ObjectProperties::remove_shape_property(Object* object, const std::string& key) {
    // This would involve complex shape transitions
    // For now, move the property to overflow and mark as deleted
    if (!object->overflow_properties_) {
        object->overflow_properties_ = std::make_unique<std::unordered_map<std::string, Object::Property>>();
    }

    // Remove from current properties and add a "deleted" marker
    // In a real implementation, this would involve shape transitions
    return true;
}

} // namespace Quanta