/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/global_functions.h"
#include "../../include/Context.h"
#include "../../include/Object.h"
#include "../../builtin/include/builtin_registry.h"
#include <iostream>
#include <limits>

namespace Quanta {

void GlobalFunctions::initialize_global_scope(Context& ctx) {
    // Basic initialization - use public interface
    if (!ctx.get_global_object()) {
        // Global object should be created by Context constructor
    }

    // Set up global constants
    setup_global_constants(ctx);

    // Register all global functions
    register_global_functions(ctx);

    // Set up console (simplified)
    setup_console_object(ctx);
}

void GlobalFunctions::register_global_functions(Context& ctx) {
    // Register all builtin functions via the builtin registry
    BuiltinRegistry::register_all_builtins(ctx);
}

Object* GlobalFunctions::get_global_object(Context& ctx) {
    return ctx.get_global_object();
}

void GlobalFunctions::set_global_property(Context& ctx, const std::string& name, const Value& value) {
    auto global = ctx.get_global_object();
    if (global) {
        global->set_property(name, value);
    }
}

Value GlobalFunctions::get_global_property(Context& ctx, const std::string& name) {
    auto global = ctx.get_global_object();
    if (global) {
        return global->get_property(name);
    }
    return Value();
}

bool GlobalFunctions::has_global_property(Context& ctx, const std::string& name) {
    auto global = ctx.get_global_object();
    if (global) {
        return global->has_property(name);
    }
    return false;
}

std::vector<std::string> GlobalFunctions::get_global_property_names(Context& ctx) {
    auto global = ctx.get_global_object();
    if (global) {
        return global->get_own_property_keys();
    }
    return {};
}

size_t GlobalFunctions::get_global_property_count(Context& ctx) {
    auto global = ctx.get_global_object();
    if (global) {
        return global->get_own_property_keys().size();
    }
    return 0;
}

void GlobalFunctions::setup_console_object(Context& ctx) {
    // Simplified console object setup - placeholder
    std::cout << "Console object setup (placeholder)" << std::endl;
}

void GlobalFunctions::setup_global_constants(Context& ctx) {
    // Set up basic constants
    set_global_property(ctx, "undefined", Value());
    set_global_property(ctx, "null", Value::null());
    set_global_property(ctx, "Infinity", Value(std::numeric_limits<double>::infinity()));
    set_global_property(ctx, "NaN", Value(std::numeric_limits<double>::quiet_NaN()));
    set_global_property(ctx, "globalThis", Value(ctx.get_global_object()));
}

void GlobalFunctions::setup_global_constructors(Context& ctx) {
    // Placeholder for constructor setup
}

void GlobalFunctions::cleanup_global_scope(Context& ctx) {
    auto global = ctx.get_global_object();
    if (global) {
        auto keys = global->get_own_property_keys();
        for (const auto& key : keys) {
            global->delete_property(key);
        }
    }
}

void GlobalFunctions::reset_global_scope(Context& ctx) {
    cleanup_global_scope(ctx);
    initialize_global_scope(ctx);
}

void GlobalFunctions::register_standard_objects(Context& ctx) {
    // Placeholder
}

void GlobalFunctions::register_error_constructors(Context& ctx) {
    // Placeholder
}

void GlobalFunctions::register_collection_constructors(Context& ctx) {
    // Placeholder - collections will be registered via their setup methods
}

void GlobalFunctions::register_utility_functions(Context& ctx) {
    // Placeholder
}

} // namespace Quanta