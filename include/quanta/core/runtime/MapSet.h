/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

namespace Quanta {

class Context;

/**
 * JavaScript Map implementation
 * ES6 Map with proper key equality semantics
 */
class Map : public Object {
private:
    struct MapEntry {
        Value key;
        Value value;
        
        MapEntry(const Value& k, const Value& v) : key(k), value(v) {}
    };
    
    std::vector<MapEntry> entries_;
    size_t size_;
    
    struct ValueHash {
        size_t operator()(const Value& v) const {
            return v.hash();
        }
    };
    
    struct ValueEqual {
        bool operator()(const Value& a, const Value& b) const {
            return a.strict_equals(b);
        }
    };
    
public:
    Map();
    virtual ~Map() = default;
    
    bool has(const Value& key) const;
    Value get(const Value& key) const;
    void set(const Value& key, const Value& value);
    bool delete_key(const Value& key);
    void clear();
    
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    
    Value get_property(const std::string& key) const override;
    
    std::vector<Value> keys() const;
    std::vector<Value> values() const;
    std::vector<std::pair<Value, Value>> entries() const;
    
    static Value map_constructor(Context& ctx, const std::vector<Value>& args);
    static Value map_set(Context& ctx, const std::vector<Value>& args);
    static Value map_get(Context& ctx, const std::vector<Value>& args);
    static Value map_has(Context& ctx, const std::vector<Value>& args);
    static Value map_delete(Context& ctx, const std::vector<Value>& args);
    static Value map_clear(Context& ctx, const std::vector<Value>& args);
    static Value map_size_getter(Context& ctx, const std::vector<Value>& args);
    static Value map_keys(Context& ctx, const std::vector<Value>& args);
    static Value map_values(Context& ctx, const std::vector<Value>& args);
    static Value map_entries(Context& ctx, const std::vector<Value>& args);
    static Value map_forEach(Context& ctx, const std::vector<Value>& args);
    static Value map_iterator_method(Context& ctx, const std::vector<Value>& args);
    
    static void setup_map_prototype(Context& ctx);
    
    static Object* prototype_object;
    
private:
    std::vector<MapEntry>::iterator find_entry(const Value& key);
    std::vector<MapEntry>::const_iterator find_entry(const Value& key) const;
};

/**
 * JavaScript Set implementation
 * ES6 Set with proper value equality semantics
 */
class Set : public Object {
private:
    std::vector<Value> values_;
    size_t size_;
    
public:
    Set();
    virtual ~Set() = default;
    
    bool has(const Value& value) const;
    void add(const Value& value);
    bool delete_value(const Value& value);
    void clear();
    
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    
    Value get_property(const std::string& key) const override;
    
    std::vector<Value> values() const;
    std::vector<std::pair<Value, Value>> entries() const;
    
    static Value set_constructor(Context& ctx, const std::vector<Value>& args);
    static Value set_add(Context& ctx, const std::vector<Value>& args);
    static Value set_has(Context& ctx, const std::vector<Value>& args);
    static Value set_delete(Context& ctx, const std::vector<Value>& args);
    static Value set_clear(Context& ctx, const std::vector<Value>& args);
    static Value set_size_getter(Context& ctx, const std::vector<Value>& args);
    static Value set_values(Context& ctx, const std::vector<Value>& args);
    static Value set_keys(Context& ctx, const std::vector<Value>& args);
    static Value set_entries(Context& ctx, const std::vector<Value>& args);
    static Value set_forEach(Context& ctx, const std::vector<Value>& args);
    static Value set_iterator_method(Context& ctx, const std::vector<Value>& args);
    
    static void setup_set_prototype(Context& ctx);
    
    static Object* prototype_object;
    
private:
    std::vector<Value>::iterator find_value(const Value& value);
    std::vector<Value>::const_iterator find_value(const Value& value) const;
};

/**
 * WeakMap implementation
 * ES6 WeakMap with object keys only
 */
class WeakMap : public Object {
private:
    std::unordered_map<Object*, Value> entries_;
    
public:
    WeakMap();
    virtual ~WeakMap() = default;
    
    bool has(Object* key) const;
    Value get(Object* key) const;
    void set(Object* key, const Value& value);
    bool delete_key(Object* key);
    
    static Value weakmap_constructor(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_set(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_get(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_has(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_delete(Context& ctx, const std::vector<Value>& args);
    
    static void setup_weakmap_prototype(Context& ctx);
    
    static Object* prototype_object;
};

/**
 * WeakSet implementation
 * ES6 WeakSet with object values only
 */
class WeakSet : public Object {
private:
    std::unordered_set<Object*> values_;
    
public:
    WeakSet();
    virtual ~WeakSet() = default;
    
    bool has(Object* value) const;
    void add(Object* value);
    bool delete_value(Object* value);
    
    static Value weakset_constructor(Context& ctx, const std::vector<Value>& args);
    static Value weakset_add(Context& ctx, const std::vector<Value>& args);
    static Value weakset_has(Context& ctx, const std::vector<Value>& args);
    static Value weakset_delete(Context& ctx, const std::vector<Value>& args);
    
    static void setup_weakset_prototype(Context& ctx);
    
    static Object* prototype_object;
};

}
