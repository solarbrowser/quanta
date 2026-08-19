/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_MATH_H
#define QUANTA_MATH_H

#include "quanta/core/runtime/Value.h"
#include <span>
#include "quanta/core/runtime/Object.h"
#include <memory>

namespace Quanta {

class Context;

class Math {
public:
    static constexpr double E = 2.718281828459045;
    static constexpr double LN2 = 0.6931471805599453;
    static constexpr double LN10 = 2.302585092994046;
    static constexpr double LOG2E = 1.4426950408889634;
    static constexpr double LOG10E = 0.4342944819032518;
    static constexpr double PI = 3.141592653589793;
    static constexpr double SQRT1_2 = 0.7071067811865476;
    static constexpr double SQRT2 = 1.4142135623730951;

    static Value abs(Context& ctx, std::span<const Value> args, Value receiver);
    static Value acos(Context& ctx, std::span<const Value> args, Value receiver);
    static Value asin(Context& ctx, std::span<const Value> args, Value receiver);
    static Value atan(Context& ctx, std::span<const Value> args, Value receiver);
    static Value atan2(Context& ctx, std::span<const Value> args, Value receiver);
    static Value ceil(Context& ctx, std::span<const Value> args, Value receiver);
    static Value cos(Context& ctx, std::span<const Value> args, Value receiver);
    static Value exp(Context& ctx, std::span<const Value> args, Value receiver);
    static Value floor(Context& ctx, std::span<const Value> args, Value receiver);
    static Value log(Context& ctx, std::span<const Value> args, Value receiver);
    static Value max(Context& ctx, std::span<const Value> args, Value receiver);
    static Value min(Context& ctx, std::span<const Value> args, Value receiver);
    static Value pow(Context& ctx, std::span<const Value> args, Value receiver);
    static Value random(Context& ctx, std::span<const Value> args, Value receiver);
    static Value round(Context& ctx, std::span<const Value> args, Value receiver);
    static Value sin(Context& ctx, std::span<const Value> args, Value receiver);
    static Value sqrt(Context& ctx, std::span<const Value> args, Value receiver);
    static Value tan(Context& ctx, std::span<const Value> args, Value receiver);
    
    static Value trunc(Context& ctx, std::span<const Value> args, Value receiver);
    static Value sign(Context& ctx, std::span<const Value> args, Value receiver);
    static Value cbrt(Context& ctx, std::span<const Value> args, Value receiver);
    static Value hypot(Context& ctx, std::span<const Value> args, Value receiver);
    static Value clz32(Context& ctx, std::span<const Value> args, Value receiver);
    static Value imul(Context& ctx, std::span<const Value> args, Value receiver);
    
    static Value sumPrecise(Context& ctx, std::span<const Value> args, Value receiver);
    static Value f16round(Context& ctx, std::span<const Value> args, Value receiver);
    static Value log10(Context& ctx, std::span<const Value> args, Value receiver);
    static Value log2(Context& ctx, std::span<const Value> args, Value receiver);
    static Value log1p(Context& ctx, std::span<const Value> args, Value receiver);
    static Value expm1(Context& ctx, std::span<const Value> args, Value receiver);
    static Value acosh(Context& ctx, std::span<const Value> args, Value receiver);
    static Value asinh(Context& ctx, std::span<const Value> args, Value receiver);
    static Value atanh(Context& ctx, std::span<const Value> args, Value receiver);
    static Value cosh(Context& ctx, std::span<const Value> args, Value receiver);
    static Value sinh(Context& ctx, std::span<const Value> args, Value receiver);
    static Value tanh(Context& ctx, std::span<const Value> args, Value receiver);
    
    static std::unique_ptr<Object> create_math_object();

private:
    static double safe_to_number(const Value& value);
    static bool is_finite_number(double value);
    static bool is_integer(double value);
    
    static bool random_initialized_;
    static void initialize_random();
};

}

#endif
