/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/Value.h"
#include "../include/Object.h"
#include "../include/String.h"
#include "../include/BigInt.h"
#include "../include/Symbol.h"
#include <sstream>
#include <cmath>
#include <limits>
#include <iostream>

namespace Quanta {

//=============================================================================
// Value Implementation
//=============================================================================

Value::Value(Object* obj) {
    // FIX: If obj is null, create undefined instead of object with null pointer
    if (!obj) {
        bits_ = QUIET_NAN | TAG_UNDEFINED;
        return;
    }
    
    // Store object pointer directly - same as other constructors
    bits_ = QUIET_NAN | TAG_OBJECT | (reinterpret_cast<uint64_t>(obj) & PAYLOAD_MASK);
}

Value::Value(const std::string& str) {
    // Create a String object directly without going through ObjectFactory
    auto string_obj = std::make_unique<String>(str);
    bits_ = QUIET_NAN | TAG_STRING | (reinterpret_cast<uint64_t>(string_obj.release()) & PAYLOAD_MASK);
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
    if (is_bigint()) {
        return as_bigint()->to_string();
    }
    if (is_symbol()) {
        return as_symbol()->to_string();
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
    if (is_bigint()) {
        return as_bigint()->to_double();
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
    return true; // objects and functions are truthy
}

Value Value::typeof_op() const {
    if (is_undefined()) return Value(std::string("undefined"));
    if (is_null()) return Value(std::string("object"));
    if (is_boolean()) return Value(std::string("boolean"));
    if (is_number()) return Value(std::string("number"));
    if (is_string()) return Value(std::string("string"));
    if (is_symbol()) return Value(std::string("symbol"));
    if (is_bigint()) return Value(std::string("bigint"));
    if (is_function()) return Value(std::string("function"));
    
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
        double a = as_number();
        double b = other.as_number();
        // Handle NaN properly: NaN is never equal to anything, including itself
        if (std::isnan(a) || std::isnan(b)) return false;
        return a == b;
    }
    if (is_string() && other.is_string()) return as_string()->str() == other.as_string()->str();
    if (is_bigint() && other.is_bigint()) return *as_bigint() == *other.as_bigint();
    if (is_symbol() && other.is_symbol()) return as_symbol()->equals(other.as_symbol());
    if (is_object() && other.is_object()) return as_object() == other.as_object();
    if (is_function() && other.is_function()) return as_function() == other.as_function();
    return false;
}

bool Value::loose_equals(const Value& other) const {
    // Implement JavaScript loose equality (==) according to ECMAScript spec
    
    // 1. If types are the same, use strict equality
    if ((is_undefined() && other.is_undefined()) ||
        (is_null() && other.is_null()) ||
        (is_boolean() && other.is_boolean()) ||
        (is_number() && other.is_number()) ||
        (is_string() && other.is_string()) ||
        (is_object() && other.is_object()) ||
        (is_function() && other.is_function())) {
        return strict_equals(other);
    }
    
    // 2. null == undefined
    if ((is_null() && other.is_undefined()) || (is_undefined() && other.is_null())) {
        return true;
    }
    
    // 3. Number and String comparison (coerce string to number)
    if (is_number() && other.is_string()) {
        return as_number() == other.to_number();
    }
    if (is_string() && other.is_number()) {
        return to_number() == other.as_number();
    }
    
    // 4. Boolean comparison (coerce boolean to number)
    if (is_boolean()) {
        return Value(to_number()).loose_equals(other);
    }
    if (other.is_boolean()) {
        return loose_equals(Value(other.to_number()));
    }
    
    // 5. Object to primitive comparison
    if (is_object() && (other.is_string() || other.is_number())) {
        return Value(to_string()).loose_equals(other);
    }
    if ((is_string() || is_number()) && other.is_object()) {
        return loose_equals(Value(other.to_string()));
    }
    
    // 6. No other cases match
    return false;
}

Value Value::add(const Value& other) const {
    // optimized: Fast path for number + number (most common case)
    if (is_number() && other.is_number()) {
        return Value(as_number() + other.as_number());
    }
    
    // Handle BigInt cases
    if (is_bigint() && other.is_bigint()) {
        BigInt result = *as_bigint() + *other.as_bigint();
        return Value(new BigInt(result));
    }
    if (is_bigint() || other.is_bigint()) {
        throw std::runtime_error("Cannot mix BigInt and other types in addition");
    }
    
    // JavaScript + operator: if either operand is string, concatenate; otherwise, add as numbers
    if (is_string() || other.is_string()) {
        return Value(to_string() + other.to_string());
    }
    
    // Both are non-string, non-BigInt - convert to numbers and add
    return Value(to_number() + other.to_number());
}

Value Value::subtract(const Value& other) const {
    // optimized: Fast path for number - number
    if (is_number() && other.is_number()) {
        return Value(as_number() - other.as_number());
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
    // optimized: Fast path for number * number (very common)
    if (is_number() && other.is_number()) {
        return Value(as_number() * other.as_number());
    }
    
    if (is_bigint() && other.is_bigint()) {
        BigInt result = *as_bigint() * *other.as_bigint();
        return Value(new BigInt(result));
    }
    if (is_bigint() || other.is_bigint()) {
        throw std::runtime_error("Cannot mix BigInt and other types in multiplication");
    }
    return Value(to_number() * other.to_number());
}

Value Value::divide(const Value& other) const {
    // optimized: Fast path for number / number with proper division by zero handling
    if (is_number() && other.is_number()) {
        double a = as_number();
        double b = other.as_number();
        
        // JavaScript division by zero behavior
        if (b == 0.0) {
            if (a == 0.0) return Value(std::numeric_limits<double>::quiet_NaN()); // 0/0 = NaN
            return Value(a > 0 ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity());
        }
        return Value(a / b);
    }
    
    return Value(to_number() / other.to_number());
}

Value Value::modulo(const Value& other) const {
    // optimized: Fast path for number % number
    if (is_number() && other.is_number()) {
        double a = as_number();
        double b = other.as_number();
        if (b == 0.0) return Value(std::numeric_limits<double>::quiet_NaN());
        return Value(std::fmod(a, b));
    }
    return Value(std::fmod(to_number(), other.to_number()));
}

Value Value::power(const Value& other) const {
    // optimized: Fast path for number ** number
    if (is_number() && other.is_number()) {
        return Value(std::pow(as_number(), other.as_number()));
    }
    return Value(std::pow(to_number(), other.to_number()));
}

Value Value::unary_plus() const {
    // optimized: Fast path for +number
    if (is_number()) {
        return *this; // Already a number, return as-is
    }
    return Value(to_number());
}

Value Value::unary_minus() const {
    // optimized: Fast path for -number
    if (is_number()) {
        return Value(-as_number());
    }
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
    if (is_bigint() && other.is_bigint()) {
        if (*as_bigint() < *other.as_bigint()) return -1;
        if (*as_bigint() > *other.as_bigint()) return 1;
        return 0;
    }
    // For simplicity, convert to strings for comparison
    std::string left_str = to_string();
    std::string right_str = other.to_string();
    if (left_str < right_str) return -1;
    if (left_str > right_str) return 1;
    return 0;
}

bool Value::instanceof_check(const Value& constructor) const {
    // instanceof requires an object/function on the left and function on the right
    if ((!is_object() && !is_function()) || !constructor.is_function()) {
        return false;
    }
    
    Function* ctor = constructor.as_function();
    std::string ctor_name = ctor->get_name();
    
    // Handle function instanceof checks first
    if (is_function()) {
        if (ctor_name == "Function") {
            return true;
        }
        if (ctor_name == "Object") {
            return true; // functions are objects
        }
        return false;
    }
    
    Object* obj = as_object();
    
    // Get the constructor's prototype
    Value prototype_prop = ctor->get_property("prototype");
    if (!prototype_prop.is_object()) {
        return false;
    }
    Object* ctor_prototype = prototype_prop.as_object();
    
    // Walk the prototype chain
    Object* current = obj;
    while (current != nullptr) {
        // Get the prototype of the current object
        Value proto = current->get_property("__proto__");
        if (!proto.is_object()) {
            break;
        }
        
        Object* current_proto = proto.as_object();
        if (current_proto == ctor_prototype) {
            return true;
        }
        
        current = current_proto;
    }
    
    // Special cases for built-in constructors (objects only, not functions)
    // Array instanceof
    if (ctor_name == "Array") {
        return obj->is_array();
    }
    
    // RegExp instanceof
    if (ctor_name == "RegExp") {
        return obj->has_property("_isRegExp");
    }
    
    // Date instanceof
    if (ctor_name == "Date") {
        return obj->has_property("_isDate");
    }
    
    // Error instanceof
    if (ctor_name == "Error" || ctor_name == "TypeError" || ctor_name == "ReferenceError") {
        return obj->has_property("_isError");
    }
    
    // Promise instanceof
    if (ctor_name == "Promise") {
        return obj->has_property("_isPromise");
    }
    
    // Object instanceof (everything is an object)
    if (ctor_name == "Object") {
        return true;
    }
    
    return false;
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