/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/map.h"
#include "../../include/Context.h"
#include "../../include/Symbol.h"
#include "../../include/Iterator.h"
#include "../../../parser/include/AST.h"
#include <algorithm>
#include <iostream>

namespace Quanta {

// Initialize static prototype reference
Object* Map::prototype_object = nullptr;

//=============================================================================
// Map Implementation
//=============================================================================

Map::Map() : Object(ObjectType::Map), size_(0) {
}

bool Map::has(const Value& key) const {
    return find_entry(key) != entries_.end();
}

Value Map::get(const Value& key) const {
    auto it = find_entry(key);
    if (it != entries_.end()) {
        return it->value;
    }
    return Value(); // undefined
}

void Map::set(const Value& key, const Value& value) {
    auto it = find_entry(key);
    if (it != entries_.end()) {
        it->value = value;
    } else {
        entries_.emplace_back(key, value);
        size_++;
    }
}

bool Map::delete_key(const Value& key) {
    auto it = find_entry(key);
    if (it != entries_.end()) {
        entries_.erase(it);
        size_--;
        return true;
    }
    return false;
}

void Map::clear() {
    entries_.clear();
    size_ = 0;
}

Value Map::get_property(const std::string& key) const {
    if (key == "size") {
        return Value(static_cast<double>(size_));
    }
    return Object::get_property(key);
}

std::vector<Value> Map::keys() const {
    std::vector<Value> result;
    result.reserve(size_);
    for (const auto& entry : entries_) {
        result.push_back(entry.key);
    }
    return result;
}

std::vector<Value> Map::values() const {
    std::vector<Value> result;
    result.reserve(size_);
    for (const auto& entry : entries_) {
        result.push_back(entry.value);
    }
    return result;
}

std::vector<std::pair<Value, Value>> Map::entries() const {
    std::vector<std::pair<Value, Value>> result;
    result.reserve(size_);
    for (const auto& entry : entries_) {
        result.emplace_back(entry.key, entry.value);
    }
    return result;
}

std::vector<Map::MapEntry>::iterator Map::find_entry(const Value& key) {
    return std::find_if(entries_.begin(), entries_.end(),
        [&key](const MapEntry& entry) {
            return entry.key.strict_equals(key);
        });
}

std::vector<Map::MapEntry>::const_iterator Map::find_entry(const Value& key) const {
    return std::find_if(entries_.begin(), entries_.end(),
        [&key](const MapEntry& entry) {
            return entry.key.strict_equals(key);
        });
}

// Map built-in methods
Value Map::map_constructor(Context& ctx, const std::vector<Value>& args) {
    auto map = std::make_unique<Map>();

    // Set up prototype chain using static reference
    if (Map::prototype_object) {
        map->set_prototype(Map::prototype_object);
    }

    // If iterable argument provided, populate map
    if (!args.empty() && args[0].is_object()) {
        Object* iterable = args[0].as_object();

        // Special case: Handle arrays directly (more reliable than iterator protocol)
        if (iterable->is_array()) {
            uint32_t length = iterable->get_length();
            for (uint32_t i = 0; i < length; i++) {
                Value entry = iterable->get_element(i);
                if (entry.is_object() && entry.as_object()->is_array()) {
                    Object* pair = entry.as_object();
                    if (pair->get_length() >= 2) {
                        Value key = pair->get_element(0);
                        Value value = pair->get_element(1);
                        map->set(key, value);
                    }
                }
            }
        } else {
            // Use iteration protocol for other iterables
            auto iterator = IterableUtils::get_iterator(args[0], ctx);
            if (iterator) {
                while (true) {
                    auto result = iterator->next();
                    if (result.done) {
                        break;
                    }

                    // Each iteration value should be a [key, value] pair
                    if (result.value.is_object() && result.value.as_object()->is_array()) {
                        Object* pair = result.value.as_object();
                        if (pair->get_length() >= 2) {
                            Value key = pair->get_element(0);
                            Value value = pair->get_element(1);
                            map->set(key, value);
                        }
                    } else {
                        // Invalid entry format - ignore or throw error
                        ctx.throw_exception(Value("Iterator value is not a [key, value] pair"));
                        break;
                    }
                }
            }
        }
    }

    return Value(map.release());
}

Value Map::map_set(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Map.prototype.set called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value("Map.prototype.set called on non-Map"));
        return Value();
    }

    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];
    Value value = args.size() < 2 ? Value() : args[1];

    map->set(key, value);
    return Value(obj); // Return the Map object for chaining
}

Value Map::map_get(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Map.prototype.get called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value("Map.prototype.get called on non-Map"));
        return Value();
    }

    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];

    return map->get(key);
}

Value Map::map_has(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Map.prototype.has called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value("Map.prototype.has called on non-Map"));
        return Value();
    }

    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];

    return Value(map->has(key));
}

Value Map::map_delete(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Map.prototype.delete called on non-object"));
        return Value();
    }

    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value("Map.prototype.delete called on non-Map"));
        return Value();
    }

    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];

    return Value(map->delete_key(key));
}

Value Map::map_clear(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Unused parameter

    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Map.prototype.clear called on non-object"));
        return Value();
    }

    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value("Map.prototype.clear called on non-Map"));
        return Value();
    }

    Map* map = static_cast<Map*>(obj);
    map->clear();
    return Value(); // undefined
}

Value Map::map_size_getter(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Unused parameter

    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Map.prototype.size called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value("Map.prototype.size called on non-Map"));
        return Value();
    }

    Map* map = static_cast<Map*>(obj);
    return Value(static_cast<double>(map->size()));
}

Value Map::map_iterator_method(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Unused parameter

    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value("Map.prototype[Symbol.iterator] called on non-object"));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value("Map.prototype[Symbol.iterator] called on non-Map"));
        return Value();
    }

    Map* map = static_cast<Map*>(obj);
    auto iterator = std::make_unique<MapIterator>(map, MapIterator::Kind::Entries);
    return Value(iterator.release());
}

void Map::setup_map_prototype(Context& ctx) {
    // Create Map constructor
    auto map_constructor_fn = ObjectFactory::create_native_function("Map", map_constructor);

    // Create Map.prototype
    auto map_prototype = ObjectFactory::create_object();

    // Add methods to Map.prototype
    auto set_fn = ObjectFactory::create_native_function("set", map_set);
    auto get_fn = ObjectFactory::create_native_function("get", map_get);
    auto has_fn = ObjectFactory::create_native_function("has", map_has);
    auto delete_fn = ObjectFactory::create_native_function("delete", map_delete);
    auto clear_fn = ObjectFactory::create_native_function("clear", map_clear);
    auto size_fn = ObjectFactory::create_native_function("size", map_size_getter);

    map_prototype->set_property("set", Value(set_fn.release()));
    map_prototype->set_property("get", Value(get_fn.release()));
    map_prototype->set_property("has", Value(has_fn.release()));
    map_prototype->set_property("delete", Value(delete_fn.release()));
    map_prototype->set_property("clear", Value(clear_fn.release()));
    map_prototype->set_property("size", Value(size_fn.release()));

    // Add Symbol.iterator method for Map iteration
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto map_iterator_fn = ObjectFactory::create_native_function("@@iterator", map_iterator_method);
        map_prototype->set_property(iterator_symbol->to_string(), Value(map_iterator_fn.release()));
    }

    // Store reference for constructor use
    Map::prototype_object = map_prototype.get();

    map_constructor_fn->set_property("prototype", Value(map_prototype.release()));
    ctx.create_binding("Map", Value(map_constructor_fn.release()));
}

} // namespace Quanta