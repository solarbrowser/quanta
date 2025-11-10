/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include <vector>
#include <string>

namespace Quanta {

class Context;
class Object;

/**
 * Global scope and function management
 * Handles global object setup, property registration, and scope management
 */
class GlobalFunctions {
public:
    /**
     * Initialize the global object with all standard global functions and properties
     */
    static void initialize_global_scope(Context& ctx);

    /**
     * Register all global functions (delegates to various builtin modules)
     */
    static void register_global_functions(Context& ctx);

    /**
     * Global object management
     */
    static Object* get_global_object(Context& ctx);
    static void set_global_property(Context& ctx, const std::string& name, const Value& value);
    static Value get_global_property(Context& ctx, const std::string& name);
    static bool has_global_property(Context& ctx, const std::string& name);

    /**
     * Global scope introspection
     */
    static std::vector<std::string> get_global_property_names(Context& ctx);
    static size_t get_global_property_count(Context& ctx);

    /**
     * Global environment setup
     */
    static void setup_console_object(Context& ctx);
    static void setup_global_constants(Context& ctx);
    static void setup_global_constructors(Context& ctx);

    /**
     * Global scope cleanup and reset
     */
    static void cleanup_global_scope(Context& ctx);
    static void reset_global_scope(Context& ctx);

private:
    /**
     * Helper functions for specific global object categories
     */
    static void register_standard_objects(Context& ctx);
    static void register_error_constructors(Context& ctx);
    static void register_collection_constructors(Context& ctx);
    static void register_utility_functions(Context& ctx);
};

} // namespace Quanta