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

static void close_iterator(Object* iter_obj, Context& ctx) {
    Value ret_fn = iter_obj->get_property("return");
    if (ret_fn.is_function()) ret_fn.as_function()->call(ctx, {}, Value(iter_obj));
}

static bool iterate_with_closing(Context& ctx, const Value& iterable_val, Object* iterable,
    const std::function<bool(const Value&, Object*)>& process) {
    Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
    if (!iter_sym) return false;
    Value iter_method = iterable->get_property(iter_sym->to_property_key());
    if (!iter_method.is_function()) {
        ctx.throw_type_error("object is not iterable (Symbol.iterator is not callable)");
        return false;
    }
    Value iter_obj = iter_method.as_function()->call(ctx, {}, iterable_val);
    if (ctx.has_exception() || !iter_obj.is_object()) return false;
    Value next_fn = iter_obj.as_object()->get_property("next");
    if (!next_fn.is_function()) return false;
    while (true) {
        Value res = next_fn.as_function()->call(ctx, {}, iter_obj);
        if (ctx.has_exception()) break;
        if (!res.is_object()) break;
        if (res.as_object()->get_property("done").to_boolean()) break;
        Value val = res.as_object()->get_property("value");
        if (!process(val, iter_obj.as_object())) break;
    }
    return true;
}

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
            // SameValueZero: NaN equals NaN, +0 equals -0
            if (key.is_number() && entry.key.is_number()) {
                if (std::isnan(key.as_number()) && std::isnan(entry.key.as_number())) return true;
            }
            return entry.key.strict_equals(key);
        });
}

std::vector<Map::MapEntry>::const_iterator Map::find_entry(const Value& key) const {
    return std::find_if(entries_.begin(), entries_.end(), 
        [&key](const MapEntry& entry) {
            // SameValueZero: NaN equals NaN, +0 equals -0
            if (key.is_number() && entry.key.is_number()) {
                if (std::isnan(key.as_number()) && std::isnan(entry.key.as_number())) return true;
            }
            return entry.key.strict_equals(key);
        });
}

Value Map::map_constructor(Context& ctx, const std::vector<Value>& args) {
    if (!ctx.is_in_constructor_call()) {
        ctx.throw_type_error("Constructor Map requires 'new'");
        return Value();
    }
    auto map = std::make_unique<Map>();

    // ES6 subclassing: use new.target.prototype if provided
    Value new_target = ctx.get_new_target();
    if (new_target.is_function()) {
        Value nt_proto = new_target.as_function()->get_property("prototype");
        if (nt_proto.is_object()) {
            map->set_prototype(nt_proto.as_object());
        } else if (Map::prototype_object) {
            map->set_prototype(Map::prototype_object);
        }
    } else if (Map::prototype_object) {
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
            bool handled = iterate_with_closing(ctx, args[0], iterable,
                [&](const Value& entry, Object* iter) -> bool {
                    if (!entry.is_object() || !entry.as_object()->is_array()) {
                        close_iterator(iter, ctx);
                        ctx.throw_type_error("Iterator value is not a [key, value] pair");
                        return false;
                    }
                    Object* pair = entry.as_object();
                    if (pair->get_length() >= 2) {
                        Value key = pair->get_element(0);
                        Value value = pair->get_element(1);
                        if (set_fn) {
                            set_fn->call(ctx, {key, value}, Value(map_obj));
                            if (ctx.has_exception()) { close_iterator(iter, ctx); return false; }
                        } else {
                            map_ptr->set(key, value);
                        }
                    }
                    return true;
                });
            if (!handled && !ctx.has_exception()) {
                auto iterator = IterableUtils::get_iterator(args[0], ctx);
                if (iterator) {
                    while (true) {
                        auto result = iterator->next();
                        if (result.done) break;
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
                        }
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
        ctx.throw_type_error("Map.prototype.set called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_type_error("Map.prototype.set called on non-Map");
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
        ctx.throw_type_error("Map.prototype.get called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_type_error("Map.prototype.get called on non-Map");
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];
    
    return map->get(key);
}

Value Map::map_has(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_type_error("Map.prototype.has called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_type_error("Map.prototype.has called on non-Map");
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    Value key = args.empty() ? Value() : args[0];
    
    return Value(map->has(key));
}

Value Map::map_delete(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_type_error("Map.prototype.delete called on non-object");
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_type_error("Map.prototype.delete called on non-Map");
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
        ctx.throw_type_error("Map.prototype.clear called on non-object");
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_type_error("Map.prototype.clear called on non-Map");
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
        ctx.throw_type_error("Map.prototype.size called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_type_error("Map.prototype.size called on non-Map");
        return Value();
    }
    
    Map* map = static_cast<Map*>(obj);
    return Value(static_cast<double>(map->size()));
}

Value Map::map_iterator_method(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_type_error("Map.prototype[Symbol.iterator] called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Map) {
        ctx.throw_type_error("Map.prototype[Symbol.iterator] called on non-Map");
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
    auto get_fn = ObjectFactory::create_native_function("get", map_get, 1);
    auto has_fn = ObjectFactory::create_native_function("has", map_has, 1);
    auto delete_fn = ObjectFactory::create_native_function("delete", map_delete, 1);
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
    {
        // Map.prototype.size is an accessor (getter only) per spec.
        PropertyDescriptor size_desc;
        size_desc.set_getter(size_fn.release());
        size_desc.set_enumerable(false);
        size_desc.set_configurable(true);
        map_prototype->set_property_descriptor("size", size_desc);
    }

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
        map_prototype->set_property(iterator_symbol->to_property_key(), Value(entries_fn.release()),
            static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    } else {
        entries_fn.release();
    }

    auto map_getOrInsert_fn = ObjectFactory::create_native_function("getOrInsert",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Map) { ctx.throw_type_error("Map.prototype.getOrInsert"); return Value(); }
            Map* m = static_cast<Map*>(obj);
            Value key = args.empty() ? Value() : args[0];
            Value val = args.size() > 1 ? args[1] : Value();
            if (m->has(key)) return m->get(key);
            m->set(key, val);
            return val;
        }, 2);
    map_prototype->set_property("getOrInsert", Value(map_getOrInsert_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto map_getOrInsertComputed_fn = ObjectFactory::create_native_function("getOrInsertComputed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Map) { ctx.throw_type_error("Map.prototype.getOrInsertComputed"); return Value(); }
            if (args.size() < 2 || !args[1].is_function()) { ctx.throw_type_error("callbackFn is not a function"); return Value(); }
            Map* m = static_cast<Map*>(obj);
            Value key = args.empty() ? Value() : args[0];
            if (m->has(key)) return m->get(key);
            Value val = args[1].as_function()->call(ctx, {key}, Value());
            if (ctx.has_exception()) return Value();
            m->set(key, val);
            return val;
        }, 2);
    map_prototype->set_property("getOrInsertComputed", Value(map_getOrInsertComputed_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

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
            if (Map::prototype_object) result_map->set_prototype(Map::prototype_object);
            // Use iterator if available, fall back to array-like length/element access.
            uint32_t length = iterable->get_length();

            for (uint32_t i = 0; i < length; i++) {
                Value element = iterable->get_element(i);
                std::vector<Value> callback_args = { element, Value(static_cast<double>(i)) };
                Value key = callback->call(ctx, callback_args);
                if (ctx.has_exception()) return Value();

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

    // Symbol.species getter: Map[Symbol.species] === Map
    {
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* self = ctx.get_this_binding();
                    return self ? Value(self) : Value();
                });
            PropertyDescriptor species_desc;
            species_desc.set_getter(species_getter.release());
            species_desc.set_enumerable(false);
            species_desc.set_configurable(true);
            map_constructor_fn->set_property_descriptor(species_sym->to_property_key(), species_desc);
        }
    }

    Object* map_proto_ptr = map_prototype.get();
    map_constructor_fn->set_property("prototype", Value(map_prototype.release()), PropertyAttributes::None);
    map_proto_ptr->set_property("constructor", Value(map_constructor_fn.get()), PropertyAttributes::BuiltinFunction);
    ctx.register_built_in_object("Map", map_constructor_fn.release());
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
            if (value.is_number() && v.is_number() && std::isnan(value.as_number()) && std::isnan(v.as_number())) return true;
            return v.strict_equals(value);
        });
}

std::vector<Value>::const_iterator Set::find_value(const Value& value) const {
    return std::find_if(values_.begin(), values_.end(),
        [&value](const Value& v) {
            if (value.is_number() && v.is_number() && std::isnan(value.as_number()) && std::isnan(v.as_number())) return true;
            return v.strict_equals(value);
        });
}

Value Set::set_constructor(Context& ctx, const std::vector<Value>& args) {
    if (!ctx.is_in_constructor_call()) {
        ctx.throw_type_error("Constructor Set requires 'new'");
        return Value();
    }
    auto set = std::make_unique<Set>();

    // ES6 subclassing: use new.target.prototype if provided
    Value new_target_s = ctx.get_new_target();
    if (new_target_s.is_function()) {
        Value nt_proto = new_target_s.as_function()->get_property("prototype");
        if (nt_proto.is_object()) {
            set->set_prototype(nt_proto.as_object());
        } else if (Set::prototype_object) {
            set->set_prototype(Set::prototype_object);
        }
    } else if (Set::prototype_object) {
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
            bool handled = iterate_with_closing(ctx, args[0], iterable,
                [&](const Value& val, Object* iter) -> bool {
                    if (add_fn) {
                        add_fn->call(ctx, {val}, Value(set_obj));
                        if (ctx.has_exception()) { close_iterator(iter, ctx); return false; }
                    } else {
                        set_ptr->add(val);
                    }
                    return true;
                });
            if (!handled && !ctx.has_exception()) {
                auto iterator = IterableUtils::get_iterator(args[0], ctx);
                if (iterator) {
                    while (true) {
                        auto result = iterator->next();
                        if (result.done) break;
                        if (add_fn) {
                            add_fn->call(ctx, {result.value}, Value(set_obj));
                        } else {
                            set_ptr->add(result.value);
                        }
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
        ctx.throw_type_error("Set.prototype.add called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_type_error("Set.prototype.add called on non-Set");
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
        ctx.throw_type_error("Set.prototype.has called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_type_error("Set.prototype.has called on non-Set");
        return Value();
    }
    
    Set* set = static_cast<Set*>(obj);
    Value value = args.empty() ? Value() : args[0];
    
    return Value(set->has(value));
}

Value Set::set_delete(Context& ctx, const std::vector<Value>& args) {
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_type_error("Set.prototype.delete called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_type_error("Set.prototype.delete called on non-Set");
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
        ctx.throw_type_error("Set.prototype.clear called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_type_error("Set.prototype.clear called on non-Set");
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
        ctx.throw_type_error("Set.prototype.size called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_type_error("Set.prototype.size called on non-Set");
        return Value();
    }
    
    Set* set = static_cast<Set*>(obj);
    return Value(static_cast<double>(set->size()));
}

Value Set::set_iterator_method(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    
    Object* obj = ctx.get_this_binding();
    if (!obj) {
        ctx.throw_type_error("Set.prototype[Symbol.iterator] called on non-object");
        return Value();
    }
    if (obj->get_type() != Object::ObjectType::Set) {
        ctx.throw_type_error("Set.prototype[Symbol.iterator] called on non-Set");
        return Value();
    }
    
    Set* set = static_cast<Set*>(obj);
    auto iterator = std::make_unique<SetIterator>(set, SetIterator::Kind::Values);
    return Value(iterator.release());
}

void Set::setup_set_prototype(Context& ctx) {
    auto set_constructor_fn = ObjectFactory::create_native_constructor("Set", set_constructor, 0);
    
    auto set_prototype = ObjectFactory::create_object();
    
    auto add_fn = ObjectFactory::create_native_function("add", set_add, 1);
    auto has_fn = ObjectFactory::create_native_function("has", set_has, 1);
    auto delete_fn = ObjectFactory::create_native_function("delete", set_delete, 1);
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
    {
        // Set.prototype.size is an accessor (getter only) per spec.
        PropertyDescriptor size_desc;
        size_desc.set_getter(size_fn.release());
        size_desc.set_enumerable(false);
        size_desc.set_configurable(true);
        set_prototype->set_property_descriptor("size", size_desc);
    }
    
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
        set_prototype->set_property(iterator_symbol->to_property_key(), Value(set_iterator_fn.release()));
    }

    // ES2024 Set methods — accept "set-like" objects (anything with .has, .size, .keys)
    // call_has(ctx, other, has_fn, v): invoke has_fn.call(other, v) and return boolean
    auto call_has = [](Context& ctx, Object* other, Value has_fn, const Value& v) -> bool {
        if (!has_fn.is_function()) return false;
        Value r = has_fn.as_function()->call(ctx, {v}, Value(other));
        return !ctx.has_exception() && r.to_boolean();
    };
    // iterate_keys(ctx, other): iterate other.keys() or fallback Symbol.iterator
    auto iterate_keys = [](Context& ctx, Object* other) -> std::vector<Value> {
        std::vector<Value> result;
        if (other->get_type() == Object::ObjectType::Set) {
            return static_cast<Set*>(other)->values();
        }
        Value keys_fn = other->get_property("keys");
        if (!keys_fn.is_function()) return result;
        Value iter = keys_fn.as_function()->call(ctx, {}, Value(other));
        if (ctx.has_exception() || !iter.is_object()) return result;
        Object* it = iter.as_object();
        for (int i = 0; i < 1000000; i++) {
            Value next_fn = it->get_property("next");
            if (!next_fn.is_function()) break;
            Value res = next_fn.as_function()->call(ctx, {}, iter);
            if (ctx.has_exception() || !res.is_object()) break;
            if (res.as_object()->get_property("done").to_boolean()) break;
            result.push_back(res.as_object()->get_property("value"));
        }
        return result;
    };
    // Validate a "set-like" argument per GetSetRecord spec: must have callable has, keys, and numeric size.
    auto validate_set_like = [](Context& ctx, Object* other) -> bool {
        Value has_fn = other->get_property("has");
        if (ctx.has_exception()) return false;
        if (!has_fn.is_function()) {
            ctx.throw_type_error("GetSetRecord: has is not callable");
            return false;
        }
        Value keys_fn = other->get_property("keys");
        if (ctx.has_exception()) return false;
        if (!keys_fn.is_function()) {
            ctx.throw_type_error("GetSetRecord: keys is not callable");
            return false;
        }
        Value size_val = other->get_property("size");
        if (ctx.has_exception()) return false;
        if (size_val.is_symbol() || size_val.is_bigint()) {
            ctx.throw_type_error("GetSetRecord: size cannot be a Symbol or BigInt");
            return false;
        }
        double num_size = size_val.to_number();
        if (ctx.has_exception()) return false;
        if (std::isnan(num_size)) {
            ctx.throw_type_error("GetSetRecord: size is NaN");
            return false;
        }
        return true;
    };
    // get_set_like_has: get the has function from a set-like arg, throw TypeError if invalid
    auto get_set_like = [](Context& ctx, const Value& v) -> std::vector<Value> {
        std::vector<Value> result;
        if (v.is_object() && v.as_object()->get_type() == Object::ObjectType::Set) {
            return static_cast<Set*>(v.as_object())->values();
        }
        return result;
    };

    auto union_fn = ObjectFactory::create_native_function("union",
        [validate_set_like, iterate_keys](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) { ctx.throw_type_error("Set.prototype.union"); return Value(); }
            Set* self = static_cast<Set*>(obj);
            if (args.empty() || !args[0].is_object()) { ctx.throw_type_error("Set.prototype.union requires a set-like"); return Value(); }
            if (!validate_set_like(ctx, args[0].as_object())) return Value();
            auto result = std::make_unique<Set>();
            for (const auto& v : self->values()) result->add(v);
            for (const auto& v : iterate_keys(ctx, args[0].as_object())) {
                if (ctx.has_exception()) return Value();
                result->add(v);
            }
            if (ctx.has_exception()) return Value();
            if (Set::prototype_object) result->set_prototype(Set::prototype_object);
            return Value(result.release());
        }, 1);
    set_prototype->set_property("union", Value(union_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto intersection_fn = ObjectFactory::create_native_function("intersection",
        [call_has, iterate_keys, validate_set_like](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) { ctx.throw_type_error("Set.prototype.intersection"); return Value(); }
            Set* self = static_cast<Set*>(obj);
            if (args.empty() || !args[0].is_object()) { ctx.throw_type_error("Set.prototype.intersection requires a set-like"); return Value(); }
            if (!validate_set_like(ctx, args[0].as_object())) return Value();
            Object* other = args[0].as_object();
            Value other_size_val = other->get_property("size");
            if (ctx.has_exception()) return Value();
            double other_size = other_size_val.to_number();
            auto result = std::make_unique<Set>();
            if ((double)self->size() <= other_size) {
                // Iterate this, call other.has() for each element.
                Value has_fn = other->get_property("has");
                if (ctx.has_exception()) return Value();
                for (const auto& v : self->values()) {
                    if (call_has(ctx, other, has_fn, v)) result->add(v);
                    if (ctx.has_exception()) return Value();
                }
            } else {
                // this.size > other.size: iterate other.keys(); normalize -0→+0 (spec step 8.b.i), add if in this.
                for (auto k : iterate_keys(ctx, other)) {
                    if (ctx.has_exception()) return Value();
                    if (k.is_number() && k.as_number() == 0.0 && std::signbit(k.as_number())) k = Value(0.0);
                    if (self->has(k)) result->add(k);
                }
            }
            if (ctx.has_exception()) return Value();
            if (Set::prototype_object) result->set_prototype(Set::prototype_object);
            return Value(result.release());
        }, 1);
    set_prototype->set_property("intersection", Value(intersection_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto difference_fn = ObjectFactory::create_native_function("difference",
        [call_has, iterate_keys, validate_set_like](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) { ctx.throw_type_error("Set.prototype.difference"); return Value(); }
            Set* self = static_cast<Set*>(obj);
            if (args.empty() || !args[0].is_object()) { ctx.throw_type_error("Set.prototype.difference requires a set-like"); return Value(); }
            if (!validate_set_like(ctx, args[0].as_object())) return Value();
            Object* other = args[0].as_object();
            Value other_size_val = other->get_property("size");
            if (ctx.has_exception()) return Value();
            double other_size = other_size_val.to_number();
            auto result = std::make_unique<Set>();
            if ((double)self->size() <= other_size) {
                // this.size <= other.size: iterate this, call other.has() for each element.
                Value has_fn = other->get_property("has");
                if (ctx.has_exception()) return Value();
                for (const auto& v : self->values()) {
                    if (!call_has(ctx, other, has_fn, v)) result->add(v);
                    if (ctx.has_exception()) return Value();
                }
            } else {
                // this.size > other.size: copy this, remove elements from other.keys().
                for (const auto& v : self->values()) result->add(v);
                for (const auto& v : iterate_keys(ctx, other)) {
                    if (ctx.has_exception()) return Value();
                    result->delete_value(v);
                }
            }
            if (ctx.has_exception()) return Value();
            if (Set::prototype_object) result->set_prototype(Set::prototype_object);
            return Value(result.release());
        }, 1);
    set_prototype->set_property("difference", Value(difference_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto symmetricDifference_fn = ObjectFactory::create_native_function("symmetricDifference",
        [iterate_keys, validate_set_like](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) { ctx.throw_type_error("Set.prototype.symmetricDifference"); return Value(); }
            Set* self = static_cast<Set*>(obj);
            if (args.empty() || !args[0].is_object()) { ctx.throw_type_error("Set.prototype.symmetricDifference requires a set-like"); return Value(); }
            if (!validate_set_like(ctx, args[0].as_object())) return Value();
            Object* other = args[0].as_object();
            // Spec: start with copy of this, then for each key in other: toggle membership (no other.has() calls).
            auto result = std::make_unique<Set>();
            for (const auto& v : self->values()) result->add(v);
            for (const auto& v : iterate_keys(ctx, other)) {
                if (ctx.has_exception()) return Value();
                if (result->has(v)) result->delete_value(v);
                else result->add(v);
            }
            if (ctx.has_exception()) return Value();
            if (Set::prototype_object) result->set_prototype(Set::prototype_object);
            return Value(result.release());
        }, 1);
    set_prototype->set_property("symmetricDifference", Value(symmetricDifference_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto isSubsetOf_fn = ObjectFactory::create_native_function("isSubsetOf",
        [call_has, validate_set_like](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) { ctx.throw_type_error("Set.prototype.isSubsetOf"); return Value(); }
            Set* self = static_cast<Set*>(obj);
            if (args.empty() || !args[0].is_object()) { ctx.throw_type_error("Set.prototype.isSubsetOf requires a set-like"); return Value(); }
            if (!validate_set_like(ctx, args[0].as_object())) return Value();
            Object* other = args[0].as_object();
            if (other->get_type() != Object::ObjectType::Set) {
                if (!other->get_property("has").is_function()) { ctx.throw_type_error("GetSetRecord: has is not callable"); return Value(); }
                if (!other->get_property("keys").is_function()) { ctx.throw_type_error("GetSetRecord: keys is not callable"); return Value(); }
            }
            if (other->get_type() == Object::ObjectType::Set) {
                Set* os = static_cast<Set*>(other);
                for (const auto& v : self->values()) if (!os->has(v)) return Value(false);
            } else {
                Value has_fn = other->get_property("has");
                for (const auto& v : self->values()) {
                    if (!call_has(ctx, other, has_fn, v)) return Value(false);
                    if (ctx.has_exception()) return Value();
                }
            }
            return Value(true);
        }, 1);
    set_prototype->set_property("isSubsetOf", Value(isSubsetOf_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto isSupersetOf_fn = ObjectFactory::create_native_function("isSupersetOf",
        [call_has, iterate_keys, validate_set_like](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) { ctx.throw_type_error("Set.prototype.isSupersetOf"); return Value(); }
            Set* self = static_cast<Set*>(obj);
            if (args.empty() || !args[0].is_object()) { ctx.throw_type_error("Set.prototype.isSupersetOf requires a set-like"); return Value(); }
            if (!validate_set_like(ctx, args[0].as_object())) return Value();
            Object* other = args[0].as_object();
            if (other->get_type() != Object::ObjectType::Set) {
                if (!other->get_property("has").is_function()) { ctx.throw_type_error("GetSetRecord: has is not callable"); return Value(); }
                if (!other->get_property("keys").is_function()) { ctx.throw_type_error("GetSetRecord: keys is not callable"); return Value(); }
            }
            Value self_has = obj->get_property("has");
            if (other->get_type() == Object::ObjectType::Set) {
                for (const auto& v : static_cast<Set*>(other)->values()) if (!self->has(v)) return Value(false);
            } else {
                for (const auto& v : iterate_keys(ctx, other)) {
                    if (!call_has(ctx, obj, self_has, v)) return Value(false);
                    if (ctx.has_exception()) return Value();
                }
            }
            return Value(true);
        }, 1);
    set_prototype->set_property("isSupersetOf", Value(isSupersetOf_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto isDisjointFrom_fn = ObjectFactory::create_native_function("isDisjointFrom",
        [call_has, validate_set_like](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::Set) { ctx.throw_type_error("Set.prototype.isDisjointFrom"); return Value(); }
            Set* self = static_cast<Set*>(obj);
            if (args.empty() || !args[0].is_object()) { ctx.throw_type_error("Set.prototype.isDisjointFrom requires a set-like"); return Value(); }
            if (!validate_set_like(ctx, args[0].as_object())) return Value();
            Object* other = args[0].as_object();
            if (other->get_type() != Object::ObjectType::Set) {
                if (!other->get_property("has").is_function()) { ctx.throw_type_error("GetSetRecord: has is not callable"); return Value(); }
                if (!other->get_property("keys").is_function()) { ctx.throw_type_error("GetSetRecord: keys is not callable"); return Value(); }
            }
            if (other->get_type() == Object::ObjectType::Set) {
                Set* os = static_cast<Set*>(other);
                for (const auto& v : self->values()) if (os->has(v)) return Value(false);
            } else {
                Value has_fn = other->get_property("has");
                for (const auto& v : self->values()) {
                    if (call_has(ctx, other, has_fn, v)) return Value(false);
                    if (ctx.has_exception()) return Value();
                }
            }
            return Value(true);
        }, 1);
    set_prototype->set_property("isDisjointFrom", Value(isDisjointFrom_fn.release()), static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    PropertyDescriptor set_tag_desc(Value(std::string("Set")), PropertyAttributes::Configurable);
    set_prototype->set_property_descriptor("Symbol.toStringTag", set_tag_desc);

    Set::prototype_object = set_prototype.get();

    // Symbol.species getter: Set[Symbol.species] === Set
    {
        Symbol* species_sym = Symbol::get_well_known(Symbol::SPECIES);
        if (species_sym) {
            auto species_getter = ObjectFactory::create_native_function("get [Symbol.species]",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)args;
                    Object* self = ctx.get_this_binding();
                    return self ? Value(self) : Value();
                });
            PropertyDescriptor species_desc;
            species_desc.set_getter(species_getter.release());
            species_desc.set_enumerable(false);
            species_desc.set_configurable(true);
            set_constructor_fn->set_property_descriptor(species_sym->to_property_key(), species_desc);
        }
    }

    Object* set_proto_ptr = set_prototype.get();
    set_constructor_fn->set_property("prototype", Value(set_prototype.release()), PropertyAttributes::None);
    set_proto_ptr->set_property("constructor", Value(set_constructor_fn.get()), PropertyAttributes::BuiltinFunction);
    ctx.register_built_in_object("Set", set_constructor_fn.release());
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
    auto get_fn = ObjectFactory::create_native_function("get", weakmap_get, 1);
    auto has_fn = ObjectFactory::create_native_function("has", weakmap_has, 1);
    auto delete_fn = ObjectFactory::create_native_function("delete", weakmap_delete, 1);
    
    weakmap_prototype->set_property("set", Value(set_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakmap_prototype->set_property("get", Value(get_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakmap_prototype->set_property("has", Value(has_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakmap_prototype->set_property("delete", Value(delete_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    
    auto wm_getOrInsert_fn = ObjectFactory::create_native_function("getOrInsert",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::WeakMap) { ctx.throw_type_error("WeakMap.prototype.getOrInsert"); return Value(); }
            WeakMap* wm = static_cast<WeakMap*>(obj);
            if (args.empty() || !args[0].is_object()) { ctx.throw_type_error("WeakMap key must be an object"); return Value(); }
            Object* key = args[0].as_object();
            Value val = args.size() > 1 ? args[1] : Value();
            if (wm->has(key)) return wm->get(key);
            wm->set(key, val);
            return val;
        }, 2);
    weakmap_prototype->set_property("getOrInsert", Value(wm_getOrInsert_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    auto wm_getOrInsertComputed_fn = ObjectFactory::create_native_function("getOrInsertComputed",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* obj = ctx.get_this_binding();
            if (!obj || obj->get_type() != Object::ObjectType::WeakMap) { ctx.throw_type_error("WeakMap.prototype.getOrInsertComputed"); return Value(); }
            if (args.size() < 2 || !args[1].is_function()) { ctx.throw_type_error("callbackFn is not a function"); return Value(); }
            WeakMap* wm = static_cast<WeakMap*>(obj);
            if (args.empty() || !args[0].is_object()) { ctx.throw_type_error("WeakMap key must be an object"); return Value(); }
            Object* key = args[0].as_object();
            if (wm->has(key)) return wm->get(key);
            Value val = args[1].as_function()->call(ctx, {Value(key)}, Value());
            if (ctx.has_exception()) return Value();
            wm->set(key, val);
            return val;
        }, 2);
    weakmap_prototype->set_property("getOrInsertComputed", Value(wm_getOrInsertComputed_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));

    WeakMap::prototype_object = weakmap_prototype.get();

    weakmap_constructor_fn->set_property("prototype", Value(weakmap_prototype.release()), PropertyAttributes::None);
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
    
    auto add_fn = ObjectFactory::create_native_function("add", weakset_add, 1);
    auto has_fn = ObjectFactory::create_native_function("has", weakset_has, 1);
    auto delete_fn = ObjectFactory::create_native_function("delete", weakset_delete, 1);
    
    weakset_prototype->set_property("add", Value(add_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakset_prototype->set_property("has", Value(has_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    weakset_prototype->set_property("delete", Value(delete_fn.release()),
        static_cast<PropertyAttributes>(PropertyAttributes::Writable | PropertyAttributes::Configurable));
    
    WeakSet::prototype_object = weakset_prototype.get();
    
    weakset_constructor_fn->set_property("prototype", Value(weakset_prototype.release()), PropertyAttributes::None);
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
        } else {
            iterate_with_closing(ctx, args[0], iterable,
                [&](const Value& entry, Object* iter) -> bool {
                    if (!entry.is_object() || !entry.as_object()->is_array()) {
                        close_iterator(iter, ctx);
                        ctx.throw_type_error("Iterator value is not a [key, value] pair");
                        return false;
                    }
                    Object* pair = entry.as_object();
                    if (pair->get_length() >= 2) {
                        Value key = pair->get_element(0);
                        Value value = pair->get_element(1);
                        if (set_fn) {
                            set_fn->call(ctx, {key, value}, Value(wm_obj));
                            if (ctx.has_exception()) { close_iterator(iter, ctx); return false; }
                        }
                    }
                    return true;
                });
        }
    }

    return Value(wm_obj);
}

Value WeakMap::weakmap_set(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("WeakMap.prototype.set requires 2 arguments");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_type_error("WeakMap.prototype.set called on non-object");
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_type_error("WeakMap.prototype.set called on non-WeakMap");
        return Value();
    }
    
    WeakMap* weakmap = static_cast<WeakMap*>(this_obj);
    
    if (args[0].is_object()) {
        Object* key = args[0].as_object();
        weakmap->set(key, args[1]);
        return Value(this_obj);
    } else {
        ctx.throw_type_error("WeakMap key must be an object");
        return Value();
    }
}

Value WeakMap::weakmap_get(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_type_error("WeakMap.prototype.get called on non-object");
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_type_error("WeakMap.prototype.get called on non-WeakMap");
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
        ctx.throw_type_error("WeakMap.prototype.has called on non-object");
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_type_error("WeakMap.prototype.has called on non-WeakMap");
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
        ctx.throw_type_error("WeakMap.prototype.delete called on non-object");
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakMap) {
        ctx.throw_type_error("WeakMap.prototype.delete called on non-WeakMap");
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
        } else {
            iterate_with_closing(ctx, args[0], iterable,
                [&](const Value& val, Object* iter) -> bool {
                    if (add_fn) {
                        add_fn->call(ctx, {val}, Value(ws_obj));
                        if (ctx.has_exception()) { close_iterator(iter, ctx); return false; }
                    }
                    return true;
                });
        }
    }

    return Value(ws_obj);
}

Value WeakSet::weakset_add(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("WeakSet.prototype.add requires an argument");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_type_error("WeakSet.prototype.add called on non-object");
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakSet) {
        ctx.throw_type_error("WeakSet.prototype.add called on non-WeakSet");
        return Value();
    }
    
    WeakSet* weakset = static_cast<WeakSet*>(this_obj);
    
    if (args[0].is_object()) {
        Object* value = args[0].as_object();
        weakset->add(value);
        return Value(this_obj);
    } else {
        ctx.throw_type_error("WeakSet value must be an object");
        return Value();
    }
}

Value WeakSet::weakset_has(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(false);
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj) {
        ctx.throw_type_error("WeakSet.prototype.has called on non-object");
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakSet) {
        ctx.throw_type_error("WeakSet.prototype.has called on non-WeakSet");
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
        ctx.throw_type_error("WeakSet.prototype.delete called on non-object");
        return Value();
    }
    
    if (this_obj->get_type() != Object::ObjectType::WeakSet) {
        ctx.throw_type_error("WeakSet.prototype.delete called on non-WeakSet");
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
