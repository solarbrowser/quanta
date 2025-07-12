#include "Value.h"
#include "String.h"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <limits>

namespace Quanta {

// Forward declarations for internal objects
class String;

// Static constants
const Value Value::UNDEFINED = Value();
const Value Value::NULL_VALUE = Value::null();
const Value Value::TRUE_VALUE = Value(true);
const Value Value::FALSE_VALUE = Value(false);
const Value Value::ZERO = Value(0.0);
const Value Value::ONE = Value(1.0);
const Value Value::NAN_VALUE = Value(std::numeric_limits<double>::quiet_NaN());
const Value Value::INFINITY_VALUE = Value(std::numeric_limits<double>::infinity());
const Value Value::NEGATIVE_INFINITY_VALUE = Value(-std::numeric_limits<double>::infinity());

// String constructor implementation (temporary - will use proper String class later)
Value::Value(const std::string& str) {
    // Create a proper String object and store its pointer
    String* string_obj = new String(str);
    uintptr_t ptr = reinterpret_cast<uintptr_t>(string_obj);
    bits_ = QUIET_NAN | TAG_STRING | (ptr & PAYLOAD_MASK);
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
    if (is_object()) return Type::Object;
    return Type::Undefined; // fallback
}

bool Value::to_boolean() const {
    if (is_boolean()) {
        return as_boolean();
    }
    if (is_undefined() || is_null()) {
        return false;
    }
    if (is_number()) {
        double num = as_number();
        return !(num == 0.0 || std::isnan(num));
    }
    if (is_string()) {
        // For now, check if empty (placeholder implementation)
        return (bits_ & PAYLOAD_MASK) != 0;
    }
    // Objects, functions, symbols, bigints are truthy
    return true;
}

double Value::to_number() const {
    if (is_number()) {
        return as_number();
    }
    if (is_undefined()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (is_null()) {
        return 0.0;
    }
    if (is_boolean()) {
        return as_boolean() ? 1.0 : 0.0;
    }
    if (is_string()) {
        // Placeholder: return 0 for now
        return 0.0;
    }
    // Objects, functions, symbols, bigints
    return std::numeric_limits<double>::quiet_NaN();
}

std::string Value::to_string() const {
    if (is_undefined()) {
        return "undefined";
    }
    if (is_null()) {
        return "null";
    }
    if (is_boolean()) {
        return as_boolean() ? "true" : "false";
    }
    if (is_number()) {
        return number_to_string(as_number());
    }
    if (is_string()) {
        return as_string()->str();
    }
    if (is_function()) {
        return "[Function]";
    }
    if (is_object()) {
        return "[Object]";
    }
    if (is_symbol()) {
        return "[Symbol]";
    }
    if (is_bigint()) {
        return "[BigInt]";
    }
    return "[Unknown]";
}

Object* Value::to_object() const {
    if (is_object()) {
        return as_object();
    }
    if (is_function()) {
        return reinterpret_cast<Object*>(as_function());
    }
    // Primitive values need to be boxed - placeholder for now
    return nullptr;
}

bool Value::strict_equals(const Value& other) const {
    // Fast path: same bits
    if (bits_ == other.bits_) {
        return true;
    }
    
    // Handle NaN case
    if (is_number() && other.is_number()) {
        double a = as_number();
        double b = other.as_number();
        if (std::isnan(a) && std::isnan(b)) {
            return false; // NaN !== NaN
        }
        return a == b;
    }
    
    return false;
}

bool Value::loose_equals(const Value& other) const {
    // If types are the same, use strict equality
    if (get_type() == other.get_type()) {
        return strict_equals(other);
    }
    
    // null == undefined
    if ((is_null() && other.is_undefined()) || (is_undefined() && other.is_null())) {
        return true;
    }
    
    // Number and boolean coercion
    if (is_number() && other.is_boolean()) {
        return strict_equals(Value(other.to_number()));
    }
    if (is_boolean() && other.is_number()) {
        return Value(to_number()).strict_equals(other);
    }
    
    // String and number coercion
    if (is_string() && other.is_number()) {
        return Value(to_number()).strict_equals(other);
    }
    if (is_number() && other.is_string()) {
        return strict_equals(Value(other.to_number()));
    }
    
    return false;
}

int Value::compare(const Value& other) const {
    // Convert to primitives first
    double a = to_number();
    double b = other.to_number();
    
    if (std::isnan(a) || std::isnan(b)) {
        return 0; // undefined comparison
    }
    
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

Value Value::add(const Value& other) const {
    // String concatenation takes precedence
    if (is_string() || other.is_string()) {
        return Value(to_string() + other.to_string());
    }
    
    // Numeric addition
    return Value(to_number() + other.to_number());
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
    double a = to_number();
    double b = other.to_number();
    return Value(std::fmod(a, b));
}

Value Value::power(const Value& other) const {
    return Value(std::pow(to_number(), other.to_number()));
}

Value Value::bitwise_and(const Value& other) const {
    int32_t a = static_cast<int32_t>(to_number());
    int32_t b = static_cast<int32_t>(other.to_number());
    return Value(static_cast<double>(a & b));
}

Value Value::bitwise_or(const Value& other) const {
    int32_t a = static_cast<int32_t>(to_number());
    int32_t b = static_cast<int32_t>(other.to_number());
    return Value(static_cast<double>(a | b));
}

Value Value::bitwise_xor(const Value& other) const {
    int32_t a = static_cast<int32_t>(to_number());
    int32_t b = static_cast<int32_t>(other.to_number());
    return Value(static_cast<double>(a ^ b));
}

Value Value::bitwise_not() const {
    int32_t a = static_cast<int32_t>(to_number());
    return Value(static_cast<double>(~a));
}

Value Value::left_shift(const Value& other) const {
    int32_t a = static_cast<int32_t>(to_number());
    uint32_t b = static_cast<uint32_t>(other.to_number()) & 0x1F;
    return Value(static_cast<double>(a << b));
}

Value Value::right_shift(const Value& other) const {
    int32_t a = static_cast<int32_t>(to_number());
    uint32_t b = static_cast<uint32_t>(other.to_number()) & 0x1F;
    return Value(static_cast<double>(a >> b));
}

Value Value::unsigned_right_shift(const Value& other) const {
    uint32_t a = static_cast<uint32_t>(to_number());
    uint32_t b = static_cast<uint32_t>(other.to_number()) & 0x1F;
    return Value(static_cast<double>(a >> b));
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

Value Value::typeof_op() const {
    switch (get_type()) {
        case Type::Undefined: return Value("undefined");
        case Type::Null: return Value("object"); // JavaScript quirk
        case Type::Boolean: return Value("boolean");
        case Type::Number: return Value("number");
        case Type::String: return Value("string");
        case Type::Symbol: return Value("symbol");
        case Type::BigInt: return Value("bigint");
        case Type::Function: return Value("function");
        case Type::Object: return Value("object");
    }
    return Value("undefined");
}

std::string Value::debug_string() const {
    std::ostringstream oss;
    oss << "Value(type=" << static_cast<int>(get_type()) 
        << ", bits=0x" << std::hex << bits_ 
        << ", value=" << to_string() << ")";
    return oss.str();
}

size_t Value::hash() const {
    if (is_number()) {
        // Hash the raw bits for numbers
        return std::hash<uint64_t>{}(bits_);
    }
    
    // For other types, hash the type and payload
    size_t type_hash = std::hash<uint8_t>{}(static_cast<uint8_t>(get_type()));
    size_t payload_hash = std::hash<uint64_t>{}(bits_ & PAYLOAD_MASK);
    
    // Combine hashes
    return type_hash ^ (payload_hash << 1);
}

void Value::mark_referenced_objects() const {
    // Mark objects for garbage collection
    if (is_object() && as_object()) {
        // Will implement when GC is ready
    }
    if (is_function() && as_function()) {
        // Will implement when GC is ready
    }
    if (is_string() && as_string()) {
        // Will implement when GC is ready
    }
}

bool Value::is_canonical_nan(uint64_t bits) {
    return (bits & EXPONENT_MASK) == EXPONENT_MASK && (bits & MANTISSA_MASK) != 0;
}

double Value::number_from_string(const std::string& str) {
    // Simple implementation - will be enhanced later
    try {
        // Handle special cases
        if (str.empty() || str == " ") return 0.0;
        if (str == "Infinity") return std::numeric_limits<double>::infinity();
        if (str == "-Infinity") return -std::numeric_limits<double>::infinity();
        
        // Parse number
        char* end;
        double result = std::strtod(str.c_str(), &end);
        
        // Check if entire string was consumed
        if (end == str.c_str() + str.length()) {
            return result;
        }
        
        return std::numeric_limits<double>::quiet_NaN();
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

std::string Value::number_to_string(double num) {
    if (std::isnan(num)) {
        return "NaN";
    }
    if (std::isinf(num)) {
        return num > 0 ? "Infinity" : "-Infinity";
    }
    if (num == 0.0) {
        return "0";
    }
    
    // Use appropriate precision
    std::ostringstream oss;
    if (num == std::floor(num) && std::abs(num) < 1e15) {
        // Integer representation
        oss << static_cast<int64_t>(num);
    } else {
        // Floating point representation
        oss << std::setprecision(17) << num;
        
        // Remove trailing zeros
        std::string result = oss.str();
        if (result.find('.') != std::string::npos) {
            result = result.substr(0, result.find_last_not_of('0') + 1);
            if (result.back() == '.') {
                result.pop_back();
            }
        }
        return result;
    }
    return oss.str();
}

} // namespace Quanta