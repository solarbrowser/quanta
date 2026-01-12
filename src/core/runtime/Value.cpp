/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Error.h"
#include <sstream>
#include <cmath>
#include <limits>
#include <iostream>
#include <cstdio>
#include <string.h>

namespace Quanta {


#if PLATFORM_POINTER_COMPRESSION
thread_local uintptr_t Value::heap_base_ = 0;
#endif

Value::Value(Object* obj) {
    if (!obj) {
        bits_ = QUIET_NAN | TAG_UNDEFINED;
        return;
    }


    uint64_t ptr_value = reinterpret_cast<uint64_t>(obj);
    uint64_t masked_value = ptr_value & PAYLOAD_MASK;
    bits_ = QUIET_NAN | TAG_OBJECT | masked_value;
}

Value::Value(const std::string& str) {
    auto string_obj = std::make_unique<String>(str);
    String* raw_ptr = string_obj.release();

    #if PLATFORM_POINTER_COMPRESSION
    uint64_t compressed = compress_pointer(raw_ptr);
    bits_ = QUIET_NAN | TAG_STRING | (compressed & PAYLOAD_MASK);
    #else
    bits_ = QUIET_NAN | TAG_STRING | (reinterpret_cast<uint64_t>(raw_ptr) & PAYLOAD_MASK);
    #endif
}

std::string Value::to_string() const {
    if (is_undefined()) {
        return "undefined";
    }
    if (is_null()) return "null";
    if (is_boolean()) {
        return as_boolean() ? "true" : "false";
    }
    if (is_number()) {
        double num = as_number();

        uint64_t bits;
        memcpy(&bits, &num, sizeof(double));

        uint64_t exponent = (bits >> 52) & 0x7FF;
        uint64_t mantissa = bits & 0xFFFFFFFFFFFFFULL;

        if (exponent == 0x7FF) {
            if (mantissa != 0) {
                return "NaN";
            } else {
                return (bits & 0x8000000000000000ULL) ? "-Infinity" : "Infinity";
            }
        }

        std::ostringstream oss;
        oss << num;
        return oss.str();
    }
    if (is_string()) {
        String* str_ptr = as_string();
        if (!str_ptr) return "[null string]";
        return str_ptr->str();
    }
    if (is_bigint()) {
        return as_bigint()->to_string();
    }
    if (is_symbol()) {
        return as_symbol()->to_string();
    }
    if (is_object()) {
        Object* obj = as_object();
        if (!obj) {
            return "null";
        }

        return obj->to_string();
    }
    if (is_function()) {
        return "[function Function]";
    }
    return "unknown";
}

std::string Value::to_property_key() const {
    if (is_symbol()) {
        return as_symbol()->to_property_key();
    }
    return to_string();
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
    if (is_bigint()) {
        return as_bigint()->to_double();
    }
    if (is_symbol()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (is_function()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (is_object()) {
        Object* obj = as_object();
        if (obj && obj->is_array()) {
            uint32_t length = obj->get_length();
            if (length == 0) {
                return 0.0;
            } else if (length == 1) {
                Value element = obj->get_element(0);
                if (!element.is_object()) {
                    return element.to_number();
                }
            }
        }
        return std::numeric_limits<double>::quiet_NaN();
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
    if (is_bigint()) {
        return as_bigint()->to_boolean();
    }
    return true;
}

Value Value::typeof_op() const {
    if (is_undefined()) return Value(std::string("undefined"));
    if (is_null()) return Value(std::string("object"));
    if (is_function()) return Value(std::string("function"));
    if (is_boolean()) return Value(std::string("boolean"));
    if (is_number()) return Value(std::string("number"));
    if (is_string()) return Value(std::string("string"));
    if (is_symbol()) return Value(std::string("symbol"));
    if (is_bigint()) return Value(std::string("bigint"));
    
    return Value(std::string("object"));
}


Value::Type Value::get_type() const {
    if (is_undefined()) return Type::Undefined;
    if (is_null()) return Type::Null;
    if (is_boolean()) return Type::Boolean;
    if (is_number()) return Type::Number;
    if (is_string()) return Type::String;
    if (is_symbol()) return Type::Symbol;
    if (is_bigint()) return Type::BigInt;
    if (is_function()) return Type::Function;
    return Type::Object;
}

bool Value::strict_equals(const Value& other) const {
    if (is_undefined() && other.is_undefined()) return true;
    if (is_null() && other.is_null()) return true;
    if (is_boolean() && other.is_boolean()) return as_boolean() == other.as_boolean();
    if (is_number() && other.is_number()) {
        if (is_nan() || other.is_nan()) {
            return false;
        }
        if (is_positive_infinity() && other.is_positive_infinity()) return true;
        if (is_negative_infinity() && other.is_negative_infinity()) return true;
        if (is_positive_infinity() || is_negative_infinity() || 
            other.is_positive_infinity() || other.is_negative_infinity()) {
            return false;
        }
        
        double a = as_number();
        double b = other.as_number();
        return a == b;
    }
    if (is_string() && other.is_string()) return as_string()->str() == other.as_string()->str();
    if (is_bigint() && other.is_bigint()) return *as_bigint() == *other.as_bigint();
    if (is_symbol() && other.is_symbol()) return as_symbol()->equals(other.as_symbol());
    if (is_object() && other.is_object()) return as_object() == other.as_object();
    if (is_function() && other.is_function()) return as_function() == other.as_function();
    return false;
}

bool Value::same_value(const Value& other) const {
    if (get_type() != other.get_type()) {
        return false;
    }

    if (is_number()) {
        double nx = to_number();
        double ny = other.to_number();

        if (std::isnan(nx) && std::isnan(ny)) {
            return true;
        }

        if (nx == 0.0 && ny == 0.0) {
            return std::signbit(nx) == std::signbit(ny);
        }

        return nx == ny;
    }

    return strict_equals(other);
}

bool Value::loose_equals(const Value& other) const {
    
    if ((is_undefined() && other.is_undefined()) ||
        (is_null() && other.is_null()) ||
        (is_boolean() && other.is_boolean()) ||
        (is_number() && other.is_number()) ||
        (is_string() && other.is_string()) ||
        (is_object() && other.is_object()) ||
        (is_function() && other.is_function())) {
        return strict_equals(other);
    }
    
    if ((is_null() && other.is_undefined()) || (is_undefined() && other.is_null())) {
        return true;
    }
    
    if (is_number() && other.is_string()) {
        return as_number() == other.to_number();
    }
    if (is_string() && other.is_number()) {
        return to_number() == other.as_number();
    }
    
    if (is_boolean()) {
        return Value(to_number()).loose_equals(other);
    }
    if (other.is_boolean()) {
        return loose_equals(Value(other.to_number()));
    }
    
    if (is_object() && (other.is_string() || other.is_number())) {
        return Value(to_string()).loose_equals(other);
    }
    if ((is_string() || is_number()) && other.is_object()) {
        return loose_equals(Value(other.to_string()));
    }
    
    return false;
}

Value Value::add(const Value& other) const {
    if (is_number() && other.is_number()) {
        double result = as_number() + other.as_number();
        if (std::isinf(result)) {
            return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
        }
        if (std::isnan(result)) {
            return Value::nan();
        }
        return Value(result);
    }
    
    if (is_bigint() && other.is_bigint()) {
        BigInt result = *as_bigint() + *other.as_bigint();
        return Value(new BigInt(result));
    }
    if (is_bigint() || other.is_bigint()) {
        throw std::runtime_error("Cannot mix BigInt and other types in addition");
    }
    
    if (is_string() || other.is_string()) {
        return Value(to_string() + other.to_string());
    }
    
    return Value(to_number() + other.to_number());
}

Value Value::subtract(const Value& other) const {
    if (is_number() && other.is_number()) {
        double result = as_number() - other.as_number();
        if (std::isinf(result)) {
            return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
        }
        if (std::isnan(result)) {
            return Value::nan();
        }
        return Value(result);
    }
    
    if (is_bigint() && other.is_bigint()) {
        BigInt result = *as_bigint() - *other.as_bigint();
        return Value(new BigInt(result));
    }
    if (is_bigint() || other.is_bigint()) {
        throw std::runtime_error("Cannot mix BigInt and other types in subtraction");
    }
    return Value(to_number() - other.to_number());
}

Value Value::multiply(const Value& other) const {
    if (is_number() && other.is_number()) {
        double result = as_number() * other.as_number();
        if (std::isinf(result)) {
            return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
        }
        if (std::isnan(result)) {
            return Value::nan();
        }
        return Value(result);
    }
    
    if (is_bigint() && other.is_bigint()) {
        BigInt result = *as_bigint() * *other.as_bigint();
        return Value(new BigInt(result));
    }
    if (is_bigint() || other.is_bigint()) {
        throw std::runtime_error("Cannot mix BigInt and other types in multiplication");
    }
    double result = to_number() * other.to_number();
    if (std::isinf(result)) {
        return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
    }
    if (std::isnan(result)) {
        return Value::nan();
    }
    return Value(result);
}

Value Value::divide(const Value& other) const {
    if (is_number() && other.is_number()) {
        double a = as_number();
        double b = other.as_number();
        
        if (b == 0.0) {
            if (a == 0.0) return Value::nan();
            return a > 0 ? Value::positive_infinity() : Value::negative_infinity();
        }
        
        double result = a / b;
        if (std::isinf(result)) {
            return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
        }
        if (std::isnan(result)) {
            return Value::nan();
        }
        
        return Value(result);
    }
    
    double result = to_number() / other.to_number();
    if (std::isinf(result)) {
        return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
    }
    if (std::isnan(result)) {
        return Value::nan();
    }
    return Value(result);
}

Value Value::modulo(const Value& other) const {
    if (is_number() && other.is_number()) {
        double a = as_number();
        double b = other.as_number();
        if (b == 0.0) return Value(std::numeric_limits<double>::quiet_NaN());
        return Value(std::fmod(a, b));
    }
    return Value(std::fmod(to_number(), other.to_number()));
}

Value Value::power(const Value& other) const {
    if (is_number() && other.is_number()) {
        return Value(std::pow(as_number(), other.as_number()));
    }
    return Value(std::pow(to_number(), other.to_number()));
}

Value Value::unary_plus() const {
    if (is_number()) {
        return *this;
    }
    return Value(to_number());
}

Value Value::unary_minus() const {
    if (is_positive_infinity()) {
        return Value::negative_infinity();
    }
    if (is_negative_infinity()) {
        return Value::positive_infinity();
    }
    if (is_nan()) {
        return Value::nan();
    }
    
    if (is_number()) {
        double result = -as_number();
        if (std::isinf(result)) {
            return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
        }
        if (std::isnan(result)) {
            return Value::nan();
        }
        return Value(result);
    }
    
    double result = -to_number();
    if (std::isinf(result)) {
        return result > 0 ? Value::positive_infinity() : Value::negative_infinity();
    }
    if (std::isnan(result)) {
        return Value::nan();
    }
    return Value(result);
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
        if (is_positive_infinity()) {
            if (other.is_positive_infinity()) return 0;
            return 1;
        }
        if (is_negative_infinity()) {
            if (other.is_negative_infinity()) return 0;
            return -1;
        }
        if (other.is_positive_infinity()) {
            return -1;
        }
        if (other.is_negative_infinity()) {
            return 1;
        }
        
        double left = as_number();
        double right = other.as_number();
        if (left < right) return -1;
        if (left > right) return 1;
        return 0;
    }
    if (is_bigint() && other.is_bigint()) {
        if (*as_bigint() < *other.as_bigint()) return -1;
        if (*as_bigint() > *other.as_bigint()) return 1;
        return 0;
    }

    if (is_number() || other.is_number()) {
        double left = to_number();
        double right = other.to_number();
        if (std::isnan(left) || std::isnan(right)) {
            return 0;
        }
        if (left < right) return -1;
        if (left > right) return 1;
        return 0;
    }

    std::string left_str = to_string();
    std::string right_str = other.to_string();
    if (left_str < right_str) return -1;
    if (left_str > right_str) return 1;
    return 0;
}

bool Value::instanceof_check(const Value& constructor) const {
    if ((!is_object() && !is_function()) || !constructor.is_function()) {
        return false;
    }
    
    Function* ctor = constructor.as_function();
    std::string ctor_name = ctor->get_name();
    
    if (is_function()) {
        if (ctor_name == "Function") {
            return true;
        }
        if (ctor_name == "Object") {
            return true;
        }
        return false;
    }
    
    Object* obj = as_object();
    
    Value prototype_prop = ctor->get_property("prototype");
    if (!prototype_prop.is_object()) {
        return false;
    }
    Object* ctor_prototype = prototype_prop.as_object();
    
    Object* current = obj;
    while (current != nullptr) {
        Object* current_proto = current->get_prototype();
        if (current_proto == nullptr) {
            break;
        }

        if (current_proto == ctor_prototype) {
            return true;
        }

        current = current_proto;
    }
    
    if (ctor_name == "Array") {
        return obj->is_array();
    }
    
    if (ctor_name == "RegExp") {
        return obj->has_property("_isRegExp");
    }
    
    if (ctor_name == "Date") {
        return obj->has_property("_isDate");
    }
    
    if (ctor_name == "Error" || ctor_name == "TypeError" || ctor_name == "ReferenceError") {
        return obj->has_property("_isError");
    }
    
    if (ctor_name == "Promise") {
        return obj->has_property("_isPromise");
    }

    if (ctor_name == "Map") {
        return obj->get_type() == Object::ObjectType::Map;
    }

    if (ctor_name == "Set") {
        return obj->get_type() == Object::ObjectType::Set;
    }

    if (ctor_name == "Object") {
        return true;
    }
    
    return false;
}


namespace ValueFactory {

Value create_function(std::unique_ptr<Function> function_obj) {
    return Value(function_obj.release());
}

}

}
