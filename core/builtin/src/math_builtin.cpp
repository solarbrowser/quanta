/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "math_builtin.h"
#include "../include/Context.h"
#include "../include/Object.h"
#include "../include/Value.h"
#include "../../parser/include/AST.h"
#include <cmath>
#include <vector>
#include <memory>
#include <random>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.7182818284590452354
#endif
#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif
#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif
#ifndef M_LOG2E
#define M_LOG2E 1.4426950408889634074
#endif
#ifndef M_LOG10E
#define M_LOG10E 0.43429448190325182765
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

namespace Quanta {

void MathBuiltin::register_math_builtin(Context& ctx) {
    // Create Math object (not a constructor, just an object)
    auto math_object = ObjectFactory::create_object();

    // Add Math constants
    math_object->set_property("PI", Value(M_PI));
    math_object->set_property("E", Value(M_E));
    math_object->set_property("LN2", Value(M_LN2));
    math_object->set_property("LN10", Value(M_LN10));
    math_object->set_property("LOG2E", Value(M_LOG2E));
    math_object->set_property("LOG10E", Value(M_LOG10E));
    math_object->set_property("SQRT1_2", Value(M_SQRT1_2));
    math_object->set_property("SQRT2", Value(M_SQRT2));

    // Add Math methods
    add_math_methods(*math_object);

    // Register Math globally
    ctx.register_built_in_object("Math", math_object.release());
}

void MathBuiltin::add_math_methods(Object& math_obj) {
    // Math.max(...values)
    auto math_max_fn = ObjectFactory::create_native_function("max",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(-std::numeric_limits<double>::infinity());
            }

            double max_val = args[0].to_number();
            for (size_t i = 1; i < args.size(); i++) {
                double val = args[i].to_number();
                if (std::isnan(val)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                if (val > max_val) {
                    max_val = val;
                }
            }
            return Value(max_val);
        });
    math_obj.set_property("max", Value(math_max_fn.release()));

    // Math.min(...values)
    auto math_min_fn = ObjectFactory::create_native_function("min",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::infinity());
            }

            double min_val = args[0].to_number();
            for (size_t i = 1; i < args.size(); i++) {
                double val = args[i].to_number();
                if (std::isnan(val)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                if (val < min_val) {
                    min_val = val;
                }
            }
            return Value(min_val);
        });
    math_obj.set_property("min", Value(math_min_fn.release()));

    // Math.abs(x)
    auto math_abs_fn = ObjectFactory::create_native_function("abs",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            double val = args[0].to_number();
            return Value(std::abs(val));
        });
    math_obj.set_property("abs", Value(math_abs_fn.release()));

    // Math.sqrt(x)
    auto math_sqrt_fn = ObjectFactory::create_native_function("sqrt",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            double val = args[0].to_number();
            return Value(std::sqrt(val));
        });
    math_obj.set_property("sqrt", Value(math_sqrt_fn.release()));

    // Math.pow(x, y)
    auto math_pow_fn = ObjectFactory::create_native_function("pow",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            double base = args[0].to_number();
            double exp = args[1].to_number();
            return Value(std::pow(base, exp));
        });
    math_obj.set_property("pow", Value(math_pow_fn.release()));

    // Math.floor(x)
    auto math_floor_fn = ObjectFactory::create_native_function("floor",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            double val = args[0].to_number();
            return Value(std::floor(val));
        });
    math_obj.set_property("floor", Value(math_floor_fn.release()));

    // Math.ceil(x)
    auto math_ceil_fn = ObjectFactory::create_native_function("ceil",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            double val = args[0].to_number();
            return Value(std::ceil(val));
        });
    math_obj.set_property("ceil", Value(math_ceil_fn.release()));

    // Math.round(x)
    auto math_round_fn = ObjectFactory::create_native_function("round",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            double val = args[0].to_number();
            return Value(std::round(val));
        });
    math_obj.set_property("round", Value(math_round_fn.release()));

    // Math.random()
    auto math_random_fn = ObjectFactory::create_native_function("random",
        [](Context& /* ctx */, const std::vector<Value>& args) -> Value {
            (void)args; // Suppress unused warning
            static std::random_device rd;
            static std::mt19937 gen(rd());
            static std::uniform_real_distribution<> dis(0.0, 1.0);
            return Value(dis(gen));
        });
    math_obj.set_property("random", Value(math_random_fn.release()));

    // TODO: Add more Math methods:
    // - sin, cos, tan, asin, acos, atan, atan2
    // - exp, log, log10, log2
    // - trunc, sign
    // - hypot, cbrt, clz32, fround, imul
}

} // namespace Quanta