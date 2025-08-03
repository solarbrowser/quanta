#include "../include/Math.h"
#include "../include/Context.h"
#include "../include/Object.h"
#include <cmath>
#include <algorithm>
#include <random>
#include <chrono>
#include <limits>

namespace Quanta {

// Static member initialization
bool Math::random_initialized_ = false;

//=============================================================================
// Math Constants and Setup
//=============================================================================

std::unique_ptr<Object> Math::create_math_object() {
    auto math_obj = std::make_unique<Object>();
    
    // Add constants
    math_obj->set_property("E", Value(E));
    math_obj->set_property("LN2", Value(LN2));
    math_obj->set_property("LN10", Value(LN10));
    math_obj->set_property("LOG2E", Value(LOG2E));
    math_obj->set_property("LOG10E", Value(LOG10E));
    math_obj->set_property("PI", Value(PI));
    math_obj->set_property("SQRT1_2", Value(SQRT1_2));
    math_obj->set_property("SQRT2", Value(SQRT2));
    
    // Add methods as placeholder strings (to be replaced with actual native functions)
    // These will be replaced with actual native functions when integrated with ObjectFactory
    math_obj->set_property("abs", Value("function abs() { [native code] }"));
    math_obj->set_property("acos", Value("function acos() { [native code] }"));
    math_obj->set_property("asin", Value("function asin() { [native code] }"));
    math_obj->set_property("atan", Value("function atan() { [native code] }"));
    math_obj->set_property("atan2", Value("function atan2() { [native code] }"));
    math_obj->set_property("ceil", Value("function ceil() { [native code] }"));
    math_obj->set_property("cos", Value("function cos() { [native code] }"));
    math_obj->set_property("exp", Value("function exp() { [native code] }"));
    math_obj->set_property("floor", Value("function floor() { [native code] }"));
    math_obj->set_property("log", Value("function log() { [native code] }"));
    math_obj->set_property("max", Value("function max() { [native code] }"));
    math_obj->set_property("min", Value("function min() { [native code] }"));
    math_obj->set_property("pow", Value("function pow() { [native code] }"));
    math_obj->set_property("random", Value("function random() { [native code] }"));
    math_obj->set_property("round", Value("function round() { [native code] }"));
    math_obj->set_property("sin", Value("function sin() { [native code] }"));
    math_obj->set_property("sqrt", Value("function sqrt() { [native code] }"));
    math_obj->set_property("tan", Value("function tan() { [native code] }"));
    
    // ES6+ methods
    math_obj->set_property("trunc", Value("function trunc() { [native code] }"));
    math_obj->set_property("sign", Value("function sign() { [native code] }"));
    math_obj->set_property("cbrt", Value("function cbrt() { [native code] }"));
    math_obj->set_property("hypot", Value("function hypot() { [native code] }"));
    math_obj->set_property("clz32", Value("function clz32() { [native code] }"));
    math_obj->set_property("imul", Value("function imul() { [native code] }"));
    
    return math_obj;
}

//=============================================================================
// Math Method Implementations
//=============================================================================

Value Math::abs(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::abs(value));
}

Value Math::acos(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::acos(value));
}

Value Math::asin(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::asin(value));
}

Value Math::atan(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::atan(value));
}

Value Math::atan2(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double y = safe_to_number(args[0]);
    double x = safe_to_number(args[1]);
    return Value(std::atan2(y, x));
}

Value Math::ceil(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::ceil(value));
}

Value Math::cos(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::cos(value));
}

Value Math::exp(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::exp(value));
}

Value Math::floor(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::floor(value));
}

Value Math::log(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::log(value));
}

Value Math::max(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(-std::numeric_limits<double>::infinity());
    }
    
    double result = -std::numeric_limits<double>::infinity();
    for (const Value& arg : args) {
        double value = safe_to_number(arg);
        if (std::isnan(value)) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
        result = std::max(result, value);
    }
    return Value(result);
}

Value Math::min(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::infinity());
    }
    
    double result = std::numeric_limits<double>::infinity();
    for (const Value& arg : args) {
        double value = safe_to_number(arg);
        if (std::isnan(value)) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
        result = std::min(result, value);
    }
    return Value(result);
}

Value Math::pow(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double base = safe_to_number(args[0]);
    double exponent = safe_to_number(args[1]);
    return Value(std::pow(base, exponent));
}

Value Math::random(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    (void)args;
    
    if (!random_initialized_) {
        initialize_random();
    }
    
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<double> dis(0.0, 1.0);
    
    return Value(dis(gen));
}

Value Math::round(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::round(value));
}

Value Math::sin(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::sin(value));
}

Value Math::sqrt(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::sqrt(value));
}

Value Math::tan(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::tan(value));
}

//=============================================================================
// ES6+ Math Methods
//=============================================================================

Value Math::trunc(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::trunc(value));
}

Value Math::sign(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    if (std::isnan(value)) return Value(std::numeric_limits<double>::quiet_NaN());
    if (value > 0) return Value(1.0);
    if (value < 0) return Value(-1.0);
    return Value(value); // +0 or -0
}

Value Math::cbrt(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::cbrt(value));
}

Value Math::hypot(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(0.0);
    }
    
    double sum = 0.0;
    for (const Value& arg : args) {
        double value = safe_to_number(arg);
        if (std::isnan(value)) {
            return Value(std::numeric_limits<double>::quiet_NaN());
        }
        if (std::isinf(value)) {
            return Value(std::numeric_limits<double>::infinity());
        }
        sum += value * value;
    }
    return Value(std::sqrt(sum));
}

Value Math::clz32(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(32.0);
    }
    
    double value = safe_to_number(args[0]);
    uint32_t x = static_cast<uint32_t>(value);
    
    if (x == 0) return Value(32.0);
    
    int count = 0;
    for (int i = 31; i >= 0; i--) {
        if ((x & (1U << i)) != 0) break;
        count++;
    }
    
    return Value(static_cast<double>(count));
}

Value Math::imul(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        return Value(0.0);
    }
    
    int32_t a = static_cast<int32_t>(safe_to_number(args[0]));
    int32_t b = static_cast<int32_t>(safe_to_number(args[1]));
    
    return Value(static_cast<double>(a * b));
}

//=============================================================================
// ES2026 Enhanced Numeric Methods
//=============================================================================

Value Math::sumPrecise(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    // Implements Kahan summation algorithm for more precise floating-point addition
    if (args.empty()) {
        return Value(0.0);
    }
    
    double sum = 0.0;
    double compensation = 0.0;
    
    for (const auto& arg : args) {
        double value = safe_to_number(arg);
        if (!is_finite_number(value)) {
            return Value(value); // Return NaN or infinity
        }
        
        double y = value - compensation;
        double t = sum + y;
        compensation = (t - sum) - y;
        sum = t;
    }
    
    return Value(sum);
}

Value Math::f16round(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    // Round to nearest 16-bit floating point representation
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    if (!is_finite_number(value)) {
        return Value(value);
    }
    
    // Simple approximation - in real implementation would use proper float16 conversion
    float f16_val = static_cast<float>(value);
    return Value(static_cast<double>(f16_val));
}

Value Math::log10(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::log10(value));
}

Value Math::log2(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::log2(value));
}

Value Math::log1p(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::log1p(value));
}

Value Math::expm1(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::expm1(value));
}

Value Math::acosh(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::acosh(value));
}

Value Math::asinh(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::asinh(value));
}

Value Math::atanh(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::atanh(value));
}

Value Math::cosh(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::cosh(value));
}

Value Math::sinh(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::sinh(value));
}

Value Math::tanh(Context& ctx, const std::vector<Value>& args) {
    (void)ctx; // Suppress unused parameter warning
    if (args.empty()) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    
    double value = safe_to_number(args[0]);
    return Value(std::tanh(value));
}

//=============================================================================
// Helper Functions
//=============================================================================

double Math::safe_to_number(const Value& value) {
    if (value.is_number()) {
        return value.to_number();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool Math::is_finite_number(double value) {
    return std::isfinite(value);
}

bool Math::is_integer(double value) {
    return std::floor(value) == value;
}

void Math::initialize_random() {
    random_initialized_ = true;
}

} // namespace Quanta