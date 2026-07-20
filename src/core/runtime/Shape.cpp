/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Shape.h"

namespace Quanta {

Shape::Shape(Shape* parent, const std::string& key, uint32_t slot_index)
    : parent_(parent), added_key_(key), slot_count_(slot_index + 1) {
    if (parent_) slots_ = parent_->slots_;
    slots_.set(key, slot_index);
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

int32_t Shape::find_slot(const std::string& key) const {
    return slots_.find(key);
}

std::vector<std::string> Shape::keys_in_order() const {
    std::vector<std::string> keys(slot_count_);
    for (const Shape* s = this; s->parent_; s = s->parent_) {
        keys[s->slot_count_ - 1] = s->added_key_;
    }
    return keys;
}

}
