#include "../include/Value.h"
#include "../include/Object.h"
#include "../include/String.h"
#include <sstream>
#include <cmath>
#include <limits>
#include <iostream>

namespace Quanta {

//=============================================================================
// Value Implementation
//=============================================================================

Value::Value(Object* obj) {
    if (obj && obj->get_type() == Object::ObjectType::Function) {
        bits_ = QUIET_NAN | TAG_FUNCTION | reinterpret_cast<uint64_t>(obj);
    } else {
        bits_ = QUIET_NAN | TAG_OBJECT | reinterpret_cast<uint64_t>(obj);
    }
}

Value::Value(const std::string& str) {
    // Create a String object directly without going through ObjectFactory
    auto string_obj = std::make_unique<String>(str);
    bits_ = QUIET_NAN | TAG_STRING | reinterpret_cast<uint64_t>(string_obj.release());
}

std::string Value::to_string() const {
    if (is_undefined()) return "undefined";
    if (is_null()) return "null";
    if (is_boolean()) return as_boolean() ? "true" : "false";
    if (is_number()) {
        double num = as_number();
        if (std::isnan(num)) return "NaN";
        if (std::isinf(num)) return num > 0 ? "Infinity" : "-Infinity";
        std::ostringstream oss;
        oss << num;
        return oss.str();
    }
    if (is_string()) {
        return as_string()->str();
    }
    if (is_object()) {
        Object* obj = as_object();
        if (obj->is_array()) {
            // Display array contents
            std::string result = "[";
            uint32_t length = obj->get_length();
            for (uint32_t i = 0; i < length; i++) {
                if (i > 0) result += ", ";
                Value element = obj->get_element(i);
                if (element.is_string()) {
                    result += "\"" + element.to_string() + "\"";
                } else {
                    result += element.to_string();
                }
            }
            result += "]";
            return result;
        } else {
            return "[object Object]";
        }
    }
    if (is_function()) {
        return "[function Function]";
    }
    return "unknown";
}

double Value::to_number() const {
    if (is_number()) return as_number();
    if (is_undefined()) return std::numeric_limits<double>::quiet_NaN();
    if (is_null()) return 0.0;
    if (is_boolean()) return as_boolean() ? 1.0 : 0.0;
    if (is_string()) {
        const std::string& str = as_string()->str();
        if (str.empty()) return 0.0;
        try {
            return std::stod(str);
        } catch (...) {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool Value::to_boolean() const {
    if (is_boolean()) return as_boolean();
    if (is_undefined() || is_null()) return false;
    if (is_number()) {
        double num = as_number();
        return !std::isnan(num) && num != 0.0;
    }
    if (is_string()) {
        return !as_string()->str().empty();
    }
    return true; // objects and functions are truthy
}

Value Value::typeof_op() const {
    if (is_undefined()) return Value(std::string("undefined"));
    if (is_null()) return Value(std::string("object"));
    if (is_boolean()) return Value(std::string("boolean"));
    if (is_number()) return Value(std::string("number"));
    if (is_string()) return Value(std::string("string"));
    if (is_function()) return Value(std::string("function"));
    return Value(std::string("object"));
}

bool Value::strict_equals(const Value& other) const {
    if (is_undefined() && other.is_undefined()) return true;
    if (is_null() && other.is_null()) return true;
    if (is_boolean() && other.is_boolean()) return as_boolean() == other.as_boolean();
    if (is_number() && other.is_number()) return as_number() == other.as_number();
    if (is_string() && other.is_string()) return as_string()->str() == other.as_string()->str();
    if (is_object() && other.is_object()) return as_object() == other.as_object();
    if (is_function() && other.is_function()) return as_function() == other.as_function();
    return false;
}

bool Value::loose_equals(const Value& other) const {
    // For now, just use strict equality - full loose equality is complex
    return strict_equals(other);
}

Value Value::add(const Value& other) const {
    if (is_number() && other.is_number()) {
        return Value(as_number() + other.as_number());
    }
    // For simplicity, convert to strings for + operation
    return Value(to_string() + other.to_string());
}

Value Value::subtract(const Value& other) const {
    return Value(to_number() - other.to_number());
}

Value Value::multiply(const Value& other) const {
    return Value(to_number() * other.to_number());
}

Value Value::divide(const Value& other) const {
    return Value(to_number() / other.to_number());
}

Value Value::modulo(const Value& other) const {
    return Value(std::fmod(to_number(), other.to_number()));
}

Value Value::power(const Value& other) const {
    return Value(std::pow(to_number(), other.to_number()));
}

Value Value::unary_plus() const {
    return Value(to_number());
}

Value Value::unary_minus() const {
    return Value(-to_number());
}

Value Value::logical_not() const {
    return Value(!to_boolean());
}

Value Value::bitwise_not() const {
    int32_t num = static_cast<int32_t>(to_number());
    return Value(static_cast<double>(~num));
}

Value Value::left_shift(const Value& other) const {
    int32_t left = static_cast<int32_t>(to_number());
    int32_t right = static_cast<int32_t>(other.to_number()) & 0x1F;
    return Value(static_cast<double>(left << right));
}

Value Value::right_shift(const Value& other) const {
    int32_t left = static_cast<int32_t>(to_number());
    int32_t right = static_cast<int32_t>(other.to_number()) & 0x1F;
    return Value(static_cast<double>(left >> right));
}

Value Value::unsigned_right_shift(const Value& other) const {
    uint32_t left = static_cast<uint32_t>(to_number());
    int32_t right = static_cast<int32_t>(other.to_number()) & 0x1F;
    return Value(static_cast<double>(left >> right));
}

Value Value::bitwise_and(const Value& other) const {
    int32_t left = static_cast<int32_t>(to_number());
    int32_t right = static_cast<int32_t>(other.to_number());
    return Value(static_cast<double>(left & right));
}

Value Value::bitwise_or(const Value& other) const {
    int32_t left = static_cast<int32_t>(to_number());
    int32_t right = static_cast<int32_t>(other.to_number());
    return Value(static_cast<double>(left | right));
}

Value Value::bitwise_xor(const Value& other) const {
    int32_t left = static_cast<int32_t>(to_number());
    int32_t right = static_cast<int32_t>(other.to_number());
    return Value(static_cast<double>(left ^ right));
}

int Value::compare(const Value& other) const {
    if (is_number() && other.is_number()) {
        double left = as_number();
        double right = other.as_number();
        if (left < right) return -1;
        if (left > right) return 1;
        return 0;
    }
    // For simplicity, convert to strings for comparison
    std::string left_str = to_string();
    std::string right_str = other.to_string();
    if (left_str < right_str) return -1;
    if (left_str > right_str) return 1;
    return 0;
}

//=============================================================================
// ValueFactory Implementation
//=============================================================================

namespace ValueFactory {

Value create_function(std::unique_ptr<Function> function_obj) {
    // Transfer ownership to Value
    return Value(function_obj.release());
}

} // namespace ValueFactory

} // namespace Quanta