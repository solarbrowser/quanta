/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/engine_builtins.h"
#include "../../include/Engine.h"
#include "../../include/Object.h"
#include <limits>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <iostream>

namespace Quanta {

// Main setup methods
void EngineBuiltins::setup_built_in_functions(Engine& engine) {
    // Register eval() function (originally lines 462-492)
    register_global_function(engine, "eval", [&engine](const std::vector<Value>& args) -> Value {
        return eval_function(args, engine);
    });

    // Register parseInt() function (originally lines 494-550)
    register_global_function(engine, "parseInt", parseInt_function);

    // Register parseFloat() function (originally lines 552-588)
    register_global_function(engine, "parseFloat", parseFloat_function);

    // Register isNaN() function (originally lines 590-598)
    register_global_function(engine, "isNaN", isNaN_function);

    // Register isFinite() function (originally lines 600-608)
    register_global_function(engine, "isFinite", isFinite_function);
}

void EngineBuiltins::setup_built_in_objects(Engine& engine) {
    // Stub - built-in objects like Array, Object etc.
    setup_math_object(engine);
    setup_date_object(engine);
    setup_json_object(engine);
    setup_console_object(engine);
}

void EngineBuiltins::setup_error_types(Engine& engine) {
    // Stub - Error, TypeError, ReferenceError etc.
}

void EngineBuiltins::setup_global_object(Engine& engine) {
    // Stub - global object setup
}

//=============================================================================
// Global Function Implementations (originally lines 462-608)
//=============================================================================

Value EngineBuiltins::eval_function(const std::vector<Value>& args, Engine& engine) {
    if (args.empty()) {
        return Value();
    }

    std::string code = args[0].to_string();
    if (code.empty()) {
        return Value();
    }

    try {
        // Execute the code in the current context
        Engine::Result result = engine.execute(code);
        if (result.success) {
            return result.value;
        } else {
            // For syntax errors, throw directly without EvalError wrapping
            throw std::runtime_error("SyntaxError: " + result.error_message);
        }
    } catch (const std::runtime_error& e) {
        // Check if this is already a SyntaxError - if so, don't wrap it
        std::string error_msg = e.what();
        if (error_msg.find("SyntaxError:") == 0) {
            throw e; // Re-throw as-is
        }
        throw std::runtime_error("EvalError: " + error_msg);
    } catch (const std::exception& e) {
        throw std::runtime_error("EvalError: " + std::string(e.what()));
    }
}

Value EngineBuiltins::parseInt_function(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value::nan();
    }

    std::string str = args[0].to_string();

    // Trim leading whitespace
    size_t start = 0;
    while (start < str.length() && std::isspace(str[start])) {
        start++;
    }

    if (start >= str.length()) {
        return Value::nan();
    }

    // Handle radix parameter
    int radix = 10;
    if (args.size() > 1) {
        double r = args[1].to_number();
        if (r >= 2 && r <= 36) {
            radix = static_cast<int>(r);
        }
    }

    // Check if string starts with a valid digit for the given radix
    char first_char = str[start];
    bool has_valid_start = false;

    if (radix == 16) {
        has_valid_start = std::isdigit(first_char) ||
                        (first_char >= 'a' && first_char <= 'f') ||
                        (first_char >= 'A' && first_char <= 'F');
    } else if (radix == 8) {
        has_valid_start = (first_char >= '0' && first_char <= '7');
    } else {
        has_valid_start = std::isdigit(first_char);
    }

    if (!has_valid_start && first_char != '+' && first_char != '-') {
        return Value::nan();
    }

    try {
        size_t pos;
        long result = std::stol(str.substr(start), &pos, radix);
        // Check if we parsed at least one character
        if (pos == 0) {
            return Value::nan();
        }
        return Value(static_cast<double>(result));
    } catch (...) {
        return Value::nan();
    }
}

Value EngineBuiltins::parseFloat_function(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value::nan();
    }

    std::string str = args[0].to_string();

    // Trim leading whitespace
    size_t start = 0;
    while (start < str.length() && std::isspace(str[start])) {
        start++;
    }

    if (start >= str.length()) {
        return Value::nan();
    }

    // Check if string starts with a valid character for a float
    char first_char = str[start];
    if (!std::isdigit(first_char) && first_char != '.' &&
        first_char != '+' && first_char != '-') {
        return Value::nan();
    }

    try {
        size_t pos;
        double result = std::stod(str.substr(start), &pos);
        // Check if we parsed at least one character
        if (pos == 0) {
            return Value::nan();
        }
        return Value(result);
    } catch (...) {
        return Value::nan();
    }
}

Value EngineBuiltins::isNaN_function(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(true);
    }

    double num = args[0].to_number();
    return Value(std::isnan(num));
}

Value EngineBuiltins::isFinite_function(const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(false);
    }

    double num = args[0].to_number();
    return Value(std::isfinite(num));
}

//=============================================================================
// Built-in Objects Setup
//=============================================================================

void EngineBuiltins::setup_math_object(Engine& engine) {
    // Create Math object
    auto math_obj = std::make_unique<Object>();

    // Math constants
    math_obj->set_property("PI", Value(3.141592653589793));
    math_obj->set_property("E", Value(2.718281828459045));
    math_obj->set_property("LN2", Value(0.6931471805599453));
    math_obj->set_property("LN10", Value(2.302585092994046));
    math_obj->set_property("LOG2E", Value(1.4426950408889634));
    math_obj->set_property("LOG10E", Value(0.4342944819032518));
    math_obj->set_property("SQRT1_2", Value(0.7071067811865476));
    math_obj->set_property("SQRT2", Value(1.4142135623730951));

    engine.set_global_property("Math", Value(math_obj.release()));
}

void EngineBuiltins::setup_date_object(Engine& engine) {
    // Create Date constructor
    auto date_constructor = std::make_unique<Object>(Object::ObjectType::Function);

    engine.set_global_property("Date", Value(date_constructor.release()));
}

void EngineBuiltins::setup_json_object(Engine& engine) {
    // Create JSON object
    auto json_obj = std::make_unique<Object>();

    // JSON methods would be added here
    engine.set_global_property("JSON", Value(json_obj.release()));
}

void EngineBuiltins::setup_console_object(Engine& engine) {
    // Create console object
    auto console_obj = std::make_unique<Object>();

    // Console methods would be added here
    engine.set_global_property("console", Value(console_obj.release()));
}

//=============================================================================
// Helper Functions
//=============================================================================

bool EngineBuiltins::is_valid_radix(int radix) {
    return radix >= 2 && radix <= 36;
}

bool EngineBuiltins::is_valid_digit_for_radix(char c, int radix) {
    if (radix <= 10) {
        return c >= '0' && c < ('0' + radix);
    } else {
        return (c >= '0' && c <= '9') ||
               (c >= 'a' && c < ('a' + radix - 10)) ||
               (c >= 'A' && c < ('A' + radix - 10));
    }
}

void EngineBuiltins::register_global_function(Engine& engine, const std::string& name,
                                            std::function<Value(const std::vector<Value>&)> func) {
    engine.register_function(name, func);
}

} // namespace Quanta