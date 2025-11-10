/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/weak_set.h"
#include "../../include/Context.h"
#include "../../../parser/include/AST.h"
#include <iostream>

namespace Quanta {

// Initialize static prototype reference
Object* WeakSet::prototype_object = nullptr;

//=============================================================================
// WeakSet Implementation
//=============================================================================

WeakSet::WeakSet() : Object(ObjectType::WeakSet) {
}

bool WeakSet::has(Object* value) const {
    return values_.find(value) != values_.end();
}

void WeakSet::add(Object* value) {
    values_.insert(value);
}

bool WeakSet::delete_value(Object* value) {
    auto it = values_.find(value);
    if (it != values_.end()) {
        values_.erase(it);
        return true;
    }
    return false;
}

Value WeakSet::weakset_constructor(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Unused parameter
    auto weakset = std::make_unique<WeakSet>();

    // Set up prototype chain using static reference
    if (WeakSet::prototype_object) {
        weakset->set_prototype(WeakSet::prototype_object);
    }

    return Value(weakset.release());
}

Value WeakSet::weakset_add(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value("WeakSet.prototype.add requires an argument"));
        return Value();
    }

    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value("WeakSet.prototype.add called on non-object"));
        return Value();
    }

    if (this_obj->get_type() != Object::ObjectType::WeakSet) {
        ctx.throw_exception(Value("WeakSet.prototype.add called on non-WeakSet"));
        return Value();
    }

    WeakSet* weakset = static_cast<WeakSet*>(this_obj);

    if (args[0].is_object()) {
        Object* value = args[0].as_object();
        weakset->add(value);
        return Value(this_obj);
    } else {
        ctx.throw_exception(Value("WeakSet value must be an object"));
        return Value();
    }
}

Value WeakSet::weakset_has(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(false);
    }

    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value("WeakSet.prototype.has called on non-object"));
        return Value();
    }

    if (this_obj->get_type() != Object::ObjectType::WeakSet) {
        ctx.throw_exception(Value("WeakSet.prototype.has called on non-WeakSet"));
        return Value();
    }

    WeakSet* weakset = static_cast<WeakSet*>(this_obj);

    if (args[0].is_object()) {
        Object* value = args[0].as_object();
        return Value(weakset->has(value));
    }

    return Value(false);
}

Value WeakSet::weakset_delete(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(false);
    }

    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value("WeakSet.prototype.delete called on non-object"));
        return Value();
    }

    if (this_obj->get_type() != Object::ObjectType::WeakSet) {
        ctx.throw_exception(Value("WeakSet.prototype.delete called on non-WeakSet"));
        return Value();
    }

    WeakSet* weakset = static_cast<WeakSet*>(this_obj);

    if (args[0].is_object()) {
        Object* value = args[0].as_object();
        return Value(weakset->delete_value(value));
    }

    return Value(false);
}

void WeakSet::setup_weakset_prototype(Context& ctx) {
    // Create WeakSet constructor
    auto weakset_constructor_fn = ObjectFactory::create_native_function("WeakSet", weakset_constructor);

    // Create WeakSet.prototype
    auto weakset_prototype = ObjectFactory::create_object();

    // Add methods to WeakSet.prototype
    auto add_fn = ObjectFactory::create_native_function("add", weakset_add);
    auto has_fn = ObjectFactory::create_native_function("has", weakset_has);
    auto delete_fn = ObjectFactory::create_native_function("delete", weakset_delete);

    weakset_prototype->set_property("add", Value(add_fn.release()));
    weakset_prototype->set_property("has", Value(has_fn.release()));
    weakset_prototype->set_property("delete", Value(delete_fn.release()));

    // Store reference for constructor use
    WeakSet::prototype_object = weakset_prototype.get();

    weakset_constructor_fn->set_property("prototype", Value(weakset_prototype.release()));
    ctx.create_binding("WeakSet", Value(weakset_constructor_fn.release()));
}

} // namespace Quanta