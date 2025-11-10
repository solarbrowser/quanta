/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include "../../include/Object.h"
#include <unordered_set>
#include <memory>

namespace Quanta {

class Context;

/**
 * WeakSet implementation
 * ES6 WeakSet with object values only
 */
class WeakSet : public Object {
private:
    std::unordered_set<Object*> values_;

public:
    WeakSet();
    virtual ~WeakSet() = default;

    // WeakSet operations
    bool has(Object* value) const;
    void add(Object* value);
    bool delete_value(Object* value);

    // WeakSet built-in methods
    static Value weakset_constructor(Context& ctx, const std::vector<Value>& args);
    static Value weakset_add(Context& ctx, const std::vector<Value>& args);
    static Value weakset_has(Context& ctx, const std::vector<Value>& args);
    static Value weakset_delete(Context& ctx, const std::vector<Value>& args);

    // Setup WeakSet prototype
    static void setup_weakset_prototype(Context& ctx);

    // Static prototype reference
    static Object* prototype_object;
};

} // namespace Quanta