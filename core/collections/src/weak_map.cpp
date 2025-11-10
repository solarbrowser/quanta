/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/weak_map.h"
#include "../../include/Context.h"
#include "../../../parser/include/AST.h"
#include <iostream>

namespace Quanta {

// Initialize static prototype reference
Object* WeakMap::prototype_object = nullptr;

//=============================================================================
// WeakMap Implementation
//=============================================================================

WeakMap::WeakMap() : Object(ObjectType::WeakMap) {
}

bool WeakMap::has(Object* key) const {
    return entries_.find(key) != entries_.end();
}

Value WeakMap::get(Object* key) const {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        return it->second;
    }
    return Value(); // undefined
}

void WeakMap::set(Object* key, const Value& value) {
    entries_[key] = value;
}

bool WeakMap::delete_key(Object* key) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        entries_.erase(it);
        return true;
    }
    return false;
}

Value WeakMap::weakmap_constructor(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Unused parameter
    auto weakmap = std::make_unique<WeakMap>();

    // Set up prototype chain using static reference
    if (WeakMap::prototype_object) {
        weakmap->set_prototype(WeakMap::prototype_object);
    }

    return Value(weakmap.release());
}

Value WeakMap::weakmap_set(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value("WeakMap.prototype.set requires 2 arguments"));
        return Value();
    }

    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value("WeakMap.prototype.set called on non-object"));
        return Value();
    }

    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_exception(Value("WeakMap.prototype.set called on non-WeakMap"));
        return Value();
    }

    WeakMap* weakmap = static_cast<WeakMap*>(this_obj);

    if (args[0].is_object()) {
        Object* key = args[0].as_object();
        weakmap->set(key, args[1]);
        return Value(this_obj);
    } else {
        ctx.throw_exception(Value("WeakMap key must be an object"));
        return Value();
    }
}

Value WeakMap::weakmap_get(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value();
    }

    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value("WeakMap.prototype.get called on non-object"));
        return Value();
    }

    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_exception(Value("WeakMap.prototype.get called on non-WeakMap"));
        return Value();
    }

    WeakMap* weakmap = static_cast<WeakMap*>(this_obj);

    if (args[0].is_object()) {
        Object* key = args[0].as_object();
        return weakmap->get(key);
    }

    return Value();
}

Value WeakMap::weakmap_has(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(false);
    }

    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value("WeakMap.prototype.has called on non-object"));
        return Value();
    }

    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_exception(Value("WeakMap.prototype.has called on non-WeakMap"));
        return Value();
    }

    WeakMap* weakmap = static_cast<WeakMap*>(this_obj);

    if (args[0].is_object()) {
        Object* key = args[0].as_object();
        return Value(weakmap->has(key));
    }

    return Value(false);
}

Value WeakMap::weakmap_delete(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(false);
    }

    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value("WeakMap.prototype.delete called on non-object"));
        return Value();
    }

    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_exception(Value("WeakMap.prototype.delete called on non-WeakMap"));
        return Value();
    }

    WeakMap* weakmap = static_cast<WeakMap*>(this_obj);

    if (args[0].is_object()) {
        Object* key = args[0].as_object();
        return Value(weakmap->delete_key(key));
    }

    return Value(false);
}

void WeakMap::setup_weakmap_prototype(Context& ctx) {
    // Create WeakMap constructor
    auto weakmap_constructor_fn = ObjectFactory::create_native_function("WeakMap", weakmap_constructor);

    // Create WeakMap.prototype
    auto weakmap_prototype = ObjectFactory::create_object();

    // Add methods to WeakMap.prototype
    auto set_fn = ObjectFactory::create_native_function("set", weakmap_set);
    auto get_fn = ObjectFactory::create_native_function("get", weakmap_get);
    auto has_fn = ObjectFactory::create_native_function("has", weakmap_has);
    auto delete_fn = ObjectFactory::create_native_function("delete", weakmap_delete);

    weakmap_prototype->set_property("set", Value(set_fn.release()));
    weakmap_prototype->set_property("get", Value(get_fn.release()));
    weakmap_prototype->set_property("has", Value(has_fn.release()));
    weakmap_prototype->set_property("delete", Value(delete_fn.release()));

    // Store reference for constructor use
    WeakMap::prototype_object = weakmap_prototype.get();

    weakmap_constructor_fn->set_property("prototype", Value(weakmap_prototype.release()));
    ctx.create_binding("WeakMap", Value(weakmap_constructor_fn.release()));
}

} // namespace Quanta