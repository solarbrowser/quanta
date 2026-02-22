/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include <memory>
#include <functional>

namespace Quanta {

class Context;

/**
 * JavaScript Proxy implementation
 * ES6 Proxy for intercepting and customizing operations
 */
class Proxy : public Object {
public:
    struct Handler {
        std::function<Value(const Value&, const Value&)> get;                   // (target, key, receiver), 3rd arg via closure
        std::function<bool(const Value&, const Value&, const Value&)> set;      // (target, key, value, receiver)
        std::function<bool(const Value&)> has;                                  // (target, key)
        std::function<bool(const Value&)> deleteProperty;                       // (target, key)
        std::function<std::vector<std::string>()> ownKeys;                      // (target)
        std::function<Value()> getPrototypeOf;                                  // (target)
        std::function<bool(Object*)> setPrototypeOf;                            // (target, proto)
        std::function<bool()> isExtensible;                                     // (target)
        std::function<bool()> preventExtensions;                                // (target)
        std::function<PropertyDescriptor(const Value&)> getOwnPropertyDescriptor; // (target, key)
        std::function<bool(const Value&, const PropertyDescriptor&)> defineProperty; // (target, key, desc)
        std::function<Value(const std::vector<Value>&, const Value&)> apply;    // (target, thisArg, argsList)
        std::function<Value(const std::vector<Value>&)> construct;              // (target, argsList, newTarget)
    };
    
private:
    Object* target_;
    Object* handler_;
    Handler parsed_handler_;
    
public:
    Proxy(Object* target, Object* handler);
    virtual ~Proxy() = default;
    
    Value get_trap(const Value& key);
    bool set_trap(const Value& key, const Value& value);
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
    Value construct_trap(const std::vector<Value>& args);
    
    static Value proxy_constructor(Context& ctx, const std::vector<Value>& args);
    static Value proxy_revocable(Context& ctx, const std::vector<Value>& args);
    
    static void setup_proxy(Context& ctx);
    
    void revoke();
    bool is_revoked() const { return target_ == nullptr; }
    Object* get_proxy_target() const { return target_; }

    Value get_property(const std::string& key) const override;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default) override;
    Object* get_prototype() const override;
    
private:
    void parse_handler();
    void throw_if_revoked(Context& ctx) const;
};

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
