/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/shape.h"
#include <functional>

namespace Quanta {

// Forward declaration for global root shape
static Shape* g_root_shape = nullptr;

// Static member initialization
uint32_t Shape::next_shape_id_ = 1;

// Shape transition cache - declared here since Object.cpp will need access
namespace {
    std::unordered_map<std::pair<Shape*, std::string>, Shape*,
                      std::hash<std::pair<Shape*, std::string>>> shape_transition_cache_;
}

// Hash function for shape transition cache
namespace std {
    template<>
    struct hash<std::pair<Shape*, std::string>> {
        size_t operator()(const std::pair<Shape*, std::string>& p) const {
            return std::hash<Shape*>{}(p.first) ^ (std::hash<std::string>{}(p.second) << 1);
        }
    };
}

Shape::Shape() : parent_(nullptr), property_count_(0), id_(next_shape_id_++) {
}

Shape::Shape(Shape* parent, const std::string& key, PropertyAttributes attrs)
    : parent_(parent), transition_key_(key), transition_attrs_(attrs),
      property_count_(parent ? parent->property_count_ + 1 : 1),
      id_(next_shape_id_++) {

    // Copy parent properties
    if (parent_) {
        properties_ = parent_->properties_;
    }

    // Add new property
    PropertyInfo info;
    info.offset = property_count_ - 1;
    info.attributes = attrs;
    info.hash = std::hash<std::string>{}(key);

    properties_[key] = info;
}

bool Shape::has_property(const std::string& key) const {
    return properties_.find(key) != properties_.end();
}

Shape::PropertyInfo Shape::get_property_info(const std::string& key) const {
    auto it = properties_.find(key);
    if (it != properties_.end()) {
        return it->second;
    }
    return PropertyInfo{0, PropertyAttributes::None, 0};
}

Shape* Shape::add_property(const std::string& key, PropertyAttributes attrs) {
    // Check cache first
    std::pair<Shape*, std::string> cache_key = {this, key};
    auto cache_it = shape_transition_cache_.find(cache_key);
    if (cache_it != shape_transition_cache_.end()) {
        return cache_it->second;
    }

    // Create new shape
    Shape* new_shape = new Shape(this, key, attrs);

    // Cache the transition
    shape_transition_cache_[cache_key] = new_shape;

    return new_shape;
}

Shape* Shape::remove_property(const std::string& key) {
    // For now, return root shape - property removal optimization can be added later
    return get_root_shape();
}

std::vector<std::string> Shape::get_property_keys() const {
    std::vector<std::string> keys;
    keys.reserve(properties_.size());

    // To preserve insertion order, walk up the parent chain and collect keys in reverse
    std::vector<std::string> reverse_keys;
    const Shape* current = this;

    while (current && current->parent_) {
        if (!current->transition_key_.empty()) {
            reverse_keys.push_back(current->transition_key_);
        }
        current = current->parent_;
    }

    // Reverse to get insertion order
    keys.reserve(reverse_keys.size());
    for (auto it = reverse_keys.rbegin(); it != reverse_keys.rend(); ++it) {
        keys.push_back(*it);
    }

    return keys;
}

std::string Shape::debug_string() const {
    return "Shape{id=" + std::to_string(id_) +
           ", props=" + std::to_string(property_count_) + "}";
}

Shape* Shape::get_root_shape() {
    if (!g_root_shape) {
        g_root_shape = new Shape();
    }
    return g_root_shape;
}

void Shape::rebuild_property_map() {
    // Rebuild the property map from parent chain
    properties_.clear();

    if (!parent_) return;

    // Copy parent properties
    properties_ = parent_->properties_;

    // Add this shape's property if it has one
    if (!transition_key_.empty()) {
        PropertyInfo info;
        info.offset = property_count_ - 1;
        info.attributes = transition_attrs_;
        info.hash = std::hash<std::string>{}(transition_key_);
        properties_[transition_key_] = info;
    }
}

} // namespace Quanta