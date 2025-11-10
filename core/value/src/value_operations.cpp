/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/value_operations.h"
#include "../include/value_core.h"
#include "../include/value_conversions.h"
#include "../../include/Object.h"
#include <cmath>
#include <algorithm>

namespace Quanta {

//=============================================================================
// ValueOperations Implementation
//=============================================================================

Value ValueOperations::add(const Value& left, const Value& right) {
    // JavaScript addition: string concatenation or numeric addition

    // If either operand is a string, concatenate
    if (left.is_string() || right.is_string()) {
        return string_concatenation(left, right);
    }

    // Otherwise, numeric addition
    double left_num = ValueConversions::to_number(left);
    double right_num = ValueConversions::to_number(right);

    double result = left_num + right_num;
    return ValueCore::create_number(result);
}

Value ValueOperations::subtract(const Value& left, const Value& right) {
    double left_num = ValueConversions::to_number(left);
    double right_num = ValueConversions::to_number(right);

    double result = left_num - right_num;
    return ValueCore::create_number(result);
}

Value ValueOperations::multiply(const Value& left, const Value& right) {
    double left_num = ValueConversions::to_number(left);
    double right_num = ValueConversions::to_number(right);

    double result = left_num * right_num;
    return ValueCore::create_number(result);
}

Value ValueOperations::divide(const Value& left, const Value& right) {
    double left_num = ValueConversions::to_number(left);
    double right_num = ValueConversions::to_number(right);

    double result = left_num / right_num;
    return ValueCore::create_number(result);
}

Value ValueOperations::modulo(const Value& left, const Value& right) {
    double left_num = ValueConversions::to_number(left);
    double right_num = ValueConversions::to_number(right);

    double result = std::fmod(left_num, right_num);
    return ValueCore::create_number(result);
}

Value ValueOperations::power(const Value& left, const Value& right) {
    double left_num = ValueConversions::to_number(left);
    double right_num = ValueConversions::to_number(right);

    double result = std::pow(left_num, right_num);
    return ValueCore::create_number(result);
}

Value ValueOperations::unary_plus(const Value& operand) {
    double num = ValueConversions::to_number(operand);
    return ValueCore::create_number(num);
}

Value ValueOperations::unary_minus(const Value& operand) {
    double num = ValueConversions::to_number(operand);
    return ValueCore::create_number(-num);
}

Value ValueOperations::unary_not(const Value& operand) {
    bool result = !ValueConversions::to_boolean(operand);
    return ValueCore::create_boolean(result);
}

Value ValueOperations::bitwise_not(const Value& operand) {
    int32_t value = ValueConversions::to_int32(operand);
    int32_t result = ~value;
    return ValueCore::create_number(static_cast<double>(result));
}

Value ValueOperations::bitwise_and(const Value& left, const Value& right) {
    int32_t left_int = ValueConversions::to_int32(left);
    int32_t right_int = ValueConversions::to_int32(right);
    int32_t result = left_int & right_int;
    return ValueCore::create_number(static_cast<double>(result));
}

Value ValueOperations::bitwise_or(const Value& left, const Value& right) {
    int32_t left_int = ValueConversions::to_int32(left);
    int32_t right_int = ValueConversions::to_int32(right);
    int32_t result = left_int | right_int;
    return ValueCore::create_number(static_cast<double>(result));
}

Value ValueOperations::bitwise_xor(const Value& left, const Value& right) {
    int32_t left_int = ValueConversions::to_int32(left);
    int32_t right_int = ValueConversions::to_int32(right);
    int32_t result = left_int ^ right_int;
    return ValueCore::create_number(static_cast<double>(result));
}

Value ValueOperations::left_shift(const Value& left, const Value& right) {
    int32_t left_int = ValueConversions::to_int32(left);
    uint32_t right_uint = ValueConversions::to_uint32(right) & 0x1F; // Only use bottom 5 bits
    int32_t result = left_int << right_uint;
    return ValueCore::create_number(static_cast<double>(result));
}

Value ValueOperations::right_shift(const Value& left, const Value& right) {
    int32_t left_int = ValueConversions::to_int32(left);
    uint32_t right_uint = ValueConversions::to_uint32(right) & 0x1F;
    int32_t result = left_int >> right_uint;
    return ValueCore::create_number(static_cast<double>(result));
}

Value ValueOperations::unsigned_right_shift(const Value& left, const Value& right) {
    uint32_t left_uint = ValueConversions::to_uint32(left);
    uint32_t right_uint = ValueConversions::to_uint32(right) & 0x1F;
    uint32_t result = left_uint >> right_uint;
    return ValueCore::create_number(static_cast<double>(result));
}

bool ValueOperations::strict_equals(const Value& left, const Value& right) {
    return left.strict_equals(right);
}

bool ValueOperations::loose_equals(const Value& left, const Value& right) {
    return left.loose_equals(right);
}

bool ValueOperations::less_than(const Value& left, const Value& right) {
    // Simplified comparison - convert to numbers
    if (left.is_string() && right.is_string()) {
        return left.to_string() < right.to_string();
    }

    double left_num = ValueConversions::to_number(left);
    double right_num = ValueConversions::to_number(right);

    if (std::isnan(left_num) || std::isnan(right_num)) {
        return false;
    }

    return left_num < right_num;
}

bool ValueOperations::less_than_or_equal(const Value& left, const Value& right) {
    return less_than(left, right) || strict_equals(left, right);
}

bool ValueOperations::greater_than(const Value& left, const Value& right) {
    return less_than(right, left);
}

bool ValueOperations::greater_than_or_equal(const Value& left, const Value& right) {
    return greater_than(left, right) || strict_equals(left, right);
}

Value ValueOperations::typeof_operation(const Value& operand) {
    if (operand.is_undefined()) return ValueCore::create_string("undefined");
    if (operand.is_null()) return ValueCore::create_string("object"); // JavaScript quirk
    if (operand.is_boolean()) return ValueCore::create_string("boolean");
    if (operand.is_number()) return ValueCore::create_string("number");
    if (operand.is_string()) return ValueCore::create_string("string");
    if (operand.is_symbol()) return ValueCore::create_string("symbol");
    if (operand.is_bigint()) return ValueCore::create_string("bigint");
    if (operand.is_function()) return ValueCore::create_string("function");
    if (operand.is_object()) return ValueCore::create_string("object");

    return ValueCore::create_string("unknown");
}

bool ValueOperations::instanceof_operation(const Value& object, const Value& constructor, Context* context) {
    // Simplified instanceof - would need proper prototype chain checking
    return false;
}

Value ValueOperations::string_concatenation(const Value& left, const Value& right) {
    std::string left_str = ValueConversions::to_string(left);
    std::string right_str = ValueConversions::to_string(right);

    std::string result = left_str + right_str;
    return ValueCore::create_string(result);
}

Value ValueOperations::increment(const Value& operand) {
    double num = ValueConversions::to_number(operand);
    return ValueCore::create_number(num + 1.0);
}

Value ValueOperations::decrement(const Value& operand) {
    double num = ValueConversions::to_number(operand);
    return ValueCore::create_number(num - 1.0);
}

Value ValueOperations::logical_and(const Value& left, const Value& right) {
    // JavaScript logical AND - returns left if falsy, otherwise right
    if (!ValueConversions::to_boolean(left)) {
        return left;
    }
    return right;
}

Value ValueOperations::logical_or(const Value& left, const Value& right) {
    // JavaScript logical OR - returns left if truthy, otherwise right
    if (ValueConversions::to_boolean(left)) {
        return left;
    }
    return right;
}

Value ValueOperations::nullish_coalescing(const Value& left, const Value& right) {
    // Nullish coalescing - returns right if left is null or undefined
    if (left.is_null() || left.is_undefined()) {
        return right;
    }
    return left;
}

bool ValueOperations::same_value(const Value& left, const Value& right) {
    // SameValue algorithm from ECMAScript specification
    if (left.is_number() && right.is_number()) {
        double left_num = left.as_number();
        double right_num = right.as_number();

        // Handle NaN case
        if (std::isnan(left_num) && std::isnan(right_num)) {
            return true;
        }

        // Handle +0 vs -0 case
        if (left_num == 0.0 && right_num == 0.0) {
            return std::signbit(left_num) == std::signbit(right_num);
        }

        return left_num == right_num;
    }

    return strict_equals(left, right);
}

bool ValueOperations::same_value_zero(const Value& left, const Value& right) {
    // SameValueZero - like SameValue but treats +0 and -0 as equal
    if (left.is_number() && right.is_number()) {
        double left_num = left.as_number();
        double right_num = right.as_number();

        // Handle NaN case
        if (std::isnan(left_num) && std::isnan(right_num)) {
            return true;
        }

        return left_num == right_num;
    }

    return strict_equals(left, right);
}

// Private helpers
double ValueOperations::perform_numeric_operation(double left, double right, const std::string& operation) {
    if (operation == "add") return left + right;
    if (operation == "subtract") return left - right;
    if (operation == "multiply") return left * right;
    if (operation == "divide") return left / right;
    if (operation == "modulo") return std::fmod(left, right);
    if (operation == "power") return std::pow(left, right);

    return std::numeric_limits<double>::quiet_NaN();
}

Value ValueOperations::handle_overflow(double result) {
    if (std::isinf(result)) {
        return result > 0 ? ValueCore::create_positive_infinity() : ValueCore::create_negative_infinity();
    }
    if (std::isnan(result)) {
        return ValueCore::create_nan();
    }
    return ValueCore::create_number(result);
}

//=============================================================================
// NumericOperations Implementation
//=============================================================================

double NumericOperations::ieee754_add(double a, double b) {
    return a + b; // Let the FPU handle IEEE 754 compliance
}

double NumericOperations::ieee754_subtract(double a, double b) {
    return a - b;
}

double NumericOperations::ieee754_multiply(double a, double b) {
    return a * b;
}

double NumericOperations::ieee754_divide(double a, double b) {
    return a / b;
}

double NumericOperations::ieee754_remainder(double a, double b) {
    return std::remainder(a, b);
}

bool NumericOperations::is_finite_number(double value) {
    return std::isfinite(value);
}

bool NumericOperations::is_positive_zero(double value) {
    return value == 0.0 && !std::signbit(value);
}

bool NumericOperations::is_negative_zero(double value) {
    return value == 0.0 && std::signbit(value);
}

double NumericOperations::to_integer_value(double value) {
    if (std::isnan(value)) return 0.0;
    if (std::isinf(value)) return value;
    if (value == 0.0) return value; // Preserve sign of zero

    return std::trunc(value);
}

//=============================================================================
// StringOperations Implementation
//=============================================================================

std::string StringOperations::concatenate_unicode(const std::string& left, const std::string& right) {
    // Simplified - just concatenate for now
    return left + right;
}

size_t StringOperations::unicode_length(const std::string& str) {
    // Simplified - return byte length (should be UTF-16 code unit count)
    return str.length();
}

int StringOperations::unicode_compare(const std::string& left, const std::string& right) {
    return left.compare(right);
}

bool StringOperations::is_empty_or_whitespace(const std::string& str) {
    return str.empty() || str.find_first_not_of(" \t\n\r\f\v") == std::string::npos;
}

std::string StringOperations::fast_repeat(const std::string& str, uint32_t count) {
    if (count == 0 || str.empty()) return "";
    if (count == 1) return str;

    std::string result;
    result.reserve(str.length() * count);

    for (uint32_t i = 0; i < count; ++i) {
        result += str;
    }

    return result;
}

} // namespace Quanta