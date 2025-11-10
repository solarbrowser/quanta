/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

namespace Quanta {

class Context;

/**
 * Central registry for all JavaScript builtin objects and functions
 */
class BuiltinRegistry {
public:
    /**
     * Register ALL builtin objects, functions, and prototypes
     */
    static void register_all_builtins(Context& ctx);

private:
    // Individual registration functions
    static void register_object_builtin(Context& ctx);
    static void register_array_builtin(Context& ctx);
    static void register_string_builtin(Context& ctx);
    static void register_function_builtin(Context& ctx);
    static void register_number_builtin(Context& ctx);
    static void register_boolean_builtin(Context& ctx);
    static void register_math_builtin(Context& ctx);
    static void register_error_builtins(Context& ctx);
    static void register_global_functions(Context& ctx);
    static void register_advanced_builtins(Context& ctx); // JSON, Date, RegExp, etc.
};

} // namespace Quanta