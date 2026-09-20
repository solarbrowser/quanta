/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/core/runtime/Value.h"
#include <span>
#include "quanta/core/runtime/Object.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

namespace Quanta {

class Context;

// SameValueZero, the equality Map and Set are both specified with: strict
// equality except that NaN matches itself and -0 matches +0. Shared by both
// collections' key indexes.
struct SameValueZeroHash { size_t operator()(const Value& v) const; };
struct SameValueZeroEqual { bool operator()(const Value& a, const Value& b) const; };

// Where a walker over a Map or a Set has got to, in terms that outlive every
// change to the storage. Entries are numbered in insertion order and the
// walker remembers the number of the next one it owes, so a deletion, a
// compaction or a clear() cannot move it: the position is looked up again by
// number whenever the layout it was taken from is gone. Live iteration is
// what the language specifies -- an entry added mid-walk is visited, one
// deleted before its turn is not, and after clear() the walk carries on with
// whatever is added next.
struct WalkCursor {
    uint64_t next_seq = 0;      // every entry numbered below this has been visited
    uint32_t epoch = ~0u;       // which storage layout `pos` was taken from
    uint32_t pos = 0;           // valid only while `epoch` still matches the collection's
};

// An entry's insertion number and its soft-delete flag in one word, so that
// numbering costs the entry nothing: the flag used to sit in a padded bool.
struct EntryTag {
    static constexpr uint64_t kDeleted = 1ull << 63;
    uint64_t bits = 0;
    explicit EntryTag(uint64_t seq) : bits(seq) {}
    bool deleted() const { return bits & kDeleted; }
    void mark_deleted() { bits |= kDeleted; }
    uint64_t seq() const { return bits & ~kDeleted; }
};

class Map : public Object {
private:
    struct MapEntry {
        Value key;
        Value value;
        EntryTag tag;  // soft-delete keeps positions stable between compactions

        MapEntry(const Value& k, const Value& v, uint64_t seq) : key(k), value(v), tag(seq) {}
    };
    
    std::vector<MapEntry> entries_;
    size_t size_;
    uint64_t next_seq_ = 0;
    // Deleted entries still in entries_. Left alone they are never reclaimed and
    // a cache that deletes and re-adds keeps growing; past half the storage the
    // vector is squeezed and `epoch_` moves so walkers find their place again.
    size_t tombstones_ = 0;
    uint32_t epoch_ = 0;
    static constexpr size_t kCompactMin = 32;
    void compact();

    // Key -> position in entries_. Insertion order and stable positions still
    // come from the vector (a live forEach walks it by index), but every
    // lookup used to be a scan of it, so building a map was quadratic.
    // Small maps stay on the scan: below the threshold a contiguous compare
    // beats hashing the key.
    std::unordered_map<Value, uint32_t, SameValueZeroHash, SameValueZeroEqual> index_;
    bool indexed_ = false;
    static constexpr size_t kLinearLimit = 8;
    void build_index();

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
    // Hands back the next live entry a walker has not seen and moves it past.
    bool next_entry(WalkCursor& cursor, Value& key, Value& value) const;
    
    static Value map_constructor(Context& ctx, std::span<const Value> args, Value receiver,
                        bool is_construct, Value new_target);
    static Value map_set(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_get(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_has(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_delete(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_clear(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_size_getter(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_keys(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_values(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_entries(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_forEach(Context& ctx, std::span<const Value> args, Value receiver);
    static Value map_iterator_method(Context& ctx, std::span<const Value> args, Value receiver);
    
    static void setup_map_prototype(Context& ctx);
    
    static constinit thread_local Object* prototype_object;
    
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
        EntryTag tag;  // see Map::MapEntry
        SetEntry(const Value& v, uint64_t seq) : value(v), tag(seq) {}
    };
    std::vector<SetEntry> values_;
    size_t size_;
    // The same bookkeeping as Map: numbering, tombstones, and the epoch that
    // tells a walker its remembered position went stale.
    uint64_t next_seq_ = 0;
    size_t tombstones_ = 0;
    uint32_t epoch_ = 0;
    static constexpr size_t kCompactMin = 32;
    void compact();

    // Same arrangement as Map: the vector keeps insertion order and stable
    // positions, the index makes lookup constant-time past a small size.
    std::unordered_map<Value, uint32_t, SameValueZeroHash, SameValueZeroEqual> index_;
    bool indexed_ = false;
    static constexpr size_t kLinearLimit = 8;
    void build_index();

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
    bool next_value(WalkCursor& cursor, Value& value) const;
    
    static Value set_constructor(Context& ctx, std::span<const Value> args, Value receiver,
                        bool is_construct, Value new_target);
    static Value set_add(Context& ctx, std::span<const Value> args, Value receiver);
    static Value set_has(Context& ctx, std::span<const Value> args, Value receiver);
    static Value set_delete(Context& ctx, std::span<const Value> args, Value receiver);
    static Value set_clear(Context& ctx, std::span<const Value> args, Value receiver);
    static Value set_size_getter(Context& ctx, std::span<const Value> args, Value receiver);
    static Value set_values(Context& ctx, std::span<const Value> args, Value receiver);
    static Value set_keys(Context& ctx, std::span<const Value> args, Value receiver);
    static Value set_entries(Context& ctx, std::span<const Value> args, Value receiver);
    static Value set_forEach(Context& ctx, std::span<const Value> args, Value receiver);
    static Value set_iterator_method(Context& ctx, std::span<const Value> args, Value receiver);
    
    static void setup_set_prototype(Context& ctx);
    
    static constinit thread_local Object* prototype_object;
    
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
    // Lazy: unregistered symbol keys (ES2023) are a rare feature relative to
    // ordinary object keys -- most WeakMaps never touch this at all.
    std::unique_ptr<std::unordered_map<class Symbol*, Value>> symbol_entries_;

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

    // Ephemeron processing hooks: only the collector's mark/sweep touches
    // these. raw_symbol_entries() returns null when never populated --
    // callers (Collector.cpp) must guard before iterating.
    std::unordered_map<Object*, Value>& raw_entries() { return entries_; }
    std::unordered_map<class Symbol*, Value>* raw_symbol_entries() { return symbol_entries_.get(); }

    static Value weakmap_constructor(Context& ctx, std::span<const Value> args, Value receiver,
                        bool is_construct, Value new_target);
    static Value weakmap_set(Context& ctx, std::span<const Value> args, Value receiver);
    static Value weakmap_get(Context& ctx, std::span<const Value> args, Value receiver);
    static Value weakmap_has(Context& ctx, std::span<const Value> args, Value receiver);
    static Value weakmap_delete(Context& ctx, std::span<const Value> args, Value receiver);

    static void setup_weakmap_prototype(Context& ctx);

    static constinit thread_local Object* prototype_object;
};

/**
 * WeakSet implementation
 * ES6 WeakSet with object values only
 */
class WeakSet : public Object {
private:
    std::unordered_set<Object*> values_;
    // Lazy: see WeakMap::symbol_entries_'s own doc comment, same rationale.
    std::unique_ptr<std::unordered_set<class Symbol*>> symbol_values_;

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

    // raw_symbol_values() returns null when never populated -- callers
    // (Collector.cpp) must guard before iterating.
    std::unordered_set<Object*>& raw_values() { return values_; }
    std::unordered_set<class Symbol*>* raw_symbol_values() { return symbol_values_.get(); }

    static Value weakset_constructor(Context& ctx, std::span<const Value> args, Value receiver,
                        bool is_construct, Value new_target);
    static Value weakset_add(Context& ctx, std::span<const Value> args, Value receiver);
    static Value weakset_has(Context& ctx, std::span<const Value> args, Value receiver);
    static Value weakset_delete(Context& ctx, std::span<const Value> args, Value receiver);

    static void setup_weakset_prototype(Context& ctx);

    static constinit thread_local Object* prototype_object;
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

    static Value weakref_constructor(Context& ctx, std::span<const Value> args, Value receiver,
                        bool is_construct, Value new_target);
    static Value weakref_deref(Context& ctx, std::span<const Value> args, Value receiver);

    static void setup_weakref_prototype(Context& ctx);

    static constinit thread_local Object* prototype_object;
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

    static Value fr_constructor(Context& ctx, std::span<const Value> args, Value receiver,
                        bool is_construct, Value new_target);
    static Value fr_register(Context& ctx, std::span<const Value> args, Value receiver);
    static Value fr_unregister(Context& ctx, std::span<const Value> args, Value receiver);

    static void setup_finalization_registry_prototype(Context& ctx);

    static constinit thread_local Object* prototype_object;
};

}
