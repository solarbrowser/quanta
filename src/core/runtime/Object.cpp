/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Object.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/core/runtime/Promise.h"
#include "quanta/parser/AST.h"
#include <algorithm>
#include <sstream>
#include <iostream>

namespace Quanta {

thread_local Context* Object::current_context_ = nullptr;

std::unordered_map<std::tuple<Shape*, std::string, PropertyAttributes>, Shape*, Object::ShapeTransitionHash> Object::shape_transition_cache_;
std::unordered_map<std::string, std::string> Object::interned_keys_;
uint32_t Shape::next_shape_id_ = 1;


static Shape* g_root_shape = nullptr;


Object::Object(ObjectType type) {
    header_.shape = new Shape();
    
    header_.prototype = nullptr;
    header_.type = type;
    header_.flags = 0;
    header_.property_count = 0;
    header_.hash_code = reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF;
    
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
    if (descriptors_) {
        auto it = descriptors_->find(key);
        if (it != descriptors_->end()) {
            return true;
        }
    }

    uint32_t index;
    if (is_array_index(key, &index)) {
        if (index >= elements_.size()) return false;
        if (deleted_elements_ && deleted_elements_->count(index) > 0) return false;
        return true;
    }

    if (header_.shape && header_.shape->has_property(key)) {
        // Check if the property was explicitly deleted
        if (deleted_shape_properties_ && deleted_shape_properties_->count(key) > 0) {
            return false;
        }
        return true;
    }

    if (overflow_properties_) {
        return overflow_properties_->find(key) != overflow_properties_->end();
    }

    return false;
}


Value Object::get_property(const std::string& key) const {
    if (this->get_type() == ObjectType::Function) {
        const Function* func = static_cast<const Function*>(this);
        
        if (key == "name") {
            return Value(func->get_name());
        }
        if (key == "length") {
            PropertyDescriptor desc = get_property_descriptor(key);
            if (desc.has_value() && desc.is_data_descriptor()) {
                return desc.get_value();
            }
            return Value(static_cast<double>(func->get_arity()));
        }
        if (key == "prototype") {
            return Value(func->get_prototype());
        }
        
        // call, apply, bind are now handled via Function.prototype
        // No need for special handling here anymore

        Value result = get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
    }
    
    if (this->get_type() == ObjectType::ArrayBuffer) {
        const ArrayBuffer* buffer = static_cast<const ArrayBuffer*>(this);
        
        if (key == "byteLength") {
            return Value(static_cast<double>(buffer->byte_length()));
        }
        if (key == "maxByteLength") {
            return Value(static_cast<double>(buffer->max_byte_length()));
        }
        if (key == "resizable") {
            return Value(buffer->is_resizable());
        }
        if (key == "_isArrayBuffer") {
            return Value(true);
        }
        
        
        Value result = get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
    }
    
    if (this->get_type() == ObjectType::TypedArray) {
        const TypedArrayBase* typed_array = static_cast<const TypedArrayBase*>(this);
        
        char* end;
        unsigned long index = std::strtoul(key.c_str(), &end, 10);
        if (*end == '\0' && index < typed_array->length()) {
            return typed_array->get_element(static_cast<size_t>(index));
        }
        
        if (key == "length") {
            return Value(static_cast<double>(typed_array->length()));
        }
        if (key == "byteLength") {
            return Value(static_cast<double>(typed_array->byte_length()));
        }
        if (key == "byteOffset") {
            return Value(static_cast<double>(typed_array->byte_offset()));
        }
        if (key == "buffer") {
            return Value(typed_array->buffer());
        }
        if (key == "BYTES_PER_ELEMENT") {
            return Value(static_cast<double>(typed_array->bytes_per_element()));
        }
        
        Value result = get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
    }
    
    if (this->get_type() == ObjectType::Array) {
        // Special case: length is always computed, not from prototype
        if (key == "length") {
            return Value(static_cast<double>(get_length()));
        }

        // Check own properties first
        Value own_result = get_own_property(key);
        if (!own_result.is_undefined()) {
            return own_result;
        }

        // Check prototype chain for overridden methods
        Object* current = header_.prototype;
        while (current) {
            Value proto_result = current->get_own_property(key);
            if (!proto_result.is_undefined()) {
                return proto_result;
            }
            current = current->get_prototype();
        }

        // Only if not found in prototype chain, return native method
        if (key == "map" || key == "filter" || key == "reduce" || key == "forEach" ||
            key == "indexOf" || key == "slice" || key == "splice" || key == "push" ||
            key == "pop" || key == "shift" || key == "unshift" || key == "join" || key == "concat" || key == "toString" || key == "groupBy" ||
            key == "reverse" || key == "sort" || key == "find" || key == "includes" ||
            key == "some" || key == "every" || key == "findIndex" || key == "flat" || key == "flatMap" || key == "reduceRight" || key == "copyWithin" ||
            key == "findLast" || key == "findLastIndex" || key == "toSpliced" || key == "fill" ||
            key == "toSorted" || key == "with" || key == "at" || key == "toReversed") {
            return Value(ObjectFactory::create_array_method(key).release());
        }
    }
    
    Value result = get_own_property(key);
    if (!result.is_undefined()) {
        return result;
    }
    
    Object* current = header_.prototype;
    while (current) {
        result = current->get_own_property(key);
        if (!result.is_undefined()) {
            return result;
        }
        current = current->get_prototype();
    }

    return Value();
}

Value Object::get_own_property(const std::string& key) const {
    uint32_t index;
    if (is_array_index(key, &index)) {
        return get_element(index);
    }


    if (header_.shape && header_.shape->has_property(key)) {
        auto info = header_.shape->get_property_info(key);
        if (info.offset < properties_.size()) {
            return properties_[info.offset];
        }
    }

    if (overflow_properties_) {
        auto it = overflow_properties_->find(key);
        if (it != overflow_properties_->end()) {
            return it->second;
        }
    }
    
    if (descriptors_) {
        auto desc_it = descriptors_->find(key);
        if (desc_it != descriptors_->end()) {
            const PropertyDescriptor& desc = desc_it->second;
            if (desc.is_accessor_descriptor() && desc.has_getter()) {
                if (key == "cookie") {
                    return Value(std::string(""));
                }
                Object* getter = desc.get_getter();
                if (getter) {
                    Function* getter_fn = dynamic_cast<Function*>(getter);
                    if (getter_fn) {
                        std::string getter_name = getter_fn->get_name();
                        if (getter_name.find("get [Symbol.") == 0) {
                            if (this->is_function()) {
                                return Value(const_cast<Function*>(static_cast<const Function*>(this)));
                            }
                            return Value(const_cast<Object*>(this));
                        }

                        return Value();
                    }
                }
            }
            if (desc.is_data_descriptor()) {
                return desc.get_value();
            }
        }
    }
    

    return Value();
}

Value Object::get_property(const Value& key) const {
    return get_property(key.to_property_key());
}

bool Object::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    if (header_.type == ObjectType::Array && key == "length") {
        double length_double = value.to_number();

        if (length_double < 0 || length_double != std::floor(length_double) || length_double > 4294967295.0) {
            if (current_context_) {
                current_context_->throw_range_error("Invalid array length");
            }
            return false;
        }

        uint32_t new_length = static_cast<uint32_t>(length_double);

        uint32_t old_length = static_cast<uint32_t>(elements_.size());

        if (new_length < old_length) {
            elements_.resize(new_length);
            if (overflow_properties_) {
                auto it = overflow_properties_->begin();
                while (it != overflow_properties_->end()) {
                    uint32_t idx;
                    if (is_array_index(it->first, &idx) && idx >= new_length) {
                        it = overflow_properties_->erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        } else if (new_length > old_length) {
            elements_.resize(new_length);
            // Mark new elements as holes
            if (!deleted_elements_) {
                deleted_elements_ = std::make_unique<std::unordered_set<uint32_t>>();
            }
            for (uint32_t i = old_length; i < new_length; ++i) {
                deleted_elements_->insert(i);
            }
        }

        Value length_value(static_cast<double>(new_length));

        if (header_.shape && header_.shape->has_property("length")) {
            auto info = header_.shape->get_property_info("length");
            if (info.offset < properties_.size()) {
                properties_[info.offset] = length_value;
                return true;
            }
        }

        if (!overflow_properties_) {
            overflow_properties_ = std::make_unique<std::unordered_map<std::string, Value>>();
        }
        (*overflow_properties_)["length"] = length_value;
        return true;
    }

    uint32_t index;
    if (is_array_index(key, &index)) {
        return set_element(index, value);
    }

    bool prop_exists = has_own_property(key);
    if (prop_exists) {
        PropertyDescriptor desc = get_property_descriptor(key);
        if (desc.is_data_descriptor() && !desc.is_writable()) {
            return false;
        }

        if (header_.shape->has_property(key)) {
            auto info = header_.shape->get_property_info(key);
            if (info.offset < properties_.size()) {
                properties_[info.offset] = value;
                if (deleted_shape_properties_) deleted_shape_properties_->erase(key);
                return true;
            }
        }

        if (overflow_properties_) {
            (*overflow_properties_)[key] = value;
            return true;
        }
    }
    
    if (!is_extensible()) {
        return false;
    }

    // Store non-default attrs in descriptor map (shape may use cached transitions with wrong attrs)
    if (attrs != PropertyAttributes::Default) {
        if (!descriptors_) {
            descriptors_ = std::make_unique<std::unordered_map<std::string, PropertyDescriptor>>();
        }
        (*descriptors_)[key] = PropertyDescriptor(value, attrs);
    }

    if (store_in_shape(key, value, attrs)) {
        return true;
    }

    return store_in_overflow(key, value);
}

bool Object::set_property(const Value& key, const Value& value, PropertyAttributes attrs) {
    return set_property(key.to_property_key(), value, attrs);
}

bool Object::delete_property(const std::string& key) {
    PropertyDescriptor desc = get_property_descriptor(key);
    if (!desc.is_configurable()) {
        return false;
    }

    uint32_t index;
    if (is_array_index(key, &index)) {
        return delete_element(index);
    }

    if (overflow_properties_) {
        auto it = overflow_properties_->find(key);
        if (it != overflow_properties_->end()) {
            overflow_properties_->erase(it);

            // ES1: Also erase from descriptors to ensure property is fully deleted
            if (descriptors_) {
                descriptors_->erase(key);
            }

            header_.property_count--;
            update_hash_code();
            return true;
        }
    }

    if (header_.shape->has_property(key)) {
        auto info = header_.shape->get_property_info(key);
        if (info.offset < properties_.size()) {
            properties_[info.offset] = Value();

            if (descriptors_) {
                descriptors_->erase(key);
            }

            // Track deleted shape properties
            if (!deleted_shape_properties_) {
                deleted_shape_properties_ = std::make_unique<std::unordered_set<std::string>>();
            }
            deleted_shape_properties_->insert(key);

            header_.property_count--;
            update_hash_code();
            return true;
        }
    }

    return false;
}

Value Object::get_element(uint32_t index) const {
    if (index < elements_.size()) {
        return elements_[index];
    }
    return Value();
}

bool Object::set_element(uint32_t index, const Value& value) {
    if (__builtin_expect(index >= elements_.size(), 0)) {
        if (__builtin_expect(index > 10000000, 0)) {
            return false;
        }
        size_t old_size = elements_.size();
        size_t new_size = index + 1;
        if (new_size > elements_.capacity()) {
            elements_.reserve(new_size * 2);
        }
        elements_.resize(new_size, Value());
        // Mark intermediate positions as holes
        if (new_size > old_size + 1) {
            if (!deleted_elements_) {
                deleted_elements_ = std::make_unique<std::unordered_set<uint32_t>>();
            }
            for (size_t i = old_size; i < index; ++i) {
                deleted_elements_->insert(static_cast<uint32_t>(i));
            }
        }

        if (__builtin_expect(header_.type == ObjectType::Array, 1)) {
            if (overflow_properties_) {
                auto it = overflow_properties_->find("length");
                if (it != overflow_properties_->end()) {
                    it->second = Value(static_cast<double>(new_size));
                }
            }
        }
    }

    elements_[index] = value;
    if (deleted_elements_) deleted_elements_->erase(index);
    return true;
}

bool Object::delete_element(uint32_t index) {
    if (index < elements_.size()) {
        elements_[index] = Value();
        if (!deleted_elements_) {
            deleted_elements_ = std::make_unique<std::unordered_set<uint32_t>>();
        }
        deleted_elements_->insert(index);
        return true;
    }
    return false;
}

std::vector<std::string> Object::get_own_property_keys() const {
    // ES6 [[OwnPropertyKeys]]: integer indices ascending, then string keys in creation order
    // Step 1: Collect ALL keys in their original storage order (preserves insertion order)
    std::vector<std::string> raw_keys;

    // Shape properties first (these preserve insertion order)
    if (header_.shape) {
        auto shape_properties = header_.shape->get_property_keys();
        for (const auto& prop_name : shape_properties) {
            raw_keys.push_back(prop_name);
        }
    }

    // Overflow properties next
    if (overflow_properties_) {
        for (const auto& pair : *overflow_properties_) {
            // Skip if already in shape
            bool in_shape = false;
            if (header_.shape) {
                auto info = header_.shape->get_property_info(pair.first);
                if (info.offset != UINT32_MAX) in_shape = true;
            }
            if (!in_shape) {
                raw_keys.push_back(pair.first);
            }
        }
    }

    // Descriptor-only properties (defineProperty'd keys not in shape/overflow)
    if (descriptors_) {
        for (const auto& pair : *descriptors_) {
            bool already = false;
            for (const auto& k : raw_keys) {
                if (k == pair.first) { already = true; break; }
            }
            if (!already) {
                raw_keys.push_back(pair.first);
            }
        }
    }

    // Elements (numeric array slots)
    for (uint32_t i = 0; i < elements_.size(); ++i) {
        if (!elements_[i].is_undefined()) {
            std::string key = std::to_string(i);
            bool already = false;
            for (const auto& k : raw_keys) {
                if (k == key) { already = true; break; }
            }
            if (!already) {
                raw_keys.push_back(key);
            }
        }
    }

    // Step 2: Partition into integer indices vs string keys
    std::vector<std::pair<uint32_t, std::string>> index_keys;
    std::vector<std::string> string_keys;

    for (const auto& key : raw_keys) {
        uint32_t idx;
        if (is_array_index(key, &idx)) {
            index_keys.push_back({idx, key});
        } else {
            string_keys.push_back(key);
        }
    }

    // Step 3: Sort integer indices ascending
    std::sort(index_keys.begin(), index_keys.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    // Step 4: Build result - integer indices first, then string keys in insertion order
    std::vector<std::string> keys;
    keys.reserve(index_keys.size() + string_keys.size());
    for (const auto& ik : index_keys) {
        keys.push_back(ik.second);
    }
    for (const auto& sk : string_keys) {
        keys.push_back(sk);
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
    // Check descriptor map first (takes precedence over shape attrs)
    if (descriptors_) {
        auto it = descriptors_->find(key);
        if (it != descriptors_->end()) {
            return it->second;
        }
    }

    // Property not in descriptor map, check if it exists and get attrs from shape
    if (has_own_property(key)) {
        Value value = get_own_property(key);
        PropertyAttributes attrs = PropertyAttributes::Default;

        // Check shape for attrs (for properties stored in shape)
        if (header_.shape->has_property(key)) {
            auto info = header_.shape->get_property_info(key);
            attrs = info.attributes;
        }

        return PropertyDescriptor(value, attrs);
    }

    return PropertyDescriptor();
}

bool Object::set_property_descriptor(const std::string& key, const PropertyDescriptor& desc) {
    // Reject new properties on non-extensible objects
    if (!is_extensible() && !has_own_property(key)) {
        return false;
    }

    if (!descriptors_) {
        descriptors_ = std::make_unique<std::unordered_map<std::string, PropertyDescriptor>>();
    }

    if (desc.is_data_descriptor()) {
        uint32_t index;
        if (is_array_index(key, &index)) {
            if (index >= elements_.size()) {
                elements_.resize(index + 1);
            }
            elements_[index] = desc.get_value();
        } else {
            // Use set_property for proper array length handling etc.
            set_property(key, desc.get_value(), desc.get_attributes());
        }
    } else if (desc.is_accessor_descriptor() || desc.is_generic_descriptor()) {
        // Ensure the property exists in shape so has_own_property works
        uint32_t index;
        if (is_array_index(key, &index)) {
            if (index >= elements_.size()) {
                elements_.resize(index + 1);
            }
        } else if (!has_own_property(key)) {
            if (store_in_shape(key, Value(), desc.get_attributes())) {
                // stored in shape
            } else {
                store_in_overflow(key, Value());
            }
        }
    }

    // Store the authoritative descriptor AFTER set_property,
    // so it doesn't get overwritten
    (*descriptors_)[key] = desc;

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
    // Set length property for generic objects (not just arrays)
    PropertyDescriptor length_desc(Value(static_cast<double>(length)),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable));
    set_property_descriptor("length", length_desc);

    // Resize elements array if this is actually an Array
    if (header_.type == ObjectType::Array && length < elements_.size()) {
        elements_.resize(length);
    }
}

void Object::push(const Value& value) {
    uint32_t length = get_length();
    if (length >= 1000000) {
        return;
    }
    set_element(length, value);
    set_length(length + 1);
}

Value Object::pop() {
    uint32_t length = get_length();
    if (length == 0) {
        return Value();
    }
    
    Value result = get_element(length - 1);
    delete_element(length - 1);
    set_length(length - 1);
    return result;
}

void Object::unshift(const Value& value) {
    uint32_t length = get_length();
    
    if (length >= 1000000) {
        return;
    }
    
    for (uint32_t i = length; i > 0; --i) {
        if (i < elements_.size()) {
            Value element = get_element(i - 1);
            set_element(i, element);
        }
    }
    
    set_element(0, value);
    set_length(length + 1);
}

Value Object::shift() {
    uint32_t length = get_length();
    if (length == 0) {
        return Value();
    }
    
    Value result = get_element(0);
    
    for (uint32_t i = 0; i < length - 1; ++i) {
        Value element = get_element(i + 1);
        set_element(i, element);
    }
    
    delete_element(length - 1);
    set_length(length - 1);
    return result;
}

std::unique_ptr<Object> Object::map(Function* callback, Context& ctx, const Value& thisArg) {
    if (header_.type != ObjectType::Array) {
        return ObjectFactory::create_array(0);
    }

    uint32_t length = get_length();
    auto result = ObjectFactory::create_array(length);

    for (uint32_t i = 0; i < length; i++) {
        Value element = get_element(i);
        if (!element.is_undefined()) {
            if (callback) {
                try {
                    std::vector<Value> args = {
                        element,
                        Value(static_cast<double>(i)),
                        Value(this)
                    };

                    Value mapped_value = callback->call(ctx, args, thisArg);
                    if (!ctx.has_exception()) {
                        result->set_element(i, mapped_value);
                    } else {
                        break;
                    }
                } catch (const std::exception& e) {
                    result->set_element(i, Value());
                }
            } else {
                result->set_element(i, element);
            }
        }
    }

    return result;
}

std::unique_ptr<Object> Object::filter(Function* callback, Context& ctx, const Value& thisArg) {
    if (header_.type != ObjectType::Array) {
        return ObjectFactory::create_array(0);
    }

    uint32_t length = get_length();
    auto result = ObjectFactory::create_array(0);
    uint32_t result_index = 0;

    for (uint32_t i = 0; i < length; i++) {
        Value element = get_element(i);
        if (!element.is_undefined()) {
            std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(this)};
            Value should_include = callback->call(ctx, args, thisArg);
            if (ctx.has_exception()) return nullptr;

            if (should_include.to_boolean()) {
                result->set_element(result_index++, element);
            }
        }
    }

    result->set_length(result_index);
    return result;
}

void Object::forEach(Function* callback, Context& ctx, const Value& thisArg) {
    if (header_.type != ObjectType::Array) {
        return;
    }

    uint32_t length = get_length();

    for (uint32_t i = 0; i < length; i++) {
        Value element = get_element(i);
        if (!element.is_undefined()) {
            std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(this)};

            Value result = callback->call(ctx, args, thisArg);
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
    
    if (initial_value.is_undefined() && length > 0) {
        accumulator = get_element(0);
        start_index = 1;
    }
    
    for (uint32_t i = start_index; i < length; i++) {
        Value element = get_element(i);
        if (!element.is_undefined()) {
            std::vector<Value> args = {accumulator, element, Value(static_cast<double>(i)), Value(this)};
            accumulator = callback->call(ctx, args);
            if (ctx.has_exception()) return Value();
        }
    }
    
    return accumulator;
}

Value Object::reduceRight(Function* callback, const Value& initial_value, Context& ctx) {
    if (header_.type != ObjectType::Array) {
        return Value();
    }

    uint32_t length = get_length();
    if (length == 0 && initial_value.is_undefined()) {
        ctx.throw_type_error("Reduce of empty array with no initial value");
        return Value();
    }

    Value accumulator = initial_value;
    int32_t start_index = static_cast<int32_t>(length) - 1;

    if (initial_value.is_undefined() && length > 0) {
        accumulator = get_element(length - 1);
        start_index = static_cast<int32_t>(length) - 2;
    }

    for (int32_t i = start_index; i >= 0; i--) {
        Value element = get_element(static_cast<uint32_t>(i));
        if (!element.is_undefined()) {
            std::vector<Value> args = {accumulator, element, Value(static_cast<double>(i)), Value(this)};
            accumulator = callback->call(ctx, args);
            if (ctx.has_exception()) return Value();
        }
    }

    return accumulator;
}

Value Object::groupBy(Function* callback, Context& ctx) {
    if (header_.type != ObjectType::Array) {
        return Value();
    }
    
    auto result = ObjectFactory::create_object();
    
    Value length_val = this->get_property("length");
    if (!length_val.is_number()) {
        return Value(result.release());
    }
    
    int length = static_cast<int>(length_val.to_number());
    
    for (int i = 0; i < length; i++) {
        Value element = this->get_property(std::to_string(i));
        
        std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(this)};
        Value key = callback->call(ctx, callback_args);
        std::string key_str = key.to_string();
        
        Value group = result->get_property(key_str);
        if (!group.is_object()) {
            auto new_group = ObjectFactory::create_array();
            result->set_property(key_str, Value(new_group.release()));
            group = result->get_property(key_str);
        }
        
        Object* group_array = group.as_object();
        Value group_length = group_array->get_property("length");
        int group_len = static_cast<int>(group_length.to_number());
        group_array->set_property(std::to_string(group_len), element);
        group_array->set_property("length", Value(static_cast<double>(group_len + 1)));
    }
    
    std::cout << "Array.groupBy: Grouped " << length << " elements into object with proper formatting" << std::endl;
    return Value(result.release());
}

std::unique_ptr<Object> Object::flat(uint32_t depth) {
    if (header_.type != ObjectType::Array) {
        return ObjectFactory::create_array(0);
    }

    auto result = ObjectFactory::create_array(0);
    uint32_t length = get_length();

    std::function<void(Object*, uint32_t)> flatten_into;
    flatten_into = [&](Object* source, uint32_t current_depth) {
        uint32_t source_length = source->get_length();
        for (uint32_t i = 0; i < source_length; i++) {
            Value element = source->get_element(i);

            if (element.is_object() && element.as_object()->is_array() && current_depth > 0) {
                flatten_into(element.as_object(), current_depth - 1);
            } else {
                result->push(element);
            }
        }
    };

    flatten_into(this, depth);
    return result;
}

std::unique_ptr<Object> Object::flatMap(Function* callback, Context& ctx, const Value& thisArg) {
    if (header_.type != ObjectType::Array) {
        return ObjectFactory::create_array(0);
    }

    auto result = ObjectFactory::create_array(0);
    uint32_t length = get_length();

    for (uint32_t i = 0; i < length; i++) {
        Value element = get_element(i);

        std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(this)};
        Value mapped = callback->call(ctx, args, thisArg);
        if (ctx.has_exception()) return result;

        if (mapped.is_object() && mapped.as_object()->is_array()) {
            Object* mapped_array = mapped.as_object();
            uint32_t mapped_length = mapped_array->get_length();
            for (uint32_t j = 0; j < mapped_length; j++) {
                result->push(mapped_array->get_element(j));
            }
        } else {
            result->push(mapped);
        }
    }

    return result;
}

Object* Object::copyWithin(int32_t target, int32_t start, int32_t end) {
    if (header_.type != ObjectType::Array) {
        return this;
    }

    uint32_t length = get_length();
    int32_t len = static_cast<int32_t>(length);

    int32_t to = target < 0 ? std::max(len + target, 0) : std::min(target, len);
    int32_t from = start < 0 ? std::max(len + start, 0) : std::min(start, len);
    int32_t final = end == -1 ? len : (end < 0 ? std::max(len + end, 0) : std::min(end, len));

    int32_t count = std::min(final - from, len - to);

    if (count <= 0) return this;

    if (from < to && to < from + count) {
        for (int32_t i = count - 1; i >= 0; i--) {
            set_element(static_cast<uint32_t>(to + i), get_element(static_cast<uint32_t>(from + i)));
        }
    } else {
        for (int32_t i = 0; i < count; i++) {
            set_element(static_cast<uint32_t>(to + i), get_element(static_cast<uint32_t>(from + i)));
        }
    }

    return this;
}

Value Object::findLast(Function* callback, Context& ctx, const Value& thisArg) {
    if (header_.type != ObjectType::Array) {
        return Value();
    }

    uint32_t length = get_length();
    for (int32_t i = static_cast<int32_t>(length) - 1; i >= 0; i--) {
        Value element = get_element(static_cast<uint32_t>(i));
        std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(this)};
        Value result = callback->call(ctx, args, thisArg);
        if (ctx.has_exception()) return Value();
        if (result.to_boolean()) {
            return element;
        }
    }
    return Value();
}

Value Object::findLastIndex(Function* callback, Context& ctx, const Value& thisArg) {
    if (header_.type != ObjectType::Array) {
        return Value(-1.0);
    }

    uint32_t length = get_length();
    for (int32_t i = static_cast<int32_t>(length) - 1; i >= 0; i--) {
        Value element = get_element(static_cast<uint32_t>(i));
        std::vector<Value> args = {element, Value(static_cast<double>(i)), Value(this)};
        Value result = callback->call(ctx, args, thisArg);
        if (ctx.has_exception()) return Value(-1.0);
        if (result.to_boolean()) {
            return Value(static_cast<double>(i));
        }
    }
    return Value(-1.0);
}

std::unique_ptr<Object> Object::toSpliced(uint32_t start, uint32_t deleteCount, const std::vector<Value>& items) {
    if (header_.type != ObjectType::Array) {
        return ObjectFactory::create_array(0);
    }

    uint32_t length = get_length();
    uint32_t actualStart = std::min(start, length);
    uint32_t actualDeleteCount = std::min(deleteCount, length - actualStart);
    uint32_t newLength = length - actualDeleteCount + items.size();

    auto result = ObjectFactory::create_array(newLength);

    for (uint32_t i = 0; i < actualStart; i++) {
        result->set_element(i, get_element(i));
    }

    for (size_t i = 0; i < items.size(); i++) {
        result->set_element(actualStart + i, items[i]);
    }

    for (uint32_t i = actualStart + actualDeleteCount; i < length; i++) {
        result->set_element(items.size() + i - actualDeleteCount, get_element(i));
    }

    return result;
}

Object* Object::fill(const Value& value, int32_t start, int32_t end) {
    if (header_.type != ObjectType::Array) {
        return this;
    }

    uint32_t length = get_length();
    int32_t len = static_cast<int32_t>(length);

    int32_t k = start < 0 ? std::max(len + start, 0) : std::min(start, len);
    int32_t final = end == -1 ? len : (end < 0 ? std::max(len + end, 0) : std::min(end, len));

    for (int32_t i = k; i < final; i++) {
        set_element(static_cast<uint32_t>(i), value);
    }

    return this;
}

std::unique_ptr<Object> Object::toSorted(Function* compareFn, Context& ctx) {
    if (header_.type != ObjectType::Array) {
        return ObjectFactory::create_array(0);
    }

    uint32_t length = get_length();
    auto result = ObjectFactory::create_array(length);

    for (uint32_t i = 0; i < length; i++) {
        result->set_element(i, get_element(i));
    }

    if (compareFn) {
        for (uint32_t i = 0; i < length - 1; i++) {
            for (uint32_t j = 0; j < length - i - 1; j++) {
                Value a = result->get_element(j);
                Value b = result->get_element(j + 1);
                std::vector<Value> args = {a, b};
                Value cmp = compareFn->call(ctx, args);
                if (ctx.has_exception()) return result;
                if (cmp.to_number() > 0) {
                    result->set_element(j, b);
                    result->set_element(j + 1, a);
                }
            }
        }
    } else {
        for (uint32_t i = 0; i < length - 1; i++) {
            for (uint32_t j = 0; j < length - i - 1; j++) {
                Value a = result->get_element(j);
                Value b = result->get_element(j + 1);
                if (a.to_string() > b.to_string()) {
                    result->set_element(j, b);
                    result->set_element(j + 1, a);
                }
            }
        }
    }

    return result;
}

std::unique_ptr<Object> Object::with_method(uint32_t index, const Value& value) {
    if (header_.type != ObjectType::Array) {
        return ObjectFactory::create_array(0);
    }

    uint32_t length = get_length();
    auto result = ObjectFactory::create_array(length);

    for (uint32_t i = 0; i < length; i++) {
        if (i == index) {
            result->set_element(i, value);
        } else {
            result->set_element(i, get_element(i));
        }
    }

    return result;
}

Value Object::at(int32_t index) {
    if (header_.type != ObjectType::Array) {
        return Value();
    }

    uint32_t length = get_length();
    int32_t len = static_cast<int32_t>(length);

    int32_t k = index < 0 ? len + index : index;

    if (k < 0 || k >= len) {
        return Value();
    }

    return get_element(static_cast<uint32_t>(k));
}

std::unique_ptr<Object> Object::toReversed() {
    if (header_.type != ObjectType::Array) {
        return ObjectFactory::create_array(0);
    }

    uint32_t length = get_length();
    auto result = ObjectFactory::create_array(length);

    for (uint32_t i = 0; i < length; i++) {
        result->set_element(i, get_element(length - 1 - i));
    }

    return result;
}

bool Object::is_extensible() const {
    return !(header_.flags & 0x01);
}

void Object::prevent_extensions() {
    header_.flags |= 0x01;
}

void Object::seal() {
    prevent_extensions();

    auto prop_names = get_own_property_keys();
    for (const auto& prop_name : prop_names) {
        PropertyDescriptor desc = get_property_descriptor(prop_name);
        desc.set_configurable(false);
        set_property_descriptor(prop_name, desc);
    }
}

void Object::freeze() {
    prevent_extensions();

    auto prop_names = get_own_property_keys();
    for (const auto& prop_name : prop_names) {
        PropertyDescriptor desc = get_property_descriptor(prop_name);
        desc.set_configurable(false);
        desc.set_writable(false);
        set_property_descriptor(prop_name, desc);
    }
}

bool Object::is_sealed() const {
    if (is_extensible()) {
        return false;
    }

    auto prop_names = get_own_property_keys();
    for (const auto& prop_name : prop_names) {
        PropertyDescriptor desc = get_property_descriptor(prop_name);
        if (desc.is_configurable()) {
            return false;
        }
    }

    return true;
}

bool Object::is_frozen() const {
    if (is_extensible()) {
        return false;
    }

    auto prop_names = get_own_property_keys();
    for (const auto& prop_name : prop_names) {
        PropertyDescriptor desc = get_property_descriptor(prop_name);
        if (desc.is_configurable() || (desc.is_data_descriptor() && desc.is_writable())) {
            return false;
        }
    }

    return true;
}

bool Object::is_array_index(const std::string& key, uint32_t* index) const {
    if (key.empty() || key[0] < '0' || key[0] > '9') {
        return false;  // Must start with a digit
    }
    if (key[0] == '0' && key.length() > 1) {
        return false;  // No leading zeros (except "0" itself)
    }

    char* end;
    unsigned long val = std::strtoul(key.c_str(), &end, 10);

    if (end == key.c_str() + key.length() && val <= 0xFFFFFFFEUL) {
        if (index) *index = static_cast<uint32_t>(val);
        return true;
    }

    return false;
}

bool Object::store_in_shape(const std::string& key, const Value& value, PropertyAttributes attrs) {
    if (header_.property_count < 32) {
        bool is_new_property = !header_.shape->has_property(key);
        
        transition_shape(key, attrs);
        
        auto info = header_.shape->get_property_info(key);
        if (info.offset >= properties_.size()) {
            properties_.resize(info.offset + 1);
        }
        properties_[info.offset] = value;
        
        if (is_new_property) {
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
    
    bool is_new_property = overflow_properties_->find(key) == overflow_properties_->end();
    
    (*overflow_properties_)[key] = value;
    
    if (is_new_property) {
        header_.property_count++;
    }
    
    update_hash_code();
    
    
    return true;
}

void Object::clear_properties() {
    properties_.clear();
    elements_.clear();
    
    if (overflow_properties_) {
        overflow_properties_->clear();
    }
    if (descriptors_) {
        descriptors_->clear();
    }
    
    property_insertion_order_.clear();
    
    header_.shape = Shape::get_root_shape();
    header_.property_count = 0;
    
    header_.type = ObjectType::Ordinary;
    header_.flags = 0;
    
    update_hash_code();
}

void Object::transition_shape(const std::string& key, PropertyAttributes attrs) {
    Shape* new_shape = header_.shape->add_property(key, attrs);
    header_.shape = new_shape;
}

void Object::update_hash_code() {
    header_.hash_code = (header_.property_count << 16) | static_cast<uint32_t>(header_.type);
}

std::string Object::to_string() const {
    if (header_.type == ObjectType::Array) {
        std::ostringstream oss;
        for (uint32_t i = 0; i < elements_.size(); ++i) {
            if (i > 0) oss << ",";
            if (!elements_[i].is_undefined()) {
                oss << elements_[i].to_string();
            }
        }
        return oss.str();
    }

    // Check for custom toString property in constructor
    // For Test262Error and similar custom errors
    Value constructor_prop = get_property("constructor");
    if (!constructor_prop.is_undefined() && constructor_prop.is_function()) {
        Function* ctor = constructor_prop.as_function();
        if (ctor) {
            Value proto_val = ctor->get_property("prototype");
            if (!proto_val.is_undefined() && proto_val.is_object()) {
                Object* proto = proto_val.as_object();
                Value proto_toString = proto->get_property("toString");
                // If prototype has a custom toString, try to format using properties
                if (!proto_toString.is_undefined() && proto_toString.is_function()) {
                    // Try to mimic what toString would do
                    Value message_prop = get_property("message");
                    if (!message_prop.is_undefined()) {
                        std::string ctor_name = ctor->get_name();
                        std::string message = message_prop.to_string();
                        if (!ctor_name.empty() && ctor_name != "Object") {
                            // Only add ": " if message is not empty
                            if (message.empty()) {
                                return ctor_name;
                            }
                            return ctor_name + ": " + message;
                        }
                    }
                }
            }
        }
    }

    // Fallback: check for name and message properties (Error objects)
    // This handles Error objects that don't have a custom toString method
    if (header_.type == ObjectType::Error) {
        Value name_prop = get_property("name");
        Value message_prop = get_property("message");

        std::string name = name_prop.is_undefined() ? "Error" : name_prop.to_string();
        std::string message = message_prop.is_undefined() ? "" : message_prop.to_string();

        if (message.empty()) {
            return name;
        }
        return name + ": " + message;
    }

    // For non-Error objects, check if they have message property (like Test262Error)
    Value message_prop = get_property("message");
    if (!message_prop.is_undefined()) {
        std::string message_str = message_prop.to_string();

        // Try to get constructor name from prototype chain
        Object* proto = header_.prototype;
        while (proto) {
            Value ctor_val = proto->get_property("constructor");
            if (!ctor_val.is_undefined() && ctor_val.is_function()) {
                Function* ctor = ctor_val.as_function();
                std::string ctor_name = ctor ? ctor->get_name() : "";
                if (!ctor_name.empty() && ctor_name != "Object") {
                    // Only add ": " if message is not empty
                    if (message_str.empty()) {
                        return ctor_name;
                    }
                    return ctor_name + ": " + message_str;
                }
            }
            proto = proto->get_prototype();
        }

        // Fallback: check if prototype has toString (custom error types like Test262Error)
        proto = header_.prototype;
        if (proto) {
            Value toString_val = proto->get_property("toString");
            if (!toString_val.is_undefined() && toString_val.is_function()) {
                // This is likely a custom error type
                // Check if it has a name property
                Value name_val = get_property("name");
                if (!name_val.is_undefined()) {
                    std::string name = name_val.to_string();
                    if (!name.empty()) {
                        // Only add ": " if message is not empty
                        if (message_str.empty()) {
                            return name;
                        }
                        return name + ": " + message_str;
                    }
                }
                // If no name but has custom toString, assume it's an error-like object
                // Default to "Error: message" format for compatibility
                if (message_str.empty()) {
                    return "Error";
                }
                return "Error: " + message_str;
            }
        }

        // Ultimate fallback: just return message
        return message_str;
    }

    return "[object Object]";
}

PropertyDescriptor Object::create_data_descriptor(const Value& value, PropertyAttributes attrs) const {
    return PropertyDescriptor(value, attrs);
}


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

void PropertyDescriptor::set_getter(Object* getter) {
    getter_ = getter;
    has_getter_ = true;
    if (type_ == Generic) type_ = Accessor;
}

void PropertyDescriptor::set_setter(Object* setter) {
    setter_ = setter;
    has_setter_ = true;
    if (type_ == Generic) type_ = Accessor;
}

void PropertyDescriptor::set_writable(bool writable) {
    if (writable) {
        attributes_ = static_cast<PropertyAttributes>(attributes_ | PropertyAttributes::Writable);
    } else {
        attributes_ = static_cast<PropertyAttributes>(attributes_ & ~PropertyAttributes::Writable);
    }
    has_writable_ = true;
    // ES5 8.10: A descriptor with [[Writable]] is a data descriptor
    if (type_ == Generic) type_ = Data;
}

void PropertyDescriptor::set_enumerable(bool enumerable) {
    if (enumerable) {
        attributes_ = static_cast<PropertyAttributes>(attributes_ | PropertyAttributes::Enumerable);
    } else {
        attributes_ = static_cast<PropertyAttributes>(attributes_ & ~PropertyAttributes::Enumerable);
    }
    has_enumerable_ = true;
}

void PropertyDescriptor::set_configurable(bool configurable) {
    if (configurable) {
        attributes_ = static_cast<PropertyAttributes>(attributes_ | PropertyAttributes::Configurable);
    } else {
        attributes_ = static_cast<PropertyAttributes>(attributes_ & ~PropertyAttributes::Configurable);
    }
    has_configurable_ = true;
}


Shape::Shape() : parent_(nullptr), property_count_(0), id_(next_shape_id_++) {
}

Shape::Shape(Shape* parent, const std::string& key, PropertyAttributes attrs)
    : parent_(parent), transition_key_(key), transition_attrs_(attrs),
      property_count_(parent ? parent->property_count_ + 1 : 1),
      id_(next_shape_id_++) {
    
    if (parent_) {
        properties_ = parent_->properties_;
    }
    
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
    std::tuple<Shape*, std::string, PropertyAttributes> cache_key = {this, key, attrs};
    auto cache_it = Object::shape_transition_cache_.find(cache_key);
    if (cache_it != Object::shape_transition_cache_.end()) {
        return cache_it->second;
    }

    Shape* new_shape = new Shape(this, key, attrs);

    Object::shape_transition_cache_[cache_key] = new_shape;

    return new_shape;
}

std::vector<std::string> Shape::get_property_keys() const {
    std::vector<std::string> keys;
    keys.reserve(properties_.size());
    
    std::vector<std::string> reverse_keys;
    const Shape* current = this;
    
    while (current && current->parent_) {
        if (!current->transition_key_.empty()) {
            reverse_keys.push_back(current->transition_key_);
        }
        current = current->parent_;
    }
    
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


Value Object::internal_get(const std::string& key) const {
    return get_property(key);
}

bool Object::internal_set(const std::string& key, const Value& value) {
    return set_property(key, value);
}

bool Object::internal_delete(const std::string& key) {
    return delete_property(key);
}

std::vector<std::string> Object::internal_own_keys() const {
    return get_own_property_keys();
}


namespace ObjectFactory {

static std::vector<std::unique_ptr<Object>> object_pool_;
static std::vector<std::unique_ptr<Object>> array_pool_;
static size_t pool_size_ = 5000;
static bool pools_initialized_ = false;

void initialize_memory_pools() {
    if (pools_initialized_) return;
    
    object_pool_.reserve(pool_size_);
    array_pool_.reserve(pool_size_);
    
    for (size_t i = 0; i < pool_size_; ++i) {
        object_pool_.push_back(std::make_unique<Object>(Object::ObjectType::Ordinary));
    }
    
    for (size_t i = 0; i < pool_size_; ++i) {
        array_pool_.push_back(std::make_unique<Object>(Object::ObjectType::Array));
    }
    
    pools_initialized_ = true;
}

std::unique_ptr<Object> get_pooled_object() {
    if (!pools_initialized_) initialize_memory_pools();
    
    if (!object_pool_.empty()) {
        auto obj = std::move(object_pool_.back());
        object_pool_.pop_back();
        
        obj->clear_properties();
        
        Object* obj_proto = get_object_prototype();
        if (obj_proto) {
            obj->set_prototype(obj_proto);
        }
        
        return obj;
    }
    
    auto obj = std::make_unique<Object>(Object::ObjectType::Ordinary);
    Object* obj_proto = get_object_prototype();
    if (obj_proto) {
        obj->set_prototype(obj_proto);
    }
    return obj;
}

std::unique_ptr<Object> get_pooled_array() {
    if (!pools_initialized_) initialize_memory_pools();
    
    if (!array_pool_.empty()) {
        auto array = std::move(array_pool_.back());
        array_pool_.pop_back();

        Object* array_proto = get_array_prototype();
        if (array_proto) {
            array->set_prototype(array_proto);
        }

        return array;
    }
    
    auto array = std::make_unique<Object>(Object::ObjectType::Array);
    Object* array_proto = get_array_prototype();
    if (array_proto) {
        array->set_prototype(array_proto);
    }
    return array;
}

void return_to_pool(std::unique_ptr<Object> obj) {
    if (!obj || !pools_initialized_) return;
    
    if (obj->get_type() == Object::ObjectType::Ordinary && object_pool_.size() < pool_size_) {
        object_pool_.push_back(std::move(obj));
    } else if (obj->get_type() == Object::ObjectType::Array && array_pool_.size() < pool_size_) {
        array_pool_.push_back(std::move(obj));
    }
}

static Object* object_prototype_object = nullptr;
static Object* array_prototype_object = nullptr;
static Object* function_prototype_object = nullptr;

void set_object_prototype(Object* prototype) {
    object_prototype_object = prototype;
}

Object* get_object_prototype() {
    return object_prototype_object;
}

void set_array_prototype(Object* prototype) {
    array_prototype_object = prototype;
}

Object* get_array_prototype() {
    return array_prototype_object;
}

void set_function_prototype(Object* prototype) {
    function_prototype_object = prototype;
}

Object* get_function_prototype() {
    return function_prototype_object;
}

std::unique_ptr<Object> create_object(Object* prototype) {
    try {
        if (!prototype) {
            return get_pooled_object();
        }
        
        return std::make_unique<Object>(prototype, Object::ObjectType::Ordinary);
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<Object> create_array(uint32_t length) {
    std::unique_ptr<Object> array;

    array = std::make_unique<Object>(Object::ObjectType::Array);

    if (!array) {
        return nullptr;
    }

    array->set_length(length);

    if (array_prototype_object) {
        array->set_prototype(array_prototype_object);
    }

    return array;
}

std::unique_ptr<Object> create_function() {
    Object* func_proto = get_function_prototype();
    if (func_proto) {
        return std::make_unique<Object>(func_proto, Object::ObjectType::Function);
    }
    return std::make_unique<Object>(Object::ObjectType::Function);
}

std::unique_ptr<Object> create_string(const std::string& value) {
    auto str_obj = std::make_unique<Object>(Object::ObjectType::String);
    PropertyDescriptor length_desc(Value(static_cast<double>(value.length())),
        static_cast<PropertyAttributes>(PropertyAttributes::None));
    str_obj->set_property_descriptor("length", length_desc);
    return str_obj;
}

std::unique_ptr<Object> create_number(double value) {
    auto num_obj = std::make_unique<Object>(Object::ObjectType::Number);
    num_obj->set_property("value", Value(value));
    return num_obj;
}

std::unique_ptr<Object> create_boolean(bool value) {
    auto bool_obj = std::make_unique<Object>(Object::ObjectType::Boolean);
    bool_obj->set_property("[[PrimitiveValue]]", Value(value));
    return bool_obj;
}

std::unique_ptr<Function> create_array_method(const std::string& method_name) {
    auto method_fn = [method_name](Context& ctx, const std::vector<Value>& args) -> Value {
        Object* array = ctx.get_this_binding();
        
        if (!array || !array->is_array()) {
            ctx.throw_exception(Value(std::string("Array method called on non-array")));
            return Value();
        }
        
        if (method_name == "map") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                auto result = array->map(args[0].as_function(), ctx, thisArg);
                return result ? Value(result.release()) : Value(ObjectFactory::create_array(0).release());
            } else {
                ctx.throw_exception(Value(std::string("TypeError: Array.map callback must be a function")));
                return Value(ObjectFactory::create_array(0).release());
            }
        } else if (method_name == "filter") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                auto result = array->filter(args[0].as_function(), ctx, thisArg);
                return result ? Value(result.release()) : Value(ObjectFactory::create_array(0).release());
            } else {
                ctx.throw_exception(Value(std::string("TypeError: Array.filter callback must be a function")));
                return Value(ObjectFactory::create_array(0).release());
            }
        } else if (method_name == "reduce") {
            if (args.size() > 0 && args[0].is_function()) {
                Value initial = args.size() > 1 ? args[1] : Value();
                return array->reduce(args[0].as_function(), initial, ctx);
            }
        } else if (method_name == "reduceRight") {
            if (args.size() > 0 && args[0].is_function()) {
                Value initial = args.size() > 1 ? args[1] : Value();
                return array->reduceRight(args[0].as_function(), initial, ctx);
            }
        } else if (method_name == "forEach") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                array->forEach(args[0].as_function(), ctx, thisArg);
                return Value();
            }
        } else if (method_name == "flat") {
            uint32_t depth = 1;
            if (args.size() > 0 && args[0].is_number()) {
                depth = static_cast<uint32_t>(args[0].to_number());
            }
            auto result = array->flat(depth);
            return result ? Value(result.release()) : Value(ObjectFactory::create_array(0).release());
        } else if (method_name == "flatMap") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                auto result = array->flatMap(args[0].as_function(), ctx, thisArg);
                return result ? Value(result.release()) : Value(ObjectFactory::create_array(0).release());
            }
        } else if (method_name == "copyWithin") {
            int32_t target = args.size() > 0 ? static_cast<int32_t>(args[0].to_number()) : 0;
            int32_t start = args.size() > 1 ? static_cast<int32_t>(args[1].to_number()) : 0;
            int32_t end = args.size() > 2 ? static_cast<int32_t>(args[2].to_number()) : -1;
            return Value(array->copyWithin(target, start, end));
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
                return Value(-1.0);
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
                Value element = array->get_element(i);
                if (element.is_null() || element.is_undefined()) {
                    result << "";
                } else {
                    result << element.to_string();
                }
            }
            return Value(result.str());
        } else if (method_name == "groupBy") {
            if (args.size() > 0 && args[0].is_function()) {
                return array->groupBy(args[0].as_function(), ctx);
            } else {
                ctx.throw_exception(Value(std::string("GroupBy requires a callback function")));
                return Value();
            }
        } else if (method_name == "reverse") {
            uint32_t length = array->get_length();
            for (uint32_t i = 0; i < length / 2; i++) {
                Value temp = array->get_element(i);
                array->set_element(i, array->get_element(length - 1 - i));
                array->set_element(length - 1 - i, temp);
            }
            return Value(array);
        } else if (method_name == "sort") {
            uint32_t length = array->get_length();
            std::vector<Value> elements;

            for (uint32_t i = 0; i < length; i++) {
                elements.push_back(array->get_element(i));
            }

            Function* compareFn = nullptr;
            if (args.size() > 0 && args[0].is_function()) {
                compareFn = args[0].as_function();
            }

            if (compareFn) {
                std::sort(elements.begin(), elements.end(), [&](const Value& a, const Value& b) {
                    std::vector<Value> comp_args = {a, b};
                    Value result = compareFn->call(ctx, comp_args);
                    if (ctx.has_exception()) return false;
                    return result.to_number() < 0;
                });
            } else {
                std::sort(elements.begin(), elements.end(), [](const Value& a, const Value& b) {
                    return a.to_string() < b.to_string();
                });
            }

            for (uint32_t i = 0; i < length; i++) {
                array->set_element(i, elements[i]);
            }

            return Value(array);
        } else if (method_name == "shift") {
            return array->shift();
        } else if (method_name == "unshift") {
            if (!args.empty()) {
                uint32_t length = array->get_length();
                uint32_t argCount = args.size();
                
                if (length + argCount >= 1000000) {
                    return Value(static_cast<double>(array->get_length()));
                }
                
                for (uint32_t i = length; i > 0; --i) {
                    Value element = array->get_element(i - 1);
                    array->set_element(i + argCount - 1, element);
                }
                
                for (uint32_t i = 0; i < argCount; ++i) {
                    array->set_element(i, args[i]);
                }
                
                array->set_length(length + argCount);
            }
            return Value(static_cast<double>(array->get_length()));
        } else if (method_name == "splice") {
            uint32_t length = array->get_length();
            uint32_t start = 0;
            uint32_t deleteCount = 0;

            if (args.size() == 0) {
                // No arguments: do nothing, return empty array
                auto deleted = ObjectFactory::create_array(0);
                return Value(deleted.release());
            }

            double start_val = args[0].to_number();
            start = static_cast<uint32_t>(start_val < 0 ? std::max(0.0, length + start_val) : std::min(start_val, static_cast<double>(length)));

            if (args.size() > 1) {
                double delete_val = args[1].to_number();
                deleteCount = static_cast<uint32_t>(std::max(0.0, std::min(delete_val, static_cast<double>(length - start))));
            } else {
                // If only start is provided, delete to end
                deleteCount = length - start;
            }
            
            auto deleted = ObjectFactory::create_array(0);
            for (uint32_t i = start; i < start + deleteCount; i++) {
                deleted->push(array->get_element(i));
            }
            
            uint32_t insertCount = args.size() > 2 ? args.size() - 2 : 0;
            
            if (insertCount > deleteCount) {
                uint32_t shiftBy = insertCount - deleteCount;
                for (uint32_t i = length; i > start + deleteCount; i--) {
                    array->set_element(i + shiftBy - 1, array->get_element(i - 1));
                }
            } else if (insertCount < deleteCount) {
                uint32_t shiftBy = deleteCount - insertCount;
                for (uint32_t i = start + deleteCount; i < length; i++) {
                    array->set_element(i - shiftBy, array->get_element(i));
                }
                for (uint32_t i = length - shiftBy; i < length; i++) {
                    array->delete_element(i);
                }
            }
            
            for (uint32_t i = 0; i < insertCount; i++) {
                array->set_element(start + i, args[i + 2]);
            }
            
            array->set_length(length - deleteCount + insertCount);
            
            return Value(deleted.release());
        } else if (method_name == "toSpliced") {
            uint32_t start = 0;
            uint32_t deleteCount = array->get_length();

            if (args.size() > 0) {
                start = static_cast<uint32_t>(std::max(0.0, args[0].to_number()));
            }
            if (args.size() > 1) {
                deleteCount = static_cast<uint32_t>(std::max(0.0, args[1].to_number()));
            }

            std::vector<Value> items;
            for (size_t i = 2; i < args.size(); i++) {
                items.push_back(args[i]);
            }

            auto result = array->toSpliced(start, deleteCount, items);
            return Value(result.release());
        } else if (method_name == "fill") {
            Value value = args.size() > 0 ? args[0] : Value();
            int32_t start = args.size() > 1 ? static_cast<int32_t>(args[1].to_number()) : 0;
            int32_t end = args.size() > 2 ? static_cast<int32_t>(args[2].to_number()) : -1;
            return Value(array->fill(value, start, end));
        } else if (method_name == "toSorted") {
            Function* compareFn = (args.size() > 0 && args[0].is_function()) ? args[0].as_function() : nullptr;
            auto result = array->toSorted(compareFn, ctx);
            return Value(result.release());
        } else if (method_name == "with") {
            if (args.size() >= 2) {
                uint32_t index = static_cast<uint32_t>(args[0].to_number());
                auto result = array->with_method(index, args[1]);
                return Value(result.release());
            }
            return Value(array);
        } else if (method_name == "at") {
            int32_t index = args.size() > 0 ? static_cast<int32_t>(args[0].to_number()) : 0;
            return array->at(index);
        } else if (method_name == "toReversed") {
            auto result = array->toReversed();
            return Value(result.release());
        } else if (method_name == "find") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(array)};
                    Value result = args[0].as_function()->call(ctx, callback_args, thisArg);
                    if (result.to_boolean()) {
                        return element;
                    }
                }
                return Value();
            }
        } else if (method_name == "findLast") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                return array->findLast(args[0].as_function(), ctx, thisArg);
            }
            return Value();
        } else if (method_name == "findLastIndex") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                return array->findLastIndex(args[0].as_function(), ctx, thisArg);
            }
            return Value(-1.0);
        } else if (method_name == "includes") {
            if (args.size() > 0) {
                Value search_element = args[0];
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);

                    if (search_element.is_number() && element.is_number()) {
                        double search_num = search_element.to_number();
                        double element_num = element.to_number();

                        if (std::isnan(search_num) && std::isnan(element_num)) {
                            return Value(true);
                        }

                        if (search_num == element_num) {
                            return Value(true);
                        }
                    } else if (element.strict_equals(search_element)) {
                        return Value(true);
                    }
                }
                return Value(false);
            }
        } else if (method_name == "some") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(array)};
                    Value result = args[0].as_function()->call(ctx, callback_args, thisArg);
                    if (result.to_boolean()) {
                        return Value(true);
                    }
                }
                return Value(false);
            }
        } else if (method_name == "every") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(array)};
                    Value result = args[0].as_function()->call(ctx, callback_args, thisArg);
                    if (!result.to_boolean()) {
                        return Value(false);
                    }
                }
                return Value(true);
            }
        } else if (method_name == "findIndex") {
            if (args.size() > 0 && args[0].is_function()) {
                Value thisArg = args.size() > 1 ? args[1] : Value();
                uint32_t length = array->get_length();
                for (uint32_t i = 0; i < length; i++) {
                    Value element = array->get_element(i);
                    std::vector<Value> callback_args = {element, Value(static_cast<double>(i)), Value(array)};
                    Value result = args[0].as_function()->call(ctx, callback_args, thisArg);
                    if (result.to_boolean()) {
                        return Value(static_cast<double>(i));
                    }
                }
                return Value(-1.0);
            }
        } else if (method_name == "flat") {
            uint32_t length = array->get_length();
            auto result = ObjectFactory::create_array(0);
            uint32_t result_index = 0;
            
            for (uint32_t i = 0; i < length; i++) {
                Value element = array->get_element(i);
                
                if (element.is_object() && element.as_object() && element.as_object()->is_array()) {
                    Object* nested_array = element.as_object();
                    uint32_t nested_length = nested_array->get_length();
                    
                    for (uint32_t j = 0; j < nested_length; j++) {
                        Value nested_element = nested_array->get_element(j);
                        result->set_element(result_index++, nested_element);
                    }
                } else {
                    result->set_element(result_index++, element);
                }
            }
            
            result->set_length(result_index);
            return Value(result.release());
        } else if (method_name == "concat") {
            auto result = ObjectFactory::create_array(0);
            uint32_t result_index = 0;

            uint32_t this_length = array->get_length();
            for (uint32_t i = 0; i < this_length; i++) {
                Value element = array->get_element(i);
                result->set_element(result_index++, element);
            }

            for (const auto& arg : args) {
                if (arg.is_object() && arg.as_object()->is_array()) {
                    Object* arg_array = arg.as_object();
                    uint32_t arg_length = arg_array->get_length();
                    for (uint32_t i = 0; i < arg_length; i++) {
                        Value element = arg_array->get_element(i);
                        result->set_element(result_index++, element);
                    }
                } else {
                    result->set_element(result_index++, arg);
                }
            }

            result->set_length(result_index);
            return Value(result.release());
        } else if (method_name == "toString") {
            std::ostringstream result;
            uint32_t length = array->get_length();
            for (uint32_t i = 0; i < length; i++) {
                if (i > 0) result << ",";
                result << array->get_element(i).to_string();
            }
            return Value(result.str());
        }

        ctx.throw_exception(Value(std::string("Invalid array method call")));
        return Value();
    };

    uint32_t arity = 1;
    if (method_name == "map" || method_name == "filter" || method_name == "forEach" ||
        method_name == "reduce" || method_name == "reduceRight" || method_name == "find" || method_name == "findIndex" ||
        method_name == "some" || method_name == "every" || method_name == "flatMap" ||
        method_name == "findLast" || method_name == "findLastIndex") {
        arity = 1;
    } else if (method_name == "flat" || method_name == "fill" || method_name == "toSorted" || method_name == "at") {
        arity = 1;
    } else if (method_name == "copyWithin" || method_name == "toSpliced" || method_name == "with") {
        arity = 2;
    } else if (method_name == "slice" || method_name == "splice" || method_name == "indexOf" ||
               method_name == "lastIndexOf") {
        arity = 2;
    } else if (method_name == "push" || method_name == "unshift" || method_name == "concat") {
        arity = 1;
    } else if (method_name == "join") {
        arity = 1;
    } else if (method_name == "pop" || method_name == "shift" || method_name == "reverse" || method_name == "toReversed") {
        arity = 0;
    }

    return std::make_unique<Function>(method_name, method_fn, arity, false);
}

std::unique_ptr<Object> create_error(const std::string& message) {
    auto error_obj = std::make_unique<Error>(Error::Type::Error, message);

    error_obj->set_property("_isError", Value(true));
    return std::unique_ptr<Object>(error_obj.release());
}

std::unique_ptr<Object> create_promise(Context* ctx) {
    auto promise_obj = std::make_unique<Promise>(ctx);
    if (ctx) {
        Value promise_ctor = ctx->get_binding("Promise");
        if (promise_ctor.is_function()) {
            Value proto = static_cast<Object*>(promise_ctor.as_function())->get_property("prototype");
            if (proto.is_object()) promise_obj->set_prototype(proto.as_object());
        }
    }
    return std::unique_ptr<Object>(promise_obj.release());
}

}

}
