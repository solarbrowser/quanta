/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "Value.h"
#include "Object.h"
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
    // Proxy handler trap signatures
    struct Handler {
        std::function<Value(const Value&)> get;
        std::function<bool(const Value&, const Value&)> set;
        std::function<bool(const Value&)> has;
        std::function<bool(const Value&)> deleteProperty;
        std::function<std::vector<std::string>()> ownKeys;
        std::function<Value(const Value&)> getPrototypeOf;
        std::function<bool(Object*)> setPrototypeOf;
        std::function<bool()> isExtensible;
        std::function<bool()> preventExtensions;
        std::function<PropertyDescriptor(const Value&)> getOwnPropertyDescriptor;
        std::function<bool(const Value&, const PropertyDescriptor&)> defineProperty;
        std::function<Value(const std::vector<Value>&)> apply;
        std::function<Value(const std::vector<Value>&)> construct;
    };
    
private:
    Object* target_;
    Object* handler_;
    Handler parsed_handler_;
    
public:
    Proxy(Object* target, Object* handler);
    virtual ~Proxy() = default;
    
    // Proxy operations
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
    
    // Proxy built-in methods
    static Value proxy_constructor(Context& ctx, const std::vector<Value>& args);
    static Value proxy_revocable(Context& ctx, const std::vector<Value>& args);
    
    // Setup Proxy
    static void setup_proxy(Context& ctx);
    
    // Revoke proxy
    void revoke();
    bool is_revoked() const { return target_ == nullptr; }
    
    // Override Object methods to use traps
    Value get_property(const std::string& key) const override;
    
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
    // Reflect methods
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
    
    // Setup Reflect
    static void setup_reflect(Context& ctx);
    
private:
    // Helper methods
    static Object* to_object(const Value& value, Context& ctx);
    static std::string to_property_key(const Value& value);
    static PropertyDescriptor to_property_descriptor(const Value& value);
    static Value from_property_descriptor(const PropertyDescriptor& desc);
};

} // namespace Quanta