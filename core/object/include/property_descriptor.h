/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"

namespace Quanta {

class Object;

enum class PropertyAttributes : uint32_t {
    None = 0,
    Writable = 1 << 0,
    Enumerable = 1 << 1,
    Configurable = 1 << 2,
    Default = Writable | Enumerable | Configurable
};

inline PropertyAttributes operator|(PropertyAttributes a, PropertyAttributes b) {
    return static_cast<PropertyAttributes>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline PropertyAttributes operator&(PropertyAttributes a, PropertyAttributes b) {
    return static_cast<PropertyAttributes>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline PropertyAttributes operator~(PropertyAttributes a) {
    return static_cast<PropertyAttributes>(~static_cast<uint32_t>(a));
}

inline bool operator&(PropertyAttributes a, uint32_t b) {
    return (static_cast<uint32_t>(a) & b) != 0;
}

/**
 * Property descriptor for JavaScript object properties
 * Supports both data properties and accessor properties with full ES specification compliance
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
};

} // namespace Quanta