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
 * String builtin registration and implementation
 */
class StringBuiltin {
public:
    /**
     * Register String constructor and all String static methods
     */
    static void register_string_builtin(Context& ctx);

private:
    // Add String static methods
    static void add_string_static_methods(Function& constructor);

    // Add String.prototype methods (charAt, substring, etc.)
    static void add_string_prototype_methods(Object& prototype);
};

} // namespace Quanta