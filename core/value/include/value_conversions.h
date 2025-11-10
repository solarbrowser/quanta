/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include <string>

namespace Quanta {

class Context;

/**
 * JavaScript Value Type Conversions
 * Implements ECMAScript specification conversion algorithms
 */
class ValueConversions {
public:
    // Primary conversion methods
    static std::string to_string(const Value& value);
    static double to_number(const Value& value);
    static bool to_boolean(const Value& value);

    // ECMAScript abstract operations
    static Value to_primitive(const Value& value, const std::string& hint = "");
    static Value ordinary_to_primitive(const Value& object, const std::string& hint);

    // Number conversion variants
    static int32_t to_int32(const Value& value);
    static uint32_t to_uint32(const Value& value);
    static int16_t to_int16(const Value& value);
    static uint16_t to_uint16(const Value& value);
    static int8_t to_int8(const Value& value);
    static uint8_t to_uint8(const Value& value);

    // BigInt conversions
    static Value to_big_int(const Value& value);
    static int64_t to_big_int64(const Value& value);
    static uint64_t to_big_uint64(const Value& value);

    // String conversion variants
    static std::string to_string_fallback(const Value& value);
    static std::string to_display_string(const Value& value);
    static std::string to_json_string(const Value& value);

    // Object string representations
    static std::string object_to_string(const Value& object);
    static std::string array_to_string(const Value& array, int max_elements = 10);
    static std::string function_to_string(const Value& function);

    // Number string formatting
    static std::string number_to_string(double number, int precision = -1);
    static std::string number_to_fixed(double number, int digits);
    static std::string number_to_exponential(double number, int digits = -1);
    static std::string number_to_precision(double number, int precision);

    // Boolean conversion helpers
    static bool is_truthy(const Value& value);
    static bool is_falsy(const Value& value);

    // Type coercion for operators
    static Value to_numeric(const Value& value);
    static double to_integer(const Value& value);
    static double to_integer_or_infinity(const Value& value);

    // Property key conversion
    static std::string to_property_key(const Value& value);
    static uint32_t to_array_index(const Value& value);

    // Date/Time conversions
    static double to_time_value(const Value& value);
    static std::string to_iso_string(double time_value);

    // Error handling
    static Value create_type_error(const std::string& message);
    static Value create_range_error(const std::string& message);

    // Context-aware conversions (for toString/valueOf methods)
    static std::string to_string_with_context(const Value& value, Context* context);
    static double to_number_with_context(const Value& value, Context* context);

private:
    // Internal helper methods
    static std::string primitive_to_string(const Value& value);
    static double primitive_to_number(const Value& value);

    // Object method calling helpers
    static Value call_to_string_method(const Value& object, Context* context);
    static Value call_value_of_method(const Value& object, Context* context);

    // Number parsing helpers
    static double parse_number_from_string(const std::string& str);
    static bool is_valid_number_string(const std::string& str);

    // String formatting helpers
    static std::string format_infinity(double value);
    static std::string format_nan();
    static std::string format_zero(double value);
    static std::string remove_trailing_zeros(const std::string& str);

    // Array formatting helpers
    static std::string format_sparse_array(const Value& array, int max_elements);
    static std::string get_array_element_string(const Value& array, uint32_t index);

    // Circular reference detection
    static bool has_circular_reference(const Value& value);
    static void mark_visiting(const Value& value);
    static void unmark_visiting(const Value& value);

    // Character encoding helpers
    static std::string escape_string(const std::string& str);
    static std::string unescape_string(const std::string& str);

    // Locale-specific formatting (basic)
    static std::string to_locale_string(const Value& value, const std::string& locale = "");
};

/**
 * Conversion Utilities for Performance
 */
namespace ConversionUtils {
    // Fast path conversions for common cases
    bool is_string_numeric(const std::string& str);
    double fast_string_to_number(const std::string& str);

    // Cached string conversions
    const std::string& get_cached_boolean_string(bool value);
    const std::string& get_cached_undefined_string();
    const std::string& get_cached_null_string();

    // Integer optimization
    bool is_representable_as_int32(double value);
    bool is_representable_as_uint32(double value);

    // String building optimization
    class StringBuffer {
    public:
        void append(const std::string& str);
        void append(char c);
        void append(double number);
        std::string to_string();
        void clear();

    private:
        std::string buffer_;
    };
}

} // namespace Quanta