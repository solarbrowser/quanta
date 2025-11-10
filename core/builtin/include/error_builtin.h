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
 * Error builtin registration and implementation
 */
class ErrorBuiltin {
public:
    /**
     * Register Error and all error types (TypeError, ReferenceError, etc.)
     */
    static void register_error_builtins(Context& ctx);

private:
    // Create Error constructor
    static void register_error_constructor(Context& ctx);

    // Create TypeError constructor
    static void register_type_error_constructor(Context& ctx);

    // Create ReferenceError constructor
    static void register_reference_error_constructor(Context& ctx);

    // Create SyntaxError constructor
    static void register_syntax_error_constructor(Context& ctx);

    // Create RangeError constructor
    static void register_range_error_constructor(Context& ctx);

    // Add Error.prototype methods
    static void add_error_prototype_methods(Object& prototype);
};

} // namespace Quanta