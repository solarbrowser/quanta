/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"

namespace Quanta {

class Context;

/**
 * JavaScript Value Operations
 * Implements ECMAScript specification operators and comparisons
 */
class ValueOperations {
public:
    // Arithmetic operations
    static Value add(const Value& left, const Value& right);
    static Value subtract(const Value& left, const Value& right);
    static Value multiply(const Value& left, const Value& right);
    static Value divide(const Value& left, const Value& right);
    static Value modulo(const Value& left, const Value& right);
    static Value power(const Value& left, const Value& right);

    // Unary operations
    static Value unary_plus(const Value& operand);
    static Value unary_minus(const Value& operand);
    static Value unary_not(const Value& operand);
    static Value bitwise_not(const Value& operand);

    // Bitwise operations
    static Value bitwise_and(const Value& left, const Value& right);
    static Value bitwise_or(const Value& left, const Value& right);
    static Value bitwise_xor(const Value& left, const Value& right);
    static Value left_shift(const Value& left, const Value& right);
    static Value right_shift(const Value& left, const Value& right);
    static Value unsigned_right_shift(const Value& left, const Value& right);

    // Comparison operations
    static bool strict_equals(const Value& left, const Value& right);
    static bool loose_equals(const Value& left, const Value& right);
    static bool less_than(const Value& left, const Value& right);
    static bool less_than_or_equal(const Value& left, const Value& right);
    static bool greater_than(const Value& left, const Value& right);
    static bool greater_than_or_equal(const Value& left, const Value& right);

    // Type checking operations
    static Value typeof_operation(const Value& operand);
    static bool instanceof_operation(const Value& object, const Value& constructor, Context* context);

    // Abstract relational comparison
    static Value abstract_relational_comparison(const Value& left, const Value& right, bool left_first = true);

    // String operations
    static Value string_concatenation(const Value& left, const Value& right);
    static Value string_repeat(const Value& str, const Value& count);

    // Increment/decrement operations
    static Value increment(const Value& operand);
    static Value decrement(const Value& operand);

    // Logical operations (short-circuit)
    static Value logical_and(const Value& left, const Value& right);
    static Value logical_or(const Value& left, const Value& right);
    static Value nullish_coalescing(const Value& left, const Value& right);

    // Object operations
    static bool has_property(const Value& object, const Value& key);
    static Value get_property(const Value& object, const Value& key, Context* context);
    static bool set_property(const Value& object, const Value& key, const Value& value, Context* context);
    static bool delete_property(const Value& object, const Value& key, Context* context);

    // Array operations
    static Value get_array_element(const Value& array, uint32_t index);
    static bool set_array_element(const Value& array, uint32_t index, const Value& value);
    static uint32_t get_array_length(const Value& array);

    // Function operations
    static Value call_function(const Value& function, const Value& this_value,
                              const std::vector<Value>& args, Context* context);
    static Value construct_object(const Value& constructor, const std::vector<Value>& args, Context* context);

    // Advanced operations
    static Value to_primitive_for_operation(const Value& value, const std::string& hint);
    static bool same_value(const Value& left, const Value& right);
    static bool same_value_zero(const Value& left, const Value& right);

    // BigInt operations
    static Value add_bigint(const Value& left, const Value& right);
    static Value subtract_bigint(const Value& left, const Value& right);
    static Value multiply_bigint(const Value& left, const Value& right);
    static Value divide_bigint(const Value& left, const Value& right);
    static Value modulo_bigint(const Value& left, const Value& right);
    static Value power_bigint(const Value& left, const Value& right);

    // Symbol operations
    static bool symbol_equals(const Value& left, const Value& right);
    static Value symbol_to_string(const Value& symbol);

private:
    // Internal arithmetic helpers
    static double perform_numeric_operation(double left, double right, const std::string& operation);
    static Value handle_overflow(double result);
    static bool is_safe_integer_operation(double left, double right, const std::string& operation);

    // Comparison helpers
    static int compare_strings(const std::string& left, const std::string& right);
    static bool compare_numbers(double left, double right, const std::string& comparison);

    // Type coercion helpers
    static std::pair<Value, Value> coerce_to_primitive(const Value& left, const Value& right);
    static std::pair<double, double> coerce_to_numeric(const Value& left, const Value& right);

    // Error handling
    static Value create_operation_error(const std::string& operation, const Value& left, const Value& right);
    static Value create_type_mismatch_error(const std::string& expected, const Value& actual);

    // Optimization helpers
    static bool can_optimize_arithmetic(const Value& left, const Value& right);
    static Value fast_integer_operation(const Value& left, const Value& right, const std::string& operation);
};

/**
 * Specialized Numeric Operations
 */
class NumericOperations {
public:
    // IEEE 754 compliant operations
    static double ieee754_add(double a, double b);
    static double ieee754_subtract(double a, double b);
    static double ieee754_multiply(double a, double b);
    static double ieee754_divide(double a, double b);
    static double ieee754_remainder(double a, double b);

    // Special value handling
    static bool is_finite_number(double value);
    static bool is_positive_zero(double value);
    static bool is_negative_zero(double value);

    // Precision and rounding
    static double to_integer_value(double value);
    static double apply_rounding(double value, int mode);

    // Mathematical functions
    static double safe_power(double base, double exponent);
    static double safe_log(double value);
    static double safe_sqrt(double value);
};

/**
 * String Operation Utilities
 */
class StringOperations {
public:
    // Unicode-aware operations
    static std::string concatenate_unicode(const std::string& left, const std::string& right);
    static size_t unicode_length(const std::string& str);
    static std::string unicode_substring(const std::string& str, size_t start, size_t length);

    // Comparison operations
    static int unicode_compare(const std::string& left, const std::string& right);
    static bool starts_with(const std::string& str, const std::string& prefix);
    static bool ends_with(const std::string& str, const std::string& suffix);

    // Optimization for common patterns
    static bool is_empty_or_whitespace(const std::string& str);
    static std::string fast_repeat(const std::string& str, uint32_t count);
};

} // namespace Quanta