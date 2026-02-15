/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/MapSet.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Iterator.h"
#include "quanta/parser/AST.h"
#include <algorithm>
#include <iostream>

namespace Quanta {

Object* Map::prototype_object = nullptr;
Object* Set::prototype_object = nullptr;
Object* WeakMap::prototype_object = nullptr;
Object* WeakSet::prototype_object = nullptr;


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
    return Value();
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

Value Map::map_constructor(Context& ctx, const std::vector<Value>& args) {
    if (!ctx.is_in_constructor_call()) {
        ctx.throw_type_error("Constructor Map requires 'new'");
        return Value();
    }
    auto map = std::make_unique<Map>();
    
    if (Map::prototype_object) {
        map->set_prototype(Map::prototype_object);
    }
    
    Map* map_ptr = map.get();
    Object* map_obj = map.release();

    if (!args.empty() && args[0].is_object()) {
        Object* iterable = args[0].as_object();
        // ES6: constructor must invoke the "set" method on the instance
        Value set_method = map_obj->get_property("set");
        Function* set_fn = set_method.is_function() ? set_method.as_function() : nullptr;

        if (iterable->is_array()) {
            uint32_t length = iterable->get_length();
            for (uint32_t i = 0; i < length; i++) {
                Value entry = iterable->get_element(i);
                if (entry.is_object() && entry.as_object()->is_array()) {
                    Object* pair = entry.as_object();
                    if (pair->get_length() >= 2) {
                        Value key = pair->get_element(0);
                        Value value = pair->get_element(1);
                        if (set_fn) {
                            set_fn->call(ctx, {key, value}, Value(map_obj));
                        } else {
                            map_ptr->set(key, value);
                        }
                    }
                }
            }
        } else {
            auto iterator = IterableUtils::get_iterator(args[0], ctx);
            if (iterator) {
                while (true) {
                    auto result = iterator->next();
                    if (result.done) {
                        break;
                    }

                    if (result.value.is_object() && result.value.as_object()->is_array()) {
                        Object* pair = result.value.as_object();
                        if (pair->get_length() >= 2) {
                            Value key = pair->get_element(0);
                            Value value = pair->get_element(1);
                            if (set_fn) {
                                set_fn->call(ctx, {key, value}, Value(map_obj));
                            } else {
                                map_ptr->set(key, value);
                            }
                        }
                    } else {
                        ctx.throw_exception(Value(std::string("Iterator value is not a [key, value] pair")));
                        break;
                    }
                }
            }
        }
    }

    return Value(map_obj);
}

Value Map::map_set(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Map.prototype.set called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value(std::string("Map.prototype.set called on non-Map")));
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];
    Value value = args.size() < 2 ? Value() : args[1];
    
    map->set(key, value);
    return Value(obj);
}

Value Map::map_get(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Map.prototype.get called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value(std::string("Map.prototype.get called on non-Map")));
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];
    
    return map->get(key);
}

Value Map::map_has(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Map.prototype.has called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value(std::string("Map.prototype.has called on non-Map")));
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];
    
    return Value(map->has(key));
}

Value Map::map_delete(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("Map.prototype.delete called on non-object")));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value(std::string("Map.prototype.delete called on non-Map")));
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];
    
    return Value(map->delete_key(key));
}

Value Map::map_clear(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value(std::string("Map.prototype.clear called on non-object")));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value(std::string("Map.prototype.clear called on non-Map")));
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    map->clear();
    return Value();
}

Value Map::map_size_getter(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Map.prototype.size called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value(std::string("Map.prototype.size called on non-Map")));
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    return Value(static_cast<double>(map->size()));
}

Value Map::map_iterator_method(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Map.prototype[Symbol.iterator] called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_exception(Value(std::string("Map.prototype[Symbol.iterator] called on non-Map")));
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    auto iterator = std::make_unique<MapIterator>(map, MapIterator::Kind::Entries);
    return Value(iterator.release());
}

void Map::setup_map_prototype(Context& ctx) {
    auto map_constructor_fn = ObjectFactory::create_native_constructor("Map", map_constructor, 0);
    
    auto map_prototype = ObjectFactory::create_object();
    
    auto set_fn = ObjectFactory::create_native_function("set", map_set);
    auto get_fn = ObjectFactory::create_native_function("get", map_get);
    auto has_fn = ObjectFactory::create_native_function("has", map_has);
    auto delete_fn = ObjectFactory::create_native_function("delete", map_delete);
    auto clear_fn = ObjectFactory::create_native_function("clear", map_clear);
    auto size_fn = ObjectFactory::create_native_function("size", map_size_getter);

    map_prototype->set_property("set", Value(set_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    map_prototype->set_property("get", Value(get_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    map_prototype->set_property("has", Value(has_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    map_prototype->set_property("delete", Value(delete_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    map_prototype->set_property("clear", Value(clear_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    map_prototype->set_property("size", Value(size_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // forEach method
    auto forEach_fn = ObjectFactory::create_native_function("forEach",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Map) {
                ctx.throw_type_error("Map.prototype.forEach called on non-Map");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("forEach requires a callback function");
                return Value();
            }
            Map* map = static_cast<Map*>(obj);
            Function* callback = args[0].as_function();
            Value this_arg = args.size() > 1 ? args[1] : Value();
            auto entries = map->entries();
            for (const auto& entry : entries) {
                std::vector<Value> cb_args = {entry.second, entry.first, Value(obj)};
                callback->call(ctx, cb_args, this_arg);
                if (ctx.has_exception()) return Value();
            }
            return Value();
        }, 1);
    map_prototype->set_property("forEach", Value(forEach_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // keys method
    auto keys_fn = ObjectFactory::create_native_function("keys",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Map) {
                ctx.throw_type_error("Map.prototype.keys called on non-Map");
                return Value();
            }
            Map* map = static_cast<Map*>(obj);
            auto iterator = std::make_unique<MapIterator>(map, MapIterator::Kind::Keys);
            return Value(iterator.release());
        }, 0);
    map_prototype->set_property("keys", Value(keys_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // values method
    auto values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Map) {
                ctx.throw_type_error("Map.prototype.values called on non-Map");
                return Value();
            }
            Map* map = static_cast<Map*>(obj);
            auto iterator = std::make_unique<MapIterator>(map, MapIterator::Kind::Values);
            return Value(iterator.release());
        }, 0);
    map_prototype->set_property("values", Value(values_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // entries method + Symbol.iterator
    auto entries_fn = ObjectFactory::create_native_function("entries", map_iterator_method, 0);
    map_prototype->set_property("entries", Value(entries_fn.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        map_prototype->set_property(iterator_symbol->to_string(), Value(entries_fn.release()),
            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    } else {
        entries_fn.release();
    }

    PropertyDescriptor map_tag_desc(Value(std::string("Map")), PropertyAttributes::Configurable);
    map_prototype->set_property_descriptor("Symbol.toStringTag", map_tag_desc);

    Map::prototype_object = map_prototype.get();

    auto map_groupBy_fn = ObjectFactory::create_native_function("groupBy",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                throw std::runtime_error("Map.groupBy requires 2 arguments");
            }

            Object* iterable = args[0].as_object();
            Function* callback = args[1].as_function();

            if (!iterable || !callback) {
                throw std::runtime_error("Invalid arguments to Map.groupBy");
            }

            auto result_map = std::make_unique<Map>();
            uint32_t length = iterable->get_length();

            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)), args[0] };
                Value key = callback->call(ctx, callback_args);

                Value group = result_map->get(key);
                Object* group_array;

                if (group.is_undefined()) {
                    auto new_array = ObjectFactory::create_array();
                    group_array = new_array.get();
                    result_map->set(key, Value(new_array.release()));
                } else {
                    group_array = group.as_object();
                }

                uint32_t group_length = group_array->get_length();
                group_array->set_element(group_length, element);
                group_array->set_length(group_length + 1);
            }

            return Value(result_map.release());
        }, 2);
    map_constructor_fn->set_property("groupBy", Value(map_groupBy_fn.release()));

    map_constructor_fn->set_property("prototype", Value(map_prototype.release()));
    ctx.create_binding("Map", Value(map_constructor_fn.release()));
}


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

Value Set::set_constructor(Context& ctx, const std::vector<Value>& args) {
    if (!ctx.is_in_constructor_call()) {
        ctx.throw_type_error("Constructor Set requires 'new'");
        return Value();
    }
    auto set = std::make_unique<Set>();
    
    if (Set::prototype_object) {
        set->set_prototype(Set::prototype_object);
    }
    
    Set* set_ptr = set.get();
    Object* set_obj = set.release();

    if (!args.empty() && args[0].is_object()) {
        Object* iterable = args[0].as_object();
        // ES6: constructor must invoke the "add" method on the instance
        Value add_method = set_obj->get_property("add");
        Function* add_fn = add_method.is_function() ? add_method.as_function() : nullptr;

        if (iterable->is_array()) {
            uint32_t length = iterable->get_length();
            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                if (add_fn) {
                    add_fn->call(ctx, {element}, Value(set_obj));
                } else {
                    set_ptr->add(element);
                }
            }
        }
        else {
            auto iterator = IterableUtils::get_iterator(args[0], ctx);
            if (iterator) {
                while (true) {
                    auto result = iterator->next();
                    if (result.done) {
                        break;
                    }
                    if (add_fn) {
                        add_fn->call(ctx, {result.value}, Value(set_obj));
                    } else {
                        set_ptr->add(result.value);
                    }
                }
            }
        }
    }

    return Value(set_obj);
}

Value Set::set_add(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Set.prototype.add called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value(std::string("Set.prototype.add called on non-Set")));
        return Value();
    }
    
    Set* set = static_cast<Set*>(obj);
    Value value = args.empty() ? Value() : args[0];
    
    set->add(value);
    return Value(obj);
}

Value Set::set_has(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Set.prototype.has called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value(std::string("Set.prototype.has called on non-Set")));
        return Value();
    }
    
    Set* set = static_cast<Set*>(obj);
    Value value = args.empty() ? Value() : args[0];
    
    return Value(set->has(value));
}

Value Set::set_delete(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Set.prototype.delete called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value(std::string("Set.prototype.delete called on non-Set")));
        return Value();
    }
    
    Set* set = static_cast<Set*>(obj);
    Value value = args.empty() ? Value() : args[0];
    
    return Value(set->delete_value(value));
}

Value Set::set_clear(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Set.prototype.clear called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value(std::string("Set.prototype.clear called on non-Set")));
        return Value();
    }
    
    Set* set = static_cast<Set*>(obj);
    set->clear();
    return Value();
}

Value Set::set_size_getter(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Set.prototype.size called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value(std::string("Set.prototype.size called on non-Set")));
        return Value();
    }
    
    Set* set = static_cast<Set*>(obj);
    return Value(static_cast<double>(set->size()));
}

Value Set::set_iterator_method(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_exception(Value(std::string("Set.prototype[Symbol.iterator] called on non-object")));
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_exception(Value(std::string("Set.prototype[Symbol.iterator] called on non-Set")));
        return Value();
    }
    
    Set* set = static_cast<Set*>(obj);
    auto iterator = std::make_unique<SetIterator>(set, SetIterator::Kind::Values);
    return Value(iterator.release());
}

void Set::setup_set_prototype(Context& ctx) {
    auto set_constructor_fn = ObjectFactory::create_native_constructor("Set", set_constructor, 0);
    
    auto set_prototype = ObjectFactory::create_object();
    
    auto add_fn = ObjectFactory::create_native_function("add", set_add);
    auto has_fn = ObjectFactory::create_native_function("has", set_has);
    auto delete_fn = ObjectFactory::create_native_function("delete", set_delete);
    auto clear_fn = ObjectFactory::create_native_function("clear", set_clear);
    auto size_fn = ObjectFactory::create_native_function("size", set_size_getter);
    
    set_prototype->set_property("add", Value(add_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    set_prototype->set_property("has", Value(has_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    set_prototype->set_property("delete", Value(delete_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    set_prototype->set_property("clear", Value(clear_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    set_prototype->set_property("size", Value(size_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    
    // forEach method
    auto forEach_fn = ObjectFactory::create_native_function("forEach",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) {
                ctx.throw_type_error("Set.prototype.forEach called on non-Set");
                return Value();
            }
            if (args.empty() || !args[0].is_function()) {
                ctx.throw_type_error("forEach requires a callback function");
                return Value();
            }
            Set* set = static_cast<Set*>(obj);
            Function* callback = args[0].as_function();
            Value this_arg = args.size() > 1 ? args[1] : Value();
            auto vals = set->values();
            for (const auto& val : vals) {
                std::vector<Value> cb_args = {val, val, Value(obj)};
                callback->call(ctx, cb_args, this_arg);
                if (ctx.has_exception()) return Value();
            }
            return Value();
        }, 1);
    set_prototype->set_property("forEach", Value(forEach_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // values method
    auto values_fn = ObjectFactory::create_native_function("values",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) {
                ctx.throw_type_error("Set.prototype.values called on non-Set");
                return Value();
            }
            Set* set = static_cast<Set*>(obj);
            auto iterator = std::make_unique<SetIterator>(set, SetIterator::Kind::Values);
            return Value(iterator.release());
        }, 0);
    set_prototype->set_property("values", Value(values_fn.get()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // keys is same as values for Set
    set_prototype->set_property("keys", Value(values_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    // entries method
    auto entries_fn = ObjectFactory::create_native_function("entries",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args;
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) {
                ctx.throw_type_error("Set.prototype.entries called on non-Set");
                return Value();
            }
            Set* set = static_cast<Set*>(obj);
            auto iterator = std::make_unique<SetIterator>(set, SetIterator::Kind::Entries);
            return Value(iterator.release());
        }, 0);
    set_prototype->set_property("entries", Value(entries_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto set_iterator_fn = ObjectFactory::create_native_function("@@iterator", set_iterator_method);
        set_prototype->set_property(iterator_symbol->to_string(), Value(set_iterator_fn.release()));
    }

    PropertyDescriptor set_tag_desc(Value(std::string("Set")), PropertyAttributes::Configurable);
    set_prototype->set_property_descriptor("Symbol.toStringTag", set_tag_desc);

    Set::prototype_object = set_prototype.get();

    set_constructor_fn->set_property("prototype", Value(set_prototype.release()));
    ctx.create_binding("Set", Value(set_constructor_fn.release()));
}


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
    return Value();
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

void WeakMap::setup_weakmap_prototype(Context& ctx) {
    auto weakmap_constructor_fn = ObjectFactory::create_native_constructor("WeakMap", weakmap_constructor, 0);
    
    auto weakmap_prototype = ObjectFactory::create_object();
    
    auto set_fn = ObjectFactory::create_native_function("set", weakmap_set);
    auto get_fn = ObjectFactory::create_native_function("get", weakmap_get);
    auto has_fn = ObjectFactory::create_native_function("has", weakmap_has);
    auto delete_fn = ObjectFactory::create_native_function("delete", weakmap_delete);
    
    weakmap_prototype->set_property("set", Value(set_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakmap_prototype->set_property("get", Value(get_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakmap_prototype->set_property("has", Value(has_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakmap_prototype->set_property("delete", Value(delete_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    
    WeakMap::prototype_object = weakmap_prototype.get();
    
    weakmap_constructor_fn->set_property("prototype", Value(weakmap_prototype.release()));
    ctx.create_binding("WeakMap", Value(weakmap_constructor_fn.release()));
}


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

void WeakSet::setup_weakset_prototype(Context& ctx) {
    auto weakset_constructor_fn = ObjectFactory::create_native_constructor("WeakSet", weakset_constructor, 0);
    
    auto weakset_prototype = ObjectFactory::create_object();
    
    auto add_fn = ObjectFactory::create_native_function("add", weakset_add);
    auto has_fn = ObjectFactory::create_native_function("has", weakset_has);
    auto delete_fn = ObjectFactory::create_native_function("delete", weakset_delete);
    
    weakset_prototype->set_property("add", Value(add_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakset_prototype->set_property("has", Value(has_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakset_prototype->set_property("delete", Value(delete_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    
    WeakSet::prototype_object = weakset_prototype.get();
    
    weakset_constructor_fn->set_property("prototype", Value(weakset_prototype.release()));
    ctx.create_binding("WeakSet", Value(weakset_constructor_fn.release()));
}

Value WeakMap::weakmap_constructor(Context& ctx, const std::vector<Value>& args) {
    if (!ctx.is_in_constructor_call()) {
        ctx.throw_type_error("Constructor WeakMap requires 'new'");
        return Value();
    }
    auto weakmap = std::make_unique<WeakMap>();

    if (WeakMap::prototype_object) {
        weakmap->set_prototype(WeakMap::prototype_object);
    }

    WeakMap* wm_ptr = weakmap.get();
    Object* wm_obj = weakmap.release();

    if (!args.empty() && args[0].is_object()) {
        Object* iterable = args[0].as_object();
        Value set_method = wm_obj->get_property("set");
        Function* set_fn = set_method.is_function() ? set_method.as_function() : nullptr;

        if (iterable->is_array()) {
            uint32_t length = iterable->get_length();
            for (uint32_t i = 0; i < length; i++) {
                Value entry = iterable->get_element(i);
                if (entry.is_object() && entry.as_object()->is_array()) {
                    Object* pair = entry.as_object();
                    if (pair->get_length() >= 2) {
                        Value key = pair->get_element(0);
                        Value value = pair->get_element(1);
                        if (set_fn) {
                            set_fn->call(ctx, {key, value}, Value(wm_obj));
                        }
                    }
                }
            }
        }
    }

    return Value(wm_obj);
}

Value WeakMap::weakmap_set(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_exception(Value(std::string("WeakMap.prototype.set requires 2 arguments")));
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value(std::string("WeakMap.prototype.set called on non-object")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_exception(Value(std::string("WeakMap.prototype.set called on non-WeakMap")));
        return Value();
    }
    
    WeakMap* weakmap = static_cast<WeakMap*>(this_obj);
    
    if (args[0].is_object()) {
        Object* key = args[0].as_object();
        weakmap->set(key, args[1]);
        return Value(this_obj);
    } else {
        ctx.throw_exception(Value(std::string("WeakMap key must be an object")));
        return Value();
    }
}

Value WeakMap::weakmap_get(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value(std::string("WeakMap.prototype.get called on non-object")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_exception(Value(std::string("WeakMap.prototype.get called on non-WeakMap")));
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
        ctx.throw_exception(Value(std::string("WeakMap.prototype.has called on non-object")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_exception(Value(std::string("WeakMap.prototype.has called on non-WeakMap")));
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
        ctx.throw_exception(Value(std::string("WeakMap.prototype.delete called on non-object")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_exception(Value(std::string("WeakMap.prototype.delete called on non-WeakMap")));
        return Value();
    }
    
    WeakMap* weakmap = static_cast<WeakMap*>(this_obj);
    
    if (args[0].is_object()) {
        Object* key = args[0].as_object();
        return Value(weakmap->delete_key(key));
    }
    
    return Value(false);
}

Value WeakSet::weakset_constructor(Context& ctx, const std::vector<Value>& args) {
    if (!ctx.is_in_constructor_call()) {
        ctx.throw_type_error("Constructor WeakSet requires 'new'");
        return Value();
    }
    auto weakset = std::make_unique<WeakSet>();

    if (WeakSet::prototype_object) {
        weakset->set_prototype(WeakSet::prototype_object);
    }

    WeakSet* ws_ptr = weakset.get();
    Object* ws_obj = weakset.release();

    if (!args.empty() && args[0].is_object()) {
        Object* iterable = args[0].as_object();
        Value add_method = ws_obj->get_property("add");
        Function* add_fn = add_method.is_function() ? add_method.as_function() : nullptr;

        if (iterable->is_array()) {
            uint32_t length = iterable->get_length();
            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                if (add_fn) {
                    add_fn->call(ctx, {element}, Value(ws_obj));
                }
            }
        }
    }

    return Value(ws_obj);
}

Value WeakSet::weakset_add(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_exception(Value(std::string("WeakSet.prototype.add requires an argument")));
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value(std::string("WeakSet.prototype.add called on non-object")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakSet) {
        ctx.throw_exception(Value(std::string("WeakSet.prototype.add called on non-WeakSet")));
        return Value();
    }
    
    WeakSet* weakset = static_cast<WeakSet*>(this_obj);
    
    if (args[0].is_object()) {
        Object* value = args[0].as_object();
        weakset->add(value);
        return Value(this_obj);
    } else {
        ctx.throw_exception(Value(std::string("WeakSet value must be an object")));
        return Value();
    }
}

Value WeakSet::weakset_has(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(false);
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_exception(Value(std::string("WeakSet.prototype.has called on non-object")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakSet) {
        ctx.throw_exception(Value(std::string("WeakSet.prototype.has called on non-WeakSet")));
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
        ctx.throw_exception(Value(std::string("WeakSet.prototype.delete called on non-object")));
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakSet) {
        ctx.throw_exception(Value(std::string("WeakSet.prototype.delete called on non-WeakSet")));
        return Value();
    }
    
    WeakSet* weakset = static_cast<WeakSet*>(this_obj);
    
    if (args[0].is_object()) {
        Object* value = args[0].as_object();
        return Value(weakset->delete_value(value));
    }
    
    return Value(false);
}

}
