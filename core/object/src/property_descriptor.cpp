/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/property_descriptor.h"
#include "../../include/Object.h"

namespace Quanta {

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

} // namespace Quanta