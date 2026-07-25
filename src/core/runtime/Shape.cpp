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

Shape* Shape::transition(const std::string& key) {
    // find() compares by value, no interning needed for the (common) case
    // this child already exists -- only intern() on an actual cache miss,
    // i.e. once per (this, key) edge ever, not once per call. transitions_
    // itself is lazy (null until this shape's first child), so a leaf
    // shape (the common terminal case) never allocates it at all.
    if (transitions_) {
        if (Shape* existing = transitions_->find(key)) return existing;
    }
    if (slot_count_ >= kMaxSlots) return nullptr;
    if (transitions_ && transitions_->size() >= kMaxTransitions) return nullptr;
    const std::string* ikey = intern(key);
    auto child = std::unique_ptr<Shape>(new Shape(this, ikey, slot_count_));
    if (!transitions_) transitions_ = std::make_unique<TransitionMap>();
    return transitions_->insert(ikey, std::move(child));
}

Shape* Shape::transition_accessor(const std::string& key) {
    if (accessor_transitions_) {
        if (Shape* existing = accessor_transitions_->find(key)) return existing;
    }
    if (slot_count_ + 2 > kMaxSlots) return nullptr;
    if (accessor_transitions_ && accessor_transitions_->size() >= kMaxTransitions) return nullptr;
    const std::string* ikey = intern(key);
    auto child = std::unique_ptr<Shape>(new Shape(this, ikey, slot_count_, /*is_accessor=*/true));
    if (!accessor_transitions_) accessor_transitions_ = std::make_unique<TransitionMap>();
    return accessor_transitions_->insert(ikey, std::move(child));
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
