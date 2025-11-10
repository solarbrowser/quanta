/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

namespace Quanta {

class Context;
class Function;
class Object;

/**
 * Object builtin registration and implementation
 */
class ObjectBuiltin {
public:
    /**
     * Register Object constructor and all Object static methods
     */
    static void register_object_builtin(Context& ctx);

private:
    /**
     * Add Object static methods (keys, values, entries, create, assign, etc.)
     */
    static void add_object_static_methods(Function& constructor);

    /**
     * Add Object.prototype methods (hasOwnProperty, toString, etc.)
     */
    static void add_object_prototype_methods(Object& prototype);
};

} // namespace Quanta