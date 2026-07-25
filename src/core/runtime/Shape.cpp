/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Shape.h"
#include <unordered_set>

namespace Quanta {

#if defined(__GLIBCXX__)
static_assert(sizeof(Shape) == 128);
#else
static_assert(sizeof(Shape) <= 256);
#endif

const std::string* Shape::intern(const std::string& key) {
    // Never erased from, so returned pointers are stable for the thread's
    // lifetime -- see the field's own doc comment in Shape.h.
    static thread_local std::unordered_set<std::string> table;
    auto it = table.find(key);
    if (it == table.end()) it = table.insert(key).first;
    return &*it;
}

Shape::Shape(Shape* parent, const std::string* key, uint32_t slot_index, bool is_accessor)
    : parent_(parent), added_key_(key),
      slot_count_(slot_index + (is_accessor ? 2u : 1u)), is_accessor_added_(is_accessor) {
    if (parent_) slots_ = parent_->slots_;
    slots_.set(key, slot_index, is_accessor);
}

Shape* Shape::root() {
    // Heap-allocated (leaked, matching "immortal for the thread's lifetime"
    // above) rather than a plain `static thread_local Shape instance` --
    // thread-local storage doesn't reliably honor over-alignment (Shape is
    // alignas(32), Object.h's TaggedShapePtr tag bits), while `new` does
    // (C++17 over-aligned dynamic allocation).
    static thread_local Shape* instance = new Shape();
    return instance;
}

Shape* Shape::transition_find(void* table, bool is_single, const std::string& key) {
    if (!table) return nullptr;
    if (is_single) {
        auto* single = static_cast<SingleTransition*>(table);
        return *single->key == key ? single->child.get() : nullptr;
    }
    return static_cast<TransitionMap*>(table)->find(key);
}

size_t Shape::transition_size(void* table, bool is_single) {
    if (!table) return 0;
    if (is_single) return 1;
    return static_cast<TransitionMap*>(table)->size();
}

std::pair<Shape*, bool> Shape::transition_insert(void*& table, bool is_single,
                                                   const std::string* key, std::unique_ptr<Shape> child) {
    Shape* raw = child.get();
    if (!table) {
        table = new SingleTransition{key, std::move(child)};
        return {raw, true};
    }
    if (is_single) {
        auto* single = static_cast<SingleTransition*>(table);
        auto* map = new TransitionMap();
        map->entries.reserve(2);
        map->insert(single->key, std::move(single->child));
        delete single;
        map->insert(key, std::move(child));
        table = map;
        return {raw, false};
    }
    static_cast<TransitionMap*>(table)->insert(key, std::move(child));
    return {raw, false};
}

Shape* Shape::transition(const std::string& key) {
    // transition_find() compares by value, no interning needed for the
    // (common) case this child already exists -- only intern() on an
    // actual cache miss, i.e. once per (this, key) edge ever, not once
    // per call. transitions_ itself is lazy (null until this shape's
    // first child), so a leaf shape (the common terminal case) never
    // allocates anything at all.
    if (Shape* existing = transition_find(transitions_, transitions_is_single_, key)) return existing;
    if (slot_count_ >= kMaxSlots) return nullptr;
    if (transition_size(transitions_, transitions_is_single_) >= kMaxTransitions) return nullptr;
    const std::string* ikey = intern(key);
    auto child = std::unique_ptr<Shape>(new Shape(this, ikey, slot_count_));
    auto [result, new_is_single] = transition_insert(transitions_, transitions_is_single_, ikey, std::move(child));
    transitions_is_single_ = new_is_single;
    return result;
}

Shape* Shape::transition_accessor(const std::string& key) {
    if (Shape* existing = transition_find(accessor_transitions_, accessor_transitions_is_single_, key)) return existing;
    if (slot_count_ + 2 > kMaxSlots) return nullptr;
    if (transition_size(accessor_transitions_, accessor_transitions_is_single_) >= kMaxTransitions) return nullptr;
    const std::string* ikey = intern(key);
    auto child = std::unique_ptr<Shape>(new Shape(this, ikey, slot_count_, /*is_accessor=*/true));
    auto [result, new_is_single] = transition_insert(accessor_transitions_, accessor_transitions_is_single_, ikey, std::move(child));
    accessor_transitions_is_single_ = new_is_single;
    return result;
}

int32_t Shape::find_slot(const std::string& key) const {
    return slots_.find(key);
}

std::vector<Shape::PropertyInfo> Shape::properties_in_order() const {
    std::vector<PropertyInfo> props;
    for (const Shape* s = this; s->parent_; s = s->parent_) {
        uint32_t width = s->is_accessor_added_ ? 2u : 1u;
        props.push_back({*s->added_key_, s->slot_count_ - width, s->is_accessor_added_});
    }
    std::reverse(props.begin(), props.end());
    return props;
}

std::vector<std::string> Shape::keys_in_order() const {
    std::vector<std::string> keys;
    keys.reserve(slot_count_);
    for (const auto& p : properties_in_order()) {
        keys.push_back(p.key);
    }
    return keys;
}

}
