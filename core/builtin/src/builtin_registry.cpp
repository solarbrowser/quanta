/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "builtin_registry.h"
#include "../include/Context.h"
#include "object_builtin.h"
#include "array_builtin.h"
#include "string_builtin.h"
#include "math_builtin.h"
#include "error_builtin.h"
#include "function_builtin.h"
#include "global_builtin.h"

namespace Quanta {

void BuiltinRegistry::register_all_builtins(Context& ctx) {
    // TODO: Replace these with calls to individual modules
    // For now, keep calling the old monolithic method

    // Core object types
    register_object_builtin(ctx);
    register_array_builtin(ctx);
    register_string_builtin(ctx);
    register_function_builtin(ctx);
    register_number_builtin(ctx);
    register_boolean_builtin(ctx);

    // Math and utilities
    register_math_builtin(ctx);
    register_error_builtins(ctx);
    register_global_functions(ctx);

    // Advanced types
    register_advanced_builtins(ctx);
}

void BuiltinRegistry::register_object_builtin(Context& ctx) {
    // Use the new Object builtin module
    ObjectBuiltin::register_object_builtin(ctx);
}

void BuiltinRegistry::register_array_builtin(Context& ctx) {
    // Use the new Array builtin module
    ArrayBuiltin::register_array_builtin(ctx);
}

void BuiltinRegistry::register_string_builtin(Context& ctx) {
    // Use the new String builtin module
    StringBuiltin::register_string_builtin(ctx);
}

void BuiltinRegistry::register_function_builtin(Context& ctx) {
    // Use the new Function builtin module
    FunctionBuiltin::register_function_builtin(ctx);
}

void BuiltinRegistry::register_number_builtin(Context& ctx) {
    // TODO: Move Number constructor code here from Context.cpp
    // For now, this is a placeholder
}

void BuiltinRegistry::register_boolean_builtin(Context& ctx) {
    // TODO: Move Boolean constructor code here from Context.cpp
    // For now, this is a placeholder
}

void BuiltinRegistry::register_math_builtin(Context& ctx) {
    // Use the new Math builtin module
    MathBuiltin::register_math_builtin(ctx);
}

void BuiltinRegistry::register_error_builtins(Context& ctx) {
    // Use the new Error builtin module
    ErrorBuiltin::register_error_builtins(ctx);
}

void BuiltinRegistry::register_global_functions(Context& ctx) {
    // Use the new Global builtin module
    GlobalBuiltin::register_global_functions(ctx);
}

void BuiltinRegistry::register_advanced_builtins(Context& ctx) {
    // TODO: Move JSON, Date, RegExp, Promise, etc. here from Context.cpp
    // For now, this is a placeholder
}

} // namespace Quanta