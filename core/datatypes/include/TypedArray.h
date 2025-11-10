/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_TYPEDARRAY_H
#define QUANTA_TYPEDARRAY_H

#include "Object.h"
#include "ArrayBuffer.h"
#include "Value.h"
#include <memory>
#include <cstdint>

namespace Quanta {

// Forward declarations
class Context;
class ArrayBuffer;

/**
 * Base class for all TypedArray variants
 * Provides common functionality for typed array operations
 */
class TypedArrayBase : public Object {
public:
    enum class ArrayType {
        INT8,         // Int8Array
        UINT8,        // Uint8Array  
        UINT8_CLAMPED,// Uint8ClampedArray
        INT16,        // Int16Array
        UINT16,       // Uint16Array
        INT32,        // Int32Array
        UINT32,       // Uint32Array
        FLOAT32,      // Float32Array
        FLOAT64,      // Float64Array
        BIGINT64,     // BigInt64Array
        BIGUINT64     // BigUint64Array
    };

protected:
    std::shared_ptr<ArrayBuffer> buffer_;
    size_t byte_offset_;
    size_t length_;
    ArrayType array_type_;
    size_t bytes_per_element_;

    // Internal methods
    uint8_t* get_data_ptr() const;
    bool check_bounds(size_t index) const;
    void validate_offset_and_length(size_t buffer_byte_length, size_t byte_offset, size_t length) const;

public:
    // Constructors
    TypedArrayBase(ArrayType type, size_t bytes_per_element);
    TypedArrayBase(ArrayType type, size_t bytes_per_element, size_t length);
    TypedArrayBase(ArrayType type, size_t bytes_per_element, std::shared_ptr<ArrayBuffer> buffer);
    TypedArrayBase(ArrayType type, size_t bytes_per_element, std::shared_ptr<ArrayBuffer> buffer, 
                   size_t byte_offset, size_t length = SIZE_MAX);
    
    virtual ~TypedArrayBase() = default;

    // Core properties
    ArrayBuffer* buffer() const { return buffer_.get(); }
    size_t byte_offset() const { return byte_offset_; }
    size_t byte_length() const { return length_ * bytes_per_element_; }
    size_t length() const { return length_; }
    size_t bytes_per_element() const { return bytes_per_element_; }
    ArrayType get_array_type() const { return array_type_; }
    
    // Type checking
    bool is_typed_array() const override { return true; }
    virtual std::string get_type_name() const = 0;
    
    // Element access (pure virtual - implemented by subclasses)
    virtual Value get_element(size_t index) const = 0;
    virtual bool set_element(size_t index, const Value& value) = 0;
    
    // Array methods
    Value subarray(size_t start, size_t end = SIZE_MAX) const;
    void set_from_array(const std::vector<Value>& source, size_t offset = 0);
    void set_from_typed_array(const TypedArrayBase& source, size_t offset = 0);
    
    // Property access override
    Value get_property(const std::string& key) const override;
    bool set_property(const std::string& key, const Value& value, PropertyAttributes attrs = PropertyAttributes::Default) override;
    
    // Element access for array-style indexing (shadows Object methods)
    Value get_element(uint32_t index) const;
    bool set_element(uint32_t index, const Value& value);
    
    // Conversion
    std::string to_string() const;
    Value to_primitive(const std::string& hint = "") const;
    
    // Static utility
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
    // Type-specific element access
    T get_typed_element(size_t index) const;
    bool set_typed_element(size_t index, T value);
    
public:
    using element_type = T;
    
    // Constructors
    TypedArray(ArrayType type, size_t length);
    TypedArray(ArrayType type, std::shared_ptr<ArrayBuffer> buffer);
    TypedArray(ArrayType type, std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX);
    
    // Element access implementation
    Value get_element(size_t index) const override;
    bool set_element(size_t index, const Value& value) override;
    
    // Direct typed access
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
    
    std::string get_type_name() const override { return "Int8Array"; }
};

class Uint8Array : public TypedArray<uint8_t> {
public:
    Uint8Array(size_t length) : TypedArray(ArrayType::UINT8, length) {}
    Uint8Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::UINT8, buffer) {}
    Uint8Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::UINT8, buffer, byte_offset, length) {}
        
    std::string get_type_name() const override { return "Uint8Array"; }
};

class Uint8ClampedArray : public TypedArray<uint8_t> {
public:
    Uint8ClampedArray(size_t length) : TypedArray(ArrayType::UINT8_CLAMPED, length) {}
    Uint8ClampedArray(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::UINT8_CLAMPED, buffer) {}
    Uint8ClampedArray(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::UINT8_CLAMPED, buffer, byte_offset, length) {}
        
    std::string get_type_name() const override { return "Uint8ClampedArray"; }
    
    // Override set_element to implement clamping behavior
    bool set_element(size_t index, const Value& value) override;
};

class Int16Array : public TypedArray<int16_t> {
public:
    Int16Array(size_t length) : TypedArray(ArrayType::INT16, length) {}
    Int16Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::INT16, buffer) {}
    Int16Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::INT16, buffer, byte_offset, length) {}
        
    std::string get_type_name() const override { return "Int16Array"; }
};

class Uint16Array : public TypedArray<uint16_t> {
public:
    Uint16Array(size_t length) : TypedArray(ArrayType::UINT16, length) {}
    Uint16Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::UINT16, buffer) {}
    Uint16Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::UINT16, buffer, byte_offset, length) {}
        
    std::string get_type_name() const override { return "Uint16Array"; }
};

class Int32Array : public TypedArray<int32_t> {
public:
    Int32Array(size_t length) : TypedArray(ArrayType::INT32, length) {}
    Int32Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::INT32, buffer) {}
    Int32Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::INT32, buffer, byte_offset, length) {}
        
    std::string get_type_name() const override { return "Int32Array"; }
};

class Uint32Array : public TypedArray<uint32_t> {
public:
    Uint32Array(size_t length) : TypedArray(ArrayType::UINT32, length) {}
    Uint32Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::UINT32, buffer) {}
    Uint32Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::UINT32, buffer, byte_offset, length) {}
        
    std::string get_type_name() const override { return "Uint32Array"; }
};

class Float32Array : public TypedArray<float> {
public:
    Float32Array(size_t length) : TypedArray(ArrayType::FLOAT32, length) {}
    Float32Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::FLOAT32, buffer) {}
    Float32Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::FLOAT32, buffer, byte_offset, length) {}
        
    std::string get_type_name() const override { return "Float32Array"; }
};

class Float64Array : public TypedArray<double> {
public:
    Float64Array(size_t length) : TypedArray(ArrayType::FLOAT64, length) {}
    Float64Array(std::shared_ptr<ArrayBuffer> buffer) : TypedArray(ArrayType::FLOAT64, buffer) {}
    Float64Array(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length = SIZE_MAX)
        : TypedArray(ArrayType::FLOAT64, buffer, byte_offset, length) {}
        
    std::string get_type_name() const override { return "Float64Array"; }
};

/**
 * TypedArray utility functions
 */
namespace TypedArrayFactory {
    // Create typed arrays from various sources
    std::unique_ptr<TypedArrayBase> create_int8_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_uint8_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_uint8_array_from_buffer(ArrayBuffer* buffer);
    std::unique_ptr<TypedArrayBase> create_uint8_clamped_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_int16_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_uint16_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_int32_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_uint32_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_float32_array(size_t length);
    std::unique_ptr<TypedArrayBase> create_float32_array_from_buffer(ArrayBuffer* buffer);
    std::unique_ptr<TypedArrayBase> create_float64_array(size_t length);
    
    // Create from ArrayBuffer
    std::unique_ptr<TypedArrayBase> create_from_buffer(TypedArrayBase::ArrayType type, 
                                                      std::shared_ptr<ArrayBuffer> buffer,
                                                      size_t byte_offset = 0, 
                                                      size_t length = SIZE_MAX);
    
    // Type detection
    bool is_typed_array(const Object* obj);
    TypedArrayBase* as_typed_array(Object* obj);
    const TypedArrayBase* as_typed_array(const Object* obj);
}

} // namespace Quanta

#endif // QUANTA_TYPEDARRAY_H