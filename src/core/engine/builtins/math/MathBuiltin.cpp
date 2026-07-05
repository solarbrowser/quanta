/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/engine/builtins/MathBuiltin.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Symbol.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <cstring>
#include <memory>
#include <random>
#include <vector>
#include "quanta/parser/AST.h"

namespace Quanta {

void register_math_builtins(Context& ctx) {
    auto math_object = ObjectFactory::create_object();

    PropertyDescriptor pi_desc(Value(3.141592653589793), PropertyAttributes::None);
    math_object->set_property_descriptor("PI", pi_desc);
    PropertyDescriptor e_desc(Value(2.718281828459045), PropertyAttributes::None);
    math_object->set_property_descriptor("E", e_desc);

    auto math_max_fn = ObjectFactory::create_native_function("max",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Every argument is coerced before any NaN short-circuits.
            std::vector<double> coerced;
            coerced.reserve(args.size());
            for (const Value& arg : args) {
                coerced.push_back(arg.to_number());
                if (ctx.has_exception()) return Value();
            }
            double result = -std::numeric_limits<double>::infinity();
            for (double value : coerced) {
                if (std::isnan(value)) return Value(std::numeric_limits<double>::quiet_NaN());
                if (value > result || (value == 0.0 && result == 0.0 &&
                                       std::signbit(result) && !std::signbit(value))) {
                    result = value;
                }
            }
            return Value(result);
        }, 2);
    math_object->set_property("max", Value(math_max_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_min_fn = ObjectFactory::create_native_function("min",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            std::vector<double> coerced;
            coerced.reserve(args.size());
            for (const Value& arg : args) {
                coerced.push_back(arg.to_number());
                if (ctx.has_exception()) return Value();
            }
            double result = std::numeric_limits<double>::infinity();
            for (double value : coerced) {
                if (std::isnan(value)) return Value(std::numeric_limits<double>::quiet_NaN());
                if (value < result || (value == 0.0 && result == 0.0 &&
                                       !std::signbit(result) && std::signbit(value))) {
                    result = value;
                }
            }
            return Value(result);
        }, 2);
    math_object->set_property("min", Value(math_min_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_round_fn = ObjectFactory::create_native_function("round",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }

            double value = args[0].to_number();
            if (ctx.has_exception()) return Value();
            if (std::isnan(value) || std::isinf(value) || value == 0.0) return Value(value);
            // Halfway cases round toward +Infinity; computed without the x+0.5
            // precision loss (e.g. round(0.49999999999999994) must be 0).
            double f = std::floor(value);
            double result = (value - f >= 0.5) ? f + 1.0 : f;
            if (result == 0.0 && std::signbit(value)) return Value(-0.0);
            return Value(result);
        }, 1);
    math_object->set_property("round", Value(math_round_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_random_fn = ObjectFactory::create_native_function("random",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            (void)args;
            static thread_local std::mt19937 gen{std::random_device{}()};
            static thread_local std::uniform_real_distribution<double> dis(0.0, 1.0);
            return Value(dis(gen));
        }, 0);
    math_object->set_property("random", Value(math_random_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_floor_fn = ObjectFactory::create_native_function("floor",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::floor(args[0].to_number()));
        }, 1);
    math_object->set_property("floor", Value(math_floor_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_ceil_fn = ObjectFactory::create_native_function("ceil",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::ceil(args[0].to_number()));
        }, 1);
    math_object->set_property("ceil", Value(math_ceil_fn.release()), PropertyAttributes::BuiltinFunction);

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
    math_object->set_property("abs", Value(math_abs_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_sqrt_fn = ObjectFactory::create_native_function("sqrt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sqrt(args[0].to_number()));
        }, 1);
    math_object->set_property("sqrt", Value(math_sqrt_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_pow_fn = ObjectFactory::create_native_function("pow",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            double base = (args.empty() ? Value() : args[0]).to_number();
            if (ctx.has_exception()) return Value();
            double exp = (args.size() > 1 ? args[1] : Value()).to_number();
            if (ctx.has_exception()) return Value();
            // JS diverges from C99 pow: NaN exponent always yields NaN (even for
            // base 1), and (+-1) ** +-Infinity is NaN rather than 1.
            if (std::isnan(exp)) return Value(exp);
            if (std::isinf(exp) && (base == 1.0 || base == -1.0)) {
                return Value(std::numeric_limits<double>::quiet_NaN());
            }
            return Value(std::pow(base, exp));
        }, 2);
    math_object->set_property("pow", Value(math_pow_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_sin_fn = ObjectFactory::create_native_function("sin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sin(args[0].to_number()));
        }, 1);
    math_object->set_property("sin", Value(math_sin_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_cos_fn = ObjectFactory::create_native_function("cos",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cos(args[0].to_number()));
        }, 1);
    math_object->set_property("cos", Value(math_cos_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_tan_fn = ObjectFactory::create_native_function("tan",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::tan(args[0].to_number()));
        }, 1);
    math_object->set_property("tan", Value(math_tan_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_log_fn = ObjectFactory::create_native_function("log",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log(args[0].to_number()));
        }, 1);
    math_object->set_property("log", Value(math_log_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_log10_fn = ObjectFactory::create_native_function("log10",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log10(args[0].to_number()));
        }, 1);
    math_object->set_property("log10", Value(math_log10_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_exp_fn = ObjectFactory::create_native_function("exp",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::exp(args[0].to_number()));
        }, 1);
    math_object->set_property("exp", Value(math_exp_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_trunc_fn = ObjectFactory::create_native_function("trunc",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            double val = args[0].to_number();
            if (std::isnan(val) || std::isinf(val)) return Value(val);
            return Value(std::trunc(val));
        }, 1);
    math_object->set_property("trunc", Value(math_trunc_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_sign_fn = ObjectFactory::create_native_function("sign",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            double val = args[0].to_number();
            if (std::isnan(val)) return Value(val);
            if (val > 0) return Value(1.0);
            if (val < 0) return Value(-1.0);
            return Value(val);
        }, 1);
    math_object->set_property("sign", Value(math_sign_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_acos_fn = ObjectFactory::create_native_function("acos",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::acos(args[0].to_number()));
        }, 1);
    math_object->set_property("acos", Value(math_acos_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_acosh_fn = ObjectFactory::create_native_function("acosh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::acosh(args[0].to_number()));
        }, 1);
    math_object->set_property("acosh", Value(math_acosh_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_asin_fn = ObjectFactory::create_native_function("asin",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::asin(args[0].to_number()));
        }, 1);
    math_object->set_property("asin", Value(math_asin_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_asinh_fn = ObjectFactory::create_native_function("asinh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::asinh(args[0].to_number()));
        }, 1);
    math_object->set_property("asinh", Value(math_asinh_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_atan_fn = ObjectFactory::create_native_function("atan",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::atan(args[0].to_number()));
        }, 1);
    math_object->set_property("atan", Value(math_atan_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_atan2_fn = ObjectFactory::create_native_function("atan2",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() < 2) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::atan2(args[0].to_number(), args[1].to_number()));
        }, 2);
    math_object->set_property("atan2", Value(math_atan2_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_atanh_fn = ObjectFactory::create_native_function("atanh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::atanh(args[0].to_number()));
        }, 1);
    math_object->set_property("atanh", Value(math_atanh_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_cbrt_fn = ObjectFactory::create_native_function("cbrt",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cbrt(args[0].to_number()));
        }, 1);
    math_object->set_property("cbrt", Value(math_cbrt_fn.release()), PropertyAttributes::BuiltinFunction);

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
    math_object->set_property("clz32", Value(math_clz32_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_cosh_fn = ObjectFactory::create_native_function("cosh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::cosh(args[0].to_number()));
        }, 1);
    math_object->set_property("cosh", Value(math_cosh_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_expm1_fn = ObjectFactory::create_native_function("expm1",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::expm1(args[0].to_number()));
        }, 1);
    math_object->set_property("expm1", Value(math_expm1_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_fround_fn = ObjectFactory::create_native_function("fround",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(static_cast<double>(static_cast<float>(args[0].to_number())));
        }, 1);
    math_object->set_property("fround", Value(math_fround_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_hypot_fn = ObjectFactory::create_native_function("hypot",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Coerce all args first (propagating exceptions), then check Infinity/NaN.
            std::vector<double> nums;
            for (const auto& arg : args) {
                double val = arg.to_number();
                if (ctx.has_exception()) return Value();
                nums.push_back(val);
            }
            bool has_nan = false;
            for (double v : nums) {
                if (std::isinf(v)) return Value(std::numeric_limits<double>::infinity());
                if (std::isnan(v)) has_nan = true;
            }
            if (has_nan) return Value(std::numeric_limits<double>::quiet_NaN());
            double sum = 0;
            for (double v : nums) sum += v * v;
            return Value(std::sqrt(sum));
        }, 2);
    math_object->set_property("hypot", Value(math_hypot_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_imul_fn = ObjectFactory::create_native_function("imul",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() < 2) return Value(0.0);
            // ToUint32 via truncation mod 2^32, then treat as signed for multiply.
            auto toU32 = [](double d) -> uint32_t {
                if (!std::isfinite(d) || d == 0.0) return 0;
                return static_cast<uint32_t>(static_cast<int64_t>(d) & 0xFFFFFFFFLL);
            };
            uint32_t a = toU32(args[0].to_number());
            uint32_t b = toU32(args[1].to_number());
            return Value(static_cast<double>(static_cast<int32_t>(a * b)));
        }, 2);
    math_object->set_property("imul", Value(math_imul_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_log1p_fn = ObjectFactory::create_native_function("log1p",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log1p(args[0].to_number()));
        }, 1);
    math_object->set_property("log1p", Value(math_log1p_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_log2_fn = ObjectFactory::create_native_function("log2",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::log2(args[0].to_number()));
        }, 1);
    math_object->set_property("log2", Value(math_log2_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_sinh_fn = ObjectFactory::create_native_function("sinh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::sinh(args[0].to_number()));
        }, 1);
    math_object->set_property("sinh", Value(math_sinh_fn.release()), PropertyAttributes::BuiltinFunction);

    auto math_tanh_fn = ObjectFactory::create_native_function("tanh",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            return Value(std::tanh(args[0].to_number()));
        }, 1);
    math_object->set_property("tanh", Value(math_tanh_fn.release()), PropertyAttributes::BuiltinFunction);

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

    // ES2024: Math.f16round -- round to nearest-even IEEE 754 float16 value.
    auto math_f16round_fn = ObjectFactory::create_native_function("f16round",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
            double x = args[0].to_number();
            if (ctx.has_exception()) return Value();
            if (std::isnan(x) || std::isinf(x) || x == 0.0) return Value(x);
            double a = std::fabs(x);
            double sign = std::signbit(x) ? -1.0 : 1.0;
            // Beyond the max-half/infinity midpoint (65504 + 16) everything rounds to Infinity.
            if (a >= 65520.0) return Value(sign * std::numeric_limits<double>::infinity());
            double result;
            if (a < std::ldexp(1.0, -14)) {
                // Subnormal half: quantum 2^-24; scaling by powers of two is exact,
                // nearbyint gives the required round-to-nearest-even in one rounding.
                result = std::ldexp(std::nearbyint(std::ldexp(a, 24)), -24);
            } else {
                int e;
                std::frexp(a, &e);
                --e;
                result = std::ldexp(std::nearbyint(std::ldexp(a, 10 - e)), e - 10);
            }
            if (result == 0.0) return Value(sign * 0.0);
            return Value(sign * result);
        }, 1);
    math_object->set_property("f16round", Value(math_f16round_fn.release()), PropertyAttributes::BuiltinFunction);

    // Math.sumPrecise: exact summation via a fixed-point superaccumulator wide
    // enough for the full double exponent range, correctly rounded at the end.
    auto math_sumPrecise_fn = ObjectFactory::create_native_function("sumPrecise",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Value iterable = args.empty() ? Value() : args[0];
            Object* obj = iterable.is_object() ? iterable.as_object()
                        : iterable.is_function() ? static_cast<Object*>(iterable.as_function()) : nullptr;
            if (!obj) { ctx.throw_type_error("Math.sumPrecise requires an iterable"); return Value(); }
            Symbol* iter_sym = Symbol::get_well_known(Symbol::ITERATOR);
            Value iter_fn = iter_sym ? obj->get_property(iter_sym->to_property_key()) : Value();
            if (ctx.has_exception()) return Value();
            if (!iter_fn.is_function()) { ctx.throw_type_error("Math.sumPrecise requires an iterable"); return Value(); }
            Value iterator = iter_fn.as_function()->call(ctx, {}, iterable);
            if (ctx.has_exception()) return Value();
            Object* it = iterator.is_object() ? iterator.as_object() : nullptr;
            if (!it) { ctx.throw_type_error("iterator is not an object"); return Value(); }
            Value next_fn = it->get_property("next");
            if (ctx.has_exception()) return Value();
            if (!next_fn.is_function()) { ctx.throw_type_error("iterator has no next method"); return Value(); }

            // IteratorClose: preserves the pending exception across the return() call.
            auto close_iterator = [&](Object* iter_obj) {
                bool had = ctx.has_exception();
                Value saved = ctx.get_exception();
                ctx.clear_exception();
                Value ret = iter_obj->get_property("return");
                if (!ctx.has_exception() && ret.is_function()) {
                    ret.as_function()->call(ctx, {}, Value(iter_obj));
                }
                ctx.clear_exception();
                if (had) ctx.throw_exception(saved, true);
            };

            // Bit b of the accumulator has weight 2^(b-1074); 70 words cover the
            // whole finite range plus carry headroom. Words carry signed partial
            // sums in base 2^32, normalized during final extraction.
            constexpr int kWords = 70;
            int64_t acc[kWords] = {0};
            bool all_minus_zero = true;
            bool seen_nan = false, seen_pos_inf = false, seen_neg_inf = false;

            while (true) {
                Value res = next_fn.as_function()->call(ctx, {}, iterator);
                if (ctx.has_exception()) return Value();
                if (!res.is_object()) { ctx.throw_type_error("iterator result is not an object"); return Value(); }
                Value done = res.as_object()->get_property("done");
                if (ctx.has_exception()) return Value();
                if (done.to_boolean()) break;
                Value v = res.as_object()->get_property("value");
                if (ctx.has_exception()) return Value();
                if (!v.is_number()) {
                    ctx.throw_type_error("Math.sumPrecise: all elements must be numbers");
                    close_iterator(it);
                    return Value();
                }
                double d = v.as_number();
                if (std::isnan(d)) { seen_nan = true; continue; }
                if (std::isinf(d)) { (d > 0 ? seen_pos_inf : seen_neg_inf) = true; all_minus_zero = false; continue; }
                if (d == 0.0) { if (!std::signbit(d)) all_minus_zero = false; continue; }
                all_minus_zero = false;

                uint64_t bits;
                std::memcpy(&bits, &d, 8);
                uint64_t mant = bits & 0x000FFFFFFFFFFFFFULL;
                int biased = static_cast<int>((bits >> 52) & 0x7FF);
                int shift;
                if (biased == 0) { shift = 0; } else { mant |= (1ULL << 52); shift = biased - 1; }
                int64_t s = (bits >> 63) ? -1 : 1;
                int word = shift / 32, off = shift % 32;
                unsigned __int128 wide = static_cast<unsigned __int128>(mant) << off;
                for (int k = 0; k < 3; ++k) {
                    acc[word + k] += s * static_cast<int64_t>(static_cast<uint32_t>(wide >> (32 * k)));
                }
            }

            if (seen_nan || (seen_pos_inf && seen_neg_inf)) return Value(std::numeric_limits<double>::quiet_NaN());
            if (seen_pos_inf) return Value(std::numeric_limits<double>::infinity());
            if (seen_neg_inf) return Value(-std::numeric_limits<double>::infinity());
            if (all_minus_zero) return Value(-0.0);

            // Normalize to words in [0, 2^32); a negative final carry means the sum is negative.
            int64_t carry = 0;
            for (int i = 0; i < kWords; ++i) {
                int64_t w = acc[i] + carry;
                carry = w >> 32;
                acc[i] = w & 0xFFFFFFFFLL;
            }
            double sign = 1.0;
            if (carry < 0) {
                sign = -1.0;
                int64_t borrow = 0;
                for (int i = 0; i < kWords; ++i) {
                    int64_t w = -acc[i] + borrow;
                    borrow = w >> 32;
                    acc[i] = w & 0xFFFFFFFFLL;
                }
            }

            int top = kWords - 1;
            while (top >= 0 && acc[top] == 0) --top;
            if (top < 0) return Value(0.0);
            int msb = 31;
            while (!((acc[top] >> msb) & 1)) --msb;
            long long p = top * 32 + msb;

            auto bit_at = [&](long long b) -> int {
                if (b < 0) return 0;
                return static_cast<int>((acc[b / 32] >> (b % 32)) & 1);
            };
            uint64_t mant = 0;
            for (long long b = p; b > p - 53 && b >= 0; --b) mant = (mant << 1) | bit_at(b);
            long long low = p - 52;
            if (low > 0) {
                int round = bit_at(low - 1);
                bool sticky = false;
                for (long long b = low - 2; b >= 0 && !sticky; --b) sticky = bit_at(b);
                if (round && (sticky || (mant & 1))) ++mant;
            } else {
                // Fewer than 53 significant bits: the value is exact as-is.
                low = 0;
                mant = 0;
                for (long long b = p; b >= 0; --b) mant = (mant << 1) | bit_at(b);
            }
            return Value(sign * std::ldexp(static_cast<double>(mant), static_cast<int>(low) - 1074));
        }, 1);
    math_object->set_property("sumPrecise", Value(math_sumPrecise_fn.release()), PropertyAttributes::BuiltinFunction);

    ctx.register_built_in_object("Math", math_object.release());
}

} // namespace Quanta
