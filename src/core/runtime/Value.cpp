/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/ProxyReflect.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/String.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/runtime/Symbol.h"
#include "quanta/core/runtime/Error.h"
#include "quanta/parser/AST.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <limits>
#include <iostream>
#include <cstdio>
#include <charconv>
#include <cstring>

namespace Quanta {

// ES1 9.5 ToInt32: Convert to signed 32-bit integer
static int32_t ToInt32(double number) {
    if (std::isnan(number) || std::isinf(number) || number == 0.0) {
        return 0;
    }

    // 2. Compute number modulo 2^32
    double int32bit = std::fmod(number, 4294967296.0);  // 2^32

    // 3. If result >= 2^31, subtract 2^32 to get signed value
    if (int32bit >= 2147483648.0) {  // 2^31
        int32bit -= 4294967296.0;  // 2^32
    } else if (int32bit < -2147483648.0) {  // -2^31
        int32bit += 4294967296.0;  // 2^32
    }

    return static_cast<int32_t>(int32bit);
}

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

    // Check if the object is actually a Function
    if (obj->is_function()) {
        bits_ = QUIET_NAN | TAG_FUNCTION | masked_value;
    } else {
        bits_ = QUIET_NAN | TAG_OBJECT | masked_value;
    }
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

        uint64_t bits = std::bit_cast<uint64_t>(num);

        uint64_t exponent = (bits >> 52) & 0x7FF;
        uint64_t mantissa = bits & 0xFFFFFFFFFFFFFULL;

        if (exponent == 0x7FF) {
            if (mantissa != 0) {
                return "NaN";
            } else {
                return (bits & 0x8000000000000000ULL) ? "-Infinity" : "Infinity";
            }
        }

        // -0 should convert to "0"
        if (num == 0.0) {
            return "0";
        }

        // Use exponential notation only if:
        // - abs value < 1e-6 (excluding 0), or
        // - abs value >= 1e21
        // Otherwise use decimal notation
        double abs_num = std::abs(num);
        bool use_exponential = (abs_num < 1e-6 && abs_num != 0.0) || abs_num >= 1e21;

        std::ostringstream oss;
        if (use_exponential) {
            oss << num;
        } else {
            // Use fixed notation for numbers in range
            // Check if it's effectively an integer
            if (num == std::floor(num)) {
                oss << std::fixed << std::setprecision(0) << num;
            } else {
                // setprecision alone still falls back to scientific notation for small
                // magnitudes (e.g. 1e-6); to_chars with chars_format::fixed forces fixed
                // notation with the shortest round-tripping digit sequence.
                char buf[64];
                auto res = std::to_chars(buf, buf + sizeof(buf), num, std::chars_format::fixed);
                oss << std::string(buf, res.ptr);
            }
        }
        std::string result = oss.str();

        // Fix exponential notation format (e-07 → e-7, e+21 → e+21)
        size_t e_pos = result.find('e');
        if (e_pos != std::string::npos && e_pos + 2 < result.length()) {
            // Check if next char is + or -
            if (result[e_pos + 1] == '+' || result[e_pos + 1] == '-') {
                // Remove leading zeros from exponent
                size_t exp_start = e_pos + 2;
                size_t first_nonzero = exp_start;
                while (first_nonzero < result.length() && result[first_nonzero] == '0') {
                    first_nonzero++;
                }
                // If all zeros or no zeros to remove, keep at least one digit
                if (first_nonzero >= result.length()) {
                    result = result.substr(0, e_pos + 2) + "0";
                } else if (first_nonzero > exp_start) {
                    result = result.substr(0, e_pos + 2) + result.substr(first_nonzero);
                }
            }
        }

        return result;
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
        return as_function()->to_string();
    }
    return "unknown";
}

std::string Value::to_property_key() const {
    if (is_symbol()) {
        return as_symbol()->to_property_key();
    }
    // For objects, ToPrimitive with "string" hint: check @@toPrimitive first, then toString/valueOf
    if ((is_object() || is_function()) && Object::current_context_) {
        Context& ctx = *Object::current_context_;
        Object* obj = is_function() ? static_cast<Object*>(as_function()) : as_object();
        // @@toPrimitive takes priority with hint "string"
        Value toPrim_fn = obj->get_property("Symbol.toPrimitive");
        if (!ctx.has_exception() && toPrim_fn.is_function()) {
            Value prim = toPrim_fn.as_function()->call(ctx, {Value(std::string("string"))}, Value(obj));
            if (!ctx.has_exception()) {
                if (prim.is_symbol()) return prim.as_symbol()->to_property_key();
                if (!prim.is_object() && !prim.is_function()) return prim.to_string();
                ctx.throw_type_error("Cannot convert object to property key");
            }
            return "";
        }
        if (ctx.has_exception()) return "";
        // String hint: try toString first
        Value toString_fn = obj->get_property("toString");
        if (toString_fn.is_function()) {
            Value prim = toString_fn.as_function()->call(ctx, {}, Value(obj));
            if (!ctx.has_exception()) {
                if (prim.is_symbol()) return prim.as_symbol()->to_property_key();
                if (!prim.is_object() && !prim.is_function()) return prim.to_string();
            }
        }
        if (!ctx.has_exception()) {
            Value valueOf_fn = obj->get_property("valueOf");
            if (valueOf_fn.is_function()) {
                Value prim = valueOf_fn.as_function()->call(ctx, {}, Value(obj));
                if (!ctx.has_exception()) {
                    if (prim.is_symbol()) return prim.as_symbol()->to_property_key();
                    if (!prim.is_object() && !prim.is_function()) return prim.to_string();
                }
            }
        }
        if (!ctx.has_exception()) {
            ctx.throw_type_error("Cannot convert object to property key");
        }
        return "";
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

        // Trim all JS WhiteSpace / LineTerminator including the full Unicode set.
        auto js_ws_len = [](const std::string& s, size_t i) -> size_t {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (std::isspace(c)) return 1;
            if (c == 0xC2 && i+1 < s.size() && static_cast<unsigned char>(s[i+1]) == 0xA0) return 2; // U+00A0
            if (i+2 < s.size()) {
                unsigned char c2 = static_cast<unsigned char>(s[i+1]), c3 = static_cast<unsigned char>(s[i+2]);
                if (c == 0xEF && c2 == 0xBB && c3 == 0xBF) return 3; // U+FEFF
                if (c == 0xE1 && c2 == 0x9A && c3 == 0x80) return 3; // U+1680
                if (c == 0xE3 && c2 == 0x80 && c3 == 0x80) return 3; // U+3000
                if (c == 0xE2 && c2 == 0x80) {
                    // U+2000-U+200A, U+2028, U+2029, U+202F
                    if (c3 >= 0x80 && c3 <= 0x8A) return 3;
                    if (c3 == 0xA8 || c3 == 0xA9 || c3 == 0xAF) return 3;
                }
                if (c == 0xE2 && c2 == 0x81 && c3 == 0x9F) return 3; // U+205F
            }
            return 0;
        };
        size_t start = 0, end = str.length();
        while (start < end) { size_t n = js_ws_len(str, start); if (!n) break; start += n; }
        // Trim end: scan backward by trying 3-byte, 2-byte, 1-byte sequences.
        while (end > start) {
            size_t n = 0;
            if (end >= 3) n = js_ws_len(str, end-3); if (n == 3) { end -= 3; continue; }
            if (end >= 2) n = js_ws_len(str, end-2); if (n == 2) { end -= 2; continue; }
            n = js_ws_len(str, end-1); if (n == 1) { end -= 1; continue; }
            break;
        }

        // If only whitespace, return 0
        if (start >= end) return 0.0;

        std::string trimmed = str.substr(start, end - start);

        // ES6: Handle binary (0b/0B) and octal (0o/0O) string literals (no underscores allowed per ToNumber).
        if (trimmed.length() >= 3 && trimmed[0] == '0') {
            if (trimmed[1] == 'b' || trimmed[1] == 'B') {
                std::string digits = trimmed.substr(2);
                if (digits.find('_') != std::string::npos) return std::numeric_limits<double>::quiet_NaN();
                try {
                    size_t consumed = 0;
                    unsigned long long v = std::stoull(digits, &consumed, 2);
                    if (consumed != digits.size()) return std::numeric_limits<double>::quiet_NaN();
                    return static_cast<double>(v);
                } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
            }
            if (trimmed[1] == 'o' || trimmed[1] == 'O') {
                std::string digits = trimmed.substr(2);
                if (digits.find('_') != std::string::npos) return std::numeric_limits<double>::quiet_NaN();
                try {
                    size_t consumed = 0;
                    unsigned long long v = std::stoull(digits, &consumed, 8);
                    if (consumed != digits.size()) return std::numeric_limits<double>::quiet_NaN();
                    return static_cast<double>(v);
                } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
            }
            if (trimmed[1] == 'x' || trimmed[1] == 'X') {
                std::string digits = trimmed.substr(2);
                if (digits.empty() || digits.find('_') != std::string::npos) return std::numeric_limits<double>::quiet_NaN();
                // Validate: only hex digits allowed (no sign, no decimal, no exponent).
                for (char c : digits) {
                    if (!std::isxdigit(static_cast<unsigned char>(c))) return std::numeric_limits<double>::quiet_NaN();
                }
                try {
                    size_t consumed = 0;
                    unsigned long long v = std::stoull(digits, &consumed, 16);
                    if (consumed != digits.size()) return std::numeric_limits<double>::quiet_NaN();
                    return static_cast<double>(v);
                } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
            }
        }
        // Reject "+0x..." / "-0x..." -- hex prefix requires exact "0x" start, no sign allowed.
        if (trimmed.length() >= 4 && (trimmed[0] == '+' || trimmed[0] == '-') &&
            trimmed[1] == '0' && (trimmed[2] == 'x' || trimmed[2] == 'X')) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        // Only "Infinity"/"+Infinity"/"-Infinity" are valid (case-sensitive); reject "INFINITY" etc.
        if (trimmed == "Infinity" || trimmed == "+Infinity") return std::numeric_limits<double>::infinity();
        if (trimmed == "-Infinity") return -std::numeric_limits<double>::infinity();
        // Reject numeric separator underscores in decimal strings.
        if (trimmed.find('_') != std::string::npos) return std::numeric_limits<double>::quiet_NaN();

        try {
            size_t consumed = 0;
            double result = std::stod(trimmed, &consumed);
            // JS ToNumber requires the *entire* trimmed string to be numeric
            if (consumed != trimmed.length()) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            // stod accepts "INFINITY"/"INF" case-insensitively; only "Infinity" is valid in JS.
            // BUT numeric overflow (e.g. "10e10000") is valid and should return Infinity.
            if (std::isinf(result) && trimmed != "Infinity" && trimmed != "+Infinity" && trimmed != "-Infinity") {
                // Reject if string looks like a word form of infinity (starts with letter)
                char first = (trimmed[0] == '+' || trimmed[0] == '-') ? trimmed[1] : trimmed[0];
                if (!std::isdigit(static_cast<unsigned char>(first))) {
                    return std::numeric_limits<double>::quiet_NaN();
                }
            }
            return result;
        } catch (...) {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    if (is_bigint()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot convert a BigInt value to a number");
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (is_symbol()) {
        if (Object::current_context_) Object::current_context_->throw_type_error("Cannot convert a Symbol value to a number");
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
        // Number/Boolean wrapper objects store their primitive in [[PrimitiveValue]]
        if (obj && obj->has_property("[[PrimitiveValue]]")) {
            Value pv = obj->get_property("[[PrimitiveValue]]");
            if (!pv.is_object()) {
                return pv.to_number();
            }
        }
        // ToPrimitive with "number" hint: @@toPrimitive("number") first, then valueOf, then toString.
        if (obj && Object::current_context_) {
            Context& ctx = *Object::current_context_;
            Symbol* toPrim_sym = Symbol::get_well_known(Symbol::TO_PRIMITIVE);
            if (toPrim_sym) {
                Value toPrim = obj->get_property(toPrim_sym->to_property_key());
                if (ctx.has_exception()) return std::numeric_limits<double>::quiet_NaN();
                if (!toPrim.is_null() && !toPrim.is_undefined()) {
                    if (!toPrim.is_function()) { ctx.throw_type_error("@@toPrimitive is not callable"); return std::numeric_limits<double>::quiet_NaN(); }
                    Value r = toPrim.as_function()->call(ctx, {Value(std::string("number"))}, Value(obj));
                    if (ctx.has_exception()) return std::numeric_limits<double>::quiet_NaN();
                    if (r.is_object() || r.is_function()) { ctx.throw_type_error("@@toPrimitive returned an object"); return std::numeric_limits<double>::quiet_NaN(); }
                    return r.to_number();
                }
            }
            Value valueOf_fn = obj->get_property("valueOf");
            if (ctx.has_exception()) return std::numeric_limits<double>::quiet_NaN();
            if (valueOf_fn.is_function()) {
                Value prim = valueOf_fn.as_function()->call(ctx, {}, Value(obj));
                if (ctx.has_exception()) return std::numeric_limits<double>::quiet_NaN();
                if (!prim.is_object() && !prim.is_function()) return prim.to_number();
            }
            Value toString_fn = obj->get_property("toString");
            if (ctx.has_exception()) return std::numeric_limits<double>::quiet_NaN();
            if (toString_fn.is_function()) {
                Value prim = toString_fn.as_function()->call(ctx, {}, Value(obj));
                if (ctx.has_exception()) return std::numeric_limits<double>::quiet_NaN();
                if (!prim.is_object() && !prim.is_function()) return prim.to_number();
            }
            ctx.throw_type_error("Cannot convert object to number");
            return std::numeric_limits<double>::quiet_NaN();
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool Value::to_boolean() const {
    if (is_boolean()) return as_boolean();
    if (is_undefined() || is_null()) return false;
    if (is_number()) {
        // NaN is falsy
        if (is_nan()) return false;

        double num = as_number();
        // 0 and -0 are falsy
        if (num == 0.0 || num == -0.0) {
            return false;
        }
        return true;
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
    // A Proxy is tagged TAG_OBJECT regardless of target, so check target_was_callable() instead.
    if (is_object() && as_object()->get_type() == Object::ObjectType::Proxy &&
        static_cast<Proxy*>(as_object())->target_was_callable()) {
        return Value(std::string("function"));
    }

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
        (is_symbol() && other.is_symbol()) ||
        (is_bigint() && other.is_bigint()) ||
        (is_object() && other.is_object()) ||
        (is_function() && other.is_function())) {
        return strict_equals(other);
    }

    // BigInt == Number: compare numerically (spec 7.2.14 step 6-7)
    if (is_bigint() && other.is_number()) {
        if (other.is_nan() || other.is_positive_infinity() || other.is_negative_infinity()) return false;
        double d = other.as_number();
        if (d != std::floor(d)) return false;
        return as_bigint()->to_double() == d;
    }
    if (is_number() && other.is_bigint()) {
        return other.loose_equals(*this);
    }
    // BigInt == String: parse string as BigInt
    if (is_bigint() && other.is_string()) {
        try {
            BigInt parsed = BigInt::from_string(other.as_string()->str());
            return *as_bigint() == parsed;
        } catch(...) { return false; }
    }
    if (is_string() && other.is_bigint()) {
        return other.loose_equals(*this);
    }
    // Object with BigInt: ToPrimitive then compare
    if (is_bigint() && (other.is_object() || other.is_function())) {
        Value prim = Value(other.to_string());
        return loose_equals(prim);
    }
    if ((is_object() || is_function()) && other.is_bigint()) {
        return other.loose_equals(*this);
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
    if (is_symbol() || other.is_symbol()) {
        throw std::runtime_error("TypeError: Cannot convert a Symbol value to a string");
    }
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
    // BigInt + string = string concatenation (spec-compliant)
    if ((is_bigint() || other.is_bigint()) && (is_string() || other.is_string())) {
        return Value(new String(to_string() + other.to_string()));
    }
    if (is_bigint() || other.is_bigint()) {
        throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    }

    if (is_string() || other.is_string()) {
        // Use rope concat: O(1) for large strings, flattens lazily on first read
        String* ls = is_string()       ? as_string() : new String(to_string());
        String* rs = other.is_string() ? other.as_string() : new String(other.to_string());
        return Value(String::make_concat(ls, rs));
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
        throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
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
        throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
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
    if (is_bigint() != other.is_bigint()) throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    if (is_bigint() && other.is_bigint()) {
        BigInt* b = other.as_bigint();
        if (!b || b->is_zero()) throw BigIntRangeError("Division by zero");
        BigInt* a = as_bigint();
        if (!a) return Value(new BigInt(0));
        return Value(new BigInt(*a / *b));
    }
    if (is_number() && other.is_number()) {
        double a = as_number();
        double b = other.as_number();

        if (b == 0.0) {
            if (std::isnan(a) || a == 0.0) return Value::nan();
            bool neg = std::signbit(a) != std::signbit(b);
            return neg ? Value::negative_infinity() : Value::positive_infinity();
        }

        double result = a / b;
        if (std::isinf(result)) {
            return std::signbit(result) ? Value::negative_infinity() : Value::positive_infinity();
        }
        if (std::isnan(result)) {
            return Value::nan();
        }

        return Value(result);
    }

    double result = to_number() / other.to_number();
    if (std::isinf(result)) {
        return std::signbit(result) ? Value::negative_infinity() : Value::positive_infinity();
    }
    if (std::isnan(result)) {
        return Value::nan();
    }
    return Value(result);
}

Value Value::modulo(const Value& other) const {
    if (is_bigint() != other.is_bigint()) throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    if (is_bigint() && other.is_bigint()) {
        BigInt* b = other.as_bigint();
        if (!b || b->is_zero()) throw BigIntRangeError("Division by zero");
        BigInt* a = as_bigint();
        if (!a) return Value(new BigInt(0));
        return Value(new BigInt(*a % *b));
    }
    if (is_number() && other.is_number()) {
        double a = as_number();
        double b = other.as_number();
        double result = std::fmod(a, b);
        if (std::isnan(result)) return Value::nan();
        return Value(result);
    }
    double result = std::fmod(to_number(), other.to_number());
    if (std::isnan(result)) return Value::nan();
    return Value(result);
}

Value Value::power(const Value& other) const {
    if (is_bigint() != other.is_bigint()) throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    if (is_bigint() && other.is_bigint()) {
        BigInt* exp = other.as_bigint();
        if (!exp) return Value(new BigInt(1));
        if (exp->is_negative()) throw BigIntRangeError("Exponent must be positive");
        BigInt* base = as_bigint();
        if (!base) return Value(new BigInt(0));
        return Value(new BigInt(BigInt::pow(*base, *exp)));
    }
    if (is_number() && other.is_number()) {
        double base = as_number();
        double exp = other.as_number();
        // Per spec: if exp is NaN, return NaN (overrides C's pow(1,NaN)=1).
        if (std::isnan(exp)) return Value::nan();
        if (std::isnan(base)) return Value::nan();
        // Per spec: if |base| == 1 and exp is ±Infinity, result is NaN
        if ((base == 1.0 || base == -1.0) && std::isinf(exp)) {
            return Value::nan();
        }
        double result = std::pow(base, exp);
        if (std::isinf(result)) return std::signbit(result) ? Value::negative_infinity() : Value::positive_infinity();
        if (std::isnan(result)) return Value::nan();
        return Value(result);
    }
    double result = std::pow(to_number(), other.to_number());
    if (std::isinf(result)) return std::signbit(result) ? Value::negative_infinity() : Value::positive_infinity();
    if (std::isnan(result)) return Value::nan();
    return Value(result);
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
    
    if (is_bigint()) {
        BigInt* bg = as_bigint();
        if (bg) {
            BigInt negated = -(*bg);
            return Value(new BigInt(negated));
        }
        return Value(new BigInt(0));
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
    if (is_bigint()) return Value(new BigInt(as_bigint()->bitwise_not()));
    int32_t num = ToInt32(to_number());
    return Value(static_cast<double>(~num));
}

Value Value::left_shift(const Value& other) const {
    if (is_bigint() != other.is_bigint()) throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    if (is_bigint()) return Value(new BigInt(as_bigint()->left_shift(*other.as_bigint())));
    int32_t left = ToInt32(to_number());
    int32_t right = ToInt32(other.to_number()) & 0x1F;
    return Value(static_cast<double>(left << right));
}

Value Value::right_shift(const Value& other) const {
    if (is_bigint() != other.is_bigint()) throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    if (is_bigint()) return Value(new BigInt(as_bigint()->right_shift(*other.as_bigint())));
    int32_t left = ToInt32(to_number());
    int32_t right = ToInt32(other.to_number()) & 0x1F;
    return Value(static_cast<double>(left >> right));
}

Value Value::unsigned_right_shift(const Value& other) const {
    if (is_bigint() || other.is_bigint()) throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    int32_t left_signed = ToInt32(to_number());
    uint32_t left = static_cast<uint32_t>(left_signed);
    int32_t right = ToInt32(other.to_number()) & 0x1F;
    return Value(static_cast<double>(left >> right));
}

Value Value::bitwise_and(const Value& other) const {
    if (is_bigint() != other.is_bigint()) throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    if (is_bigint()) return Value(new BigInt(as_bigint()->bitwise_and(*other.as_bigint())));
    int32_t left = ToInt32(to_number());
    int32_t right = ToInt32(other.to_number());
    return Value(static_cast<double>(left & right));
}

Value Value::bitwise_or(const Value& other) const {
    if (is_bigint() != other.is_bigint()) throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    if (is_bigint()) return Value(new BigInt(as_bigint()->bitwise_or(*other.as_bigint())));
    int32_t left = ToInt32(to_number());
    int32_t right = ToInt32(other.to_number());
    return Value(static_cast<double>(left | right));
}

Value Value::bitwise_xor(const Value& other) const {
    if (is_bigint() != other.is_bigint()) throw BigIntTypeError("Cannot mix BigInt and other types, use explicit conversions");
    if (is_bigint()) return Value(new BigInt(as_bigint()->bitwise_xor(*other.as_bigint())));
    int32_t left = ToInt32(to_number());
    int32_t right = ToInt32(other.to_number());
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
    if (!is_object() && !is_function()) return false;

    // A Proxy wrapping a callable/constructible function is a valid instanceof RHS.
    if (constructor.is_object() && constructor.as_object()->get_type() == Object::ObjectType::Proxy) {
        Proxy* proxy_ctor = static_cast<Proxy*>(constructor.as_object());
        if (!proxy_ctor->target_was_callable()) return false;
        Object* obj = is_function() ? static_cast<Object*>(as_function()) : as_object();
        Value prototype_prop = proxy_ctor->get_property("prototype");
        if (!prototype_prop.is_object() && !prototype_prop.is_function()) return false;
        Object* ctor_prototype = prototype_prop.is_function()
            ? static_cast<Object*>(prototype_prop.as_function()) : prototype_prop.as_object();
        Object* current = obj->get_prototype();
        while (current) {
            if (current == ctor_prototype) return true;
            if (Object::current_context_ && Object::current_context_->has_exception()) return false;
            current = current->get_prototype();
        }
        return false;
    }

    if (!constructor.is_function()) return false;

    Function* ctor = constructor.as_function();
    std::string ctor_name = ctor->get_name();
    
    if (is_function()) {
        if (ctor_name == "Function") return true;
        if (ctor_name == "Object") return true;
        Function* fn = as_function();
        Value prototype_prop = ctor->get_property("prototype");
        if (prototype_prop.is_object()) {
            Object* ctor_prototype = prototype_prop.as_object();
            Object* current = fn->get_prototype();
            while (current) {
                if (current == ctor_prototype) return true;
                if (Object::current_context_ && Object::current_context_->has_exception()) return false;
                current = current->get_prototype();
            }
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
        // A Proxy's getPrototypeOf trap may have thrown; don't fall through to the ctor_name checks below.
        if (Object::current_context_ && Object::current_context_->has_exception()) return false;
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

    return false;
}


namespace ValueFactory {

Value create_function(std::unique_ptr<Function> function_obj) {
    return Value(function_obj.release());
}

}

}
