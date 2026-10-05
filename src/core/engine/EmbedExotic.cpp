/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Exotic host objects: a generic one whose internal methods are the host's, and HTML's WindowProxy and
// cross-origin objects built the same way.

#include "quanta/Embed.h"
#include "quanta/core/runtime/Symbol.h"

#include <map>

namespace Quanta::Embed {

namespace {

// ---- Keys and descriptors --------------------------------------------------------------

Value key_value(const std::string& key) {
    if (key.find("Symbol.") == 0) {
        if (Symbol* sym = Symbol::get_well_known(key)) return Value(sym);
    }
    if (key.find("@@sym:") == 0) {
        if (Symbol* sym = Symbol::find_by_property_key(key)) return Value(sym);
    }
    return Value(key);
}

std::string key_string(const Value& key) {
    if (key.is_symbol()) return key.as_symbol()->to_property_key();
    return key.to_string();
}

bool is_symbol_key(const std::string& key) {
    return key.rfind("@@sym:", 0) == 0 || key.rfind("Symbol.", 0) == 0;
}

bool array_index(const std::string& key, uint32_t& out) {
    if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == '0')) return false;
    uint64_t value = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (value >= 4294967295ull) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

Value value_of(Object* o) {
    if (!o) return Value::null();
    if (o->get_type() == Object::ObjectType::Function) return Value(static_cast<Function*>(o));
    return Value(o);
}

Descriptor to_embed(const PropertyDescriptor& d) {
    Descriptor out;
    if (d.is_accessor_descriptor()) {
        out.has_get = d.has_getter();
        out.has_set = d.has_setter();
        if (d.get_getter()) out.get = value_of(d.get_getter());
        if (d.get_setter()) out.set = value_of(d.get_setter());
    } else {
        out.has_value = true;
        out.value = d.get_value();
        out.has_writable = true;
        out.writable = d.is_writable();
    }
    out.has_enumerable = true;
    out.enumerable = d.is_enumerable();
    out.has_configurable = true;
    out.configurable = d.is_configurable();
    return out;
}

PropertyDescriptor from_embed(const Descriptor& d) {
    PropertyDescriptor out;
    if (d.has_get || d.has_set) {
        if (d.has_get) out.set_getter(d.get.is_function() ? static_cast<Object*>(d.get.as_function()) : nullptr);
        if (d.has_set) out.set_setter(d.set.is_function() ? static_cast<Object*>(d.set.as_function()) : nullptr);
    }
    if (d.has_value) out.set_value(d.value);
    if (d.has_writable) out.set_writable(d.writable);
    if (d.has_enumerable) out.set_enumerable(d.enumerable);
    if (d.has_configurable) out.set_configurable(d.configurable);
    return out;
}

// The error a cross-origin access raises, made in the caller's realm: a DOMException named SecurityError if the
// realm has one, otherwise an Error named so.
void throw_security_error(Context& caller, const std::string& message) {
    RealmScope realm_scope(caller.realm());
    Value dom = Get(caller, Value(caller.get_global_object()), "DOMException");
    if (caller.has_exception()) return;
    if (dom.is_function()) {
        Value args[] = {Value(message), Value(std::string("SecurityError"))};
        Value error = Construct(caller, dom, Args(args, 2));
        if (!caller.has_exception()) Throw(caller, error);
        return;
    }
    Value error = NewError(caller, "Error", message);
    if (caller.has_exception()) return;
    Descriptor name;
    name.value = Value(std::string("SecurityError"));
    name.has_value = name.has_writable = name.has_configurable = true;
    name.writable = name.configurable = true;
    DefineProperty(caller, error, "name", name);
    Throw(caller, error);
}

// ---- A generic exotic object ----------------------------------------------------------------

struct ExoticHost : DOMObject {
    ExoticHooks hooks;

    static bool ExoticGetOwnProperty(Context& c, ExoticHost& self, const std::string& key, PropertyDescriptor& out) {
        if (!self.hooks.getOwnProperty) {
            if (!self.ordinary_has_own_property(key)) return false;
            out = self.ordinary_get_own_property(key);
            return true;
        }
        std::optional<Descriptor> d = self.hooks.getOwnProperty(c, Value(static_cast<Object*>(&self)), key_value(key));
        if (!d) return false;
        out = from_embed(*d);
        // A data descriptor the host gave in full is complete; from_embed leaves absent fields at their defaults.
        return true;
    }
    static bool ExoticDefineOwnProperty(Context& c, ExoticHost& self, const std::string& key, const PropertyDescriptor& d) {
        if (!self.hooks.defineOwnProperty) return self.ordinary_define_own_property(key, d);
        return self.hooks.defineOwnProperty(c, Value(static_cast<Object*>(&self)), key_value(key), to_embed(d));
    }
    static bool ExoticHasProperty(Context& c, ExoticHost& self, const std::string& key) {
        if (!self.hooks.hasProperty) return self.ordinary_has_property(key);
        return self.hooks.hasProperty(c, Value(static_cast<Object*>(&self)), key_value(key));
    }
    static Value ExoticGet(Context& c, ExoticHost& self, const std::string& key) {
        if (!self.hooks.get) return self.ordinary_get(key);
        Value me(static_cast<Object*>(&self));
        return self.hooks.get(c, me, key_value(key), me);
    }
    static bool ExoticSet(Context& c, ExoticHost& self, const std::string& key, const Value& value) {
        if (!self.hooks.set) return self.ordinary_set(key, value);
        Value me(static_cast<Object*>(&self));
        return self.hooks.set(c, me, key_value(key), value, me);
    }
    static bool ExoticDelete(Context& c, ExoticHost& self, const std::string& key) {
        if (!self.hooks.deleteProperty) return self.ordinary_delete(key);
        return self.hooks.deleteProperty(c, Value(static_cast<Object*>(&self)), key_value(key));
    }
    static std::vector<std::string> ExoticOwnKeys(Context& c, ExoticHost& self) {
        if (!self.hooks.ownPropertyKeys) return self.ordinary_own_keys();
        ValueList keys = self.hooks.ownPropertyKeys(c, Value(static_cast<Object*>(&self)));
        std::vector<std::string> out;
        for (const Value& k : keys) out.push_back(key_string(k));
        return out;
    }
    static bool ExoticGetPrototypeOf(Context& c, ExoticHost& self, Object*& out) {
        if (!self.hooks.getPrototypeOf) return false;
        out = self.hooks.getPrototypeOf(c, Value(static_cast<Object*>(&self))).as_object_or_null();
        return true;
    }
    static bool ExoticSetPrototypeOf(Context& c, ExoticHost& self, Object* prototype) {
        if (!self.hooks.setPrototypeOf) return false;
        return self.hooks.setPrototypeOf(c, Value(static_cast<Object*>(&self)), value_of(prototype));
    }
    static bool ExoticIsExtensible(Context& c, ExoticHost& self) {
        if (!self.hooks.isExtensible) return self.is_extensible();
        return self.hooks.isExtensible(c, Value(static_cast<Object*>(&self)));
    }
    static bool ExoticPreventExtensions(Context& c, ExoticHost& self) {
        if (!self.hooks.preventExtensions) { self.prevent_extensions(); return true; }
        return self.hooks.preventExtensions(c, Value(static_cast<Object*>(&self)));
    }
};

// ---- WindowProxy and the cross-origin objects --------------------------------------------------

// One type for both: a WindowProxy has a window to forward to; a cross-origin Location is its own target.
struct CrossOriginHost : DOMObject {
    CrossOriginHooks hooks;
    Object* target = nullptr;              // the window; null for an object that is its own target
    Quanta::Realm* target_realm = nullptr; // whose script the target is
    bool is_window = false;

    // The cross-origin property descriptors handed out, per caller realm, so that the same function is seen each
    // time (HTML's cross-origin property descriptor map).
    struct Cached {
        Quanta::Realm* caller;
        std::string key;
        PropertyDescriptor descriptor;
    };
    std::vector<Cached> cache;

    void Visit(Visitor& v) {
        v.Mark(target);
        for (const Cached& c : cache) {
            v.Mark(c.descriptor.get_value());
            v.Mark(c.descriptor.get_getter());
            v.Mark(c.descriptor.get_setter());
        }
    }

    Object* self_object() { return static_cast<Object*>(this); }
    // The object whose own properties are the real ones.
    Object* window() { return target ? target : self_object(); }

    static Embed::Realm* embed_realm(Quanta::Realm* realm) {
        if (!realm || realm->dead() || !realm->engine()) return nullptr;
        return static_cast<Embed::Realm*>(realm->engine()->host_realm());
    }

    // IsPlatformObjectSameOrigin.
    bool same_origin(Context& caller) {
        Quanta::Realm* from = caller.realm();
        if (from == target_realm) return true;
        if (!hooks.sameOrigin) return false;
        return hooks.sameOrigin(embed_realm(from), embed_realm(target_realm));
    }

    // OrdinaryGetOwnProperty on the real object.
    bool real_own(const std::string& key, PropertyDescriptor& out) {
        if (!target) {
            if (!ordinary_has_own_property(key)) return false;
            out = ordinary_get_own_property(key);
            return true;
        }
        if (!target->has_own_property(key)) return false;
        out = target->get_property_descriptor(key);
        return true;
    }

    const CrossOriginProperty* find_property(const std::string& key) const {
        for (const CrossOriginProperty& p : hooks.properties) if (p.name == key) return &p;
        return nullptr;
    }

    // A function that calls `original` with the `this` it was called with (a WindowProxy: a host method unwraps it
    // itself, UnwrapWindowProxy), made in the caller's realm.
    Value wrap(Context& caller, const Value& original, const std::string& name) {
        auto kept = std::make_shared<Persistent>(caller, original);
        return NewFunction(caller, name, 0, [kept](Context& ctx, Value thisValue, Args args, Value) -> Value {
            return Call(ctx, kept->Get(), thisValue, args);
        });
    }

    // CrossOriginGetOwnPropertyHelper.
    bool cross_origin_own(Context& caller, const std::string& key, PropertyDescriptor& out) {
        Quanta::Realm* from = caller.realm();
        for (const Cached& c : cache) {
            if (c.caller == from && c.key == key) { out = c.descriptor; return true; }
        }
        const CrossOriginProperty* property = find_property(key);
        if (!property) return false;
        PropertyDescriptor original;
        if (!real_own(key, original)) return false;
        PropertyDescriptor made;
        if (!property->getter && !property->setter) {
            // An operation: its function, wrapped.
            Value function = original.is_accessor_descriptor() ? Value() : original.get_value();
            if (!function.is_function()) return false;
            made = PropertyDescriptor(wrap(caller, function, function.as_function()->get_name()),
                                      static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
        } else {
            Object* getter = nullptr;
            Object* setter = nullptr;
            if (property->getter && original.is_accessor_descriptor() && original.get_getter()) {
                getter = wrap(caller, value_of(original.get_getter()), "get " + key).as_function();
            }
            if (property->setter && original.is_accessor_descriptor() && original.get_setter()) {
                setter = wrap(caller, value_of(original.get_setter()), "set " + key).as_function();
            }
            made = PropertyDescriptor(getter, setter, static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
        }
        cache.push_back({from, key, made});
        NoteWrite();   // the wrappers are young cells this object now reports
        out = made;
        return true;
    }

    // CrossOriginPropertyFallback: true with a descriptor for the few keys that read as undefined, otherwise a
    // SecurityError (and false).
    bool fallback(Context& caller, const std::string& key, PropertyDescriptor& out) {
        if (key == "then" || key == "Symbol.toStringTag" || key == "Symbol.hasInstance" || key == "Symbol.isConcatSpreadable") {
            out = PropertyDescriptor(Value(), static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
            return true;
        }
        throw_security_error(caller, "Blocked a cross-origin access to the property '" + key + "'");
        return false;
    }

    std::optional<Value> child_at(uint32_t index) {
        if (!is_window || !hooks.childAt) return std::nullopt;
        return hooks.childAt(embed_realm(target_realm), index);
    }

    // [[GetOwnProperty]] of a WindowProxy or a cross-origin object. False: none (an exception may be pending).
    static bool ExoticGetOwnProperty(Context& c, CrossOriginHost& self, const std::string& key, PropertyDescriptor& out) {
        uint32_t index;
        if (self.is_window && !is_symbol_key(key) && array_index(key, index)) {
            if (std::optional<Value> child = self.child_at(index)) {
                out = PropertyDescriptor(*child, static_cast<PropertyAttributes>(
                    PropertyAttributes::Enumerable | PropertyAttributes::Configurable));
                return true;
            }
            if (self.same_origin(c)) return self.real_own(key, out);
            return self.fallback(c, key, out);
        }
        if (self.same_origin(c)) return self.real_own(key, out);
        if (self.cross_origin_own(c, key, out)) return true;
        if (c.has_exception()) return false;
        if (self.is_window && self.hooks.childNamed && !is_symbol_key(key)) {
            if (std::optional<Value> child = self.hooks.childNamed(embed_realm(self.target_realm), key)) {
                out = PropertyDescriptor(*child, static_cast<PropertyAttributes>(PropertyAttributes::Configurable));
                return true;
            }
        }
        return self.fallback(c, key, out);
    }

    static bool ExoticDefineOwnProperty(Context& c, CrossOriginHost& self, const std::string& key, const PropertyDescriptor& d) {
        if (self.same_origin(c)) {
            uint32_t index;
            if (self.is_window && !is_symbol_key(key) && array_index(key, index)) return false;
            if (!self.target) return self.ordinary_define_own_property(key, d);
            return self.target->set_property_descriptor(key, d);
        }
        throw_security_error(c, "Blocked a cross-origin attempt to define the property '" + key + "'");
        return false;
    }

    static bool ExoticHasProperty(Context& c, CrossOriginHost& self, const std::string& key) {
        PropertyDescriptor desc;
        if (ExoticGetOwnProperty(c, self, key, desc)) return true;
        if (c.has_exception()) return false;
        if (!self.same_origin(c)) return false;
        Object* parent = self.window()->get_prototype();
        return parent && parent->has_property(key);
    }

    // OrdinaryGet with the object itself as the receiver, so that a getter sees the proxy as `this`.
    static Value get_from(Object* start, const std::string& key, const Value& receiver, Context& c) {
        for (Object* o = start; o; o = o->get_prototype()) {
            if (o->get_type() == Object::ObjectType::Proxy) return o->get_property(key);
            if (!o->has_own_property(key)) continue;
            PropertyDescriptor d = o->get_property_descriptor(key);
            if (d.is_accessor_descriptor()) {
                if (!d.get_getter() || !d.get_getter()->is_function()) return Value();
                return Call(c, value_of(d.get_getter()), receiver);
            }
            return d.get_value();
        }
        return Value();
    }

    static Value ExoticGet(Context& c, CrossOriginHost& self, const std::string& key) {
        Value me(self.self_object());
        if (self.same_origin(c)) {
            uint32_t index;
            if (self.is_window && !is_symbol_key(key) && array_index(key, index)) {
                if (std::optional<Value> child = self.child_at(index)) return *child;
            }
            if (!self.target) return self.ordinary_get(key);
            return get_from(self.target, key, me, c);
        }
        // CrossOriginGet
        PropertyDescriptor desc;
        if (!ExoticGetOwnProperty(c, self, key, desc)) return Value();
        if (desc.is_data_descriptor()) return desc.get_value();
        if (!desc.get_getter()) {
            throw_security_error(c, "Blocked a cross-origin read of the property '" + key + "'");
            return Value();
        }
        return Call(c, value_of(desc.get_getter()), me);
    }

    static bool ExoticSet(Context& c, CrossOriginHost& self, const std::string& key, const Value& value) {
        Value me(self.self_object());
        if (self.same_origin(c)) {
            uint32_t index;
            if (self.is_window && !is_symbol_key(key) && array_index(key, index)) return false;
            // A setter anywhere in the chain gets the proxy; otherwise the data property is the window's own.
            if (self.target) {
                for (Object* o = self.target; o; o = o->get_prototype()) {
                    if (o->get_type() == Object::ObjectType::Proxy) break;
                    if (!o->has_own_property(key)) continue;
                    PropertyDescriptor d = o->get_property_descriptor(key);
                    if (d.is_accessor_descriptor()) {
                        if (!d.get_setter()) return false;
                        Call(c, value_of(d.get_setter()), me, Args(&value, 1));
                        return !c.has_exception();
                    }
                    if (!d.is_writable()) return false;
                    break;
                }
                return self.target->set_property(key, value);
            }
            return self.ordinary_set(key, value);
        }
        // CrossOriginSet
        PropertyDescriptor desc;
        if (!ExoticGetOwnProperty(c, self, key, desc)) return false;
        if (desc.is_accessor_descriptor() && desc.get_setter()) {
            Call(c, value_of(desc.get_setter()), me, Args(&value, 1));
            return !c.has_exception();
        }
        throw_security_error(c, "Blocked a cross-origin write to the property '" + key + "'");
        return false;
    }

    static bool ExoticDelete(Context& c, CrossOriginHost& self, const std::string& key) {
        if (self.same_origin(c)) {
            uint32_t index;
            if (self.is_window && !is_symbol_key(key) && array_index(key, index)) {
                PropertyDescriptor desc;
                return !ExoticGetOwnProperty(c, self, key, desc);
            }
            if (!self.target) return self.ordinary_delete(key);
            return self.target->delete_property(key);
        }
        throw_security_error(c, "Blocked a cross-origin attempt to delete the property '" + key + "'");
        return false;
    }

    static std::vector<std::string> ExoticOwnKeys(Context& c, CrossOriginHost& self) {
        std::vector<std::string> keys;
        if (self.is_window && self.hooks.childCount) {
            const uint32_t count = self.hooks.childCount(embed_realm(self.target_realm));
            for (uint32_t i = 0; i < count; i++) keys.push_back(std::to_string(i));
        }
        if (self.same_origin(c)) {
            std::vector<std::string> own = self.target ? self.target->get_own_property_keys() : self.ordinary_own_keys();
            for (std::string& k : own) keys.push_back(std::move(k));
            return keys;
        }
        // CrossOriginOwnPropertyKeys
        for (const CrossOriginProperty& p : self.hooks.properties) keys.push_back(p.name);
        keys.push_back("Symbol.toStringTag");
        keys.push_back("Symbol.hasInstance");
        keys.push_back("Symbol.isConcatSpreadable");
        return keys;
    }

    static bool ExoticGetPrototypeOf(Context& c, CrossOriginHost& self, Object*& out) {
        out = self.same_origin(c) ? self.window()->get_prototype() : nullptr;
        return true;
    }
    // SetImmutablePrototype: only the prototype it already has.
    static bool ExoticSetPrototypeOf(Context& c, CrossOriginHost& self, Object* prototype) {
        Object* current = self.same_origin(c) ? self.window()->get_prototype() : nullptr;
        return prototype == current;
    }
    static bool ExoticIsExtensible(Context&, CrossOriginHost&) { return true; }
    static bool ExoticPreventExtensions(Context&, CrossOriginHost&) { return false; }
};

// The Window behind a WindowProxy, as an object.
CrossOriginHost* proxy_of(const Value& value) {
    return DOMObject::Cast<CrossOriginHost>(value);
}

Quanta::Realm* realm_of_embed(Realm& realm) {
    return realm.GetContext().realm();
}

}

Value NewExoticObject(Context& ctx, ExoticHooks hooks, const Value& prototype) {
    RealmScope realm_scope(ctx.realm());
    ExoticHost* object = Heap::Allocate<ExoticHost>();
    object->hooks = std::move(hooks);
    Object* parent = prototype.as_object_or_null();
    object->initialize_prototype(parent);
    return Value(static_cast<Object*>(object));
}

Value NewWindowProxy(Realm& window, CrossOriginHooks hooks) {
    Context& ctx = window.GetContext();
    RealmScope realm_scope(ctx.realm());
    CrossOriginHost* proxy = Heap::Allocate<CrossOriginHost>();
    proxy->hooks = std::move(hooks);
    proxy->is_window = true;
    proxy->target = ctx.get_global_object();
    proxy->target_realm = realm_of_embed(window);
    proxy->initialize_prototype(proxy->target ? proxy->target->get_prototype() : nullptr);
    Value value(static_cast<Object*>(proxy));
    window.SetGlobalProxy(value);
    return value;
}

bool SetWindowProxyTarget(const Value& proxy, Realm& window) {
    CrossOriginHost* host = proxy_of(proxy);
    if (!host || !host->is_window) return false;
    Context& ctx = window.GetContext();
    host->target = ctx.get_global_object();
    host->target_realm = realm_of_embed(window);
    host->NoteWrite();
    // The prototype of the window behind it is the proxy's own.
    Object* parent = host->target ? host->target->get_prototype() : nullptr;
    host->set_prototype(parent);
    window.SetGlobalProxy(proxy);
    return true;
}

Value UnwrapWindowProxy(const Value& value) {
    CrossOriginHost* host = proxy_of(value);
    if (!host || !host->is_window || !host->target) return value;
    return Value(host->target);
}

bool IsWindowProxy(const Value& value) {
    CrossOriginHost* host = proxy_of(value);
    return host && host->is_window;
}

Value NewCrossOriginObject(Realm& realm, const Value& prototype, CrossOriginHooks hooks) {
    Context& ctx = realm.GetContext();
    RealmScope realm_scope(ctx.realm());
    CrossOriginHost* object = Heap::Allocate<CrossOriginHost>();
    object->hooks = std::move(hooks);
    object->target_realm = realm_of_embed(realm);
    object->initialize_prototype(prototype.as_object_or_null());
    return Value(static_cast<Object*>(object));
}

void Realm::SetGlobalProxy(const Value& proxy) {
    Object* object = proxy.as_object_or_null();
    Context& ctx = GetContext();
    Quanta::Realm* realm = ctx.realm();
    if (!object || !realm) return;
    realm->global_proxy = object;
    ctx.set_this_value(proxy);
    // globalThis names the proxy.
    if (Object* global = ctx.get_global_object()) {
        PropertyDescriptor desc(proxy, static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
        global->set_property_descriptor("globalThis", desc);
    }
}

}
