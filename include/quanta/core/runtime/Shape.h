/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_RUNTIME_SHAPE_H
#define QUANTA_RUNTIME_SHAPE_H

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
class Shape {
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

    // O(1): the slot index for `key` across this shape's full property set
    // (own key plus everything inherited from parent_), or -1 if absent.
    int32_t find_slot(const std::string& key) const;

    uint32_t slot_count() const { return slot_count_; }

    // Keys in insertion order (root-to-here), for Object.keys()/for-in over
    // a shape-mode object. O(slot_count) -- an enumeration op, not the
    // property get/set hot path.
    std::vector<std::string> keys_in_order() const;

private:
    Shape() = default;
    Shape(Shape* parent, const std::string& key, uint32_t slot_index);

    static constexpr uint32_t kMaxTransitions = 128;
    static constexpr uint32_t kMaxSlots = 128;

    Shape* parent_ = nullptr;
    std::string added_key_;
    uint32_t slot_count_ = 0;
    // Flattened own+inherited lookup table: rebuilt once per shape (at
    // transition time), not per access -- the whole point is O(1) find_slot.
    std::unordered_map<std::string, uint32_t> slots_;
    std::unordered_map<std::string, std::unique_ptr<Shape>> transitions_;
};

}

#endif
