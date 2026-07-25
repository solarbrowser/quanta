/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_RUNTIME_SHAPE_H
#define QUANTA_RUNTIME_SHAPE_H

#include "quanta/core/runtime/SmallMapPool.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Quanta {

// A shape describes the layout of an object's fast-path (default-attribute,
// data-property) storage: which keys exist, in what order, at what slot
// index. Objects that add the same keys in the same order share one Shape
// instance via the transition tree, so the layout itself is never
// duplicated per object -- only each object's own slot values are.
//
// Not a GC cell: shapes are thread-local and effectively immortal for the
// thread's lifetime (agents never share cells, so there is nothing to gain
// from sharing shapes either). Two independent caps bound the tree, and a
// refused transition means the caller falls back to dictionary-mode storage
// for that one object:
//  - kMaxTransitions bounds each node's child count (tree width), so a
//    pathological per-object-unique-key pattern can't fan the tree out.
//  - kMaxSlots bounds chain depth (properties per object). Every new shape
//    copies its parent's flattened slot table, so an uncapped chain costs
//    O(n^2) time and memory as one object keeps adding properties.
// 32-byte aligned so Object can tag ObjectType (5 bits) into a Shape*'s
// low bits -- see Object::kTypeTagMask. Cheap here since shapes are shared
// across every object with the same layout, not allocated per-object.
class alignas(32) Shape {
public:
    // The empty shape every plain object starts from.
    static Shape* root();

    // The child shape after adding `key`, memoized so any other object
    // adding the same key from this same shape gets the identical child.
    // Returns nullptr if either cap would be exceeded (kMaxTransitions
    // children and `key` is not one of them, or kMaxSlots properties
    // deep) -- the caller must fall back to dictionary-mode storage
    // rather than grow the tree further.
    Shape* transition(const std::string& key);

    // Like transition(), but reserves TWO consecutive slots (getter, then
    // setter) for `key` instead of one, and marks the slot accessor-kind --
    // see Object::add_accessor_shape_property_cached. Memoized in a
    // SEPARATE tree (accessor_transitions_) from transition()'s: the same
    // (parent shape, key) pair transitioning once as plain data and once as
    // an accessor must never collide on the same cached child.
    Shape* transition_accessor(const std::string& key);

    // O(1): the slot index for `key` across this shape's full property set
    // (own key plus everything inherited from parent_), or -1 if absent.
    int32_t find_slot(const std::string& key) const;

    // True if `key` names an accessor-kind slot (its value at find_slot(key)
    // is a getter Value; the paired setter Value lives at find_slot(key)+1).
    bool is_accessor_slot(const std::string& key) const { return slots_.is_accessor(key); }

    uint32_t slot_count() const { return slot_count_; }

    // Keys in insertion order (root-to-here), for Object.keys()/for-in over
    // a shape-mode object. O(slot_count) -- an enumeration op, not the
    // property get/set hot path. Flattened from properties_in_order() so an
    // accessor's second (setter) physical slot never surfaces as its own
    // blank/duplicate entry.
    std::vector<std::string> keys_in_order() const;

    // One entry per LOGICAL property (unlike keys_in_order()/slot_count(),
    // which are physical-slot-indexed) -- an accessor property still yields
    // exactly one PropertyInfo here even though it occupies two slots.
    // migrate_to_dictionary_mode() is the only consumer: it needs to know,
    // per key, where its value(s) live AND whether to read them as a plain
    // value or as a getter/setter pair.
    struct PropertyInfo { std::string key; uint32_t slot_index; bool is_accessor; };
    std::vector<PropertyInfo> properties_in_order() const;

private:
    Shape() = default;
    Shape(Shape* parent, const std::string* key, uint32_t slot_index, bool is_accessor = false);

    static constexpr uint32_t kMaxTransitions = 128;
    static constexpr uint32_t kMaxSlots = 128;

    // Canonicalizes `key` to a stable address, shrinking every key field
    // below (added_key_ and both slot/transition tables' Entry::key) from a
    // 32-byte inline std::string to an 8-byte pointer. Backed by a
    // thread_local set that, like Shape itself, is never erased from for
    // the thread's lifetime, so returned pointers stay valid as long as any
    // Shape does.
    //
    // Only called from transition()/transition_accessor()'s cache-miss
    // path -- once per (parent shape, key) tree edge, never per property
    // access. find_slot()/is_accessor_slot() (the get/set hot path) never
    // call this; they compare against the interned pointee by value instead.
    static const std::string* intern(const std::string& key);

    Shape* parent_ = nullptr;
    const std::string* added_key_ = nullptr;
    uint32_t slot_count_ = 0;
    // Packed into one byte (bit-fields, zero extra storage over a single
    // bool -- verified via mirror struct). True if THIS shape's own link
    // (added_key_) reserved two slots (getter then setter) instead of one
    // -- see transition_accessor().
    bool is_accessor_added_ : 1 = false;
    // Discriminants for transitions_/accessor_transitions_ below: true
    // means the field points to a SingleTransition, false means it's
    // either null or a TransitionMap*. See those fields' own doc comment.
    bool transitions_is_single_ : 1 = false;
    bool accessor_transitions_is_single_ : 1 = false;

    // Slot table (key -> flattened slot index), same inline+overflow idiom
    // as HybridDescriptorMap (Object.h). No migration/erase needed --
    // find_slot always returns by value, no address-stability to protect.
    // Explicitly copyable (unlike HybridDescriptorMap's owning Object): the
    // constructor below copies a parent's whole table before appending its
    // own key.
    //
    // Inline entries store an interned key (8-byte pointer, see
    // Shape::intern), but find()/is_accessor() take a plain, possibly-
    // uninterned key and compare against the pointee by value -- they must
    // never force an intern() call. Only set() takes an already-interned
    // pointer (its caller, Shape's own constructor, always has one on
    // hand). Overflow stays keyed by plain std::string: it's just a
    // unique_ptr either way, so interning it wouldn't shrink Shape.
    struct SlotMap {
        static constexpr size_t kInlineCapacity = 4;
        // is_accessor: true if `value` is the FIRST of a 2-slot getter/setter
        // pair (see Shape's own is_accessor_added_ doc comment) rather than a
        // single plain-data slot index.
        struct Entry { const std::string* key = nullptr; uint32_t value = 0; bool in_use = false; bool is_accessor = false; };
        using OverflowEntry = std::pair<uint32_t, bool>; // (slot index, is_accessor)
        std::array<Entry, kInlineCapacity> inline_entries;
        using OverflowMap = std::unordered_map<std::string, OverflowEntry, std::hash<std::string>,
                                                std::equal_to<std::string>,
                                                SmallMapAllocator<std::pair<const std::string, OverflowEntry>>>;
        std::unique_ptr<OverflowMap> overflow;

        SlotMap() = default;
        SlotMap(const SlotMap& other)
            : inline_entries(other.inline_entries),
              overflow(other.overflow ? std::make_unique<OverflowMap>(*other.overflow) : nullptr) {}
        SlotMap& operator=(const SlotMap& other) {
            if (this == &other) return *this;
            inline_entries = other.inline_entries;
            overflow = other.overflow ? std::make_unique<OverflowMap>(*other.overflow) : nullptr;
            return *this;
        }
        SlotMap(SlotMap&&) = default;
        SlotMap& operator=(SlotMap&&) = default;

        int32_t find(const std::string& key) const {
            for (const auto& e : inline_entries) {
                if (e.in_use && *e.key == key) return static_cast<int32_t>(e.value);
            }
            if (overflow) {
                auto it = overflow->find(key);
                if (it != overflow->end()) return static_cast<int32_t>(it->second.first);
            }
            return -1;
        }
        bool is_accessor(const std::string& key) const {
            for (const auto& e : inline_entries) {
                if (e.in_use && *e.key == key) return e.is_accessor;
            }
            if (overflow) {
                auto it = overflow->find(key);
                if (it != overflow->end()) return it->second.second;
            }
            return false;
        }
        // `key` must already be interned (see the struct's own doc comment).
        void set(const std::string* key, uint32_t value, bool is_accessor = false) {
            for (auto& e : inline_entries) {
                if (e.in_use && *e.key == *key) { e.value = value; e.is_accessor = is_accessor; return; }
            }
            for (auto& e : inline_entries) {
                if (!e.in_use) { e.key = key; e.value = value; e.in_use = true; e.is_accessor = is_accessor; return; }
            }
            if (!overflow) overflow = std::make_unique<OverflowMap>();
            (*overflow)[*key] = {value, is_accessor};
        }
    };
    SlotMap slots_;

    // Transition table (key -> child Shape*), same idiom, sized 8 not 4
    // (root shapes fan out wider than typical slot counts -- object_literals.js's
    // computed-key benchmark alone drives 8 children of root). Never
    // copyable. Entries own unique_ptr<Shape>, never Shape by value: Shape*
    // is cached permanently elsewhere (FeedbackSlot, Object::shape_), so
    // only the pointer may move between inline/overflow, never the pointee.
    //
    // Same interned-inline/plain-overflow split as SlotMap above, and the
    // same rule: find() takes a possibly-uninterned key and compares by
    // value, only insert() requires an already-interned one.
    struct TransitionMap {
        static constexpr size_t kInlineCapacity = 8;
        struct Entry { const std::string* key = nullptr; std::unique_ptr<Shape> value; bool in_use = false; };
        std::array<Entry, kInlineCapacity> inline_entries;
        using OverflowMap = std::unordered_map<std::string, std::unique_ptr<Shape>, std::hash<std::string>,
                                                std::equal_to<std::string>,
                                                SmallMapAllocator<std::pair<const std::string, std::unique_ptr<Shape>>>>;
        std::unique_ptr<OverflowMap> overflow;

        Shape* find(const std::string& key) const {
            for (const auto& e : inline_entries) {
                if (e.in_use && *e.key == key) return e.value.get();
            }
            if (overflow) {
                auto it = overflow->find(key);
                if (it != overflow->end()) return it->second.get();
            }
            return nullptr;
        }
        size_t size() const {
            size_t n = 0;
            for (const auto& e : inline_entries) if (e.in_use) n++;
            if (overflow) n += overflow->size();
            return n;
        }
        // `key` must already be interned (see the struct's own doc comment).
        Shape* insert(const std::string* key, std::unique_ptr<Shape> child) {
            Shape* raw = child.get();
            for (auto& e : inline_entries) {
                if (!e.in_use) { e.key = key; e.value = std::move(child); e.in_use = true; return raw; }
            }
            if (!overflow) overflow = std::make_unique<OverflowMap>();
            (*overflow)[*key] = std::move(child);
            return raw;
        }
    };

    // The 0/1/many split below (same idea V8/JSC/SpiderMonkey use for their
    // own transition tables) exists because most shape nodes have exactly
    // one child, or none -- paying for TransitionMap's 8-wide inline array
    // (200 bytes) on the first child regardless is wasteful when the
    // common case needs only 16.
    struct SingleTransition { const std::string* key = nullptr; std::unique_ptr<Shape> child; };

    // Reads-by-value against `table`'s current state (null / single /
    // TransitionMap*, discriminated by `is_single`) -- never interns, same
    // hot-path-safety rule as TransitionMap::find() above.
    static Shape* transition_find(void* table, bool is_single, const std::string& key);
    static size_t transition_size(void* table, bool is_single);
    // `key` must already be interned. Promotes null->single->TransitionMap
    // as needed, updating `table` in place; returns the new child and the
    // new is_single state (the caller's is_single field is a bit-field, so
    // it can't be passed by reference -- assign the second element back).
    static std::pair<Shape*, bool> transition_insert(void*& table, bool is_single,
                                                       const std::string* key, std::unique_ptr<Shape> child);

    // Lazy: null until this shape's first child (most shapes -- the "fully
    // built object" terminal ones -- never get one). transition()/
    // transition_accessor() allocate on their own cache-miss path;
    // find_slot() never touches these at all, so this costs the get/set
    // hot path nothing either way. Tagged by transitions_is_single_/
    // accessor_transitions_is_single_ above: either a SingleTransition* or
    // a TransitionMap*, see transition_find/transition_insert.
    void* transitions_ = nullptr;
    // Separate memoization tree for transition_accessor() -- see its own
    // doc comment for why this must not share transitions_.
    void* accessor_transitions_ = nullptr;
};

}

#endif
