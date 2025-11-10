/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

namespace Quanta {

class Context;

/**
 * Global functions registration and implementation
 */
class GlobalBuiltin {
public:
    /**
     * Register all global functions (parseInt, parseFloat, isNaN, etc.)
     */
    static void register_global_functions(Context& ctx);

private:
    // Register parsing functions
    static void register_parse_functions(Context& ctx);

    // Register type checking functions
    static void register_type_check_functions(Context& ctx);

    // Register encoding/decoding functions
    static void register_encoding_functions(Context& ctx);

    // Register eval function
    static void register_eval_function(Context& ctx);
};

} // namespace Quanta