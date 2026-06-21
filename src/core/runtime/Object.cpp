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
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/parser/AST.h"
#include <algorithm>
#include <sstream>
#include <iostream>

namespace Quanta {

thread_local Context* Object::current_context_ = nullptr;

std::unordered_map<std::string, std::string> Object::interned_keys_;

static Value make_prop_key_value(const std::string& key) {
    if (key.find("Symbol.") == 0) {
        Symbol* sym = Symbol::get_well_known(key);
        if (sym) return Value(sym);
    }
    return Value(key);
}


Object::Object(ObjectType type) {
    header_.prototype = nullptr;
    header_.type = type;
    header_.flags = 0;
    header_.property_count = 0;
    header_.hash_code = reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF;

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
        if (current->get_type() == ObjectType::Proxy) {
            // Invoke Proxy has trap for prototype chain traversal
            return static_cast<Proxy*>(current)->has_trap(Value(key));
        }
        if (current->has_own_property(key)) {
            return true;
        }
        current = current->get_prototype();
    }
    return false;
}

void Object::add_private_field(const std::string& key, const Value& value) {
    // PrivateFieldAdd: creates the private field slot on this instance.
    // Spec: if object is not extensible, throw TypeError.
    if (!is_extensible()) {
        if (current_context_) {
            current_context_->throw_type_error("Cannot add private field to a non-extensible object");
        }
        return;
    }
    if (!overflow_properties_) {
        overflow_properties_ = std::make_unique<std::unordered_map<std::string, Value>>();
    }
    if (overflow_properties_->find(key) == overflow_properties_->end()) {
        (*overflow_properties_)[key] = value;
        property_insertion_order_.push_back(key);
    }
}

bool Object::has_private_slot(const std::string& key) const {
    // Check if object has a private field/method without the # filter
    if (descriptors_) {
        auto it = descriptors_->find(key);
        if (it != descriptors_->end()) return true;
    }
    if (overflow_properties_) {
        if (overflow_properties_->find(key) != overflow_properties_->end()) return true;
    }
    return false;
}

bool Object::has_own_property(const std::string& key) const {
    // Private fields (#name) are not exposed as own properties (spec: private slot semantics)
    if (!key.empty() && key[0] == '#') return false;

    if (this->get_type() == ObjectType::Proxy) {
        return const_cast<Proxy*>(static_cast<const Proxy*>(this))->has_trap(make_prop_key_value(key));
    }

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

    if (overflow_properties_) {
        return overflow_properties_->find(key) != overflow_properties_->end();
    }

    return false;
}


Value Object::get_property(const std::string& key) const {
    if (this->get_type() == ObjectType::Proxy) {
        return const_cast<Proxy*>(static_cast<const Proxy*>(this))->get_trap(make_prop_key_value(key));
    }

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
        // Own accessor with no getter legitimately yields undefined -- stop here,
        // don't fall through to the prototype chain or the native-method fallback below.
        if (has_own_property(key)) {
            return Value();
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
    // An own accessor property with no getter legitimately yields undefined --
    // that must NOT fall through to the prototype chain (spec [[Get]] stops at
    // the first own property, calling its getter or returning undefined if none).
    if (has_own_property(key)) {
        return Value();
    }

    const Object* original_receiver = this;
    Object* current = header_.prototype;
    while (current) {
        // When prototype is a Proxy, invoke its get trap with the original receiver
        if (current->get_type() == ObjectType::Proxy) {
            Value receiver_val = original_receiver->is_function()
                ? Value(const_cast<Function*>(static_cast<const Function*>(original_receiver)))
                : Value(const_cast<Object*>(original_receiver));
            return static_cast<Proxy*>(current)->get_trap(Value(key), receiver_val);
        }
        // For inherited "get [Symbol.xxx]" accessors, return the original receiver
        if (current->descriptors_) {
            auto desc_it = current->descriptors_->find(key);
            if (desc_it != current->descriptors_->end()) {
                const PropertyDescriptor& desc = desc_it->second;
                if (desc.is_accessor_descriptor() && desc.has_getter()) {
                    Function* getter_fn = dynamic_cast<Function*>(desc.get_getter());
                    if (getter_fn && getter_fn->get_name().find("get [Symbol.") == 0) {
                        if (original_receiver->is_function()) {
                            return Value(const_cast<Function*>(static_cast<const Function*>(original_receiver)));
                        }
                        return Value(const_cast<Object*>(original_receiver));
                    }
                }
            }
        }
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
        // Check descriptors first (handles accessor properties like get "0"(){...})
        if (descriptors_) {
            auto desc_it = descriptors_->find(key);
            if (desc_it != descriptors_->end()) {
                const PropertyDescriptor& desc = desc_it->second;
                if (desc.is_accessor_descriptor()) {
                    if (desc.has_getter()) {
                        Object* getter = desc.get_getter();
                        if (getter && current_context_) {
                            Function* getter_fn = dynamic_cast<Function*>(getter);
                            if (getter_fn) {
                                return getter_fn->call(*current_context_, {}, Value(const_cast<Object*>(this)));
                            }
                        }
                    }
                    return Value(); // setter-only accessor: undefined, don't walk prototype
                }
                if (desc.is_data_descriptor()) {
                    return desc.get_value();
                }
            }
        }
        return get_element(index);
    }


    // Check accessor descriptors BEFORE shape: shape stores Value() placeholder for them.
    // Data descriptors are checked AFTER shape since shape holds the live value for those.
    if (descriptors_) {
        auto desc_it = descriptors_->find(key);
        if (desc_it != descriptors_->end() && desc_it->second.is_accessor_descriptor()) {
            const PropertyDescriptor& desc = desc_it->second;
            if (desc.has_getter()) {
                if (key == "cookie") {
                    return Value(std::string(""));
                }
                Object* getter = desc.get_getter();
                if (getter) {
                    Function* getter_fn = dynamic_cast<Function*>(getter);
                    if (getter_fn) {
                        if (!getter_fn->is_native() && current_context_) {
                            return getter_fn->call(*current_context_, {}, Value(const_cast<Object*>(this)));
                        }
                        if (this->is_function()) {
                            return Value(const_cast<Function*>(static_cast<const Function*>(this)));
                        }
                        return Value(const_cast<Object*>(this));
                    }
                }
            }
            return Value(); // accessor with no getter
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
        if (desc_it != descriptors_->end() && desc_it->second.is_data_descriptor()) {
            return desc_it->second.get_value();
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
        }
        Value length_value(static_cast<double>(new_length));

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
    // If property does not exist on this object, check if a Proxy ancestor has a set trap
    if (!prop_exists) {
        Object* current = header_.prototype;
        while (current) {
            if (current->get_type() == ObjectType::Proxy) {
                Value receiver_val = this->is_function()
                    ? Value(const_cast<Function*>(static_cast<const Function*>(this)))
                    : Value(const_cast<Object*>(this));
                static_cast<Proxy*>(current)->set_trap(Value(key), value, receiver_val);
                return true;
            }
            if (current->has_own_property(key)) break;
            current = current->get_prototype();
        }
    }
    // Check prototype chain for accessor descriptors (setter invocation)
    if (!prop_exists) {
        Object* cur = header_.prototype;
        while (cur) {
            PropertyDescriptor inherited_desc = cur->get_property_descriptor(key);
            if (inherited_desc.is_accessor_descriptor() && inherited_desc.has_setter()) {
                Object* setter = inherited_desc.get_setter();
                if (setter && current_context_) {
                    Function* setter_fn = dynamic_cast<Function*>(setter);
                    if (setter_fn) {
                        Value receiver = this->is_function()
                            ? Value(static_cast<Function*>(this))
                            : Value(this);
                        setter_fn->call(*current_context_, {value}, receiver);
                    }
                }
                return true;
            }
            if (cur->has_own_property(key)) break;
            cur = cur->get_prototype();
        }
    }

    if (prop_exists) {
        PropertyDescriptor desc = get_property_descriptor(key);
        if (desc.is_accessor_descriptor() && desc.has_setter()) {
            Object* setter = desc.get_setter();
            if (setter && current_context_) {
                Function* setter_fn = dynamic_cast<Function*>(setter);
                if (setter_fn) {
                    Value receiver = this->is_function()
                        ? Value(static_cast<Function*>(this))
                        : Value(this);
                    setter_fn->call(*current_context_, {value}, receiver);
                }
            }
            return true;
        }
        // Accessor with no setter
        if (desc.is_accessor_descriptor()) {
            return false;
        }
        if (desc.is_data_descriptor() && !desc.is_writable()) {
            return false;
        }

        if (overflow_properties_) {
            (*overflow_properties_)[key] = value;
            if (descriptors_) {
                auto dit = descriptors_->find(key);
                if (dit != descriptors_->end() && dit->second.is_data_descriptor()) {
                    dit->second.set_value(value);
                }
            }
            return true;
        }
    }
    
    if (!is_extensible()) {
        return false;
    }

    // Store non-default attrs in descriptor map. Insertion-order tracking happens in
    // store_in_overflow below (which always runs for a new key now), not here -- pushing
    // here too would double-insert the key into property_insertion_order_.
    if (attrs != PropertyAttributes::Default) {
        if (!descriptors_) {
            descriptors_ = std::make_unique<std::unordered_map<std::string, PropertyDescriptor>>();
        }
        (*descriptors_)[key] = PropertyDescriptor(value, attrs);
    }

    return store_in_overflow(key, value);
}

bool Object::set_property(const Value& key, const Value& value, PropertyAttributes attrs) {
    return set_property(key.to_property_key(), value, attrs);
}

// OrdinarySet semantics: if prototype chain has non-writable data property, silently fail
bool Object::ordinary_set(const std::string& key, const Value& value) {
    if (!has_own_property(key)) {
        Object* cur = header_.prototype;
        while (cur) {
            if (cur->has_own_property(key)) {
                PropertyDescriptor desc = cur->get_property_descriptor(key);
                if (desc.is_data_descriptor() && !desc.is_writable()) {
                    return false;
                }
                break;
            }
            cur = cur->get_prototype();
        }
    }
    return set_property(key, value);
}

void Object::remove_own_property(const std::string& key) {
    if (descriptors_) descriptors_->erase(key);
    if (overflow_properties_) {
        overflow_properties_->erase(key);
        property_insertion_order_.erase(
            std::remove(property_insertion_order_.begin(), property_insertion_order_.end(), key),
            property_insertion_order_.end());
    }
}

bool Object::delete_property(const std::string& key) {
    // Spec: deleting a non-existent property always returns true
    if (!has_own_property(key)) {
        return true;
    }

    PropertyDescriptor desc = get_property_descriptor(key);
    if (!desc.is_configurable()) {
        return false;
    }

    uint32_t index;
    if (is_array_index(key, &index)) {
        // Mapped arguments accessor: descriptor erase alone makes the property gone,
        // even if delete_element finds no physical elements_ slot to clear.
        bool had_descriptor = descriptors_ && descriptors_->erase(key) > 0;
        bool deleted = delete_element(index);
        return deleted || had_descriptor;
    }

    if (overflow_properties_) {
        auto it = overflow_properties_->find(key);
        if (it != overflow_properties_->end()) {
            overflow_properties_->erase(it);

            // ES1: Also erase from descriptors to ensure property is fully deleted
            if (descriptors_) {
                descriptors_->erase(key);
            }

            // Mirrors the push in store_in_overflow -- without this, a set/delete/set
            // cycle on the same key would grow property_insertion_order_ unboundedly.
            property_insertion_order_.erase(
                std::remove(property_insertion_order_.begin(), property_insertion_order_.end(), key),
                property_insertion_order_.end());

            header_.property_count--;
            update_hash_code();
            return true;
        }
    }

    // Fallback: property may only exist in descriptors (e.g. Function "prototype" intercepted
    // by Function::set_property without storing in shape/overflow)
    if (descriptors_) {
        auto it = descriptors_->find(key);
        if (it != descriptors_->end()) {
            descriptors_->erase(it);
            if (header_.property_count > 0) header_.property_count--;
            update_hash_code();
            return true;
        }
    }

    return false;
}

Value Object::get_element(uint32_t index) const {
    // TypedArrayBase::get_element(size_t) is a different signature, not a virtual
    // override of this uint32_t one -- dispatch explicitly so generic Array.prototype
    // methods invoked via .call()/.apply() on a typed array read real backing-store data.
    if (header_.type == ObjectType::TypedArray) {
        return static_cast<const TypedArrayBase*>(this)->get_element(static_cast<size_t>(index));
    }
    // Arguments: check all descriptors (accessor AND data) since they hold live bindings.
    if (header_.type == ObjectType::Arguments && descriptors_) {
        auto it = descriptors_->find(std::to_string(index));
        if (it != descriptors_->end()) {
            if (it->second.is_accessor_descriptor() && it->second.has_getter()) {
                Object* getter = it->second.get_getter();
                if (getter && current_context_) {
                    Function* gfn = dynamic_cast<Function*>(getter);
                    if (gfn) return gfn->call(*current_context_, {}, Value(const_cast<Object*>(this)));
                }
            }
            if (it->second.is_data_descriptor()) return it->second.get_value();
        }
    }

    // For all other types: check descriptors_ for both accessor and data properties.
    // Data descriptor values are kept in sync by set_element, so prefer them over elements_.
    if (header_.type != ObjectType::Arguments && descriptors_) {
        auto it = descriptors_->find(std::to_string(index));
        if (it != descriptors_->end()) {
            if (it->second.is_accessor_descriptor()) {
                if (it->second.has_getter()) {
                    Object* getter = it->second.get_getter();
                    if (getter && current_context_) {
                        Function* gfn = dynamic_cast<Function*>(getter);
                        if (gfn) return gfn->call(*current_context_, {}, Value(const_cast<Object*>(this)));
                    }
                }
                return Value(); // setter-only: own property shadows prototype
            }
            if (it->second.is_data_descriptor()) return it->second.get_value();
        }
    }

    bool is_hole = deleted_elements_ && deleted_elements_->count(index) > 0;
    if (index < elements_.size() && !is_hole) {
        return elements_[index];
    }

    // For non-Arguments/TypedArray (includes Array holes/out-of-bounds): check
    // data descriptors, overflow, and walk the prototype chain.
    if (header_.type != ObjectType::Arguments && header_.type != ObjectType::TypedArray) {
        std::string key = std::to_string(index);
        if (descriptors_) {
            auto it = descriptors_->find(key);
            if (it != descriptors_->end() && it->second.is_data_descriptor())
                return it->second.get_value();
        }
        if (overflow_properties_) {
            auto it = overflow_properties_->find(key);
            if (it != overflow_properties_->end()) return it->second;
        }
        Object* proto = get_prototype();
        return proto ? proto->get_property(key) : Value();
    }
    return Value();
}

bool Object::set_element(uint32_t index, const Value& value) {
    // Same dispatch problem as get_element: TypedArrayBase::set_element(size_t) doesn't
    // override this uint32_t signature, so generic Array.prototype methods called via
    // .call()/.apply() on a typed array must be routed there explicitly.
    if (header_.type == ObjectType::TypedArray) {
        return const_cast<TypedArrayBase*>(static_cast<const TypedArrayBase*>(this))
            ->set_element(static_cast<size_t>(index), value);
    }
    // Check descriptors_ for all types: respect accessor setters and writable flags.
    if (descriptors_) {
        auto it = descriptors_->find(std::to_string(index));
        if (it != descriptors_->end()) {
            if (it->second.is_accessor_descriptor()) {
                if (it->second.has_setter()) {
                    Object* setter = it->second.get_setter();
                    if (setter && current_context_) {
                        Function* setter_fn = dynamic_cast<Function*>(setter);
                        if (setter_fn) setter_fn->call(*current_context_, {value}, Value(this));
                    }
                }
                return true;
            }
            if (it->second.is_data_descriptor()) {
                if (!it->second.is_writable()) return false;
                it->second.set_value(value); // keep descriptor in sync with elements_
            }
        }
    }
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
                // never shrink length on a write within bounds
                if (it != overflow_properties_->end() && it->second.to_number() < new_size) {
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

    // Overflow and descriptor-only properties, in creation order via property_insertion_order_
    // -- overflow_properties_ is an unordered_map and has no enumeration order of its own.
    for (const auto& key : property_insertion_order_) {
        bool in_overflow = overflow_properties_ && overflow_properties_->count(key) > 0;
        bool in_descriptors = descriptors_ && descriptors_->count(key) > 0;
        if (!in_overflow && !in_descriptors) continue;
        bool already = false;
        for (const auto& k : raw_keys) {
            if (k == key) { already = true; break; }
        }
        if (!already) {
            raw_keys.push_back(key);
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

std::vector<std::string> Object::get_internal_property_keys() const {
    return get_own_property_keys();
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
            // ES6 9.4.4: mapped arguments slots appear as DATA descriptors to callers.
            // Internally we use accessor descriptors for the param aliasing, but expose
            // {value, writable, enumerable, configurable} to [[GetOwnProperty]].
            if (header_.type == ObjectType::Arguments && it->second.is_accessor_descriptor()) {
                Function* gfn = it->second.get_getter()
                    ? dynamic_cast<Function*>(it->second.get_getter()) : nullptr;
                if (gfn && gfn->has_property("__param_map__")) {
                    Value cur = (current_context_ && !gfn->has_property("__param_map_severed__"))
                        ? gfn->call(*current_context_, {}, Value(const_cast<Object*>(this)))
                        : Value();
                    PropertyDescriptor data(cur);
                    data.set_writable(true);
                    data.set_enumerable(it->second.is_enumerable());
                    data.set_configurable(it->second.is_configurable());
                    return data;
                }
            }
            return it->second;
        }
    }

    // Property not in descriptor map but exists (e.g. plain overflow-stored data property):
    // default attrs.
    if (has_own_property(key)) {
        Value value = get_own_property(key);
        return PropertyDescriptor(value, PropertyAttributes::Default);
    }

    return PropertyDescriptor();
}

bool Object::set_property_descriptor(const std::string& key, const PropertyDescriptor& desc) {
    // Captured before any placeholder (store_in_overflow) write below can make
    // has_own_property look true for a property that's brand new in this very call.
    bool existed_before_this_call = has_own_property(key);

    // Reject new properties on non-extensible objects
    if (!is_extensible() && !existed_before_this_call) {
        return false;
    }

    if (!descriptors_) {
        descriptors_ = std::make_unique<std::unordered_map<std::string, PropertyDescriptor>>();
    }

    // Early non-configurable check -- runs BEFORE elements_ is written.
    // Covers both descriptors_-stored and externally-stored (e.g. Array.length) properties.
    if (current_context_ && has_own_property(key)) {
        PropertyDescriptor existing_desc;
        auto existing_it = descriptors_ ? descriptors_->find(key) : (decltype(descriptors_->begin()))descriptors_->end();
        bool in_descriptors = descriptors_ && existing_it != descriptors_->end();
        if (in_descriptors) {
            existing_desc = existing_it->second;
        } else {
            existing_desc = get_property_descriptor(key);
        }
        if (existing_desc.has_configurable() && !existing_desc.is_configurable()) {
            if (desc.has_configurable() && desc.is_configurable()) {
                return false; // caller (defineProperty_fn/defineProperties_fn) handles throwing
            }
            if (!desc.is_generic_descriptor()) {
                bool existing_is_accessor = existing_desc.is_accessor_descriptor();
                bool new_is_accessor = desc.is_accessor_descriptor();
                if (existing_is_accessor != new_is_accessor) return false;
                if (!existing_is_accessor && existing_desc.has_writable() && !existing_desc.is_writable()) {
                    if (desc.has_writable() && desc.is_writable()) return false;
                }
            }
        }
    }

    if (desc.is_data_descriptor()) {
        // ES6 9.4.4.3: if Arguments has a live param-mapped accessor for this index,
        // {value:V} updates the binding and keeps the mapping active (do not replace the accessor).
        // Only sever when desc explicitly sets writable:false.
        if (header_.type == ObjectType::Arguments && desc.has_value() && descriptors_) {
            auto it = descriptors_->find(key);
            if (it != descriptors_->end() && it->second.is_accessor_descriptor()) {
                Function* gfn = it->second.get_getter()
                    ? dynamic_cast<Function*>(it->second.get_getter()) : nullptr;
                if (gfn && gfn->has_property("__param_map__") && current_context_) {
                    // Update param binding
                    if (it->second.has_setter()) {
                        Function* setter_fn = dynamic_cast<Function*>(it->second.get_setter());
                        if (setter_fn) setter_fn->call(*current_context_, {desc.get_value()}, Value(this));
                    }
                    // Apply any attribute changes to the accessor descriptor
                    if (desc.has_enumerable())   it->second.set_enumerable(desc.is_enumerable());
                    if (desc.has_configurable()) it->second.set_configurable(desc.is_configurable());
                    // Sever only when explicitly non-writable
                    if (desc.has_writable() && !desc.is_writable()) {
                        PropertyDescriptor data(desc.get_value());
                        data.set_writable(false);
                        data.set_enumerable(it->second.is_enumerable());
                        data.set_configurable(it->second.is_configurable());
                        (*descriptors_)[key] = data;
                        uint32_t idx;
                        if (is_array_index(key, &idx)) {
                            if (idx >= elements_.size()) elements_.resize(idx + 1);
                            elements_[idx] = desc.get_value();
                        }
                    }
                    return true;
                }
                // Non-param accessor (user-installed): just call its setter if present
                if (it->second.has_setter() && current_context_) {
                    Function* setter_fn = dynamic_cast<Function*>(it->second.get_setter());
                    if (setter_fn) setter_fn->call(*current_context_, {desc.get_value()}, Value(this));
                }
            }
        }
        uint32_t index;
        if (is_array_index(key, &index)) {
            // cap growth to avoid huge upfront allocation on a sparse high index
            if (desc.has_value() && index <= 10000000) {
                if (index >= elements_.size()) {
                    elements_.resize(index + 1);
                }
                elements_[index] = desc.get_value();
            }
        } else {
            // Directly update overflow to bypass writable check on existing props
            if (desc.has_value()) {
                if (overflow_properties_ && overflow_properties_->count(key)) {
                    (*overflow_properties_)[key] = desc.get_value();
                } else {
                    set_property(key, desc.get_value(), desc.get_attributes());
                }
            }
        }
    } else if (desc.is_accessor_descriptor() || desc.is_generic_descriptor()) {
        // Ensure the property exists (in overflow) so has_own_property works
        uint32_t index;
        if (is_array_index(key, &index)) {
            uint32_t new_size = index + 1;
            if (header_.type == ObjectType::Array && new_size > get_length()) {
                if (overflow_properties_) {
                    auto it = overflow_properties_->find("length");
                    if (it != overflow_properties_->end()) {
                        it->second = Value(static_cast<double>(new_size));
                    }
                }
            }
        } else if (!has_own_property(key)) {
            store_in_overflow(key, Value());
        }
    }

    // Merge attribute-only descriptors: generic descriptors, and data descriptors that
    // specify writable/enumerable/configurable but not value (e.g. defineProperty with
    // {writable:false} alone must not overwrite the existing value).
    bool is_attr_only = desc.is_generic_descriptor() ||
                        (desc.is_data_descriptor() && !desc.has_value());
    if (is_attr_only) {
        if (descriptors_->count(key)) {
            PropertyDescriptor& existing = (*descriptors_)[key];
            // Non-configurable property enforcement -- only when configurable was explicitly set to false
            // and only when called from a live JS context (not during engine initialization)
            if (current_context_ && existing.has_configurable() && !existing.is_configurable()) {
                // Cannot change configurable to true
                if (desc.has_configurable() && desc.is_configurable()) {
                    current_context_->throw_type_error("Cannot redefine non-configurable property: " + key);
                    return false;
                }
                // Cannot change enumerable
                if (existing.has_enumerable() && desc.has_enumerable() && desc.is_enumerable() != existing.is_enumerable()) {
                    current_context_->throw_type_error("Cannot change enumerable of non-configurable property: " + key);
                    return false;
                }
            }
            // ES6 9.4.4.3: if defineProperty makes a mapped arg non-writable, sever the mapping.
            if (header_.type == ObjectType::Arguments && existing.is_accessor_descriptor() &&
                    desc.has_writable() && !desc.is_writable()) {
                Function* gfn = existing.get_getter()
                    ? dynamic_cast<Function*>(existing.get_getter()) : nullptr;
                if (gfn && gfn->has_property("__param_map__") && current_context_) {
                    Value cur = gfn->call(*current_context_, {}, Value(this));
                    PropertyDescriptor data(cur);
                    data.set_writable(false);
                    data.set_enumerable(existing.is_enumerable());
                    data.set_configurable(existing.is_configurable());
                    if (desc.has_enumerable())   data.set_enumerable(desc.is_enumerable());
                    if (desc.has_configurable()) data.set_configurable(desc.is_configurable());
                    existing = data;
                    uint32_t idx;
                    if (is_array_index(key, &idx)) {
                        if (idx >= elements_.size()) elements_.resize(idx + 1);
                        elements_[idx] = cur;
                    }
                    return true;
                }
            }
            if (desc.has_writable())     existing.set_writable(desc.is_writable());
            if (desc.has_enumerable())   existing.set_enumerable(desc.is_enumerable());
            if (desc.has_configurable()) existing.set_configurable(desc.is_configurable());
        } else {
            // No existing descriptor entry -- create one preserving the current value.
            // Brand-new properties default to WEC=false (spec); only a property that
            // genuinely existed before this call inherits the WEC=true shape default.
            Value current_val = get_property(key);
            PropertyDescriptor merged(current_val,
                existed_before_this_call ? PropertyAttributes::Default : PropertyAttributes::None);
            if (desc.has_writable())     merged.set_writable(desc.is_writable());
            if (desc.has_enumerable())   merged.set_enumerable(desc.is_enumerable());
            if (desc.has_configurable()) merged.set_configurable(desc.is_configurable());
            (*descriptors_)[key] = merged;
            property_insertion_order_.push_back(key);
        }
    } else {
        // When replacing with a data/accessor descriptor that doesn't specify all attribute
        // flags, merge the unspecified ones from the existing descriptor so we don't silently
        // reset writable/enumerable/configurable to their defaults.
        if (descriptors_->count(key)) {
            PropertyDescriptor& existing = (*descriptors_)[key];
            // Enforce non-configurable constraints (same as ObjectBuiltin.cpp user-space check,
            // but also catches C++ callers like JSON.parse's CreateDataProperty).
            if (current_context_ && existing.has_configurable() && !existing.is_configurable()) {
                if (desc.has_configurable() && desc.is_configurable()) return false;
                bool existing_is_accessor = existing.is_accessor_descriptor();
                bool new_is_accessor = desc.is_accessor_descriptor();
                if (existing_is_accessor != new_is_accessor) return false;
                if (!existing_is_accessor && existing.has_writable() && !existing.is_writable()) {
                    if (desc.has_writable() && desc.is_writable()) return false;
                }
            }
            PropertyDescriptor merged = desc;
            if (!desc.has_writable())     { if (existing.has_writable())     merged.set_writable(existing.is_writable()); }
            if (!desc.has_enumerable())   { if (existing.has_enumerable())   merged.set_enumerable(existing.is_enumerable()); }
            if (!desc.has_configurable()) { if (existing.has_configurable()) merged.set_configurable(existing.is_configurable()); }
            // Preserve existing getter/setter when not specified in the new accessor descriptor.
            // Exception: don't preserve param-mapped Arguments setters (spec 10.4.4.4: defining
            // an accessor on a mapped slot severs the mapping, clearing the setter).
            if (desc.is_accessor_descriptor() && existing.is_accessor_descriptor()) {
                bool is_param_mapped = false;
                if (existing.has_getter()) {
                    Function* gfn = dynamic_cast<Function*>(existing.get_getter());
                    if (gfn && gfn->has_property("__param_map__")) is_param_mapped = true;
                }
                if (!is_param_mapped) {
                    if (!desc.has_getter() && existing.has_getter()) merged.set_getter(existing.get_getter());
                    if (!desc.has_setter() && existing.has_setter()) merged.set_setter(existing.get_setter());
                }
            }
            (*descriptors_)[key] = merged;
        } else {
            // No existing descriptor entry. For a property already in shape/overflow,
            // preserve the unspecified attributes (shape default is WEC=true) so that
            // defineProperty({value:X}) on an existing enumerable prop keeps it enumerable.
            if (existed_before_this_call) {
                PropertyDescriptor current = get_property_descriptor(key);
                PropertyDescriptor merged = desc;
                if (!desc.has_writable()     && current.has_writable())     merged.set_writable(current.is_writable());
                if (!desc.has_enumerable()   && current.has_enumerable())   merged.set_enumerable(current.is_enumerable());
                if (!desc.has_configurable() && current.has_configurable()) merged.set_configurable(current.is_configurable());
                (*descriptors_)[key] = merged;
            } else {
                (*descriptors_)[key] = desc;
            }
            property_insertion_order_.push_back(key);
        }
    }

    return true;
}

uint32_t Object::get_length() const {
    if (header_.type == ObjectType::TypedArray) {
        return static_cast<uint32_t>(static_cast<const TypedArrayBase*>(this)->length());
    }
    if (header_.type == ObjectType::Array || header_.type == ObjectType::Arguments) {
        Value length_val = get_own_property("length");
        if (length_val.is_number()) {
            return static_cast<uint32_t>(length_val.as_number());
        }
    }
    // plain array-likes (e.g. Array.prototype.every.call(plainObj)) store length as a property
    // ToLength per spec: coerce length via ToNumber. Apply to all non-Array/Arguments types
    // so that Boolean/Number/String wrappers and custom objects with prototype-chain length work.
    if (header_.type != ObjectType::Array && header_.type != ObjectType::Arguments
        && header_.type != ObjectType::TypedArray && has_property("length")) {
        Value length_val = get_property("length");
        if (!length_val.is_symbol() && !length_val.is_undefined() && !length_val.is_null()) {
            double n = length_val.to_number();
            if (!std::isnan(n) && n >= 0) {
                if (std::isinf(n)) return static_cast<uint32_t>(elements_.size()); // Infinity: use actual size
                return static_cast<uint32_t>(std::min(n, (double)UINT32_MAX));
            }
            return 0; // NaN or negative: ToLength = 0
        }
        // length property exists but getter returns undefined/null: ToLength = 0
        return 0;
    }
    // No "length" property at all on a generic object: ToLength(undefined) = 0.
    if (header_.type != ObjectType::Array && header_.type != ObjectType::Arguments
        && header_.type != ObjectType::TypedArray) {
        return 0;
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

bool Object::store_in_overflow(const std::string& key, const Value& value) {
    if (!overflow_properties_) {
        overflow_properties_ = std::make_unique<std::unordered_map<std::string, Value>>();
    }
    
    bool is_new_property = overflow_properties_->find(key) == overflow_properties_->end();

    (*overflow_properties_)[key] = value;

    if (is_new_property) {
        header_.property_count++;
        // overflow_properties_ is an unordered_map (no enumeration order of its own) --
        // property_insertion_order_ is what get_own_property_keys() relies on for spec
        // creation-order enumeration. Mirrored back out in delete_property below.
        property_insertion_order_.push_back(key);
    }
    
    update_hash_code();
    
    
    return true;
}

void Object::clear_properties() {
    elements_.clear();

    if (overflow_properties_) {
        overflow_properties_->clear();
    }
    if (descriptors_) {
        descriptors_->clear();
    }

    property_insertion_order_.clear();

    header_.property_count = 0;

    header_.type = ObjectType::Ordinary;
    header_.flags = 0;

    update_hash_code();
}

void Object::update_hash_code() {
    header_.hash_code = (header_.property_count << 16) | static_cast<uint32_t>(header_.type);
}

std::string Object::to_string() const {
    // String/Number wrapper objects store their primitive in [[PrimitiveValue]]
    if (has_property("[[PrimitiveValue]]")) {
        Value pv = get_property("[[PrimitiveValue]]");
        if (pv.is_string()) {
            return pv.to_string();
        }
    }

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

    if (header_.type == ObjectType::Proxy) {
        return "[object Object]";
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
    str_obj->set_property("[[PrimitiveValue]]", Value(value));
    // String exotic: each index is a non-writable, non-configurable, enumerable character property
    for (size_t i = 0; i < value.size(); i++) {
        PropertyDescriptor char_desc(Value(std::string(1, value[i])),
            static_cast<PropertyAttributes>(PropertyAttributes::Enumerable));
        str_obj->set_property_descriptor(std::to_string(i), char_desc);
    }
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
