/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../../include/Object.h"
#include <unordered_map>
#include <vector>
#include <memory>

namespace Quanta {

// EXTRACTED FROM Object.cpp - Shape system implementation (lines 1130-1220)

// Static member initialization (originally lines 22-24)
std::unordered_map<std::pair<Shape*, std::string>, Shape*, Object::ShapeTransitionHash> Object::shape_transition_cache_;
std::unordered_map<std::string, std::string> Object::interned_keys_;
uint32_t Shape::next_shape_id_ = 1;

// Global root shape (originally line 28)
static Shape* g_root_shape = nullptr;

//=============================================================================
// Shape Implementation - EXTRACTED from Object.cpp
//=============================================================================

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
    auto cache_it = Object::shape_transition_cache_.find(cache_key);
    if (cache_it != Object::shape_transition_cache_.end()) {
        return cache_it->second;
    }

    // Create new shape
    Shape* new_shape = new Shape(this, key, attrs);

    // Cache the transition
    Object::shape_transition_cache_[cache_key] = new_shape;

    return new_shape;
}

std::vector<std::string> Shape::get_property_keys() const {
    std::vector<std::string> keys;
    keys.reserve(properties_.size());

    for (const auto& [key, info] : properties_) {
        keys.push_back(key);
    }

    return keys;
}

uint32_t Shape::get_property_count() const {
    return property_count_;
}

uint32_t Shape::get_id() const {
    return id_;
}

Shape* Shape::get_parent() const {
    return parent_;
}

const std::string& Shape::get_transition_key() const {
    return transition_key_;
}

PropertyAttributes Shape::get_transition_attributes() const {
    return transition_attrs_;
}

bool Shape::is_root() const {
    return parent_ == nullptr;
}

Shape* Shape::get_root_shape() {
    if (!g_root_shape) {
        g_root_shape = new Shape();
    }
    return g_root_shape;
}

void Shape::cleanup_root_shape() {
    delete g_root_shape;
    g_root_shape = nullptr;
}

std::vector<Shape*> Shape::get_transition_chain() const {
    std::vector<Shape*> chain;

    const Shape* current = this;
    while (current) {
        chain.push_back(const_cast<Shape*>(current));
        current = current->parent_;
    }

    // Reverse to get root-to-current order
    std::reverse(chain.begin(), chain.end());
    return chain;
}

bool Shape::is_compatible_with(const Shape* other) const {
    if (!other) return false;
    if (this == other) return true;

    // Check if they have the same properties
    return properties_.size() == other->properties_.size() &&
           std::equal(properties_.begin(), properties_.end(), other->properties_.begin());
}

Shape* Shape::find_transition(const std::string& key) const {
    std::pair<Shape*, std::string> cache_key = {const_cast<Shape*>(this), key};
    auto cache_it = Object::shape_transition_cache_.find(cache_key);

    if (cache_it != Object::shape_transition_cache_.end()) {
        return cache_it->second;
    }

    return nullptr;
}

void Shape::clear_transition_cache() {
    Object::shape_transition_cache_.clear();
}

size_t Shape::get_transition_cache_size() {
    return Object::shape_transition_cache_.size();
}

void Shape::print_debug_info() const {
    std::cout << "Shape ID: " << id_ << std::endl;
    std::cout << "Parent: " << (parent_ ? std::to_string(parent_->id_) : "null") << std::endl;
    std::cout << "Property Count: " << property_count_ << std::endl;

    if (!transition_key_.empty()) {
        std::cout << "Transition Key: " << transition_key_ << std::endl;
        std::cout << "Transition Attrs: " << static_cast<int>(transition_attrs_) << std::endl;
    }

    std::cout << "Properties:" << std::endl;
    for (const auto& [key, info] : properties_) {
        std::cout << "  " << key << " -> offset:" << info.offset
                  << " attrs:" << static_cast<int>(info.attributes) << std::endl;
    }
}

// Object Shape Transition Hash implementation
size_t Object::ShapeTransitionHash::operator()(const std::pair<Shape*, std::string>& pair) const {
    size_t h1 = std::hash<Shape*>{}(pair.first);
    size_t h2 = std::hash<std::string>{}(pair.second);
    return h1 ^ (h2 << 1);
}

// String interning utilities
const std::string& Object::intern_string(const std::string& str) {
    auto it = interned_keys_.find(str);
    if (it != interned_keys_.end()) {
        return it->second;
    }

    interned_keys_[str] = str;
    return interned_keys_[str];
}

void Object::clear_interned_strings() {
    interned_keys_.clear();
}

size_t Object::get_interned_string_count() {
    return interned_keys_.size();
}

// Shape optimization utilities
namespace ShapeOptimization {

bool should_use_inline_cache(const Shape* shape) {
    return shape && shape->get_property_count() <= 16;
}

bool should_transition_to_dictionary(const Object* object) {
    // Transition to dictionary mode if too many properties or too many deletions
    return object && (
        (object->header_.shape && object->header_.shape->get_property_count() > 64) ||
        (object->overflow_properties_ && object->overflow_properties_->size() > 32)
    );
}

void optimize_shape_transitions() {
    // Cleanup unused shape transitions
    auto& cache = Object::shape_transition_cache_;

    // In a real implementation, this would use reference counting
    // or mark-and-sweep to clean up unused shapes

    // For now, just clear if cache gets too large
    if (cache.size() > 10000) {
        cache.clear();
    }
}

std::unordered_map<uint32_t, size_t> analyze_shape_usage() {
    std::unordered_map<uint32_t, size_t> usage_counts;

    // Analyze shape transition cache usage
    for (const auto& [key, shape] : Object::shape_transition_cache_) {
        usage_counts[shape->get_id()]++;
    }

    return usage_counts;
}

} // namespace ShapeOptimization

} // namespace Quanta