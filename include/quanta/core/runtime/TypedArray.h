/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_TYPEDARRAY_H
#define QUANTA_TYPEDARRAY_H

#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/Value.h"
#include "quanta/core/runtime/BigInt.h"
#include <memory>
#include <cstdint>

namespace Quanta {

class Context;
class ArrayBuffer;

class TypedArrayBase : public Object {
public:
    enum class ArrayType {
        INT8,
        UINT8,
        UINT8_CLAMPED,
        INT16,
        UINT16,
        INT32,
        UINT32,
        FLOAT32,
        FLOAT64,
        BIGINT64,
        BIGUINT64
    };

protected:
    std::shared_ptr<ArrayBuffer> buffer_;
    size_t byte_offset_;
    size_t length_;
    ArrayType array_type_;
    size_t bytes_per_element_;
    bool is_length_tracking_;

    uint8_t* get_data_ptr() const;
    bool check_bounds(size_t index) const;
    void validate_offset_and_length(size_t buffer_byte_length, size_t byte_offset, size_t length) const;

public:
    // Non-virtual: none of the 11 concrete numeric TypedArray<T> leaves
    // override trace() or add extra cell references -- this one
    // implementation is the whole story, reached directly from Object's
    // ObjectType::TypedArray switch case. A vtable here would misalign the
    // Object subobject for a base with none of its own (see Object.h's
    // note on why Object itself carries no vtable).
    void trace(Visitor& v);
    TypedArrayBase(ArrayType type, size_t bytes_per_element);
    TypedArrayBase(ArrayType type, size_t bytes_per_element, size_t length);
    TypedArrayBase(ArrayType type, size_t bytes_per_element, std::shared_ptr<ArrayBuffer> buffer);
    TypedArrayBase(ArrayType type, size_t bytes_per_element, std::shared_ptr<ArrayBuffer> buffer,
                   size_t byte_offset, size_t length = SIZE_MAX);

    ~TypedArrayBase() = default;

    ArrayBuffer* buffer() const { return buffer_.get(); }
    size_t byte_offset() const { return byte_offset_; }
    // Spec-current length, re-derived live from the backing buffer's current size for a
    // length-tracking view (one created without an explicit length over a resizable buffer),
    // and 0 for a fixed-length view whose window no longer fits after the buffer shrank.
    size_t current_length() const;
    bool is_out_of_bounds() const;
    size_t byte_length() const { return current_length() * bytes_per_element_; }
    size_t length() const { return current_length(); }
    bool is_length_tracking() const { return is_length_tracking_; }
    size_t bytes_per_element() const { return bytes_per_element_; }
    ArrayType get_array_type() const { return array_type_; }

    // CanonicalNumericIndexString: true (with `out` set) only for a number's exact canonical string form.
    static bool canonical_numeric_index(const std::string& key, double& out);
    // IsValidIntegerIndex: non-negative integer, not -0, within current_length().
    bool is_valid_integer_index(double idx) const;


    // All 11 concrete leaves just spell the type differently; no per-type
    // behavior, so no dispatch needed at all.
    std::string get_type_name() const { return array_type_to_string(array_type_); }

    // Non-virtual: switches on array_type_ to the exact concrete leaf
    // (Int8Array/.../BigUint64Array) instead of virtual dispatch -- a
    // vtable here would misalign the Object subobject (see trace()'s
    // comment above). Defined out-of-line (TypedArray.cpp): needs every
    // leaf's full definition, which comes later in this header.
    Value get_element(size_t index) const;
    bool set_element(size_t index, const Value& value);
    
    Value subarray(size_t start, size_t end = SIZE_MAX) const;
    void set_from_array(const std::vector<Value>& source, size_t offset = 0);
    void set_from_typed_array(const TypedArrayBase& source, size_t offset = 0);
    
    // None of these eight are virtual on Object anymore -- Object's own
    // get_property()/etc. switch on get_type() and dispatch here directly.
    Value get_property(const std::string& key) const;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default);
    // Integer-indexed properties (0..current_length()-1) live in the backing buffer, not in the
    // generic Object property table, so [[OwnPropertyKeys]]/[[GetOwnProperty]]/for-in/Object.keys
    // need their own view of them here -- re-derived live so a resizable buffer's current size is reflected.
    std::vector<std::string> get_own_property_keys() const;
    bool has_own_property(const std::string& key) const;
    bool has_property(const std::string& key) const;
    bool delete_property(const std::string& key);
    bool set_property_descriptor(const std::string& key, const PropertyDescriptor& desc);
    PropertyDescriptor get_property_descriptor(const std::string& key) const;
    
    bool set_element(uint32_t index, const Value& value);
    
    std::string to_string() const;
    Value to_primitive(const std::string& hint = "") const;
    
    static std::string array_type_to_string(ArrayType type);
    static size_t get_bytes_per_element(ArrayType type);
};

/**
 * Template base for specific typed array implementations
 */
template<typename T>
class TypedArray : public TypedArrayBase {
private:
    static_assert(sizeof(T) <= 8, "TypedArray element size must be <= 8 bytes");

protected:
    T get_typed_element(size_t index) const;
    bool set_typed_element(size_t index, T value);
    
public:
    using element_type = T;
    
    TypedArray(ArrayType type, size_t length);
    TypedArray(ArrayType type, std::shared_ptr<ArrayBuffer> buffer);
    TypedArray(ArrayType type, std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX);
    
    Value get_element(size_t index) const;
    bool set_element(size_t index, const Value& value);

    T at(size_t index) const { return get_typed_element(index); }
    void set(size_t index, T value) { set_typed_element(index, value); }
};

/**
 * Specific TypedArray implementations
 */
class Int8Array : public TypedArray<int8_t> {
public:
    Int8Array(size_t length) : TypedArray(ArrayType::INT8, length) {}
    Int8Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::INT8, buffer) {}
    Int8Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::INT8, buffer, byte_offset, length) {}
};

class Uint8Array : public TypedArray<uint8_t> {
public:
    Uint8Array(size_t length) : TypedArray(ArrayType::UINT8, length) {}
    Uint8Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::UINT8, buffer) {}
    Uint8Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::UINT8, buffer, byte_offset, length) {}
};

class Uint8ClampedArray : public TypedArray<uint8_t> {
public:
    Uint8ClampedArray(size_t length) : TypedArray(ArrayType::UINT8_CLAMPED, length) {}
    Uint8ClampedArray(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::UINT8_CLAMPED, buffer) {}
    Uint8ClampedArray(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::UINT8_CLAMPED, buffer, byte_offset, length) {}

    bool set_element(size_t index, const Value& value);
};

class Int16Array : public TypedArray<int16_t> {
public:
    Int16Array(size_t length) : TypedArray(ArrayType::INT16, length) {}
    Int16Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::INT16, buffer) {}
    Int16Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::INT16, buffer, byte_offset, length) {}
};

class Uint16Array : public TypedArray<uint16_t> {
public:
    Uint16Array(size_t length) : TypedArray(ArrayType::UINT16, length) {}
    Uint16Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::UINT16, buffer) {}
    Uint16Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::UINT16, buffer, byte_offset, length) {}
};

class Int32Array : public TypedArray<int32_t> {
public:
    Int32Array(size_t length) : TypedArray(ArrayType::INT32, length) {}
    Int32Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::INT32, buffer) {}
    Int32Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::INT32, buffer, byte_offset, length) {}
};

class Uint32Array : public TypedArray<uint32_t> {
public:
    Uint32Array(size_t length) : TypedArray(ArrayType::UINT32, length) {}
    Uint32Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::UINT32, buffer) {}
    Uint32Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::UINT32, buffer, byte_offset, length) {}
};

class Float32Array : public TypedArray<float> {
public:
    Float32Array(size_t length) : TypedArray(ArrayType::FLOAT32, length) {}
    Float32Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::FLOAT32, buffer) {}
    Float32Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::FLOAT32, buffer, byte_offset, length) {}
};

class Float64Array : public TypedArray<double> {
public:
    Float64Array(size_t length) : TypedArray(ArrayType::FLOAT64, length) {}
    Float64Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::FLOAT64, buffer) {}
    Float64Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::FLOAT64, buffer, byte_offset, length) {}
};

class BigInt64Array : public TypedArrayBase {
public:
    BigInt64Array(size_t length);
    BigInt64Array(std::shared_ptr<ArrayBuffer> buffer);
    BigInt64Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX);

    Value get_element(size_t index) const;
    bool set_element(size_t index, const Value& value);
};

class BigUint64Array : public TypedArrayBase {
public:
    BigUint64Array(size_t length);
    BigUint64Array(std::shared_ptr<ArrayBuffer> buffer);
    BigUint64Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX);

    Value get_element(size_t index) const;
    bool set_element(size_t index, const Value& value);
};

/**
 * TypedArray utility functions
 */
namespace TypedArrayFactory {
    std::unique_ptr<TypedArrayBase> create_int8_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_uint8_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_uint8_array_from_buffer(ArrayBuffer* buffer);
    std::unique_ptr<TypedArrayBase> create_uint8_clamped_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_uint8_clamped_array_from_buffer(ArrayBuffer* buffer);
    std::unique_ptr<TypedArrayBase> create_int16_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_uint16_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_int32_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_uint32_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_float32_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_float32_array_from_buffer(ArrayBuffer* buffer);
    std::unique_ptr<TypedArrayBase> create_float64_array(size_t length);
    
    std::unique_ptr<TypedArrayBase> create_from_buffer(TypedArrayBase::ArrayType type, 
                                                      std::shared_ptr<ArrayBuffer> buffer,
                                                      size_t byte_offset = 0, 
                                                      size_t length = SIZE_MAX);
    
    bool is_typed_array(const Object* obj);
    TypedArrayBase* as_typed_array(Object* obj);
    const TypedArrayBase* as_typed_array(const Object* obj);
}

}

#endif
