/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Context.h"
#include "../../include/Value.h"
#include <string>
#include <vector>

namespace Quanta {

class Environment;

/**
 * Context Bindings - Variable binding management
 * EXTRACTED FROM Context.cpp - Variable binding functionality
 */
class ContextBindings {
public:
    // Binding operations
    static bool has_binding(const Context& ctx, const std::string& name);
    static Value get_binding(Context& ctx, const std::string& name);
    static bool set_binding(Context& ctx, const std::string& name, const Value& value);
    static bool delete_binding(Context& ctx, const std::string& name);

    // Binding creation
    static bool create_binding(Context& ctx, const std::string& name, const Value& value, bool mutable_binding = true);
    static bool create_var_binding(Context& ctx, const std::string& name, const Value& value, bool mutable_binding = true);
    static bool create_lexical_binding(Context& ctx, const std::string& name, const Value& value, bool mutable_binding = true);

    // Environment management
    static void set_lexical_environment(Context& ctx, Environment* env);
    static Environment* get_lexical_environment(const Context& ctx);
    static void set_variable_environment(Context& ctx, Environment* env);
    static Environment* get_variable_environment(const Context& ctx);

    // Environment stack operations
    static void push_lexical_environment(Context& ctx, Environment* env);
    static Environment* pop_lexical_environment(Context& ctx);

    // This binding
    static void set_this_binding(Context& ctx, Object* this_obj);
    static Object* get_this_binding(const Context& ctx);

    // Debug and introspection
    static std::vector<std::string> get_all_binding_names(const Context& ctx);
    static void print_all_bindings(const Context& ctx);
    static bool is_binding_mutable(const Context& ctx, const std::string& name);
    static bool is_binding_initialized(const Context& ctx, const std::string& name);
};

} // namespace Quanta