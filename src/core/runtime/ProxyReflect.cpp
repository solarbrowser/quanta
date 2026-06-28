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

namespace Quanta {

// Helper: convert a Value key (possibly Symbol) to a property key string
static std::string to_prop_key(const Value& key) {
    if (key.is_symbol()) return key.as_symbol()->to_property_key();
    return key.to_string();
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

Proxy::Proxy(Object* target, Object* handler)
    : Object(ObjectType::Proxy), target_(target), handler_(handler) {
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
        // Invariant: non-configurable accessor without getter => result must be undefined
        if (target_desc.is_accessor_descriptor() && !target_desc.is_configurable() && !target_desc.has_getter()) {
            if (!result.is_undefined()) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'get' proxy invariant violated: non-configurable accessor without getter");
                throw std::runtime_error("TypeError: 'get' proxy invariant violated: non-configurable accessor without getter");
            }
        }
        return result;
    }

    return target_->get_property(to_prop_key(key));
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
            // Invariant: non-configurable accessor without setter => cannot set
            if (target_desc.is_accessor_descriptor() && !target_desc.is_configurable() && !target_desc.has_setter()) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'set' proxy invariant violated: non-configurable accessor without setter");
                throw std::runtime_error("TypeError: 'set' proxy invariant violated: non-configurable accessor without setter");
            }
        }
        return result;
    }

    // No set trap: target.[[Set]] runs Receiver.[[GetOwnProperty]] then [[DefineOwnProperty]].
    std::string key_str = to_prop_key(key);
    PropertyDescriptor own_desc = target_->get_property_descriptor(key_str);
    bool is_writable_data = !own_desc.is_accessor_descriptor() &&
                            (!own_desc.is_data_descriptor() || own_desc.is_writable());
    if (is_writable_data) {
        PropertyDescriptor existing_desc = get_own_property_descriptor_trap(Value(key_str));
        if (Object::current_context_ && Object::current_context_->has_exception()) return false;
        // Existing property: update value only. New property: CreateDataProperty semantics (all attrs true).
        PropertyDescriptor new_desc;
        new_desc.set_value(value);
        bool existing_exists = existing_desc.is_data_descriptor() || existing_desc.is_accessor_descriptor();
        if (!existing_exists) {
            new_desc.set_writable(true);
            new_desc.set_enumerable(true);
            new_desc.set_configurable(true);
        }
        return define_property_trap(Value(key_str), new_desc);
    }
    return target_->set_property(key_str, value);
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
            std::string key_str = key.to_string();
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
            std::string key_str = key.to_string();
            // Invariant: cannot report success for non-configurable property
            PropertyDescriptor target_desc = target_->get_property_descriptor(key_str);
            if (target_desc.is_data_descriptor() || target_desc.is_accessor_descriptor()) {
                if (!target_desc.is_configurable()) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'deleteProperty' proxy invariant violated: non-configurable property cannot be deleted");
                    throw std::runtime_error("TypeError: 'deleteProperty' proxy invariant violated: non-configurable property cannot be deleted");
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
            if (trap_result.is_object()) {
                Object* arr = trap_result.as_object();
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
        // Invariant: must include all non-configurable own properties
        std::vector<std::string> target_keys = target_->get_own_property_keys();
        for (const auto& tkey : target_keys) {
            // Skip symbol-keyed properties (stored as "@@sym:..." strings internally)
            if (tkey.find("@@sym:") == 0) continue;
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
                if (rkey.find("@@sym:") == 0) continue;
                bool found = std::find(target_keys.begin(), target_keys.end(), rkey) != target_keys.end();
                if (!found) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'ownKeys' proxy invariant violated: non-extensible target, key not in target");
                    throw std::runtime_error("TypeError: 'ownKeys' proxy invariant violated: non-extensible target, key not in target");
                }
            }
            for (const auto& tkey : target_keys) {
                if (tkey.find("@@sym:") == 0) continue;
                bool found = std::find(result.begin(), result.end(), tkey) != result.end();
                if (!found) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'ownKeys' proxy invariant violated: non-extensible target, missing target key");
                    throw std::runtime_error("TypeError: 'ownKeys' proxy invariant violated: non-extensible target, missing target key");
                }
            }
        }
        return result;
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

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        Value proto_val = proto ? Value(proto) : Value::null();
        bool result = ctx ? trap_fn->call(*ctx, {Value(target_), proto_val}, Value(handler_)).to_boolean()
                           : (target_->set_prototype(proto), true);
        // Invariant: if target is non-extensible, new proto must match target's current proto
        if (!target_->is_extensible()) {
            Object* target_proto = target_->get_prototype();
            if (proto != target_proto) {
                if (Object::current_context_) Object::current_context_->throw_type_error("'setPrototypeOf' proxy invariant violated: cannot change prototype of non-extensible target");
                throw std::runtime_error("TypeError: 'setPrototypeOf' proxy invariant violated: cannot change prototype of non-extensible target");
            }
        }
        return result;
    }

    target_->set_prototype(proto);
    return true;
}

bool Proxy::is_extensible_trap() {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'isExtensible' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("isExtensible");
    if (Object::current_context_ && Object::current_context_->has_exception()) return target_->is_extensible();

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        bool result = ctx ? trap_fn->call(*ctx, {Value(target_)}, Value(handler_)).to_boolean()
                           : target_->is_extensible();
        // Invariant: must return same value as Object.isExtensible(target)
        bool target_result = target_->is_extensible();
        if (result != target_result) {
            if (Object::current_context_) Object::current_context_->throw_type_error("'isExtensible' proxy invariant violated: must return same as target.isExtensible()");
            throw std::runtime_error("TypeError: 'isExtensible' proxy invariant violated: must return same as target.isExtensible()");
        }
        return result;
    }

    return target_->is_extensible();
}

bool Proxy::prevent_extensions_trap() {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'preventExtensions' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    Function* trap_fn = get_trap_method("preventExtensions");
    if (Object::current_context_ && Object::current_context_->has_exception()) return false;

    if (trap_fn) {
        Context* ctx = Object::current_context_;
        bool result = ctx ? trap_fn->call(*ctx, {Value(target_)}, Value(handler_)).to_boolean()
                           : (target_->prevent_extensions(), true);
        // Invariant: if result is true, target must be non-extensible
        if (result && target_->is_extensible()) {
            if (Object::current_context_) Object::current_context_->throw_type_error("'preventExtensions' proxy invariant violated: trap returned true but target is still extensible");
            throw std::runtime_error("TypeError: 'preventExtensions' proxy invariant violated: trap returned true but target is still extensible");
        }
        return result;
    }

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
            if (!trap_result.is_undefined() && !trap_result.is_null()) {
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
        std::string key_str = key.to_string();
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
            auto desc_obj = ObjectFactory::create_object();
            if (desc.has_value()) desc_obj->set_property("value", desc.get_value());
            desc_obj->set_property("writable", Value(desc.is_writable()));
            desc_obj->set_property("enumerable", Value(desc.is_enumerable()));
            desc_obj->set_property("configurable", Value(desc.is_configurable()));
            std::vector<Value> args = {Value(target_), key, Value(desc_obj.release())};
            result = trap_fn->call(*ctx, args, Value(handler_)).to_boolean();
        }
        if (!result) return false;

        // Invariant checks after trap returns true
        std::string key_str = key.to_string();
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
            // If setting configurable=false but target property is configurable → TypeError
            if (setting_config_false) {
                PropertyDescriptor target_desc = target_->get_property_descriptor(key_str);
                if (target_desc.is_configurable()) {
                    if (Object::current_context_) Object::current_context_->throw_type_error("'defineProperty' proxy invariant violated: cannot set configurable:false when target property is configurable");
                    throw std::runtime_error("TypeError: 'defineProperty' proxy invariant violated: cannot set configurable:false when target property is configurable");
                }
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

    // Invariant: proxy is only callable if target is callable
    if (!target_->is_function()) {
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

    Function* func = static_cast<Function*>(target_);
    if (ctx) return func->call(*ctx, args, this_value);

    return Value();
}

Value Proxy::construct_trap(const std::vector<Value>& args) {
    if (is_revoked()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot perform 'construct' on a proxy that has been revoked");
        throw std::runtime_error("TypeError: Proxy has been revoked");
    }

    // Invariant: proxy is only constructable if target is constructable
    if (!target_->is_function()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("proxy target is not a constructor");
        throw std::runtime_error("TypeError: proxy target is not a constructor");
    }
    Function* target_fn = static_cast<Function*>(target_);
    if (!target_fn->is_constructor()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("proxy target is not a constructor");
        throw std::runtime_error("TypeError: proxy target is not a constructor");
    }

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
            std::vector<Value> call_args = {Value(target_), Value(args_array.release()), Value(target_)};
            result = trap_fn->call(*trap_ctx, call_args, Value(handler_));
        }
        // Invariant: construct trap must return an object
        if (!result.is_object() && !result.is_function()) {
            if (Object::current_context_) Object::current_context_->throw_type_error("proxy construct trap must return an object");
            throw std::runtime_error("TypeError: proxy construct trap must return an object");
        }
        return result;
    }

    Context* ctx = Object::current_context_;
    if (!ctx) return Value();

    // GetPrototypeFromConstructor: the Function constructor body will call new_target->get_property("prototype")
    // which fires the Proxy get trap once (via Object::get_property dispatch). Don't call get_trap here.
    auto new_object = ObjectFactory::create_object();
    Value target_proto = target_fn->get_property("prototype");
    if (target_proto.is_object()) {
        new_object->set_prototype(target_proto.as_object());
    } else if (target_proto.is_function()) {
        new_object->set_prototype(target_proto.as_object());
    }

    Value this_value(new_object.get());

    Value super_constructor_prop = target_fn->get_property("__super_constructor__");
    if (!super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        ctx->create_binding("__super__", super_constructor_prop);
    }

    ctx->set_in_constructor_call(true);
    ctx->set_super_called(false);
    ctx->set_new_target(Value(static_cast<Object*>(this)));
    Value result = target_fn->call(*ctx, args, this_value);
    bool super_was_called = ctx->was_super_called();
    ctx->set_in_constructor_call(false);
    ctx->set_new_target(Value());
    if (ctx->has_exception()) return Value();

    if (!super_was_called && !super_constructor_prop.is_undefined() && super_constructor_prop.is_function()) {
        Function* super_ctor = super_constructor_prop.as_function();
        ctx->set_in_constructor_call(true);
        ctx->set_new_target(Value(static_cast<Object*>(this)));
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
    if (is_revoked()) throw std::runtime_error("TypeError: Proxy has been revoked");
    Function* trap_fn = get_trap_method("getPrototypeOf");
    if (Object::current_context_ && Object::current_context_->has_exception()) return nullptr;
    if (trap_fn) {
        Context* ctx = Object::current_context_;
        Value result = ctx ? trap_fn->call(*ctx, {Value(target_)}, Value(handler_)) : Value();
        if (Object::current_context_ && Object::current_context_->has_exception()) return nullptr;
        if (result.is_null()) return nullptr;
        if (result.is_object()) return result.as_object();
        if (result.is_function()) return result.as_function();
    }
    return target_ ? target_->get_prototype() : nullptr;
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
    
    auto revoke_fn = ObjectFactory::create_native_function("revoke", 
        [proxy_ptr](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            proxy_ptr->revoke();
            return Value();
        });
    
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("proxy", Value(proxy.release()));
    result_obj->set_property("revoke", Value(revoke_fn.release()));
    
    return Value(result_obj.release());
}

void Proxy::setup_proxy(Context& ctx) {
    auto proxy_constructor_fn = ObjectFactory::create_native_constructor("Proxy", proxy_constructor);
    // Per spec, Proxy constructor does not have a 'prototype' property
    proxy_constructor_fn->remove_own_property("prototype");

    auto revocable_fn = ObjectFactory::create_native_function("revocable", proxy_revocable);
    proxy_constructor_fn->set_property("revocable", Value(revocable_fn.release()));

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

    PropertyDescriptor own_desc;
    bool has_own = false;
    if (O->get_type() == Object::ObjectType::Proxy) {
        own_desc = static_cast<Proxy*>(O)->get_own_property_descriptor_trap(Value(key));
        if (ctx.has_exception()) return false;
        has_own = own_desc.has_value() || own_desc.is_accessor_descriptor();
    } else {
        has_own = O->has_own_property(key);
        if (has_own) own_desc = O->get_property_descriptor(key);
    }

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
    Value value = args.size() > 2 ? args[2] : Value();
    Object* receiver = target;
    if (args.size() > 3) {
        receiver = args[3].is_function() ? static_cast<Object*>(args[3].as_function()) : args[3].as_object();
        if (!receiver) {
            ctx.throw_type_error("Reflect.set receiver must be an object");
            return Value();
        }
    }

    bool result;
    if (target->get_type() == Object::ObjectType::Proxy) {
        result = static_cast<Proxy*>(target)->set_trap(Value(key), value, Value(static_cast<Object*>(receiver)));
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
    return Value(target->delete_property(key));
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
    
    auto keys = target->get_own_property_keys();

    // ES6 [[OwnPropertyKeys]] order: integer indices (ascending), then string keys (insertion), then symbols (insertion)
    std::vector<std::string> index_keys, string_keys, symbol_keys;
    for (const auto& key : keys) {
        if (key.find("@@sym:") == 0) {
            symbol_keys.push_back(key);
        } else {
            bool is_index = !key.empty();
            for (char c : key) {
                if (c < '0' || c > '9') { is_index = false; break; }
            }
            if (is_index && key.length() <= 10) {
                index_keys.push_back(key);
            } else {
                string_keys.push_back(key);
            }
        }
    }
    std::sort(index_keys.begin(), index_keys.end(), [](const std::string& a, const std::string& b) {
        return std::stoul(a) < std::stoul(b);
    });

    auto result_array = ObjectFactory::create_array(keys.size());
    uint32_t idx = 0;
    for (const auto& key : index_keys) {
        result_array->set_element(idx++, Value(key));
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
    if (!target) {
        return Value(false);
    }
    
    Object* proto = args[1].is_null() ? nullptr :
                   (args[1].is_object() ? args[1].as_object() : nullptr);
    // Non-extensible: only allow if new prototype === current prototype
    if (!target->is_extensible()) {
        return Value(proto == target->get_prototype());
    }

    target->set_prototype(proto);
    return Value(true);
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

Value Reflect::reflect_apply(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 3) {
        ctx.throw_type_error("Reflect.apply requires three arguments");
        return Value();
    }
    
    if (!args[0].is_function()) {
        ctx.throw_type_error("Reflect.apply first argument must be a function");
        return Value();
    }
    
    Function* target = args[0].as_function();
    Value this_value = args[1];
    
    std::vector<Value> apply_args;
    if (args[2].is_object()) {
        Object* args_obj = args[2].as_object();
        if (args_obj->is_array()) {
            uint32_t length = args_obj->get_length();
            for (uint32_t i = 0; i < length; ++i) {
                apply_args.push_back(args_obj->get_element(i));
            }
        }
    }
    
    return target->call(ctx, apply_args, this_value);
}

Value Reflect::reflect_construct(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("Reflect.construct requires at least two arguments");
        return Value();
    }

    if (!args[0].is_function()) {
        ctx.throw_type_error("Reflect.construct first argument must be a function");
        return Value();
    }

    Function* target = args[0].as_function();

    // Get newTarget (3rd argument), default to target if not provided
    Function* newTarget = target;
    if (args.size() >= 3) {
        if (!args[2].is_function()) {
            ctx.throw_type_error("Reflect.construct newTarget must be a constructor");
            return Value();
        }
        newTarget = args[2].as_function();
    }

    // Check if newTarget is a constructor
    if (!newTarget->is_constructor()) {
        ctx.throw_type_error("newTarget is not a constructor");
        return Value();
    }

    std::vector<Value> construct_args;
    if (args[1].is_object()) {
        Object* args_obj = args[1].as_object();
        if (args_obj->is_array()) {
            uint32_t length = args_obj->get_length();
            for (uint32_t i = 0; i < length; ++i) {
                construct_args.push_back(args_obj->get_element(i));
            }
        }
    }

    // If newTarget == target, use normal construct. Explicitly set new.target rather than
    // relying on Function::construct's own fallback (which only fires when new.target was
    // undefined) -- this call may itself be nested inside another constructor's body, which
    // would otherwise leak that enclosing new.target into this independent construction.
    if (newTarget == target) {
        Value old_new_target = ctx.get_new_target();
        ctx.set_new_target(Value(static_cast<Object*>(target)));
        Value result = target->construct(ctx, construct_args);
        ctx.set_new_target(old_new_target);
        return result;
    }

    // Reflect.construct(target, args, newTarget): call target with new.target = newTarget
    // Create new object using newTarget's prototype
    auto new_object = ObjectFactory::create_object();
    Value nt_proto = newTarget->get_property("prototype");
    // Spec fallback: if newTarget.prototype isn't an object, use target's own
    // intrinsic default prototype rather than leaving the bare Object.prototype.
    if (!nt_proto.is_object()) nt_proto = target->get_property("prototype");
    if (nt_proto.is_object()) new_object->set_prototype(nt_proto.as_object());

    bool was_in_constructor = ctx.is_in_constructor_call();
    Value old_new_target = ctx.get_new_target();

    ctx.set_in_constructor_call(true);
    ctx.set_new_target(Value(static_cast<Object*>(newTarget)));
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
    if (args.size() < 3) {
        ctx.throw_type_error("Reflect.defineProperty requires three arguments");
        return Value();
    }
    
    Object* target = to_object(args[0], ctx);
    if (!target) {
        return Value(false);
    }
    
    std::string key = to_property_key(args[1]);
    
    if (!args[2].is_object()) {
        ctx.throw_type_error("Property descriptor must be an object");
        return Value(false);
    }
    
    Object* desc_obj = args[2].as_object();
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
    
    auto get_fn = ObjectFactory::create_native_function("get", reflect_get);
    auto set_fn = ObjectFactory::create_native_function("set", reflect_set);
    auto has_fn = ObjectFactory::create_native_function("has", reflect_has);
    auto delete_property_fn = ObjectFactory::create_native_function("deleteProperty", reflect_delete_property);
    auto own_keys_fn = ObjectFactory::create_native_function("ownKeys", reflect_own_keys, 1);
    auto get_prototype_of_fn = ObjectFactory::create_native_function("getPrototypeOf", reflect_get_prototype_of, 1);
    auto set_prototype_of_fn = ObjectFactory::create_native_function("setPrototypeOf", reflect_set_prototype_of);
    auto is_extensible_fn = ObjectFactory::create_native_function("isExtensible", reflect_is_extensible, 1);
    auto prevent_extensions_fn = ObjectFactory::create_native_function("preventExtensions", reflect_prevent_extensions, 1);
    auto apply_fn = ObjectFactory::create_native_function("apply", reflect_apply);
    auto construct_fn = ObjectFactory::create_native_function("construct", reflect_construct);
    auto get_own_property_descriptor_fn = ObjectFactory::create_native_function("getOwnPropertyDescriptor", reflect_get_own_property_descriptor, 2);
    auto define_property_fn = ObjectFactory::create_native_function("defineProperty", reflect_define_property, 3);
    
    reflect_obj->set_property("get", Value(get_fn.release()));
    reflect_obj->set_property("set", Value(set_fn.release()));
    reflect_obj->set_property("has", Value(has_fn.release()));
    reflect_obj->set_property("deleteProperty", Value(delete_property_fn.release()));
    reflect_obj->set_property("ownKeys", Value(own_keys_fn.release()));
    reflect_obj->set_property("getPrototypeOf", Value(get_prototype_of_fn.release()));
    reflect_obj->set_property("setPrototypeOf", Value(set_prototype_of_fn.release()));
    reflect_obj->set_property("isExtensible", Value(is_extensible_fn.release()));
    reflect_obj->set_property("preventExtensions", Value(prevent_extensions_fn.release()));
    reflect_obj->set_property("apply", Value(apply_fn.release()));
    reflect_obj->set_property("construct", Value(construct_fn.release()));
    reflect_obj->set_property("getOwnPropertyDescriptor", Value(get_own_property_descriptor_fn.release()));
    reflect_obj->set_property("defineProperty", Value(define_property_fn.release()));
    
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
