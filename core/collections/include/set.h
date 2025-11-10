/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include "../../include/Object.h"
#include <vector>
#include <memory>

namespace Quanta {

class Context;

/**
 * JavaScript Set implementation
 * ES6 Set with proper value equality semantics
 */
class Set : public Object {
private:
    std::vector<Value> values_;
    size_t size_;

public:
    Set();
    virtual ~Set() = default;

    // Set operations
    bool has(const Value& value) const;
    void add(const Value& value);
    bool delete_value(const Value& value);
    void clear();

    // Set properties
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // Override get_property to handle size property
    Value get_property(const std::string& key) const override;

    // Iterator support
    std::vector<Value> values() const;
    std::vector<std::pair<Value, Value>> entries() const; // [value, value] pairs

    // Set built-in methods
    static Value set_constructor(Context& ctx, const std::vector<Value>& args);
    static Value set_add(Context& ctx, const std::vector<Value>& args);
    static Value set_has(Context& ctx, const std::vector<Value>& args);
    static Value set_delete(Context& ctx, const std::vector<Value>& args);
    static Value set_clear(Context& ctx, const std::vector<Value>& args);
    static Value set_size_getter(Context& ctx, const std::vector<Value>& args);
    static Value set_values(Context& ctx, const std::vector<Value>& args);
    static Value set_keys(Context& ctx, const std::vector<Value>& args); // Alias for values
    static Value set_entries(Context& ctx, const std::vector<Value>& args);
    static Value set_forEach(Context& ctx, const std::vector<Value>& args);
    static Value set_iterator_method(Context& ctx, const std::vector<Value>& args);

    // Setup Set prototype
    static void setup_set_prototype(Context& ctx);

    // Static prototype reference
    static Object* prototype_object;

private:
    std::vector<Value>::iterator find_value(const Value& value);
    std::vector<Value>::const_iterator find_value(const Value& value) const;
};

} // namespace Quanta