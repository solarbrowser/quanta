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

class Map : public Object {
private:
    struct MapEntry {
        Value key;
        Value value;
        bool deleted = false; // soft-delete: keeps insertion-order positions stable for live forEach.

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
    void trace(Visitor& v);
    ~Map() = default;
    
    bool has(const Value& key) const;
    Value get(const Value& key) const;
    void set(const Value& key, const Value& value);
    bool delete_key(const Value& key);
    void clear();
    
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    
    // No longer virtual on Object -- see Object::get_property()'s own switch-based dispatch.
    Value get_property(const std::string& key) const;
    
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
    
    static thread_local Object* prototype_object;
    
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
    struct SetEntry {
        Value value;
        bool deleted = false; // soft-delete: keeps insertion-order positions stable for live forEach.
        explicit SetEntry(const Value& v) : value(v) {}
    };
    std::vector<SetEntry> values_;
    size_t size_;

public:
    Set();
    void trace(Visitor& v);
    ~Set() = default;
    
    bool has(const Value& value) const;
    void add(const Value& value);
    bool delete_value(const Value& value);
    void clear();
    
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    
    // No longer virtual on Object -- see Object::get_property()'s own switch-based dispatch.
    Value get_property(const std::string& key) const;
    
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
    
    static thread_local Object* prototype_object;
    
private:
    std::vector<SetEntry>::iterator find_value(const Value& value);
    std::vector<SetEntry>::const_iterator find_value(const Value& value) const;
};

/**
 * WeakMap implementation
 * ES6 WeakMap with object keys only
 */
class WeakMap : public Object {
private:
    std::unordered_map<Object*, Value> entries_;
    std::unordered_map<class Symbol*, Value> symbol_entries_; // unregistered symbol keys (ES2023)

public:
    WeakMap();
    // Keys are weakly held: trace() reports the map to the collector's
    // ephemeron pass instead of visiting entries_/symbol_entries_ directly,
    // so a value is only kept alive while its key is (see Collector.cpp).
    void trace(Visitor& v);
    ~WeakMap() = default;

    bool has(Object* key) const;
    Value get(Object* key) const;
    void set(Object* key, const Value& value);
    bool delete_key(Object* key);
    bool has_symbol(class Symbol* sym) const;
    Value get_symbol(class Symbol* sym) const;
    void set_symbol(class Symbol* sym, const Value& value);
    bool delete_symbol(class Symbol* sym);

    // Ephemeron processing hooks: only the collector's mark/sweep touches these.
    std::unordered_map<Object*, Value>& raw_entries() { return entries_; }
    std::unordered_map<class Symbol*, Value>& raw_symbol_entries() { return symbol_entries_; }

    static Value weakmap_constructor(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_set(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_get(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_has(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_delete(Context& ctx, const std::vector<Value>& args);

    static void setup_weakmap_prototype(Context& ctx);

    static thread_local Object* prototype_object;
};

/**
 * WeakSet implementation
 * ES6 WeakSet with object values only
 */
class WeakSet : public Object {
private:
    std::unordered_set<Object*> values_;
    std::unordered_set<class Symbol*> symbol_values_; // unregistered symbol members (ES2023)

public:
    WeakSet();
    // Weakly held: see WeakMap::trace.
    void trace(Visitor& v);
    ~WeakSet() = default;

    bool has(Object* value) const;
    void add(Object* value);
    bool delete_value(Object* value);
    bool has_symbol(class Symbol* sym) const;
    void add_symbol(class Symbol* sym);
    bool delete_symbol(class Symbol* sym);

    std::unordered_set<Object*>& raw_values() { return values_; }
    std::unordered_set<class Symbol*>& raw_symbol_values() { return symbol_values_; }

    static Value weakset_constructor(Context& ctx, const std::vector<Value>& args);
    static Value weakset_add(Context& ctx, const std::vector<Value>& args);
    static Value weakset_has(Context& ctx, const std::vector<Value>& args);
    static Value weakset_delete(Context& ctx, const std::vector<Value>& args);

    static void setup_weakset_prototype(Context& ctx);

    static thread_local Object* prototype_object;
};

/**
 * WeakRef implementation (ES2021)
 */
class WeakRef : public Object {
private:
    Object* target_object_ = nullptr;
    class Symbol* target_symbol_ = nullptr;

public:
    explicit WeakRef(Object* target);
    explicit WeakRef(class Symbol* target);
    // Does not visit the target: a live WeakRef pointing at a dead target is
    // the normal, observable end state (deref() then returns undefined).
    void trace(Visitor& v);
    ~WeakRef() = default;

    Value deref() const;
    Object* target_object() const { return target_object_; }
    class Symbol* target_symbol() const { return target_symbol_; }
    // Collector-only: called once the ephemeron pass proves the target dead,
    // before its cell is swept.
    void clear_target() { target_object_ = nullptr; target_symbol_ = nullptr; }

    static Value weakref_constructor(Context& ctx, const std::vector<Value>& args);
    static Value weakref_deref(Context& ctx, const std::vector<Value>& args);

    static void setup_weakref_prototype(Context& ctx);

    static thread_local Object* prototype_object;
};

/**
 * FinalizationRegistry implementation (ES2021)
 */
class FinalizationRegistry : public Object {
public:
    struct Cell {
        Object* target_object = nullptr;
        class Symbol* target_symbol = nullptr;
        Value held_value;
        Object* token_object = nullptr;
        class Symbol* token_symbol = nullptr;
        // Set by the collector once the target is proven dead; the queued
        // cleanup job delivers the callback and erases the cell afterward.
        bool cleared = false;
    };

private:
    Function* cleanup_callback_ = nullptr;
    std::vector<Cell> cells_;
    Context* context_ = nullptr; // creating realm's global context, for job queueing

public:
    FinalizationRegistry(Function* cleanup_callback, Context* ctx);
    // Strongly traces the callback and each cell's heldValue; targets/tokens
    // are weak (see WeakMap::trace).
    void trace(Visitor& v);
    ~FinalizationRegistry() = default;

    void register_target(Object* target_obj, class Symbol* target_sym, const Value& held,
                         Object* token_obj, class Symbol* token_sym);
    bool unregister(Object* token_obj, class Symbol* token_sym);

    std::vector<Cell>& raw_cells() { return cells_; }
    Function* cleanup_callback() const { return cleanup_callback_; }
    // Collector-only: queues a microtask that delivers every cell already
    // marked cleared, removing each as its callback returns.
    void enqueue_cleanup_job();

    static Value fr_constructor(Context& ctx, const std::vector<Value>& args);
    static Value fr_register(Context& ctx, const std::vector<Value>& args);
    static Value fr_unregister(Context& ctx, const std::vector<Value>& args);

    static void setup_finalization_registry_prototype(Context& ctx);

    static thread_local Object* prototype_object;
};

}
