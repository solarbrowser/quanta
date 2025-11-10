/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/value_conversions.h"
#include "../include/value_core.h"
#include "../../include/Object.h"
#include "../../include/String.h"
#include "../../include/Symbol.h"
#include "../../include/BigInt.h"
#include <sstream>
#include <cmath>
#include <limits>
#include <algorithm>

namespace Quanta {

//=============================================================================
// ValueConversions Implementation
//=============================================================================

std::string ValueConversions::to_string(const Value& value) {
    if (value.is_undefined()) {
        return "undefined";
    }
    if (value.is_null()) {
        return "null";
    }
    if (value.is_boolean()) {
        return value.as_boolean() ? "true" : "false";
    }
    if (value.is_number()) {
        return number_to_string(value.as_number());
    }
    if (value.is_string()) {
        String* str_ptr = value.as_string();
        return str_ptr ? str_ptr->str() : "[null string]";
    }
    if (value.is_bigint()) {
        return value.as_bigint()->to_string();
    }
    if (value.is_symbol()) {
        return value.as_symbol()->to_string();
    }
    if (value.is_object()) {
        return object_to_string(value);
    }

    return "[unknown value]";
}

double ValueConversions::to_number(const Value& value) {
    if (value.is_number()) {
        return value.as_number();
    }
    if (value.is_undefined()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (value.is_null()) {
        return 0.0;
    }
    if (value.is_boolean()) {
        return value.as_boolean() ? 1.0 : 0.0;
    }
    if (value.is_string()) {
        String* str_ptr = value.as_string();
        if (!str_ptr) return std::numeric_limits<double>::quiet_NaN();

        std::string str = str_ptr->str();
        return parse_number_from_string(str);
    }
    if (value.is_bigint()) {
        // BigInt to number conversion throws TypeError in strict mode
        // For simplicity, return NaN
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (value.is_symbol()) {
        // Symbol to number conversion throws TypeError
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (value.is_object()) {
        // Convert object to primitive first, then to number
        Value primitive = to_primitive(value, "number");
        return to_number(primitive);
    }

    return std::numeric_limits<double>::quiet_NaN();
}

bool ValueConversions::to_boolean(const Value& value) {
    if (value.is_boolean()) {
        return value.as_boolean();
    }
    if (value.is_undefined() || value.is_null()) {
        return false;
    }
    if (value.is_number()) {
        double num = value.as_number();
        return !std::isnan(num) && num != 0.0 && num != -0.0;
    }
    if (value.is_string()) {
        String* str_ptr = value.as_string();
        return str_ptr && !str_ptr->str().empty();
    }
    if (value.is_bigint()) {
        // BigInt is falsy only if it's 0n
        return !value.as_bigint()->is_zero();
    }
    if (value.is_symbol()) {
        return true; // Symbols are always truthy
    }
    if (value.is_object()) {
        return true; // Objects are always truthy
    }

    return false;
}

Value ValueConversions::to_primitive(const Value& value, const std::string& hint) {
    if (ValueCore::is_primitive(value)) {
        return value;
    }

    // For objects, call toPrimitive internal method
    if (value.is_object()) {
        return ordinary_to_primitive(value, hint);
    }

    return value;
}

Value ValueConversions::ordinary_to_primitive(const Value& object, const std::string& hint) {
    // Simplified implementation - in real JS engine this would call
    // valueOf() and toString() methods based on hint

    if (hint == "string") {
        // Try toString() first, then valueOf()
        std::string str = object_to_string(object);
        return ValueCore::create_string(str);
    } else {
        // Try valueOf() first, then toString()
        // For simplicity, just return string representation
        std::string str = object_to_string(object);
        return ValueCore::create_string(str);
    }
}

int32_t ValueConversions::to_int32(const Value& value) {
    double num = to_number(value);
    if (!std::isfinite(num) || num == 0.0) {
        return 0;
    }

    // Apply ToInt32 algorithm
    int64_t int64_val = static_cast<int64_t>(num);
    return static_cast<int32_t>(int64_val & 0xFFFFFFFF);
}

uint32_t ValueConversions::to_uint32(const Value& value) {
    double num = to_number(value);
    if (!std::isfinite(num) || num == 0.0) {
        return 0;
    }

    // Apply ToUint32 algorithm
    uint64_t uint64_val = static_cast<uint64_t>(std::abs(num));
    return static_cast<uint32_t>(uint64_val & 0xFFFFFFFF);
}

std::string ValueConversions::number_to_string(double number, int precision) {
    if (std::isnan(number)) {
        return "NaN";
    }
    if (std::isinf(number)) {
        return number > 0 ? "Infinity" : "-Infinity";
    }
    if (number == 0.0) {
        return "0";
    }

    std::ostringstream oss;
    if (precision >= 0) {
        oss.precision(precision);
        oss << std::fixed << number;
    } else {
        oss << number;
    }

    std::string result = oss.str();
    return remove_trailing_zeros(result);
}

std::string ValueConversions::object_to_string(const Value& object) {
    Object* obj = object.as_object();
    if (!obj) {
        return "null";
    }

    if (obj->is_array()) {
        return array_to_string(object);
    }
    if (obj->is_function()) {
        return function_to_string(object);
    }

    // Default object string representation
    return "[object Object]";
}

std::string ValueConversions::array_to_string(const Value& array, int max_elements) {
    Object* arr = array.as_object();
    if (!arr || !arr->is_array()) {
        return "[object Array]";
    }

    std::string result = "[";
    uint32_t length = arr->get_length();
    uint32_t display_length = std::min(length, static_cast<uint32_t>(max_elements));

    for (uint32_t i = 0; i < display_length; ++i) {
        if (i > 0) result += ", ";

        Value element = arr->get_element(i);
        if (element.is_undefined()) {
            result += "undefined";
        } else if (element.is_string()) {
            result += "\"" + element.to_string() + "\"";
        } else {
            result += element.to_string();
        }
    }

    if (length > display_length) {
        result += ", ...";
    }

    result += "]";
    return result;
}

std::string ValueConversions::function_to_string(const Value& function) {
    // Simplified function string representation
    return "[function Function]";
}

bool ValueConversions::is_truthy(const Value& value) {
    return to_boolean(value);
}

bool ValueConversions::is_falsy(const Value& value) {
    return !to_boolean(value);
}

std::string ValueConversions::to_property_key(const Value& value) {
    if (value.is_string()) {
        return value.to_string();
    } else if (value.is_symbol()) {
        return value.as_symbol()->to_string();
    } else {
        return to_string(value);
    }
}

// Private helper methods
std::string ValueConversions::primitive_to_string(const Value& value) {
    return to_string(value); // Delegate to main to_string
}

double ValueConversions::primitive_to_number(const Value& value) {
    return to_number(value); // Delegate to main to_number
}

double ValueConversions::parse_number_from_string(const std::string& str) {
    if (str.empty()) {
        return 0.0;
    }

    // Trim whitespace
    std::string trimmed = str;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r\f\v"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r\f\v") + 1);

    if (trimmed.empty()) {
        return 0.0;
    }

    // Handle special cases
    if (trimmed == "Infinity") {
        return std::numeric_limits<double>::infinity();
    }
    if (trimmed == "-Infinity") {
        return -std::numeric_limits<double>::infinity();
    }
    if (trimmed == "NaN") {
        return std::numeric_limits<double>::quiet_NaN();
    }

    try {
        return std::stod(trimmed);
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

std::string ValueConversions::remove_trailing_zeros(const std::string& str) {
    if (str.find('.') == std::string::npos) {
        return str;
    }

    std::string result = str;
    while (!result.empty() && result.back() == '0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }

    return result.empty() ? "0" : result;
}

//=============================================================================
// ConversionUtils Implementation
//=============================================================================

namespace ConversionUtils {

bool is_string_numeric(const std::string& str) {
    if (str.empty()) return false;

    try {
        std::stod(str);
        return true;
    } catch (...) {
        return false;
    }
}

double fast_string_to_number(const std::string& str) {
    try {
        return std::stod(str);
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

const std::string& get_cached_boolean_string(bool value) {
    static const std::string true_str = "true";
    static const std::string false_str = "false";
    return value ? true_str : false_str;
}

const std::string& get_cached_undefined_string() {
    static const std::string undefined_str = "undefined";
    return undefined_str;
}

const std::string& get_cached_null_string() {
    static const std::string null_str = "null";
    return null_str;
}

bool is_representable_as_int32(double value) {
    return std::isfinite(value) && value >= INT32_MIN && value <= INT32_MAX &&
           std::floor(value) == value;
}

bool is_representable_as_uint32(double value) {
    return std::isfinite(value) && value >= 0 && value <= UINT32_MAX &&
           std::floor(value) == value;
}

void StringBuffer::append(const std::string& str) {
    buffer_ += str;
}

void StringBuffer::append(char c) {
    buffer_ += c;
}

void StringBuffer::append(double number) {
    buffer_ += ValueConversions::number_to_string(number);
}

std::string StringBuffer::to_string() {
    return buffer_;
}

void StringBuffer::clear() {
    buffer_.clear();
}

} // namespace ConversionUtils

} // namespace Quanta