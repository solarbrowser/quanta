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
 * Array builtin registration and implementation
 */
class ArrayBuiltin {
public:
    /**
     * Register Array constructor and all Array static methods
     */
    static void register_array_builtin(Context& ctx);

private:
    // Add Array static methods (Array.from, Array.of, Array.isArray, etc.)
    static void add_array_static_methods(Function& constructor);

    // Add Array.prototype methods (push, pop, slice, etc.)
    static void add_array_prototype_methods(Object& prototype);
};

} // namespace Quanta