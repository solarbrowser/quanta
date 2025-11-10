/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

namespace Quanta {

class Context;
class Object;

/**
 * Math object registration and implementation
 */
class MathBuiltin {
public:
    /**
     * Register Math object and all Math methods
     */
    static void register_math_builtin(Context& ctx);

private:
    // Add Math methods (max, min, abs, sqrt, etc.)
    static void add_math_methods(Object& math_obj);
};

} // namespace Quanta