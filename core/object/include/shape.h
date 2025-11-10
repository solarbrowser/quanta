/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "property_descriptor.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>

namespace Quanta {

/**
 * Shape (Hidden Class) system for JavaScript objects
 * Optimizes property access by creating shared property layouts
 */
class Shape {
public:
    struct PropertyInfo {
        uint32_t offset;        // Offset in properties array
        PropertyAttributes attributes;
        uint32_t hash;          // Property name hash for fast lookup
    };

private:
    Shape* parent_;
    std::string transition_key_;
    PropertyAttributes transition_attrs_;
    std::unordered_map<std::string, PropertyInfo> properties_;
    uint32_t property_count_;
    uint32_t id_;

    static uint32_t next_shape_id_;

public:
    Shape();
    Shape(Shape* parent, const std::string& key, PropertyAttributes attrs);
    ~Shape() = default;

    // Shape information
    uint32_t get_id() const { return id_; }
    uint32_t get_property_count() const { return property_count_; }
    Shape* get_parent() const { return parent_; }

    // Property lookup
    bool has_property(const std::string& key) const;
    PropertyInfo get_property_info(const std::string& key) const;

    // Shape transitions
    Shape* add_property(const std::string& key, PropertyAttributes attrs);
    Shape* remove_property(const std::string& key);

    // Enumeration
    std::vector<std::string> get_property_keys() const;

    // Debugging
    std::string debug_string() const;

    // Static root shape
    static Shape* get_root_shape();

private:
    void rebuild_property_map();
};

} // namespace Quanta