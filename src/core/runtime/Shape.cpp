/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Shape.h"

namespace Quanta {

Shape::Shape(Shape* parent, const std::string& key, uint32_t slot_index, bool is_accessor)
    : parent_(parent), added_key_(key),
      slot_count_(slot_index + (is_accessor ? 2u : 1u)), is_accessor_added_(is_accessor) {
    if (parent_) slots_ = parent_->slots_;
    slots_.set(key, slot_index, is_accessor);
}

Shape* Shape::root() {
    static thread_local Shape instance;
    return &instance;
}

Shape* Shape::transition(const std::string& key) {
    if (Shape* existing = transitions_.find(key)) return existing;
    if (slot_count_ >= kMaxSlots) return nullptr;
    if (transitions_.size() >= kMaxTransitions) return nullptr;
    auto child = std::unique_ptr<Shape>(new Shape(this, key, slot_count_));
    return transitions_.insert(key, std::move(child));
}

Shape* Shape::transition_accessor(const std::string& key) {
    if (Shape* existing = accessor_transitions_.find(key)) return existing;
    if (slot_count_ + 2 > kMaxSlots) return nullptr;
    if (accessor_transitions_.size() >= kMaxTransitions) return nullptr;
    auto child = std::unique_ptr<Shape>(new Shape(this, key, slot_count_, /*is_accessor=*/true));
    return accessor_transitions_.insert(key, std::move(child));
}

int32_t Shape::find_slot(const std::string& key) const {
    return slots_.find(key);
}

std::vector<Shape::PropertyInfo> Shape::properties_in_order() const {
    std::vector<PropertyInfo> props;
    for (const Shape* s = this; s->parent_; s = s->parent_) {
        uint32_t width = s->is_accessor_added_ ? 2u : 1u;
        props.push_back({s->added_key_, s->slot_count_ - width, s->is_accessor_added_});
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
