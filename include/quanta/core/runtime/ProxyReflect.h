/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include <memory>

namespace Quanta {

class Context;

class Proxy : public Object {
private:
    Object* target_;
    Object* handler_;
    // [[Call]] is a static internal slot fixed at construction, surviving revocation.
    bool target_was_callable_;

public:
    Proxy(Object* target, Object* handler);
    void trace(Visitor& v);
    ~Proxy() = default;

    bool target_was_callable() const { return target_was_callable_; }
    
    Value get_trap(const Value& key);
    Value get_trap(const Value& key, const Value& receiver);
    bool set_trap(const Value& key, const Value& value);
    bool set_trap(const Value& key, const Value& value, const Value& receiver);
    bool has_trap(const Value& key);
    bool delete_trap(const Value& key);
    std::vector<std::string> own_keys_trap();
    Value get_prototype_of_trap();
    bool set_prototype_of_trap(Object* proto);
    bool is_extensible_trap();
    bool prevent_extensions_trap();
    PropertyDescriptor get_own_property_descriptor_trap(const Value& key);
    bool define_property_trap(const Value& key, const PropertyDescriptor& desc);
    Value apply_trap(const std::vector<Value>& args, const Value& this_value);
    // new_target defaults to this proxy; Reflect.construct may pass a different one.
    Value construct_trap(const std::vector<Value>& args, Object* new_target = nullptr);
    
    static Value proxy_constructor(Context& ctx, const std::vector<Value>& args);
    static Value proxy_revocable(Context& ctx, const std::vector<Value>& args);
    
    static void setup_proxy(Context& ctx);
    
    void revoke();
    bool is_revoked() const { return target_ == nullptr; }
    Object* get_proxy_target() const { return target_; }
    Object* get_proxy_handler() const { return handler_; }

    // None of these -- nor get_element()/get_length()/get_prototype() below --
    // are virtual on Object anymore; Object's own switch-based dispatch
    // reaches these directly.
    Value get_property(const std::string& key) const;
    PropertyDescriptor get_property_descriptor(const std::string& key) const;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    bool has_property(const std::string& key) const;
    bool delete_property(const std::string& key);
    // None of these three are virtual on Object anymore -- see
    // Object::get_element()/get_length()/get_prototype()'s own switch-based
    // dispatch in Object.cpp.
    Value get_element(uint32_t index) const;
    uint32_t get_length() const;
    Object* get_prototype() const;
    
private:
    // Lazy GetMethod(handler, name); check has_exception() to tell "absent" from "not callable".
    Function* get_trap_method(const char* name) const;
    void throw_if_revoked(Context& ctx) const;
};

// get_type()-based replacement for dynamic_cast<Proxy*> -- see as_function() in Object.h.
inline Proxy* as_proxy(Object* obj) {
    return (obj && obj->get_type() == Object::ObjectType::Proxy) ? static_cast<Proxy*>(obj) : nullptr;
}
inline const Proxy* as_proxy(const Object* obj) {
    return (obj && obj->get_type() == Object::ObjectType::Proxy) ? static_cast<const Proxy*>(obj) : nullptr;
}

/**
 * JavaScript Reflect implementation
 * ES6 Reflect for default object operations
 */
class Reflect {
public:
    static Value reflect_get(Context& ctx, const std::vector<Value>& args);
    static Value reflect_set(Context& ctx, const std::vector<Value>& args);
    static Value reflect_has(Context& ctx, const std::vector<Value>& args);
    static Value reflect_delete_property(Context& ctx, const std::vector<Value>& args);
    static Value reflect_own_keys(Context& ctx, const std::vector<Value>& args);
    static Value reflect_get_prototype_of(Context& ctx, const std::vector<Value>& args);
    static Value reflect_set_prototype_of(Context& ctx, const std::vector<Value>& args);
    static Value reflect_is_extensible(Context& ctx, const std::vector<Value>& args);
    static Value reflect_prevent_extensions(Context& ctx, const std::vector<Value>& args);
    static Value reflect_get_own_property_descriptor(Context& ctx, const std::vector<Value>& args);
    static Value reflect_define_property(Context& ctx, const std::vector<Value>& args);
    static Value reflect_apply(Context& ctx, const std::vector<Value>& args);
    static Value reflect_construct(Context& ctx, const std::vector<Value>& args);
    
    static void setup_reflect(Context& ctx);
    
private:
    static Object* to_object(const Value& value, Context& ctx);
    static std::string to_property_key(const Value& value);
    static PropertyDescriptor to_property_descriptor(const Value& value);
    static Value from_property_descriptor(const PropertyDescriptor& desc);
};

}
