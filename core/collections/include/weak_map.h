/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include "../../include/Object.h"
#include <unordered_map>
#include <memory>

namespace Quanta {

class Context;

/**
 * WeakMap implementation
 * ES6 WeakMap with object keys only
 */
class WeakMap : public Object {
private:
    std::unordered_map<Object*, Value> entries_;

public:
    WeakMap();
    virtual ~WeakMap() = default;

    // WeakMap operations
    bool has(Object* key) const;
    Value get(Object* key) const;
    void set(Object* key, const Value& value);
    bool delete_key(Object* key);

    // WeakMap built-in methods
    static Value weakmap_constructor(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_set(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_get(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_has(Context& ctx, const std::vector<Value>& args);
    static Value weakmap_delete(Context& ctx, const std::vector<Value>& args);

    // Setup WeakMap prototype
    static void setup_weakmap_prototype(Context& ctx);

    // Static prototype reference
    static Object* prototype_object;
};

} // namespace Quanta