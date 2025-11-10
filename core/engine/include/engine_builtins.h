/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Engine.h"
#include "../../include/Value.h"
#include <vector>
#include <functional>

namespace Quanta {

/**
 * Engine Built-ins - Built-in functions and objects setup
 * EXTRACTED FROM Engine.cpp - Built-in JavaScript functions and objects
 */
class EngineBuiltins {
public:
    // Setup methods
    static void setup_built_in_functions(Engine& engine);
    static void setup_built_in_objects(Engine& engine);
    static void setup_error_types(Engine& engine);
    static void setup_global_object(Engine& engine);

    // Global function implementations
    static Value eval_function(const std::vector<Value>& args, Engine& engine);
    static Value parseInt_function(const std::vector<Value>& args);
    static Value parseFloat_function(const std::vector<Value>& args);
    static Value isNaN_function(const std::vector<Value>& args);
    static Value isFinite_function(const std::vector<Value>& args);

    // Built-in objects
    static void setup_math_object(Engine& engine);
    static void setup_date_object(Engine& engine);
    static void setup_json_object(Engine& engine);
    static void setup_console_object(Engine& engine);

private:
    // Helper functions
    static bool is_valid_radix(int radix);
    static bool is_valid_digit_for_radix(char c, int radix);
    static void register_global_function(Engine& engine, const std::string& name,
                                       std::function<Value(const std::vector<Value>&)> func);
};

} // namespace Quanta