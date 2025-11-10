/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/value_core.h"
#include "../../include/Object.h"
#include "../../include/String.h"
#include "../../include/Symbol.h"
#include "../../include/BigInt.h"
#include <iostream>
#include <sstream>
#include <limits>

namespace Quanta {

#if PLATFORM_POINTER_COMPRESSION
thread_local uintptr_t ValueCore::heap_base_ = 0;
#endif

//=============================================================================
// ValueCore Implementation
//=============================================================================

Value ValueCore::create_undefined() {
    Value val;
    val.bits_ = QUIET_NAN | TAG_UNDEFINED;
    return val;
}

Value ValueCore::create_null() {
    Value val;
    val.bits_ = QUIET_NAN | TAG_NULL;
    return val;
}

Value ValueCore::create_boolean(bool value) {
    Value val;
    val.bits_ = encode_boolean(value);
    return val;
}

Value ValueCore::create_number(double value) {
    Value val;
    val.bits_ = encode_number(value);
    return val;
}

Value ValueCore::create_string(const std::string& str) {
    // Create String object
    auto string_obj = std::make_unique<String>(str);
    String* raw_ptr = string_obj.release();
    return create_string(raw_ptr);
}

Value ValueCore::create_string(String* str_obj) {
    Value val;
    val.bits_ = encode_pointer(str_obj, TAG_STRING);
    return val;
}

Value ValueCore::create_object(Object* obj) {
    if (!obj) {
        return create_undefined();
    }

    Value val;
    val.bits_ = encode_pointer(obj, TAG_OBJECT);
    return val;
}

Value ValueCore::create_symbol(Symbol* sym) {
    Value val;
    val.bits_ = encode_pointer(sym, TAG_SYMBOL);
    return val;
}

Value ValueCore::create_bigint(BigInt* bigint) {
    Value val;
    val.bits_ = encode_pointer(bigint, TAG_BIGINT);
    return val;
}

Value ValueCore::create_nan() {
    Value val;
    val.bits_ = std::numeric_limits<double>::quiet_NaN();
    return val;
}

Value ValueCore::create_positive_infinity() {
    Value val;
    val.bits_ = POSITIVE_INFINITY_BITS;
    return val;
}

Value ValueCore::create_negative_infinity() {
    Value val;
    val.bits_ = NEGATIVE_INFINITY_BITS;
    return val;
}

ValueCore::ValueType ValueCore::get_value_type(const Value& value) {
    if (value.is_undefined()) return ValueType::Undefined;
    if (value.is_null()) return ValueType::Null;
    if (value.is_boolean()) return ValueType::Boolean;
    if (value.is_number()) return ValueType::Number;
    if (value.is_string()) return ValueType::String;
    if (value.is_symbol()) return ValueType::Symbol;
    if (value.is_bigint()) return ValueType::BigInt;
    if (value.is_function()) return ValueType::Function;
    return ValueType::Object;
}

bool ValueCore::is_primitive(const Value& value) {
    return value.is_undefined() || value.is_null() || value.is_boolean() ||
           value.is_number() || value.is_string() || value.is_symbol() || value.is_bigint();
}

bool ValueCore::is_numeric(const Value& value) {
    return value.is_number() || value.is_bigint();
}

bool ValueCore::is_callable(const Value& value) {
    return value.is_function() || (value.is_object() && value.as_object()->is_callable());
}

uint64_t ValueCore::encode_boolean(bool value) {
    return QUIET_NAN | TAG_BOOLEAN | (value ? 1ULL : 0ULL);
}

uint64_t ValueCore::encode_number(double value) {
    // For normal numbers, store directly as double bits
    if (std::isfinite(value) && !std::isnan(value)) {
        union {
            double d;
            uint64_t bits;
        } converter;
        converter.d = value;
        return converter.bits;
    }

    // Handle special values
    if (std::isnan(value)) {
        return std::numeric_limits<uint64_t>::max(); // NaN representation
    } else if (std::isinf(value)) {
        return value > 0 ? POSITIVE_INFINITY_BITS : NEGATIVE_INFINITY_BITS;
    }

    return 0;
}

uint64_t ValueCore::encode_pointer(void* ptr, uint64_t tag) {
    if (!ptr) {
        return QUIET_NAN | TAG_NULL;
    }

#if PLATFORM_POINTER_COMPRESSION
    uint64_t compressed = compress_pointer(ptr);
    return QUIET_NAN | tag | (compressed & PAYLOAD_MASK);
#else
    uint64_t ptr_value = reinterpret_cast<uint64_t>(ptr);
    return QUIET_NAN | tag | (ptr_value & PAYLOAD_MASK);
#endif
}

bool ValueCore::decode_boolean(uint64_t bits) {
    return (bits & 1ULL) != 0;
}

double ValueCore::decode_number(uint64_t bits) {
    // Check if it's a tagged value
    if (is_tagged_value(bits)) {
        // Handle special tagged numbers
        if (bits == POSITIVE_INFINITY_BITS) {
            return std::numeric_limits<double>::infinity();
        }
        if (bits == NEGATIVE_INFINITY_BITS) {
            return -std::numeric_limits<double>::infinity();
        }
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Normal number - decode directly
    union {
        uint64_t bits;
        double d;
    } converter;
    converter.bits = bits;
    return converter.d;
}

void* ValueCore::decode_pointer(uint64_t bits) {
    uint64_t ptr_bits = bits & PAYLOAD_MASK;

#if PLATFORM_POINTER_COMPRESSION
    return decompress_pointer(ptr_bits);
#else
    return reinterpret_cast<void*>(ptr_bits);
#endif
}

bool ValueCore::is_special_number(uint64_t bits) {
    return bits == POSITIVE_INFINITY_BITS || bits == NEGATIVE_INFINITY_BITS;
}

bool ValueCore::is_tagged_value(uint64_t bits) {
    return (bits & QUIET_NAN) == QUIET_NAN;
}

uint64_t ValueCore::get_tag(uint64_t bits) {
    return bits & TAG_MASK;
}

#if PLATFORM_POINTER_COMPRESSION
void ValueCore::set_heap_base(uintptr_t base) {
    heap_base_ = base;
}

uintptr_t ValueCore::get_heap_base() {
    return heap_base_;
}

uint64_t ValueCore::compress_pointer(void* ptr) {
    if (!ptr) return 0;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < heap_base_) {
        return 0; // Invalid pointer
    }

    uintptr_t offset = addr - heap_base_;
    return static_cast<uint64_t>(offset);
}

void* ValueCore::decompress_pointer(uint64_t compressed) {
    if (compressed == 0) return nullptr;

    uintptr_t addr = heap_base_ + static_cast<uintptr_t>(compressed);
    return reinterpret_cast<void*>(addr);
}
#endif

bool ValueCore::is_valid_value(const Value& value) {
    // Basic validation - check for common corruption patterns
    uint64_t bits = value.bits_;

    // Check for obviously invalid bit patterns
    if (is_tagged_value(bits)) {
        uint64_t tag = get_tag(bits);
        if (tag > TAG_OBJECT) {
            return false; // Invalid tag
        }
    }

    return true;
}

std::string ValueCore::describe_value(const Value& value) {
    std::ostringstream oss;

    if (value.is_undefined()) {
        oss << "undefined";
    } else if (value.is_null()) {
        oss << "null";
    } else if (value.is_boolean()) {
        oss << "boolean(" << (value.as_boolean() ? "true" : "false") << ")";
    } else if (value.is_number()) {
        oss << "number(" << value.as_number() << ")";
    } else if (value.is_string()) {
        oss << "string(\"" << value.to_string() << "\")";
    } else if (value.is_object()) {
        oss << "object(" << value.as_object() << ")";
    } else {
        oss << "unknown";
    }

    return oss.str();
}

void ValueCore::debug_print_bits(uint64_t bits) {
    std::cout << "Bits: 0x" << std::hex << bits << std::dec;

    if (is_tagged_value(bits)) {
        std::cout << " (tagged, tag=" << get_tag(bits) << ")";
    } else {
        std::cout << " (number=" << decode_number(bits) << ")";
    }

    std::cout << std::endl;
}

bool ValueCore::is_in_heap_range(void* ptr) {
    // Basic heap range check
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    return addr != 0 && (addr & 0x7) == 0; // Non-null and aligned
}

bool ValueCore::is_aligned_pointer(void* ptr) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    return (addr & 0x7) == 0; // 8-byte aligned
}

//=============================================================================
// ValueUtils Implementation
//=============================================================================

namespace ValueUtils {

bool can_convert_to_number(const Value& value) {
    return value.is_number() || value.is_boolean() ||
           value.is_null() || value.is_undefined() ||
           (value.is_string() && !value.to_string().empty());
}

bool can_convert_to_string(const Value& value) {
    return true; // All values can be converted to string
}

bool can_convert_to_boolean(const Value& value) {
    return true; // All values can be converted to boolean
}

bool is_array_index(const Value& value) {
    if (!value.is_number()) return false;

    double num = value.as_number();
    if (!std::isfinite(num) || num < 0) return false;

    uint32_t index = static_cast<uint32_t>(num);
    return static_cast<double>(index) == num && index < 0xFFFFFFFFU;
}

uint32_t to_array_index(const Value& value) {
    if (!is_array_index(value)) return 0xFFFFFFFFU;
    return static_cast<uint32_t>(value.as_number());
}

bool is_integer(const Value& value) {
    if (!value.is_number()) return false;

    double num = value.as_number();
    return std::isfinite(num) && std::floor(num) == num;
}

bool is_safe_integer(const Value& value) {
    if (!is_integer(value)) return false;

    double num = value.as_number();
    return std::abs(num) <= 0x1FFFFFFFFFFFFF; // 2^53 - 1
}

std::string to_property_key(const Value& value) {
    if (value.is_string()) {
        return value.to_string();
    } else if (value.is_symbol()) {
        return value.as_symbol()->to_string();
    } else {
        return value.to_string();
    }
}

bool is_smi(const Value& value) {
    if (!value.is_number()) return false;

    double num = value.as_number();
    return is_integer(value) && num >= INT32_MIN && num <= INT32_MAX;
}

int32_t to_smi(const Value& value) {
    if (!is_smi(value)) return 0;
    return static_cast<int32_t>(value.as_number());
}

Value from_smi(int32_t value) {
    return ValueCore::create_number(static_cast<double>(value));
}

} // namespace ValueUtils

} // namespace Quanta