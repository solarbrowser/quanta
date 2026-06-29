/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/core/engine/Context.h"
#include "quanta/parser/AST.h"
#include <iostream>
#include <algorithm>
#include <unordered_set>
#include <cstdlib>

namespace Quanta {

// Helper: convert a Value key (possibly Symbol) to a property key string
static std::string to_prop_key(const Value& key) {
    if (key.is_symbol()) return key.as_symbol()->to_property_key();
    return key.to_string();
}

// OrdinarySet(O, P, V, Receiver), defined further below -- forward declared for set_trap's use.
static bool ordinary_set_with_receiver(Object* O, const std::string& key, const Value& value, Object* receiver, Context& ctx);
// from_prop_key is defined further below; forward-declared for ordinary_get_with_receiver's use.
static Value from_prop_key(const std::string& key);

// OrdinaryGet(O, P, Receiver): walks O's prototype chain, calls accessors with receiver as `this`.
static Value ordinary_get_with_receiver(Object* O, const std::string& key, Object* receiver) {
    PropertyDescriptor desc = O->get_property_descriptor(key);
    if (desc.is_data_descriptor()) return desc.get_value();
    if (desc.is_accessor_descriptor()) {
        Function* getter = desc.get_getter() ? dynamic_cast<Function*>(desc.get_getter()) : nullptr;
        if (!getter) return Value();
        Context* ctx = Object::current_context_;
        if (!ctx) return Value();
        return getter->call(*ctx, {}, receiver ? Value(receiver) : Value());
    }
    Object* proto = O->get_prototype();
    if (!proto) return Value();
    if (proto->get_type() == Object::ObjectType::Proxy) {
        return static_cast<Proxy*>(proto)->get_trap(from_prop_key(key), receiver ? Value(receiver) : Value());
    }
    return ordinary_get_with_receiver(proto, key, receiver);
}

// Helper: create Value from internal property key string, restoring actual Symbol objects.
static Value from_prop_key(const std::string& key) {
    if (key.find("Symbol.") == 0) {
        Symbol* sym = Symbol::get_well_known(key);
        if (sym) return Value(sym);
    }
    if (key.find("@@sym:") == 0) {
        Symbol* sym = Symbol::find_by_property_key(key);
        if (sym) return Value(sym);
    }
    return Value(key);
}

// IsCallable(target): recurses through nested Proxy targets, since [[Call]] existence is static.
static bool target_callable(Object* target) {
    if (!target) return false;
    if (target->get_type() == Object::ObjectType::Proxy) return static_cast<Proxy*>(target)->target_was_callable();
    return target->is_function();
}

// OrdinarySetPrototypeOf: same-value short-circuit, extensibility check, then cycle detection.
static bool ordinary_set_prototype_of(Object* obj, Object* new_proto) {
    if (new_proto == obj->get_prototype()) return true;
    if (obj->has_own_property("__immutableProto__")) return false;
    if (!obj->is_extensible()) return false;
    Object* p = new_proto;
    while (p) {
        if (p == obj) return false;
        if (p->get_type() == Object::ObjectType::Proxy) break;
        p = p->get_prototype();
    }
    obj->set_prototype(new_proto);
    return true;
}

Proxy::Proxy(Object* target, Object* handler)
    : Object(ObjectType::Proxy), target_(target), handler_(handler),
      target_was_callable_(target_callable(target)) {
}

Function* Proxy::get_trap_method(const char* name) const {
    if (!handler_) return nullptr;
    Value trap = handler_->get_property(name);
    if (trap.is_undefined() || trap.is_null()) return nullptr;
    if (!trap.is_function()) {
        if (Object::current_context_) {
            Object::current_context_->throw_type_error(std::string("Proxy trap \"") + name + "\" is not callable");
        }
        return nullptr;
    }
    return trap.as_function();
}

Value Proxy::get_trap(const Value& key) {
    return get_trap(key, Value(static_cast<Object*>(this)));
}

Value Proxy::get_trap(const Value& key, const Value& receiver) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'get' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("get");
    if (Object::current_context_ && Object::current_context_->has_exception()) return Value();

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        Value result = ctx ? trap_fn->call(*ctx, {Value(target_), key, receiver}, Value(handler_))
                            : target_->get_property(key.to_string());
        // The trap may have revoked this proxy itself (target_ now null) -- skip the
        // invariant checks below, which read target_.
        if (is_revoked()) return result;
        // Invariant: non-writable, non-configurable data property => result must equal target value
        std::string key_str = to_prop_key(key);
        PropertyDescriptor target_desc = target_->get_property_descriptor(key_str);
        if (target_desc.is_data_descriptor() && !target_desc.is_writable() && !target_desc.is_configurable()) {
            Value tval = target_desc.get_value();
            bool same = (result.is_undefined() && tval.is_undefined()) ||
                        (result.is_null() && tval.is_null()) ||
                        (result.is_number() && tval.is_number() && result.as_number() == tval.as_number()) ||
                        (result.is_string() && tval.is_string() && result.to_string() == tval.to_string()) ||
                        (result.is_boolean() && tval.is_boolean() && result.to_boolean() == tval.to_boolean()) ||
                        (result.is_object() && tval.is_object() && result.as_object() == tval.as_object()) ||
                        (result.is_function() && tval.is_function() && result.as_function() == tval.as_function());
            if (!same) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'get' proxy invariant violated: non-writable non-configurable property");
                throw std::runtime_error("TypeError: 'get' proxy invariant violated: non-writable non-configurable property");
            }
        }
        // Invariant: non-configurable accessor without an actual getter function => result must be undefined.
        if (target_desc.is_accessor_descriptor() && !target_desc.is_configurable() && !target_desc.get_getter()) {
            if (!result.is_undefined()) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'get' proxy invariant violated: non-configurable accessor without getter");
                throw std::runtime_error("TypeError: 'get' proxy invariant violated: non-configurable accessor without getter");
            }
        }
        return result;
    }

    // No get trap: forward target.[[Get]](P, Receiver) preserving the receiver for accessor `this`.
    Object* recv = receiver.is_function() ? static_cast<Object*>(receiver.as_function())
                 : receiver.is_object() ? receiver.as_object() : nullptr;
    if (target_->get_type() == ObjectType::Proxy) {
        return static_cast<Proxy*>(target_)->get_trap(key, receiver);
    }
    return ordinary_get_with_receiver(target_, to_prop_key(key), recv);
}

bool Proxy::set_trap(const Value& key, const Value& value) {
    return set_trap(key, value, Value(static_cast<Object*>(this)));
}

bool Proxy::set_trap(const Value& key, const Value& value, const Value& receiver) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'set' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("set");
    if (Object::current_context_ && Object::current_context_->has_exception()) return false;

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        bool result = ctx ? trap_fn->call(*ctx, {Value(target_), key, value, receiver}, Value(handler_)).to_boolean()
                           : target_->set_property(key.to_string(), value);
        if (result) {
            // Invariant: non-writable, non-configurable data property => new value must equal existing
            std::string key_str = to_prop_key(key);
            PropertyDescriptor target_desc = target_->get_property_descriptor(key_str);
            if (target_desc.is_data_descriptor() && !target_desc.is_writable() && !target_desc.is_configurable()) {
                Value tval = target_desc.get_value();
                bool same = (value.is_undefined() && tval.is_undefined()) ||
                            (value.is_null() && tval.is_null()) ||
                            (value.is_number() && tval.is_number() && value.as_number() == tval.as_number()) ||
                            (value.is_string() && tval.is_string() && value.to_string() == tval.to_string()) ||
                            (value.is_boolean() && tval.is_boolean() && value.to_boolean() == tval.to_boolean()) ||
                            (value.is_object() && tval.is_object() && value.as_object() == tval.as_object()) ||
                            (value.is_function() && tval.is_function() && value.as_function() == tval.as_function());
                if (!same) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'set' proxy invariant violated: non-writable non-configurable property");
                    throw std::runtime_error("TypeError: 'set' proxy invariant violated: non-writable non-configurable property");
                }
            }
            // Invariant: non-configurable accessor without an actual setter function => cannot set.
            if (target_desc.is_accessor_descriptor() && !target_desc.is_configurable() && !target_desc.get_setter()) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'set' proxy invariant violated: non-configurable accessor without setter");
                throw std::runtime_error("TypeError: 'set' proxy invariant violated: non-configurable accessor without setter");
            }
        }
        return result;
    }

    // No set trap: delegate to target.[[Set]](P, V, Receiver) -- OrdinarySet against the real receiver.
    std::string key_str = to_prop_key(key);
    Context* ctx = Object::current_context_;
    if (!ctx) { target_->set_property(key_str, value); return true; }
    Object* receiver_obj = receiver.is_function() ? static_cast<Object*>(receiver.as_function())
                          : receiver.is_object() ? receiver.as_object() : nullptr;
    return ordinary_set_with_receiver(target_, key_str, value, receiver_obj, *ctx);
}

bool Proxy::has_trap(const Value& key) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'has' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("has");
    if (Object::current_context_ && Object::current_context_->has_exception()) return false;

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        bool result = ctx ? trap_fn->call(*ctx, {Value(target_), key}, Value(handler_)).to_boolean()
                           : target_->has_property(key.to_string());
        if (Object::current_context_ && Object::current_context_->has_exception()) return false;
        if (!result) {
            std::string key_str = to_prop_key(key);
            if (target_->has_own_property(key_str)) {
                PropertyDescriptor target_desc = target_->get_property_descriptor(key_str);
                // Invariant: cannot hide non-configurable own property
                if (!target_desc.is_configurable()) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'has' proxy invariant violated: non-configurable own property cannot be reported non-existent");
                    throw std::runtime_error("TypeError: 'has' proxy invariant violated: non-configurable own property cannot be reported non-existent");
                }
                // Invariant: cannot hide own property of non-extensible target
                if (!target_->is_extensible()) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'has' proxy invariant violated: own property of non-extensible target cannot be reported non-existent");
                    throw std::runtime_error("TypeError: 'has' proxy invariant violated: own property of non-extensible target cannot be reported non-existent");
                }
            }
        }
        return result;
    }

    // Use has_own_property to avoid Object.prototype methods leaking into with scopes
    // and causing infinite recursion via get trap re-entrancy.
    std::string key_str = to_prop_key(key);
    if (target_->has_own_property(key_str)) return true;
    Object* proto = target_->get_prototype();
    while (proto) {
        if (proto->has_own_property(key_str)) return true;
        proto = proto->get_prototype();
    }
    return false;
}

bool Proxy::delete_trap(const Value& key) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'deleteProperty' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("deleteProperty");
    if (Object::current_context_ && Object::current_context_->has_exception()) return false;

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        bool result = ctx ? trap_fn->call(*ctx, {Value(target_), key}, Value(handler_)).to_boolean()
                           : target_->delete_property(key.to_string());
        if (result) {
            std::string key_str = to_prop_key(key);
            PropertyDescriptor target_desc = target_->get_property_descriptor(key_str);
            if (target_desc.is_data_descriptor() || target_desc.is_accessor_descriptor()) {
                if (!target_desc.is_configurable()) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'deleteProperty' proxy invariant violated: non-configurable property cannot be deleted");
                    throw std::runtime_error("TypeError: 'deleteProperty' proxy invariant violated: non-configurable property cannot be deleted");
                }
                bool target_extensible = target_->get_type() == ObjectType::Proxy
                    ? static_cast<Proxy*>(target_)->is_extensible_trap() : target_->is_extensible();
                if (Object::current_context_ && Object::current_context_->has_exception()) return false;
                if (!target_extensible) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'deleteProperty' proxy invariant violated: cannot report success for existing property on non-extensible target");
                    throw std::runtime_error("TypeError: 'deleteProperty' proxy invariant violated: cannot report success for existing property on non-extensible target");
                }
            }
        }
        return result;
    }

    return target_->delete_property(key.to_string());
}

std::vector<std::string> Proxy::own_keys_trap() {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'ownKeys' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("ownKeys");
    if (Object::current_context_ && Object::current_context_->has_exception()) return {};

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        std::vector<std::string> result;
        if (!ctx) {
            result = target_->get_own_property_keys();
        } else {
            Value trap_result = trap_fn->call(*ctx, {Value(target_)}, Value(handler_));
            if (ctx->has_exception()) return {};
            if (!trap_result.is_object() && !trap_result.is_function()) {
                ctx->throw_type_error("'ownKeys' trap must return an object");
                return {};
            }
            Object* arr = trap_result.is_function() ? static_cast<Object*>(trap_result.as_function()) : trap_result.as_object();
            uint32_t len = arr->get_length();
            for (uint32_t i = 0; i < len; ++i) {
                Value elem = arr->get_element(i);
                if (!elem.is_string() && !elem.is_symbol()) {
                    ctx->throw_type_error("'ownKeys' trap must return a list of strings and symbols");
                    return {};
                }
                result.push_back(elem.is_symbol() ? elem.as_symbol()->to_property_key() : elem.to_string());
            }
        }
        // Invariant: no duplicate keys (spec step 22)
        {
            std::unordered_set<std::string> seen;
            for (const auto& rk : result) {
                if (!seen.insert(rk).second) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'ownKeys' proxy trap returned duplicate key");
                    throw std::runtime_error("TypeError: 'ownKeys' proxy trap returned duplicate key");
                }
            }
        }
        // Invariant: must include all non-configurable own properties (string or symbol keys)
        std::vector<std::string> target_keys = target_->get_type() == ObjectType::Proxy
            ? static_cast<Proxy*>(target_)->own_keys_trap() : target_->get_own_property_keys();
        for (const auto& tkey : target_keys) {
            PropertyDescriptor td = target_->get_property_descriptor(tkey);
            if (!td.is_configurable()) {
                bool found = std::find(result.begin(), result.end(), tkey) != result.end();
                if (!found) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'ownKeys' proxy invariant violated: must report all non-configurable own properties");
                    throw std::runtime_error("TypeError: 'ownKeys' proxy invariant violated: must report all non-configurable own properties");
                }
            }
        }
        // Invariant: if target non-extensible, result must match exactly the target's own keys
        if (!target_->is_extensible()) {
            for (const auto& rkey : result) {
                bool found = std::find(target_keys.begin(), target_keys.end(), rkey) != target_keys.end();
                if (!found) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'ownKeys' proxy invariant violated: non-extensible target, key not in target");
                    throw std::runtime_error("TypeError: 'ownKeys' proxy invariant violated: non-extensible target, key not in target");
                }
            }
            for (const auto& tkey : target_keys) {
                bool found = std::find(result.begin(), result.end(), tkey) != result.end();
                if (!found) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'ownKeys' proxy invariant violated: non-extensible target, missing target key");
                    throw std::runtime_error("TypeError: 'ownKeys' proxy invariant violated: non-extensible target, missing target key");
                }
            }
        }
        return result;
    }

    if (target_->get_type() == ObjectType::Proxy) {
        return static_cast<Proxy*>(target_)->own_keys_trap();
    }
    return target_->get_own_property_keys();
}

Value Proxy::get_prototype_of_trap() {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'getPrototypeOf' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("getPrototypeOf");
    if (Object::current_context_ && Object::current_context_->has_exception()) return Value();

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        Value result;
        if (!ctx) {
            Object* p = target_->get_prototype();
            result = p ? Value(p) : Value::null();
        } else {
            result = trap_fn->call(*ctx, {Value(target_)}, Value(handler_));
            if (ctx->has_exception()) return Value();
            if (!result.is_null() && !result.is_object() && !result.is_function()) {
                ctx->throw_type_error("'getPrototypeOf' trap must return an object or null");
                return Value();
            }
        }
        // Invariant: if target is non-extensible, returned prototype must match target's prototype
        if (!target_->is_extensible()) {
            Object* target_proto = target_->get_prototype();
            Object* result_proto = result.is_null() ? nullptr :
                result.is_object() ? result.as_object() :
                result.is_function() ? static_cast<Object*>(result.as_function()) : nullptr;
            if (result_proto != target_proto) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'getPrototypeOf' proxy invariant violated: must return target's prototype for non-extensible target");
                throw std::runtime_error("TypeError: 'getPrototypeOf' proxy invariant violated: must return target's prototype for non-extensible target");
            }
        }
        return result;
    }

    Object* proto = target_->get_prototype();
    return proto ? Value(proto) : Value::null();
}

bool Proxy::set_prototype_of_trap(Object* proto) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'setPrototypeOf' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("setPrototypeOf");
    if (Object::current_context_ && Object::current_context_->has_exception()) return false;

    bool target_is_proxy = target_->get_type() == ObjectType::Proxy;
    if (!trap_fn) {
        if (target_is_proxy) return static_cast<Proxy*>(target_)->set_prototype_of_trap(proto);
        return ordinary_set_prototype_of(target_, proto);
    }

    Context* ctx = Object::current_context_;
    if (!ctx) { target_->set_prototype(proto); return true; }

    Value proto_val = proto ? Value(proto) : Value::null();
    bool boolean_trap_result = trap_fn->call(*ctx, {Value(target_), proto_val}, Value(handler_)).to_boolean();
    if (ctx->has_exception()) return false;
    if (!boolean_trap_result) return false;

    // IsExtensible(target) and target.[[GetPrototypeOf]]() must go through target's own trap if it's a Proxy.
    bool extensible_target = target_is_proxy ? static_cast<Proxy*>(target_)->is_extensible_trap() : target_->is_extensible();
    if (ctx->has_exception()) return false;
    if (extensible_target) return true;

    Object* target_proto;
    if (target_is_proxy) {
        Value tp = static_cast<Proxy*>(target_)->get_prototype_of_trap();
        if (ctx->has_exception()) return false;
        target_proto = tp.is_null() ? nullptr : (tp.is_function() ? static_cast<Object*>(tp.as_function()) : tp.as_object());
    } else {
        target_proto = target_->get_prototype();
    }
    if (proto != target_proto) {
        ctx->throw_type_error("'setPrototypeOf' proxy invariant violated: cannot change prototype of non-extensible target");
        return false;
    }
    return true;
}

bool Proxy::is_extensible_trap() {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'isExtensible' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    bool target_is_proxy = target_->get_type() == ObjectType::Proxy;
    Function* trap_fn = get_trap_method("isExtensible");
    if (Object::current_context_ && Object::current_context_->has_exception()) return false;

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        if (!ctx) return target_is_proxy ? static_cast<Proxy*>(target_)->is_extensible_trap() : target_->is_extensible();
        bool result = trap_fn->call(*ctx, {Value(target_)}, Value(handler_)).to_boolean();
        if (ctx->has_exception()) return false;
        // Invariant: must return same value as Object.isExtensible(target)
        bool target_result = target_is_proxy ? static_cast<Proxy*>(target_)->is_extensible_trap() : target_->is_extensible();
        if (ctx->has_exception()) return false;
        if (result != target_result) {
            ctx->throw_type_error("'isExtensible' proxy invariant violated: must return same as target.isExtensible()");
            return false;
        }
        return result;
    }

    return target_is_proxy ? static_cast<Proxy*>(target_)->is_extensible_trap() : target_->is_extensible();
}

bool Proxy::prevent_extensions_trap() {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'preventExtensions' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    bool target_is_proxy = target_->get_type() == ObjectType::Proxy;
    Function* trap_fn = get_trap_method("preventExtensions");
    if (Object::current_context_ && Object::current_context_->has_exception()) return false;

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        if (!ctx) { target_->prevent_extensions(); return true; }
        bool result = trap_fn->call(*ctx, {Value(target_)}, Value(handler_)).to_boolean();
        if (ctx->has_exception()) return false;
        // Invariant: if result is true, target must be non-extensible
        bool target_extensible = target_is_proxy ? static_cast<Proxy*>(target_)->is_extensible_trap() : target_->is_extensible();
        if (ctx->has_exception()) return false;
        if (result && target_extensible) {
            ctx->throw_type_error("'preventExtensions' proxy invariant violated: trap returned true but target is still extensible");
            return false;
        }
        return result;
    }

    if (target_is_proxy) return static_cast<Proxy*>(target_)->prevent_extensions_trap();
    target_->prevent_extensions();
    return true;
}

PropertyDescriptor Proxy::get_own_property_descriptor_trap(const Value& key) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'getOwnPropertyDescriptor' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("getOwnPropertyDescriptor");
    if (Object::current_context_ && Object::current_context_->has_exception()) return PropertyDescriptor();

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        PropertyDescriptor result;
        if (!ctx) {
            result = target_->get_type() == Object::ObjectType::Proxy
                ? static_cast<Proxy*>(target_)->get_own_property_descriptor_trap(key)
                : target_->get_property_descriptor(key.to_string());
        } else {
            Value trap_result = trap_fn->call(*ctx, {Value(target_), key}, Value(handler_));
            if (ctx->has_exception()) return PropertyDescriptor();
            if (!trap_result.is_undefined()) {
                if (!trap_result.is_object() && !trap_result.is_function()) {
                    ctx->throw_type_error("'getOwnPropertyDescriptor' trap result must be an object or undefined");
                    return PropertyDescriptor();
                }
                Object* desc_obj = trap_result.is_function()
                    ? static_cast<Object*>(trap_result.as_function()) : trap_result.as_object();
                if (desc_obj->has_own_property("value")) result.set_value(desc_obj->get_property("value"));
                if (desc_obj->has_own_property("writable")) result.set_writable(desc_obj->get_property("writable").to_boolean());
                if (desc_obj->has_own_property("enumerable")) result.set_enumerable(desc_obj->get_property("enumerable").to_boolean());
                if (desc_obj->has_own_property("configurable")) result.set_configurable(desc_obj->get_property("configurable").to_boolean());
                if (desc_obj->has_own_property("get")) {
                    Value getter = desc_obj->get_property("get");
                    if (getter.is_function()) result.set_getter(getter.as_function());
                }
                if (desc_obj->has_own_property("set")) {
                    Value setter = desc_obj->get_property("set");
                    if (setter.is_function()) result.set_setter(setter.as_function());
                }
            }
        }
        std::string key_str = to_prop_key(key);
        bool result_exists = result.is_data_descriptor() || result.is_accessor_descriptor();
        PropertyDescriptor target_desc = target_->get_property_descriptor(key_str);
        bool target_has_own = target_->has_own_property(key_str);
        bool target_configurable = target_has_own && target_desc.is_configurable();

        if (!result_exists) {
            // Trap returned undefined: property reported non-existent
            // Invariant: non-configurable own property cannot be reported non-existent
            if (target_has_own && !target_configurable) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'getOwnPropertyDescriptor' proxy invariant violated: non-configurable property cannot be non-existent");
                throw std::runtime_error("TypeError: 'getOwnPropertyDescriptor' proxy invariant violated: non-configurable property cannot be non-existent");
            }
            // Invariant: if target is non-extensible and property exists, cannot report non-existent
            if (!target_->is_extensible() && target_has_own) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'getOwnPropertyDescriptor' proxy invariant violated: non-extensible target, cannot hide existing property");
                throw std::runtime_error("TypeError: 'getOwnPropertyDescriptor' proxy invariant violated: non-extensible target, cannot hide existing property");
            }
        } else {
            // Trap returned a descriptor
            // Invariant: cannot report existent if non-extensible target doesn't have it
            if (!target_->is_extensible() && !target_has_own) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'getOwnPropertyDescriptor' proxy invariant violated: cannot report non-existent property on non-extensible target");
                throw std::runtime_error("TypeError: 'getOwnPropertyDescriptor' proxy invariant violated: cannot report non-existent property on non-extensible target");
            }
            // Invariant: cannot report non-configurable if target property doesn't exist or is configurable
            if (!result.is_configurable()) {
                if (!target_has_own) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'getOwnPropertyDescriptor' proxy invariant violated: cannot report non-configurable for non-existent property");
                    throw std::runtime_error("TypeError: 'getOwnPropertyDescriptor' proxy invariant violated: cannot report non-configurable for non-existent property");
                }
                if (target_configurable) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'getOwnPropertyDescriptor' proxy invariant violated: cannot report non-configurable for configurable property");
                    throw std::runtime_error("TypeError: 'getOwnPropertyDescriptor' proxy invariant violated: cannot report non-configurable for configurable property");
                }
                // Invariant: a non-configurable, non-writable result needs a non-writable target too
                if (result.is_data_descriptor() && result.has_writable() && !result.is_writable() &&
                    target_desc.is_data_descriptor() && target_desc.is_writable()) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'getOwnPropertyDescriptor' proxy invariant violated: non-configurable non-writable result requires non-writable target");
                    throw std::runtime_error("TypeError: 'getOwnPropertyDescriptor' proxy invariant violated: non-configurable non-writable result requires non-writable target");
                }
            }
        }
        return result;
    }

    if (target_->get_type() == Object::ObjectType::Proxy) {
        return static_cast<Proxy*>(target_)->get_own_property_descriptor_trap(key);
    }
    return target_->get_property_descriptor(key.to_string());
}

bool Proxy::define_property_trap(const Value& key, const PropertyDescriptor& desc) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'defineProperty' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("defineProperty");
    if (Object::current_context_ && Object::current_context_->has_exception()) return false;

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        bool result;
        if (!ctx) {
            result = target_->set_property_descriptor(key.to_string(), desc);
        } else {
            // FromPropertyDescriptor: only include keys actually present on this (possibly partial) descriptor.
            auto desc_obj = ObjectFactory::create_object();
            if (desc.has_value()) desc_obj->set_property("value", desc.get_value());
            if (desc.has_writable()) desc_obj->set_property("writable", Value(desc.is_writable()));
            if (desc.has_enumerable()) desc_obj->set_property("enumerable", Value(desc.is_enumerable()));
            if (desc.has_configurable()) desc_obj->set_property("configurable", Value(desc.is_configurable()));
            std::vector<Value> args = {Value(target_), key, Value(desc_obj.release())};
            result = trap_fn->call(*ctx, args, Value(handler_)).to_boolean();
            if (ctx->has_exception()) return false;
        }
        if (!result) return false;

        // Invariant checks after trap returns true
        std::string key_str = to_prop_key(key);
        bool target_extensible = target_->is_extensible();
        bool target_has_own = target_->has_own_property(key_str);
        bool setting_config_false = desc.has_configurable() && !desc.is_configurable();

        if (!target_has_own) {
            // Cannot add property to non-extensible target
            if (!target_extensible) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'defineProperty' proxy invariant violated: cannot add property to non-extensible target");
                throw std::runtime_error("TypeError: 'defineProperty' proxy invariant violated: cannot add property to non-extensible target");
            }
            // Cannot define non-configurable property that doesn't exist on target
            if (setting_config_false) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'defineProperty' proxy invariant violated: cannot define non-configurable property absent from target");
                throw std::runtime_error("TypeError: 'defineProperty' proxy invariant violated: cannot define non-configurable property absent from target");
            }
        } else {
            PropertyDescriptor target_desc = target_->get_property_descriptor(key_str);
            if (!target_desc.is_configurable()) {
                // IsCompatiblePropertyDescriptor against a non-configurable target property.
                const char* msg = "'defineProperty' proxy invariant violated: descriptor not compatible with non-configurable target property";
                if (desc.has_configurable() && desc.is_configurable()) {
                    if (Object::current_context_) Object::current_context_->throw_type_error(msg);
                    throw std::runtime_error(std::string("TypeError: ") + msg);
                }
                if (desc.has_enumerable() && desc.is_enumerable() != target_desc.is_enumerable()) {
                    if (Object::current_context_) Object::current_context_->throw_type_error(msg);
                    throw std::runtime_error(std::string("TypeError: ") + msg);
                }
                bool desc_is_generic = !desc.is_data_descriptor() && !desc.is_accessor_descriptor();
                if (!desc_is_generic && desc.is_accessor_descriptor() != target_desc.is_accessor_descriptor()) {
                    if (Object::current_context_) Object::current_context_->throw_type_error(msg);
                    throw std::runtime_error(std::string("TypeError: ") + msg);
                }
                if (target_desc.is_accessor_descriptor()) {
                    if (desc.has_getter() && desc.get_getter() != target_desc.get_getter()) {
                        if (Object::current_context_) Object::current_context_->throw_type_error(msg);
                        throw std::runtime_error(std::string("TypeError: ") + msg);
                    }
                    if (desc.has_setter() && desc.get_setter() != target_desc.get_setter()) {
                        if (Object::current_context_) Object::current_context_->throw_type_error(msg);
                        throw std::runtime_error(std::string("TypeError: ") + msg);
                    }
                } else if (!target_desc.is_writable()) {
                    if (desc.has_writable() && desc.is_writable()) {
                        if (Object::current_context_) Object::current_context_->throw_type_error(msg);
                        throw std::runtime_error(std::string("TypeError: ") + msg);
                    }
                    if (desc.has_value() && !desc.get_value().same_value(target_desc.get_value())) {
                        if (Object::current_context_) Object::current_context_->throw_type_error(msg);
                        throw std::runtime_error(std::string("TypeError: ") + msg);
                    }
                } else if (desc.has_writable() && !desc.is_writable()) {
                    // Non-configurable but still-writable target: defineProperty can't make it non-writable.
                    if (Object::current_context_) Object::current_context_->throw_type_error(msg);
                    throw std::runtime_error(std::string("TypeError: ") + msg);
                }
            }
            // Separate from the above: configurable target rejects settingConfigFalse outright.
            if (setting_config_false && target_desc.is_configurable()) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'defineProperty' proxy invariant violated: cannot set configurable:false when target property is configurable");
                throw std::runtime_error("TypeError: 'defineProperty' proxy invariant violated: cannot set configurable:false when target property is configurable");
            }
        }
        return true;
    }

    return target_->set_property_descriptor(key.to_string(), desc);
}

Value Proxy::apply_trap(const std::vector<Value>& args, const Value& this_value) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'apply' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    // Invariant: proxy is only callable if target is callable (target may itself be a callable Proxy).
    if (!target_->is_function() && !(target_->get_type() == ObjectType::Proxy && static_cast<Proxy*>(target_)->target_was_callable())) {
        if (Object::current_context_) Object::current_context_->throw_type_error("proxy target is not callable");
        throw std::runtime_error("TypeError: proxy target is not callable");
    }

    Function* trap_fn = get_trap_method("apply");
    if (Object::current_context_ && Object::current_context_->has_exception()) return Value();

    Context* ctx = Object::current_context_;
    if (trap_fn) {
        if (!ctx) return Value();
        auto args_array = ObjectFactory::create_array(static_cast<uint32_t>(args.size()));
        for (size_t i = 0; i < args.size(); ++i)
            args_array->set_element(static_cast<uint32_t>(i), args[i]);
        std::vector<Value> call_args = {Value(target_), this_value, Value(args_array.release())};
        return trap_fn->call(*ctx, call_args, Value(handler_));
    }

    if (!ctx) return Value();
    if (target_->get_type() == ObjectType::Proxy) {
        return static_cast<Proxy*>(target_)->apply_trap(args, this_value);
    }
    Function* func = static_cast<Function*>(target_);
    return func->call(*ctx, args, this_value);
}

// IsConstructor(obj): recurses through nested Proxy targets to the underlying Function.
static bool object_is_constructor(Object* obj) {
    if (obj->get_type() == Object::ObjectType::Proxy) {
        return object_is_constructor(static_cast<Proxy*>(obj)->get_proxy_target());
    }
    return obj->is_function() && static_cast<Function*>(obj)->is_constructor();
}

Value Proxy::construct_trap(const std::vector<Value>& args, Object* new_target) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'construct' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    // Invariant: proxy is only constructable if target is constructable (target may itself be a Proxy).
    bool target_is_proxy = target_->get_type() == ObjectType::Proxy;
    bool target_constructible = target_is_proxy
        ? object_is_constructor(target_)
        : (target_->is_function() && static_cast<Function*>(target_)->is_constructor());
    if (!target_constructible) {
        if (Object::current_context_) Object::current_context_->throw_type_error("proxy target is not a constructor");
        throw std::runtime_error("TypeError: proxy target is not a constructor");
    }

    Object* nt = new_target ? new_target : static_cast<Object*>(this);
    Value nt_value(nt);

    Function* trap_fn = get_trap_method("construct");
    if (Object::current_context_ && Object::current_context_->has_exception()) return Value();

    if (trap_fn) {
        Context* trap_ctx = Object::current_context_;
        Value result;
        if (!trap_ctx) {
            result = Value();
        } else {
            auto args_array = ObjectFactory::create_array(static_cast<uint32_t>(args.size()));
            for (size_t i = 0; i < args.size(); ++i)
                args_array->set_element(static_cast<uint32_t>(i), args[i]);
            std::vector<Value> call_args = {Value(target_), Value(args_array.release()), nt_value};
            result = trap_fn->call(*trap_ctx, call_args, Value(handler_));
        }
        if (trap_ctx && trap_ctx->has_exception()) return Value();
        // Invariant: construct trap must return an object
        if (!result.is_object() && !result.is_function()) {
            if (Object::current_context_) Object::current_context_->throw_type_error("proxy construct trap must return an object");
            throw std::runtime_error("TypeError: proxy construct trap must return an object");
        }
        return result;
    }

    Context* ctx = Object::current_context_;
    if (!ctx) return Value();

    // No construct trap: propagate to target's own [[Construct]] (target may be a Proxy too).
    if (target_is_proxy) {
        return static_cast<Proxy*>(target_)->construct_trap(args, nt);
    }
    Function* target_fn = static_cast<Function*>(target_);

    // GetPrototypeFromConstructor(newTarget): may fire the proxy's get trap and revoke it.
    auto new_object = ObjectFactory::create_object();
    Value target_proto = nt->get_property("prototype");
    if (ctx->has_exception()) return Value();
    if (target_proto.is_object()) {
        new_object->set_prototype(target_proto.as_object());
    } else if (target_proto.is_function()) {
        new_object->set_prototype(static_cast<Object*>(target_proto.as_function()));
    } else if (nt == this && is_revoked()) {
        // GetFunctionRealm(constructor): a revoked proxy has no [[ProxyHandler]] to find a realm through.
        ctx->throw_type_error("Cannot perform 'get' on a proxy that has been revoked");
        return Value();
    }

    Value this_value(new_object.get());

    Value super_constructor_prop = target_fn->get_property("__super_constructor__");
    if (!super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        ctx->create_binding("__super__", super_constructor_prop);
    }

    ctx->set_in_constructor_call(true);
    ctx->set_super_called(false);
    ctx->set_new_target(nt_value);
    // Tells Function::call's native path this is a construct invocation, so it won't reset new.target to undefined.
    ctx->set_pending_construct_call(true);
    Value result = target_fn->call(*ctx, args, this_value);
    bool super_was_called = ctx->was_super_called();
    ctx->set_in_constructor_call(false);
    ctx->set_new_target(Value());
    if (ctx->has_exception()) return Value();

    if (!super_was_called && !super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        Function* super_ctor = super_constructor_prop.as_function();
        ctx->set_in_constructor_call(true);
        ctx->set_new_target(nt_value);
        ctx->set_pending_construct_call(true);
        Value super_result = super_ctor->call(*ctx, args, this_value);
        ctx->set_in_constructor_call(false);
        ctx->set_new_target(Value());
        if (!super_result.is_undefined()) result = super_result;
    }

    if ((result.is_object() || result.is_function()) && result.as_object() != new_object.get()) {
        return result;
    }
    return Value(new_object.release());
}

bool Proxy::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    return set_trap(Value(key), value);
}

bool Proxy::has_property(const std::string& key) const {
    return const_cast<Proxy*>(this)->has_trap(Value(key));
}

bool Proxy::delete_property(const std::string& key) {
    return delete_trap(Value(key));
}

Value Proxy::get_element(uint32_t index) const {
    return get_property(std::to_string(index));
}

uint32_t Proxy::get_length() const {
    Value lv = get_property("length");
    return lv.is_number() ? static_cast<uint32_t>(lv.as_number()) : 0;
}

Object* Proxy::get_prototype() const {
    // Delegate so virtual-dispatch callers (e.g. instanceof) get the same invariant checks as Object.getPrototypeOf.
    Value result = const_cast<Proxy*>(this)->get_prototype_of_trap();
    if (result.is_null() || (Object::current_context_ && Object::current_context_->has_exception())) return nullptr;
    if (result.is_function()) return result.as_function();
    if (result.is_object()) return result.as_object();
    return nullptr;
}

void Proxy::revoke() {
    target_ = nullptr;
    handler_ = nullptr;
}

void Proxy::throw_if_revoked(Context& ctx) const {
    if (is_revoked()) {
        ctx.throw_type_error("Proxy has been revoked");
    }
}


Value Proxy::get_property(const std::string& key) const {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'get' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }
    // Pass actual Symbol objects for well-known symbol keys so get trap receives them correctly
    return const_cast<Proxy*>(this)->get_trap(from_prop_key(key));
}

PropertyDescriptor Proxy::get_property_descriptor(const std::string& key) const {
    return const_cast<Proxy*>(this)->get_own_property_descriptor_trap(from_prop_key(key));
}


Value Proxy::proxy_constructor(Context& ctx, const std::vector<Value>& args) {
    if (!ctx.is_in_constructor_call()) {
        ctx.throw_type_error("Constructor Proxy requires 'new'");
        return Value();
    }
    if (args.size() < 2) {
        ctx.throw_type_error("Proxy constructor requires target and handler arguments");
        return Value();
    }
    
    if ((!args[0].is_object() && !args[0].is_function()) || !args[1].is_object()) {
        ctx.throw_type_error("Proxy constructor requires object arguments");
        return Value();
    }

    Object* target = args[0].is_function() ? static_cast<Object*>(args[0].as_function()) : args[0].as_object();
    Object* handler = args[1].as_object();
    
    auto proxy = std::make_unique<Proxy>(target, handler);
    return Value(proxy.release());
}

Value Proxy::proxy_revocable(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("Proxy.revocable requires target and handler arguments");
        return Value();
    }
    
    if ((!args[0].is_object() && !args[0].is_function()) || !args[1].is_object()) {
        ctx.throw_type_error("Proxy.revocable requires object arguments");
        return Value();
    }

    Object* target = args[0].is_function() ? static_cast<Object*>(args[0].as_function()) : args[0].as_object();
    Object* handler = args[1].as_object();
    
    auto proxy = std::make_unique<Proxy>(target, handler);
    Proxy* proxy_ptr = proxy.get();
    
    // Proxy revocation functions are anonymous (name: "").
    auto revoke_fn = ObjectFactory::create_native_function("",
        [proxy_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            proxy_ptr->revoke();
            return Value();
        }, 0);

    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("proxy", Value(proxy.release()));
    result_obj->set_property("revoke", Value(revoke_fn.release()));

    return Value(result_obj.release());
}

void Proxy::setup_proxy(Context& ctx) {
    auto proxy_constructor_fn = ObjectFactory::create_native_constructor("Proxy", proxy_constructor, 2);
    // Per spec, Proxy has no 'prototype'; set_function_prototype(null) clears both the raw prototype_ ptr and the descriptor.
    proxy_constructor_fn->set_function_prototype(nullptr);

    auto revocable_fn = ObjectFactory::create_native_function("revocable", proxy_revocable, 2);
    proxy_constructor_fn->set_property("revocable", Value(revocable_fn.release()), PropertyAttributes::BuiltinFunction);

    ctx.register_built_in_object("Proxy", proxy_constructor_fn.release());
}


Value Reflect::reflect_get(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("Reflect.get requires at least one argument");
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }
    
    std::string key = args.size() > 1 ? to_property_key(args[1]) : "";
    if (ctx.has_exception()) return Value();
    Value receiver = args.size() > 2 ? args[2] : args[0];

    // Walk the prototype chain of target to find an accessor; if found, call the getter
    // with `receiver` as `this` (spec OrdinaryGet with explicit receiver).
    Object* current = target;
    while (current) {
        if (current->get_type() == Object::ObjectType::Proxy) {
            return static_cast<Proxy*>(current)->get_trap(Value(key), receiver);
        }
        PropertyDescriptor desc = current->get_property_descriptor(key);
        if (desc.is_accessor_descriptor()) {
            if (!desc.has_getter()) return Value();
            Function* getter_fn = dynamic_cast<Function*>(desc.get_getter());
            if (!getter_fn) return Value();
            return getter_fn->call(ctx, {}, receiver);
        }
        if (desc.has_value()) return desc.get_value();
        current = current->get_prototype();
    }
    return Value();
}

// ES 9.1.9.1/9.1.9.2 OrdinarySet/OrdinarySetWithOwnDescriptor: when Receiver differs
// from the object being walked, data-property writes go through Receiver's own
// [[GetOwnProperty]]/[[DefineOwnProperty]] (which fire Proxy traps if Receiver is a
// Proxy), not a plain write on the original target.
static bool ordinary_set_with_receiver(Object* O, const std::string& key, const Value& value, Object* receiver, Context& ctx) {
    // TypedArray's own [[Set]] bypasses OrdinarySet entirely for a canonical numeric key.
    if (O->get_type() == Object::ObjectType::TypedArray) {
        TypedArrayBase* ta = static_cast<TypedArrayBase*>(O);
        double num_idx;
        if (ta->canonical_numeric_index(key, num_idx)) {
            if (O == receiver) {
                ta->set_property(key, value, PropertyAttributes::Default);
                return !ctx.has_exception();
            }
            if (!ta->is_valid_integer_index(num_idx)) {
                return true;
            }
            if (!receiver) return false;
            PropertyDescriptor existing;
            bool receiver_has_own = false;
            if (receiver->get_type() == Object::ObjectType::Proxy) {
                existing = static_cast<Proxy*>(receiver)->get_own_property_descriptor_trap(Value(key));
                if (ctx.has_exception()) return false;
                receiver_has_own = existing.has_value() || existing.is_accessor_descriptor();
            } else {
                receiver_has_own = receiver->has_own_property(key);
                if (receiver_has_own) existing = receiver->get_property_descriptor(key);
            }
            if (receiver_has_own) {
                if (existing.is_accessor_descriptor()) return false;
                if (existing.has_writable() && !existing.is_writable()) return false;
                PropertyDescriptor value_desc;
                value_desc.set_value(value);
                if (receiver->get_type() == Object::ObjectType::Proxy) {
                    return static_cast<Proxy*>(receiver)->define_property_trap(Value(key), value_desc);
                }
                return receiver->set_property_descriptor(key, value_desc);
            }
            PropertyDescriptor new_desc(value, PropertyAttributes::Default);
            if (receiver->get_type() == Object::ObjectType::Proxy) {
                return static_cast<Proxy*>(receiver)->define_property_trap(Value(key), new_desc);
            }
            return receiver->set_property_descriptor(key, new_desc);
        }
    }

    // O.[[Set]] on a Proxy means its actual [[Set]] (the "set" trap), not a manual OrdinarySet replay.
    if (O->get_type() == Object::ObjectType::Proxy) {
        bool result = static_cast<Proxy*>(O)->set_trap(Value(key), value, Value(receiver));
        return !ctx.has_exception() && result;
    }

    PropertyDescriptor own_desc;
    bool has_own = O->has_own_property(key);
    if (has_own) own_desc = O->get_property_descriptor(key);

    if (!has_own) {
        Object* parent = O->get_prototype();
        if (ctx.has_exception()) return false;
        if (parent) return ordinary_set_with_receiver(parent, key, value, receiver, ctx);
        own_desc = PropertyDescriptor(Value(), PropertyAttributes::Default);
    }

    if (own_desc.is_accessor_descriptor()) {
        if (!own_desc.has_setter() || !own_desc.get_setter()) return false;
        Function* setter = dynamic_cast<Function*>(own_desc.get_setter());
        if (!setter) return false;
        setter->call(ctx, {value}, Value(receiver));
        return !ctx.has_exception();
    }

    if (own_desc.has_writable() && !own_desc.is_writable()) return false;
    if (!receiver) return false;

    PropertyDescriptor existing;
    bool receiver_has_own = false;
    if (receiver->get_type() == Object::ObjectType::Proxy) {
        existing = static_cast<Proxy*>(receiver)->get_own_property_descriptor_trap(Value(key));
        if (ctx.has_exception()) return false;
        receiver_has_own = existing.has_value() || existing.is_accessor_descriptor();
    } else {
        receiver_has_own = receiver->has_own_property(key);
        if (receiver_has_own) existing = receiver->get_property_descriptor(key);
    }

    if (receiver_has_own) {
        if (existing.is_accessor_descriptor()) return false;
        if (existing.has_writable() && !existing.is_writable()) return false;
        // Spec valueDesc = {[[Value]]: V} -- a partial descriptor that only updates
        // the value, leaving writable/enumerable/configurable as already set.
        PropertyDescriptor value_desc;
        value_desc.set_value(value);
        if (receiver->get_type() == Object::ObjectType::Proxy) {
            return static_cast<Proxy*>(receiver)->define_property_trap(Value(key), value_desc);
        }
        return receiver->set_property_descriptor(key, value_desc);
    }

    PropertyDescriptor new_desc(value, PropertyAttributes::Default);
    if (receiver->get_type() == Object::ObjectType::Proxy) {
        return static_cast<Proxy*>(receiver)->define_property_trap(Value(key), new_desc);
    }
    return receiver->set_property_descriptor(key, new_desc);
}

Value Reflect::reflect_set(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("Reflect.set requires at least two arguments");
        return Value();
    }

    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }

    std::string key = to_property_key(args[1]);
    if (ctx.has_exception()) return Value();
    Value value = args.size() > 2 ? args[2] : Value();
    // Receiver need not be an Object -- a non-object receiver just makes [[Set]] return false.
    Value receiver_value = args.size() > 3 ? args[3] : Value(static_cast<Object*>(target));
    Object* receiver = receiver_value.is_function() ? static_cast<Object*>(receiver_value.as_function())
                      : receiver_value.is_object() ? receiver_value.as_object() : nullptr;

    bool result;
    if (target->get_type() == Object::ObjectType::Proxy) {
        result = static_cast<Proxy*>(target)->set_trap(Value(key), value, receiver_value);
        if (ctx.has_exception()) return Value();
    } else {
        result = ordinary_set_with_receiver(target, key, value, receiver, ctx);
        if (ctx.has_exception()) return Value();
    }
    return Value(result);
}

Value Reflect::reflect_has(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("Reflect.has requires two arguments");
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    std::string key = to_property_key(args[1]);
    if (ctx.has_exception()) return Value();
    return Value(target->has_property(key));
}

Value Reflect::reflect_delete_property(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("Reflect.deleteProperty requires two arguments");
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    std::string key = to_property_key(args[1]);
    if (ctx.has_exception()) return Value();
    return Value(target->delete_property(key));
}

// Array index per spec: digits only, no leading zero (except "0"), value <= 2^32-2.
static bool is_array_index_key(const std::string& key, uint32_t* index) {
    if (key.empty() || key[0] < '0' || key[0] > '9') return false;
    if (key[0] == '0' && key.length() > 1) return false;
    char* end;
    unsigned long val = std::strtoul(key.c_str(), &end, 10);
    if (end != key.c_str() + key.length() || val > 0xFFFFFFFEUL) return false;
    *index = static_cast<uint32_t>(val);
    return true;
}

Value Reflect::reflect_own_keys(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("Reflect.ownKeys requires one argument");
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }
    
    std::vector<std::string> keys;
    bool from_proxy_trap = target->get_type() == Object::ObjectType::Proxy;
    if (from_proxy_trap) {
        try {
            keys = static_cast<Proxy*>(target)->own_keys_trap();
        } catch (const std::runtime_error&) {
            if (!ctx.has_exception()) ctx.throw_type_error("'ownKeys' proxy invariant violated");
            return Value();
        }
        if (ctx.has_exception()) return Value();
    } else {
        keys = target->get_own_property_keys();
    }

    auto result_array = ObjectFactory::create_array(static_cast<uint32_t>(keys.size()));

    if (from_proxy_trap) {
        // Proxy trap result order is spec-defined by the trap; preserve it as-is.
        uint32_t i = 0;
        for (const auto& key : keys) {
            if (key.find("@@sym:") == 0 || key.find("Symbol.") == 0) {
                Symbol* sym = Symbol::find_by_property_key(key);
                if (!sym) sym = Symbol::get_well_known(key);
                result_array->set_element(i++, sym ? Value(sym) : Value(key));
            } else {
                result_array->set_element(i++, Value(key));
            }
        }
        return Value(result_array.release());
    }

    // Ordinary target: ES6 [[OwnPropertyKeys]] order -- integer indices ascending, then strings, then symbols.
    std::vector<std::pair<uint32_t, std::string>> index_keys;
    std::vector<std::string> string_keys, symbol_keys;
    for (const auto& key : keys) {
        if (key.find("@@sym:") == 0) {
            symbol_keys.push_back(key);
            continue;
        }
        uint32_t idx;
        if (is_array_index_key(key, &idx)) {
            index_keys.push_back({idx, key});
        } else {
            string_keys.push_back(key);
        }
    }
    std::sort(index_keys.begin(), index_keys.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    uint32_t idx = 0;
    for (const auto& ik : index_keys) {
        result_array->set_element(idx++, Value(ik.second));
    }
    for (const auto& key : string_keys) {
        result_array->set_element(idx++, Value(key));
    }
    for (const auto& key : symbol_keys) {
        Symbol* sym = Symbol::find_by_property_key(key);
        if (sym) {
            result_array->set_element(idx++, Value(sym));
        } else {
            result_array->set_element(idx++, Value(key));
        }
    }

    return Value(result_array.release());
}

Value Reflect::reflect_get_prototype_of(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("Reflect.getPrototypeOf requires one argument");
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }
    
    Object* proto = target->get_prototype();
    return proto ? Value(proto) : Value::null();
}

Value Reflect::reflect_set_prototype_of(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("Reflect.setPrototypeOf requires two arguments");
        return Value();
    }

    Object* target = to_object(args[0], ctx);
    if (!target) return Value();

    if (!args[1].is_null() && !args[1].is_object() && !args[1].is_function()) {
        ctx.throw_type_error("Reflect.setPrototypeOf proto must be an Object or null");
        return Value();
    }
    Object* proto = args[1].is_null() ? nullptr :
                   (args[1].is_function() ? static_cast<Object*>(args[1].as_function()) : args[1].as_object());

    if (target->get_type() == Object::ObjectType::Proxy) {
        bool status = static_cast<Proxy*>(target)->set_prototype_of_trap(proto);
        if (ctx.has_exception()) return Value();
        return Value(status);
    }

    return Value(ordinary_set_prototype_of(target, proto));
}

Value Reflect::reflect_is_extensible(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("Reflect.isExtensible requires one argument");
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }

    if (target->get_type() == Object::ObjectType::Proxy) {
        bool result = static_cast<Proxy*>(target)->is_extensible_trap();
        if (ctx.has_exception()) return Value();
        return Value(result);
    }
    return Value(target->is_extensible());
}

Value Reflect::reflect_prevent_extensions(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("Reflect.preventExtensions requires one argument");
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }

    if (target->get_type() == Object::ObjectType::Proxy) {
        bool result = static_cast<Proxy*>(target)->prevent_extensions_trap();
        if (ctx.has_exception()) return Value();
        return Value(result);
    }
    target->prevent_extensions();
    return Value(true);
}

// IsCallable(value): true for a Function, or a Proxy wrapping a callable target.
static bool is_callable_value(const Value& value) {
    if (value.is_function()) return true;
    return value.is_object() && value.as_object()->get_type() == Object::ObjectType::Proxy &&
           static_cast<Proxy*>(value.as_object())->target_was_callable();
}

Value Reflect::reflect_apply(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 3) {
        ctx.throw_type_error("Reflect.apply requires three arguments");
        return Value();
    }

    if (!is_callable_value(args[0])) {
        ctx.throw_type_error("Reflect.apply first argument must be a function");
        return Value();
    }

    Value this_value = args[1];

    // CreateListFromArrayLike(argumentsList): any object with a "length" property, not just arrays.
    if (!args[2].is_object() && !args[2].is_function()) {
        ctx.throw_type_error("CreateListFromArrayLike: argumentsList must be an object");
        return Value();
    }
    Object* args_obj = args[2].is_function() ? static_cast<Object*>(args[2].as_function()) : args[2].as_object();
    Value length_val = args_obj->get_property("length");
    if (ctx.has_exception()) return Value();
    uint32_t length = static_cast<uint32_t>(length_val.to_number());
    std::vector<Value> apply_args;
    for (uint32_t i = 0; i < length; ++i) {
        apply_args.push_back(args_obj->get_property(std::to_string(i)));
        if (ctx.has_exception()) return Value();
    }

    if (args[0].is_object() && args[0].as_object()->get_type() == Object::ObjectType::Proxy) {
        return static_cast<Proxy*>(args[0].as_object())->apply_trap(apply_args, this_value);
    }
    return args[0].as_function()->call(ctx, apply_args, this_value);
}

// IsConstructor(value): true for a constructor Function, or a Proxy whose target chain is one.
static bool is_constructor_value(const Value& value) {
    if (value.is_function()) return value.as_function()->is_constructor();
    return value.is_object() && value.as_object()->get_type() == Object::ObjectType::Proxy &&
           object_is_constructor(value.as_object());
}

Value Reflect::reflect_construct(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("Reflect.construct requires at least two arguments");
        return Value();
    }

    if (!is_constructor_value(args[0])) {
        ctx.throw_type_error("Reflect.construct first argument must be a function");
        return Value();
    }

    // Get newTarget (3rd argument), default to target if not provided
    Value new_target_value = args.size() >= 3 ? args[2] : args[0];
    if (!is_constructor_value(new_target_value)) {
        ctx.throw_type_error("Reflect.construct newTarget must be a constructor");
        return Value();
    }

    // CreateListFromArrayLike(argumentsList): any object with a "length" property, not just arrays.
    if (!args[1].is_object() && !args[1].is_function()) {
        ctx.throw_type_error("Reflect.construct argumentsList must be an object");
        return Value();
    }
    Object* args_obj = args[1].is_function() ? static_cast<Object*>(args[1].as_function()) : args[1].as_object();
    Value args_length_val = args_obj->get_property("length");
    if (ctx.has_exception()) return Value();
    uint32_t args_length = static_cast<uint32_t>(args_length_val.to_number());
    std::vector<Value> construct_args;
    for (uint32_t i = 0; i < args_length; ++i) {
        construct_args.push_back(args_obj->get_property(std::to_string(i)));
        if (ctx.has_exception()) return Value();
    }

    Object* new_target_obj = new_target_value.is_function()
        ? static_cast<Object*>(new_target_value.as_function()) : new_target_value.as_object();

    // Proxy target: dispatch straight to its [[Construct]], passing newTarget through.
    if (args[0].is_object() && args[0].as_object()->get_type() == Object::ObjectType::Proxy) {
        return static_cast<Proxy*>(args[0].as_object())->construct_trap(construct_args, new_target_obj);
    }

    Function* target = args[0].as_function();

    // If newTarget == target, use normal construct. Explicitly set new.target rather than
    // relying on Function::construct's own fallback (which only fires when new.target was
    // undefined) -- this call may itself be nested inside another constructor's body, which
    // would otherwise leak that enclosing new.target into this independent construction.
    if (new_target_obj == static_cast<Object*>(target)) {
        Value old_new_target = ctx.get_new_target();
        ctx.set_new_target(Value(static_cast<Object*>(target)));
        Value result = target->construct(ctx, construct_args);
        ctx.set_new_target(old_new_target);
        return result;
    }

    // Reflect.construct(target, args, newTarget): call target with new.target = newTarget
    // Create new object using newTarget's prototype
    auto new_object = ObjectFactory::create_object();
    Value nt_proto = new_target_obj->get_property("prototype");
    // Spec fallback: if newTarget.prototype isn't an object, use target's own
    // intrinsic default prototype rather than leaving the bare Object.prototype.
    if (!nt_proto.is_object()) nt_proto = target->get_property("prototype");
    if (nt_proto.is_object()) new_object->set_prototype(nt_proto.as_object());

    bool was_in_constructor = ctx.is_in_constructor_call();
    Value old_new_target = ctx.get_new_target();

    ctx.set_in_constructor_call(true);
    ctx.set_new_target(new_target_value);
    ctx.set_pending_construct_call(true);
    Value result = target->call(ctx, construct_args, Value(new_object.get()));
    ctx.set_in_constructor_call(was_in_constructor);
    ctx.set_new_target(old_new_target);

    if (!ctx.has_exception() && (result.is_object() || result.is_function())) {
        // Native constructors that allocate their own backing object (e.g. ArrayBuffer,
        // TypedArray) return a fresh object instead of mutating `this` -- it never went
        // through the prototype fallback above, so apply it here too.
        Object* result_obj = result.is_function() ? static_cast<Object*>(result.as_function()) : result.as_object();
        if (result_obj != new_object.get() && !result_obj->get_prototype_raw() && nt_proto.is_object()) {
            result_obj->set_prototype(nt_proto.as_object());
        }
        return result;
    }
    return Value(new_object.release());
}

Value Reflect::reflect_get_own_property_descriptor(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("Reflect.getOwnPropertyDescriptor requires two arguments");
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value();
    }

    std::string key = to_property_key(args[1]);
    if (ctx.has_exception()) return Value();

    PropertyDescriptor desc = target->get_type() == Object::ObjectType::Proxy
        ? static_cast<Proxy*>(target)->get_own_property_descriptor_trap(Value(key))
        : target->get_property_descriptor(key);
    if (ctx.has_exception()) return Value();

    return from_property_descriptor(desc);
}

Value Reflect::reflect_define_property(Context& ctx, const std::vector<Value>& args) {
    Object* target = to_object(args.empty() ? Value() : args[0], ctx);
    if (!target) return Value(false);

    std::string key = to_property_key(args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();

    Value attrs_val = args.size() > 2 ? args[2] : Value();
    if (!attrs_val.is_object()) {
        ctx.throw_type_error("Property descriptor must be an object");
        return Value(false);
    }

    Object* desc_obj = attrs_val.as_object();
    PropertyDescriptor prop_desc;
    
    if (desc_obj->has_own_property("get")) {
        Value getter = desc_obj->get_property("get");
        if (getter.is_function()) {
            prop_desc.set_getter(getter.as_object());
        }
    }
    
    if (desc_obj->has_own_property("set")) {
        Value setter = desc_obj->get_property("set");
        if (setter.is_function()) {
            prop_desc.set_setter(setter.as_object());
        }
    }
    
    if (desc_obj->has_own_property("value")) {
        Value value = desc_obj->get_property("value");
        prop_desc.set_value(value);
    }
    
    if (desc_obj->has_own_property("writable")) {
        prop_desc.set_writable(desc_obj->get_property("writable").to_boolean());
    }
    
    if (desc_obj->has_own_property("enumerable")) {
        prop_desc.set_enumerable(desc_obj->get_property("enumerable").to_boolean());
    }
    
    if (desc_obj->has_own_property("configurable")) {
        prop_desc.set_configurable(desc_obj->get_property("configurable").to_boolean());
    }
    
    bool success = target->set_property_descriptor(key, prop_desc);
    return Value(success);
}

void Reflect::setup_reflect(Context& ctx) {
    auto reflect_obj = ObjectFactory::create_object();
    
    auto get_fn = ObjectFactory::create_native_function("get", reflect_get, 2);
    auto set_fn = ObjectFactory::create_native_function("set", reflect_set, 3);
    auto has_fn = ObjectFactory::create_native_function("has", reflect_has, 2);
    auto delete_property_fn = ObjectFactory::create_native_function("deleteProperty", reflect_delete_property, 2);
    auto own_keys_fn = ObjectFactory::create_native_function("ownKeys", reflect_own_keys, 1);
    auto get_prototype_of_fn = ObjectFactory::create_native_function("getPrototypeOf", reflect_get_prototype_of, 1);
    auto set_prototype_of_fn = ObjectFactory::create_native_function("setPrototypeOf", reflect_set_prototype_of, 2);
    auto is_extensible_fn = ObjectFactory::create_native_function("isExtensible", reflect_is_extensible, 1);
    auto prevent_extensions_fn = ObjectFactory::create_native_function("preventExtensions", reflect_prevent_extensions, 1);
    auto apply_fn = ObjectFactory::create_native_function("apply", reflect_apply, 3);
    auto construct_fn = ObjectFactory::create_native_function("construct", reflect_construct, 2);
    auto get_own_property_descriptor_fn = ObjectFactory::create_native_function("getOwnPropertyDescriptor", reflect_get_own_property_descriptor, 2);
    auto define_property_fn = ObjectFactory::create_native_function("defineProperty", reflect_define_property, 3);

    // Reflect.* methods are writable, configurable, non-enumerable per spec (BuiltinFunction attrs).
    reflect_obj->set_property("get", Value(get_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("set", Value(set_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("has", Value(has_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("deleteProperty", Value(delete_property_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("ownKeys", Value(own_keys_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("getPrototypeOf", Value(get_prototype_of_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("setPrototypeOf", Value(set_prototype_of_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("isExtensible", Value(is_extensible_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("preventExtensions", Value(prevent_extensions_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("apply", Value(apply_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("construct", Value(construct_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("getOwnPropertyDescriptor", Value(get_own_property_descriptor_fn.release()), PropertyAttributes::BuiltinFunction);
    reflect_obj->set_property("defineProperty", Value(define_property_fn.release()), PropertyAttributes::BuiltinFunction);

    PropertyDescriptor reflect_tag_desc(Value(std::string("Reflect")), PropertyAttributes::Configurable);
    reflect_obj->set_property_descriptor("Symbol.toStringTag", reflect_tag_desc);

    ctx.register_built_in_object("Reflect", reflect_obj.release());
}

Object* Reflect::to_object(const Value& value, Context& ctx) {
    if (value.is_object()) {
        return value.as_object();
    }
    if (value.is_function()) {
        return static_cast<Object*>(value.as_function());
    }
    ctx.throw_type_error("Reflect operation requires an object");
    return nullptr;
}

std::string Reflect::to_property_key(const Value& value) {
    return value.to_property_key();
}

PropertyDescriptor Reflect::to_property_descriptor(const Value& value) {
    return PropertyDescriptor(value);
}

Value Reflect::from_property_descriptor(const PropertyDescriptor& desc) {
    // get_property_descriptor()/get_own_property_descriptor_trap() only return a Generic-typed
    // descriptor for a property that doesn't exist at all (existing data/accessor properties are
    // always typed Data/Accessor) -- so Generic here means "undefined" per spec.
    if (desc.is_generic_descriptor()) {
        return Value();
    }
    auto desc_obj = ObjectFactory::create_object();
    if (desc.is_accessor_descriptor()) {
        Object* getter = desc.has_getter() ? desc.get_getter() : nullptr;
        Object* setter = desc.has_setter() ? desc.get_setter() : nullptr;
        desc_obj->set_property("get", getter ? Value(static_cast<Function*>(getter)) : Value());
        desc_obj->set_property("set", setter ? Value(static_cast<Function*>(setter)) : Value());
    } else {
        desc_obj->set_property("value", desc.get_value());
        desc_obj->set_property("writable", Value(desc.is_writable()));
    }
    desc_obj->set_property("enumerable", Value(desc.is_enumerable()));
    desc_obj->set_property("configurable", Value(desc.is_configurable()));
    return Value(desc_obj.release());
}

}
