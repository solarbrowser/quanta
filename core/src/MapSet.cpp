#include "MapSet.h"
#include "Context.h"
#include "Symbol.h"
#include "../../parser/include/AST.h"
#include <algorithm>
#include <iostream>

namespace Quanta {

// Initialize static prototype references
Object* Map::prototype_object = nullptr;
Object* Set::prototype_object = nullptr;
Object* WeakMap::prototype_object = nullptr;
Object* WeakSet::prototype_object = nullptr;

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
        // TODO: Implement iteration protocol for proper initialization
        // For now, just create empty map
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
    
    // Store reference for constructor use
    Map::prototype_object = map_prototype.get();
    
    map_constructor_fn->set_property("prototype", Value(map_prototype.release()));
    ctx.create_binding("Map", Value(map_constructor_fn.release()));
}

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
        // TODO: Implement iteration protocol for proper initialization
        // For now, just create empty set
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
    
    // Store reference for constructor use
    Set::prototype_object = set_prototype.get();
    
    set_constructor_fn->set_property("prototype", Value(set_prototype.release()));
    ctx.create_binding("Set", Value(set_constructor_fn.release()));
}

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

// WeakMap static methods
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

// WeakSet static methods
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

} // namespace Quanta