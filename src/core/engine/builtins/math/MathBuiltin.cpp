/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/MathBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include <cmath>
#include <limits>
#include <memory>
#include <vector>
#include "quanta/parser/AST.h"

namespace Quanta {

void register_math_builtins(Context& ctx) {
    auto math_object = std::make_unique<Object>();

    PropertyDescriptor pi_desc(Value(3.141592653589793), PropertyAttributes::None);
    math_object->set_property_descriptor("PI", pi_desc);
    PropertyDescriptor e_desc(Value(2.718281828459045), PropertyAttributes::None);
    math_object->set_property_descriptor("E", e_desc);

    static std::vector<std::unique_ptr<Function>> s_math_owned_functions;
    auto store_fn = [](std::unique_ptr<Function> func) -> Function* {
        Function* ptr = func.get();
        s_math_owned_functions.push_back(std::move(func));
        return ptr;
    };

    auto math_max_fn = ObjectFactory::create_native_function("max",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value::negative_infinity();
            }

            double result = -std::numeric_limits<double>::infinity();
            for (const Value& arg : args) {
                if (arg.is_nan()) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                double value = arg.to_number();
                if (std::isnan(value)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                result = std::max(result, value);
            }
            return Value(result);
        }, 0);
    math_object->set_property("max", Value(store_fn(std::move(math_max_fn))), PropertyAttributes::BuiltinFunction);

    auto math_min_fn = ObjectFactory::create_native_function("min",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value::positive_infinity();
            }

            double result = std::numeric_limits<double>::infinity();
            for (const Value& arg : args) {
                if (arg.is_nan()) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                double value = arg.to_number();
                if (std::isnan(value)) {
                    return Value(std::numeric_limits<double>::quiet_NaN());
                }
                result = std::min(result, value);
            }
            return Value(result);
        }, 0);
    math_object->set_property("min", Value(store_fn(std::move(math_min_fn))), PropertyAttributes::BuiltinFunction);

    auto math_round_fn = ObjectFactory::create_native_function("round",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            double value = args[0].to_number();
            return Value(std::round(value));
        }, 1);
    math_object->set_property("round", Value(store_fn(std::move(math_round_fn))), PropertyAttributes::BuiltinFunction);

    auto math_random_fn = ObjectFactory::create_native_function("random",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            return Value(static_cast<double>(rand()) / RAND_MAX);
        }, 0);
    math_object->set_property("random", Value(store_fn(std::move(math_random_fn))), PropertyAttributes::BuiltinFunction);

    auto math_floor_fn = ObjectFactory::create_native_function("floor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::floor(args[0].to_number()));
        }, 1);
    math_object->set_property("floor", Value(store_fn(std::move(math_floor_fn))), PropertyAttributes::BuiltinFunction);

    auto math_ceil_fn = ObjectFactory::create_native_function("ceil",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::ceil(args[0].to_number()));
        }, 1);
    math_object->set_property("ceil", Value(store_fn(std::move(math_ceil_fn))), PropertyAttributes::BuiltinFunction);

    auto math_abs_fn = ObjectFactory::create_native_function("abs",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            double value = args[0].to_number();
            if (std::isinf(value)) {
                return Value::positive_infinity();
            }
            return Value(std::abs(value));
        }, 1);
    math_object->set_property("abs", Value(store_fn(std::move(math_abs_fn))), PropertyAttributes::BuiltinFunction);

    auto math_sqrt_fn = ObjectFactory::create_native_function("sqrt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sqrt(args[0].to_number()));
        }, 1);
    math_object->set_property("sqrt", Value(store_fn(std::move(math_sqrt_fn))), PropertyAttributes::BuiltinFunction);

    auto math_pow_fn = ObjectFactory::create_native_function("pow",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() < 2) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::pow(args[0].to_number(), args[1].to_number()));
        }, 2);
    math_object->set_property("pow", Value(store_fn(std::move(math_pow_fn))), PropertyAttributes::BuiltinFunction);

    auto math_sin_fn = ObjectFactory::create_native_function("sin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sin(args[0].to_number()));
        }, 1);
    math_object->set_property("sin", Value(store_fn(std::move(math_sin_fn))), PropertyAttributes::BuiltinFunction);

    auto math_cos_fn = ObjectFactory::create_native_function("cos",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cos(args[0].to_number()));
        }, 1);
    math_object->set_property("cos", Value(store_fn(std::move(math_cos_fn))), PropertyAttributes::BuiltinFunction);

    auto math_tan_fn = ObjectFactory::create_native_function("tan",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::tan(args[0].to_number()));
        }, 1);
    math_object->set_property("tan", Value(store_fn(std::move(math_tan_fn))), PropertyAttributes::BuiltinFunction);

    auto math_log_fn = ObjectFactory::create_native_function("log",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log(args[0].to_number()));
        }, 1);
    math_object->set_property("log", Value(store_fn(std::move(math_log_fn))), PropertyAttributes::BuiltinFunction);

    auto math_log10_fn = ObjectFactory::create_native_function("log10",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log10(args[0].to_number()));
        }, 1);
    math_object->set_property("log10", Value(store_fn(std::move(math_log10_fn))), PropertyAttributes::BuiltinFunction);

    auto math_exp_fn = ObjectFactory::create_native_function("exp",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::exp(args[0].to_number()));
        }, 1);
    math_object->set_property("exp", Value(store_fn(std::move(math_exp_fn))), PropertyAttributes::BuiltinFunction);

    auto math_trunc_fn = ObjectFactory::create_native_function("trunc",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(0.0);
            double val = args[0].to_number();
            if (std::isinf(val)) return Value(val);
            if (std::isnan(val)) return Value(0.0);
            return Value(std::trunc(val));
        }, 1);
    math_object->set_property("trunc", Value(store_fn(std::move(math_trunc_fn))), PropertyAttributes::BuiltinFunction);

    auto math_sign_fn = ObjectFactory::create_native_function("sign",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(0.0);
            double val = args[0].to_number();
            if (std::isnan(val)) return Value(0.0);
            if (val > 0) return Value(1.0);
            if (val < 0) return Value(-1.0);
            return Value(val);
        }, 1);
    math_object->set_property("sign", Value(store_fn(std::move(math_sign_fn))), PropertyAttributes::BuiltinFunction);

    auto math_acos_fn = ObjectFactory::create_native_function("acos",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::acos(args[0].to_number()));
        }, 1);
    math_object->set_property("acos", Value(store_fn(std::move(math_acos_fn))), PropertyAttributes::BuiltinFunction);

    auto math_acosh_fn = ObjectFactory::create_native_function("acosh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::acosh(args[0].to_number()));
        }, 1);
    math_object->set_property("acosh", Value(store_fn(std::move(math_acosh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_asin_fn = ObjectFactory::create_native_function("asin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::asin(args[0].to_number()));
        }, 1);
    math_object->set_property("asin", Value(store_fn(std::move(math_asin_fn))), PropertyAttributes::BuiltinFunction);

    auto math_asinh_fn = ObjectFactory::create_native_function("asinh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::asinh(args[0].to_number()));
        }, 1);
    math_object->set_property("asinh", Value(store_fn(std::move(math_asinh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_atan_fn = ObjectFactory::create_native_function("atan",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::atan(args[0].to_number()));
        }, 1);
    math_object->set_property("atan", Value(store_fn(std::move(math_atan_fn))), PropertyAttributes::BuiltinFunction);

    auto math_atan2_fn = ObjectFactory::create_native_function("atan2",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() < 2) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::atan2(args[0].to_number(), args[1].to_number()));
        }, 2);
    math_object->set_property("atan2", Value(store_fn(std::move(math_atan2_fn))), PropertyAttributes::BuiltinFunction);

    auto math_atanh_fn = ObjectFactory::create_native_function("atanh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::atanh(args[0].to_number()));
        }, 1);
    math_object->set_property("atanh", Value(store_fn(std::move(math_atanh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_cbrt_fn = ObjectFactory::create_native_function("cbrt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cbrt(args[0].to_number()));
        }, 1);
    math_object->set_property("cbrt", Value(store_fn(std::move(math_cbrt_fn))), PropertyAttributes::BuiltinFunction);

    auto math_clz32_fn = ObjectFactory::create_native_function("clz32",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(32.0);
            uint32_t n = static_cast<uint32_t>(args[0].to_number());
            if (n == 0) return Value(32.0);
            int count = 0;
            for (int i = 31; i >= 0; i--) {
                if (n & (1U << i)) break;
                count++;
            }
            return Value(static_cast<double>(count));
        }, 1);
    math_object->set_property("clz32", Value(store_fn(std::move(math_clz32_fn))), PropertyAttributes::BuiltinFunction);

    auto math_cosh_fn = ObjectFactory::create_native_function("cosh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cosh(args[0].to_number()));
        }, 1);
    math_object->set_property("cosh", Value(store_fn(std::move(math_cosh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_expm1_fn = ObjectFactory::create_native_function("expm1",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::expm1(args[0].to_number()));
        }, 1);
    math_object->set_property("expm1", Value(store_fn(std::move(math_expm1_fn))), PropertyAttributes::BuiltinFunction);

    auto math_fround_fn = ObjectFactory::create_native_function("fround",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(static_cast<double>(static_cast<float>(args[0].to_number())));
        }, 1);
    math_object->set_property("fround", Value(store_fn(std::move(math_fround_fn))), PropertyAttributes::BuiltinFunction);

    auto math_hypot_fn = ObjectFactory::create_native_function("hypot",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            double sum = 0;
            for (const auto& arg : args) {
                double val = arg.to_number();
                sum += val * val;
            }
            return Value(std::sqrt(sum));
        }, 2);
    math_object->set_property("hypot", Value(store_fn(std::move(math_hypot_fn))), PropertyAttributes::BuiltinFunction);

    auto math_imul_fn = ObjectFactory::create_native_function("imul",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() < 2) return Value(0.0);
            int32_t a = static_cast<int32_t>(args[0].to_number());
            int32_t b = static_cast<int32_t>(args[1].to_number());
            return Value(static_cast<double>(a * b));
        }, 2);
    math_object->set_property("imul", Value(store_fn(std::move(math_imul_fn))), PropertyAttributes::BuiltinFunction);

    auto math_log1p_fn = ObjectFactory::create_native_function("log1p",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log1p(args[0].to_number()));
        }, 1);
    math_object->set_property("log1p", Value(store_fn(std::move(math_log1p_fn))), PropertyAttributes::BuiltinFunction);

    auto math_log2_fn = ObjectFactory::create_native_function("log2",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log2(args[0].to_number()));
        }, 1);
    math_object->set_property("log2", Value(store_fn(std::move(math_log2_fn))), PropertyAttributes::BuiltinFunction);

    auto math_sinh_fn = ObjectFactory::create_native_function("sinh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sinh(args[0].to_number()));
        }, 1);
    math_object->set_property("sinh", Value(store_fn(std::move(math_sinh_fn))), PropertyAttributes::BuiltinFunction);

    auto math_tanh_fn = ObjectFactory::create_native_function("tanh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::tanh(args[0].to_number()));
        }, 1);
    math_object->set_property("tanh", Value(store_fn(std::move(math_tanh_fn))), PropertyAttributes::BuiltinFunction);

    PropertyDescriptor ln10_desc(Value(2.302585092994046), PropertyAttributes::None);
    math_object->set_property_descriptor("LN10", ln10_desc);
    PropertyDescriptor ln2_desc(Value(0.6931471805599453), PropertyAttributes::None);
    math_object->set_property_descriptor("LN2", ln2_desc);
    PropertyDescriptor log10e_desc(Value(0.4342944819032518), PropertyAttributes::None);
    math_object->set_property_descriptor("LOG10E", log10e_desc);
    PropertyDescriptor log2e_desc(Value(1.4426950408889634), PropertyAttributes::None);
    math_object->set_property_descriptor("LOG2E", log2e_desc);
    PropertyDescriptor sqrt1_2_desc(Value(0.7071067811865476), PropertyAttributes::None);
    math_object->set_property_descriptor("SQRT1_2", sqrt1_2_desc);
    PropertyDescriptor sqrt2_desc(Value(1.4142135623730951), PropertyAttributes::None);
    math_object->set_property_descriptor("SQRT2", sqrt2_desc);

    PropertyDescriptor math_tag_desc(Value(std::string("Math")), PropertyAttributes::Configurable);
    math_object->set_property_descriptor("Symbol.toStringTag", math_tag_desc);

    ctx.register_built_in_object("Math", math_object.release());
}

} // namespace Quanta
