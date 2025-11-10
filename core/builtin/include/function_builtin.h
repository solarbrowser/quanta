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
 * Function builtin registration and implementation
 */
class FunctionBuiltin {
public:
    /**
     * Register Function constructor and Function.prototype methods
     */
    static void register_function_builtin(Context& ctx);

private:
    // Add Function.prototype methods (call, apply, bind)
    static void add_function_prototype_methods(Object& prototype);
};

} // namespace Quanta