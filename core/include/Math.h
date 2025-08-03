#ifndef QUANTA_MATH_H
#define QUANTA_MATH_H

#include "Value.h"
#include "Object.h"
#include <memory>

namespace Quanta {

// Forward declarations
class Context;

/**
 * JavaScript Math object implementation
 * Provides all standard Math methods and constants
 */
class Math {
public:
    // Math constants
    static constexpr double E = 2.718281828459045;
    static constexpr double LN2 = 0.6931471805599453;
    static constexpr double LN10 = 2.302585092994046;
    static constexpr double LOG2E = 1.4426950408889634;
    static constexpr double LOG10E = 0.4342944819032518;
    static constexpr double PI = 3.141592653589793;
    static constexpr double SQRT1_2 = 0.7071067811865476;
    static constexpr double SQRT2 = 1.4142135623730951;

    // Math methods
    static Value abs(Context& ctx, const std::vector<Value>& args);
    static Value acos(Context& ctx, const std::vector<Value>& args);
    static Value asin(Context& ctx, const std::vector<Value>& args);
    static Value atan(Context& ctx, const std::vector<Value>& args);
    static Value atan2(Context& ctx, const std::vector<Value>& args);
    static Value ceil(Context& ctx, const std::vector<Value>& args);
    static Value cos(Context& ctx, const std::vector<Value>& args);
    static Value exp(Context& ctx, const std::vector<Value>& args);
    static Value floor(Context& ctx, const std::vector<Value>& args);
    static Value log(Context& ctx, const std::vector<Value>& args);
    static Value max(Context& ctx, const std::vector<Value>& args);
    static Value min(Context& ctx, const std::vector<Value>& args);
    static Value pow(Context& ctx, const std::vector<Value>& args);
    static Value random(Context& ctx, const std::vector<Value>& args);
    static Value round(Context& ctx, const std::vector<Value>& args);
    static Value sin(Context& ctx, const std::vector<Value>& args);
    static Value sqrt(Context& ctx, const std::vector<Value>& args);
    static Value tan(Context& ctx, const std::vector<Value>& args);
    
    // ES6+ Math methods
    static Value trunc(Context& ctx, const std::vector<Value>& args);
    static Value sign(Context& ctx, const std::vector<Value>& args);
    static Value cbrt(Context& ctx, const std::vector<Value>& args);
    static Value hypot(Context& ctx, const std::vector<Value>& args);
    static Value clz32(Context& ctx, const std::vector<Value>& args);
    static Value imul(Context& ctx, const std::vector<Value>& args);
    
    // ES2026 enhanced numeric methods
    static Value sumPrecise(Context& ctx, const std::vector<Value>& args);
    static Value f16round(Context& ctx, const std::vector<Value>& args);
    static Value log10(Context& ctx, const std::vector<Value>& args);
    static Value log2(Context& ctx, const std::vector<Value>& args);
    static Value log1p(Context& ctx, const std::vector<Value>& args);
    static Value expm1(Context& ctx, const std::vector<Value>& args);
    static Value acosh(Context& ctx, const std::vector<Value>& args);
    static Value asinh(Context& ctx, const std::vector<Value>& args);
    static Value atanh(Context& ctx, const std::vector<Value>& args);
    static Value cosh(Context& ctx, const std::vector<Value>& args);
    static Value sinh(Context& ctx, const std::vector<Value>& args);
    static Value tanh(Context& ctx, const std::vector<Value>& args);
    
    // Create Math global object
    static std::unique_ptr<Object> create_math_object();

private:
    // Helper functions
    static double safe_to_number(const Value& value);
    static bool is_finite_number(double value);
    static bool is_integer(double value);
    
    // Random number generation
    static bool random_initialized_;
    static void initialize_random();
};

} // namespace Quanta

#endif // QUANTA_MATH_H