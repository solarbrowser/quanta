/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include <string>
#include <memory>

namespace Quanta {

class Object;
class String;
class BigInt;
class Symbol;

/**
 * Core Value Construction and Type Management
 */
class ValueCore {
public:
    // Core value types
    enum class ValueType {
        Undefined,
        Null,
        Boolean,
        Number,
        String,
        Symbol,
        BigInt,
        Object,
        Function
    };

    // NaN-boxing constants and utilities
    static constexpr uint64_t QUIET_NAN = 0x7FF8000000000000ULL;
    static constexpr uint64_t TAG_MASK = 0x7ULL;
    static constexpr uint64_t PAYLOAD_MASK = 0x0000FFFFFFFFFFFFULL;

    // Type tags
    static constexpr uint64_t TAG_UNDEFINED = 0x1ULL;
    static constexpr uint64_t TAG_NULL = 0x2ULL;
    static constexpr uint64_t TAG_BOOLEAN = 0x3ULL;
    static constexpr uint64_t TAG_STRING = 0x4ULL;
    static constexpr uint64_t TAG_SYMBOL = 0x5ULL;
    static constexpr uint64_t TAG_BIGINT = 0x6ULL;
    static constexpr uint64_t TAG_OBJECT = 0x7ULL;

    // Special number values
    static constexpr uint64_t POSITIVE_INFINITY_BITS = 0x7FF0000000000000ULL;
    static constexpr uint64_t NEGATIVE_INFINITY_BITS = 0xFFF0000000000000ULL;

    // Static factory methods
    static Value create_undefined();
    static Value create_null();
    static Value create_boolean(bool value);
    static Value create_number(double value);
    static Value create_string(const std::string& str);
    static Value create_string(String* str_obj);
    static Value create_object(Object* obj);
    static Value create_symbol(Symbol* sym);
    static Value create_bigint(BigInt* bigint);

    // Special number factories
    static Value create_nan();
    static Value create_positive_infinity();
    static Value create_negative_infinity();

    // Type checking utilities
    static ValueType get_value_type(const Value& value);
    static bool is_primitive(const Value& value);
    static bool is_numeric(const Value& value);
    static bool is_callable(const Value& value);

    // NaN-boxing utilities
    static uint64_t encode_boolean(bool value);
    static uint64_t encode_number(double value);
    static uint64_t encode_pointer(void* ptr, uint64_t tag);

    static bool decode_boolean(uint64_t bits);
    static double decode_number(uint64_t bits);
    static void* decode_pointer(uint64_t bits);

    // Special value checks
    static bool is_special_number(uint64_t bits);
    static bool is_tagged_value(uint64_t bits);
    static uint64_t get_tag(uint64_t bits);

    // Memory management for pointer compression
#if PLATFORM_POINTER_COMPRESSION
    static void set_heap_base(uintptr_t base);
    static uintptr_t get_heap_base();
    static uint64_t compress_pointer(void* ptr);
    static void* decompress_pointer(uint64_t compressed);
#endif

    // Value validation and debugging
    static bool is_valid_value(const Value& value);
    static std::string describe_value(const Value& value);
    static void debug_print_bits(uint64_t bits);

private:
    // Internal helper methods
    static bool is_in_heap_range(void* ptr);
    static bool is_aligned_pointer(void* ptr);

#if PLATFORM_POINTER_COMPRESSION
    static thread_local uintptr_t heap_base_;
#endif
};

/**
 * Value Utility Functions
 */
namespace ValueUtils {
    // Type conversion helpers
    bool can_convert_to_number(const Value& value);
    bool can_convert_to_string(const Value& value);
    bool can_convert_to_boolean(const Value& value);

    // JavaScript-specific utilities
    bool is_array_index(const Value& value);
    uint32_t to_array_index(const Value& value);

    bool is_integer(const Value& value);
    bool is_safe_integer(const Value& value);

    // Property key conversion
    std::string to_property_key(const Value& value);

    // Optimization helpers
    bool is_smi(const Value& value); // Small integer optimization
    int32_t to_smi(const Value& value);
    Value from_smi(int32_t value);
}

} // namespace Quanta