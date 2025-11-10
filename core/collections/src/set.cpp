/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/set.h"
#include "../../include/Context.h"
#include "../../include/Symbol.h"
#include "../../include/Iterator.h"
#include "../../../parser/include/AST.h"
#include <algorithm>
#include <iostream>

namespace Quanta {

// Initialize static prototype reference
Object* Set::prototype_object = nullptr;

//=============================================================================
// Set Implementation
//=============================================================================

Set::Set() : Object(ObjectType::Set), size_(0) {
}

bool Set::has(const Value& value) const {
    return find_value(value) != values_.end();
}

void Set::add(const Value& value) {
    if (find_value(value) == values_.end()) {
        values_.push_back(value);
        size_++;
    }
}

bool Set::delete_value(const Value& value) {
    auto it = find_value(value);
    if (it != values_.end()) {
        values_.erase(it);
        size_--;
        return true;
    }
    return false;
}

void Set::clear() {
    values_.clear();
    size_ = 0;
}

Value Set::get_property(const std::string& key) const {
    if (key == "size") {
        return Value(static_cast<double>(size_));
    }
    return Object::get_property(key);
}

std::vector<Value> Set::values() const {
    return values_;
}

std::vector<std::pair<Value, Value>> Set::entries() const {
    std::vector<std::pair<Value, Value>> result;
    result.reserve(size_);
    for (const auto& value : values_) {
        result.emplace_back(value, value);
    }
    return result;
}

std::vector<Value>::iterator Set::find_value(const Value& value) {
    return std::find_if(values_.begin(), values_.end(),
        [&value](const Value& v) {
            return v.strict_equals(value);
        });
}

std::vector<Value>::const_iterator Set::find_value(const Value& value) const {
    return std::find_if(values_.begin(), values_.end(),
        [&value](const Value& v) {
            return v.strict_equals(value);
        });
}

// Set built-in methods
Value Set::set_constructor(Context& ctx, const std::vector<Value>& args) {
    auto set = std::make_unique<Set>();

    // Set up prototype chain using static reference
    if (Set::prototype_object) {
        set->set_prototype(Set::prototype_object);
    }

    // If iterable argument provided, populate set
    if (!args.empty() && args[0].is_object()) {
        Object* iterable = args[0].as_object();
        if (iterable->is_array()) {
            // Array case: add all elements
            uint32_t length = iterable->get_length();
            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                set->add(element);
            }
        }
        // Use iteration protocol for other iterables
        else {
            auto iterator = IterableUtils::get_iterator(args[0], ctx);
            if (iterator) {
                while (true) {
                    auto result = iterator->next();
                    if (result.done) {
                        break;
                    }

                    // Add each iteration value to the set
                    set->add(result.value);
                }
            }
        }
    }

    return Value(set.release());
}

Value Set::set_add(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Set.prototype.add called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value("Set.prototype.add called on non-Set"));
        return Value();
    }

    Set* set = static_cast<Set*>(obj);
    Value value = args.empty() ? Value() : args[0];

    set->add(value);
    return Value(obj); // Return the Set object for chaining
}

Value Set::set_has(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Set.prototype.has called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value("Set.prototype.has called on non-Set"));
        return Value();
    }

    Set* set = static_cast<Set*>(obj);
    Value value = args.empty() ? Value() : args[0];

    return Value(set->has(value));
}

Value Set::set_delete(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Set.prototype.delete called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value("Set.prototype.delete called on non-Set"));
        return Value();
    }

    Set* set = static_cast<Set*>(obj);
    Value value = args.empty() ? Value() : args[0];

    return Value(set->delete_value(value));
}

Value Set::set_clear(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Unused parameter

    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Set.prototype.clear called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value("Set.prototype.clear called on non-Set"));
        return Value();
    }

    Set* set = static_cast<Set*>(obj);
    set->clear();
    return Value(); // undefined
}

Value Set::set_size_getter(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Unused parameter

    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Set.prototype.size called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value("Set.prototype.size called on non-Set"));
        return Value();
    }

    Set* set = static_cast<Set*>(obj);
    return Value(static_cast<double>(set->size()));
}

Value Set::set_iterator_method(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Unused parameter

    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Set.prototype[Symbol.iterator] called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value("Set.prototype[Symbol.iterator] called on non-Set"));
        return Value();
    }

    Set* set = static_cast<Set*>(obj);
    auto iterator = std::make_unique<SetIterator>(set, SetIterator::Kind::Values);
    return Value(iterator.release());
}

void Set::setup_set_prototype(Context& ctx) {
    // Create Set constructor
    auto set_constructor_fn = ObjectFactory::create_native_function("Set", set_constructor);

    // Create Set.prototype
    auto set_prototype = ObjectFactory::create_object();

    // Add methods to Set.prototype
    auto add_fn = ObjectFactory::create_native_function("add", set_add);
    auto has_fn = ObjectFactory::create_native_function("has", set_has);
    auto delete_fn = ObjectFactory::create_native_function("delete", set_delete);
    auto clear_fn = ObjectFactory::create_native_function("clear", set_clear);
    auto size_fn = ObjectFactory::create_native_function("size", set_size_getter);

    set_prototype->set_property("add", Value(add_fn.release()));
    set_prototype->set_property("has", Value(has_fn.release()));
    set_prototype->set_property("delete", Value(delete_fn.release()));
    set_prototype->set_property("clear", Value(clear_fn.release()));
    set_prototype->set_property("size", Value(size_fn.release()));

    // Add Symbol.iterator method for Set iteration
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto set_iterator_fn = ObjectFactory::create_native_function("@@iterator", set_iterator_method);
        set_prototype->set_property(iterator_symbol->to_string(), Value(set_iterator_fn.release()));
    }

    // Store reference for constructor use
    Set::prototype_object = set_prototype.get();

    set_constructor_fn->set_property("prototype", Value(set_prototype.release()));
    ctx.create_binding("Set", Value(set_constructor_fn.release()));
}

} // namespace Quanta