/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/TypedArray.h"
#include "quanta/ArrayBuffer.h"
#include "quanta/Context.h"
#include "quanta/Error.h"
#include <algorithm>
#include <sstream>
#include <cmath>

static void quanta_memcpy(void* dest, const void* src, size_t count) {
    const char* s = static_cast<const char*>(src);
    char* d = static_cast<char*>(dest);
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
}

static void quanta_memset(void* dest, int value, size_t count) {
    char* d = static_cast<char*>(dest);
    char val = static_cast<char>(value);
    for (size_t i = 0; i < count; ++i) {
        d[i] = val;
    }
}

namespace Quanta {


TypedArrayBase::TypedArrayBase(ArrayType type, size_t bytes_per_element)
    : Object(ObjectType::TypedArray), array_type_(type), bytes_per_element_(bytes_per_element),
      byte_offset_(0), length_(0) {
}

TypedArrayBase::TypedArrayBase(ArrayType type, size_t bytes_per_element, size_t length)
    : Object(ObjectType::TypedArray), array_type_(type), bytes_per_element_(bytes_per_element),
      byte_offset_(0), length_(length) {
    size_t byte_length = length * bytes_per_element;
    buffer_ = std::make_shared<ArrayBuffer>(byte_length);
}

TypedArrayBase::TypedArrayBase(ArrayType type, size_t bytes_per_element, std::shared_ptr<ArrayBuffer> buffer)
    : Object(ObjectType::TypedArray), array_type_(type), bytes_per_element_(bytes_per_element),
      buffer_(buffer), byte_offset_(0) {
    if (!buffer_) {
        throw std::invalid_argument("ArrayBuffer cannot be null");
    }
    if (buffer_->is_detached()) {
        throw std::runtime_error("Cannot construct TypedArray from detached ArrayBuffer");
    }
    
    size_t buffer_byte_length = buffer_->byte_length();
    if (buffer_byte_length % bytes_per_element_ != 0) {
        throw std::range_error("ArrayBuffer byte length is not a multiple of element size");
    }
    length_ = buffer_byte_length / bytes_per_element_;
}

TypedArrayBase::TypedArrayBase(ArrayType type, size_t bytes_per_element, std::shared_ptr<ArrayBuffer> buffer, 
                               size_t byte_offset, size_t length)
    : Object(ObjectType::TypedArray), array_type_(type), bytes_per_element_(bytes_per_element),
      buffer_(buffer), byte_offset_(byte_offset) {
    if (!buffer_) {
        throw std::invalid_argument("ArrayBuffer cannot be null");
    }
    if (buffer_->is_detached()) {
        throw std::runtime_error("Cannot construct TypedArray from detached ArrayBuffer");
    }
    
    size_t buffer_byte_length = buffer_->byte_length();
    validate_offset_and_length(buffer_byte_length, byte_offset, length);
    
    if (length == SIZE_MAX) {
        if ((buffer_byte_length - byte_offset) % bytes_per_element_ != 0) {
            throw std::range_error("Remaining buffer space is not a multiple of element size");
        }
        length_ = (buffer_byte_length - byte_offset) / bytes_per_element_;
    } else {
        length_ = length;
    }
}

uint8_t* TypedArrayBase::get_data_ptr() const {
    if (!buffer_ || buffer_->is_detached()) {
        return nullptr;
    }
    return buffer_->data() + byte_offset_;
}

bool TypedArrayBase::check_bounds(size_t index) const {
    return index < length_ && !buffer_->is_detached();
}

void TypedArrayBase::validate_offset_and_length(size_t buffer_byte_length, size_t byte_offset, size_t length) const {
    if (byte_offset > buffer_byte_length) {
        throw std::range_error("TypedArray byte offset exceeds ArrayBuffer size");
    }
    if (byte_offset % bytes_per_element_ != 0) {
        throw std::range_error("TypedArray byte offset is not aligned to element size");
    }
    if (length != SIZE_MAX) {
        size_t required_bytes = length * bytes_per_element_;
        if (byte_offset + required_bytes > buffer_byte_length) {
            throw std::range_error("TypedArray extends beyond ArrayBuffer bounds");
        }
    }
}

Value TypedArrayBase::get_property(const std::string& key) const {
    char* end;
    unsigned long index = std::strtoul(key.c_str(), &end, 10);
    if (*end == '\0' && index < length_) {
        return get_element(static_cast<size_t>(index));
    }
    
    if (key == "length") {
        return Value(static_cast<double>(length_));
    }
    if (key == "byteLength") {
        return Value(static_cast<double>(byte_length()));
    }
    if (key == "byteOffset") {
        return Value(static_cast<double>(byte_offset_));
    }
    if (key == "buffer") {
        return Value(buffer_.get());
    }
    if (key == "BYTES_PER_ELEMENT") {
        return Value(static_cast<double>(bytes_per_element_));
    }
    
    return Object::get_property(key);
}

bool TypedArrayBase::set_property(const std::string& key, const Value& value, PropertyAttributes attrs) {
    char* end;
    unsigned long index = std::strtoul(key.c_str(), &end, 10);
    if (*end == '\0' && index < length_) {
        return set_element(static_cast<size_t>(index), value);
    }
    
    return Object::set_property(key, value, attrs);
}

std::string TypedArrayBase::to_string() const {
    if (buffer_->is_detached()) {
        return "[object " + get_type_name() + "]";
    }
    
    std::ostringstream oss;
    for (size_t i = 0; i < length_; ++i) {
        if (i > 0) oss << ",";
        oss << get_element(i).to_string();
    }
    return oss.str();
}

Value TypedArrayBase::to_primitive(const std::string& hint) const {
    if (hint == "number") {
        return Value(static_cast<double>(length_));
    }
    return Value(to_string());
}

std::string TypedArrayBase::array_type_to_string(ArrayType type) {
    switch (type) {
        case ArrayType::INT8: return "Int8Array";
        case ArrayType::UINT8: return "Uint8Array";
        case ArrayType::UINT8_CLAMPED: return "Uint8ClampedArray";
        case ArrayType::INT16: return "Int16Array";
        case ArrayType::UINT16: return "Uint16Array";
        case ArrayType::INT32: return "Int32Array";
        case ArrayType::UINT32: return "Uint32Array";
        case ArrayType::FLOAT32: return "Float32Array";
        case ArrayType::FLOAT64: return "Float64Array";
        case ArrayType::BIGINT64: return "BigInt64Array";
        case ArrayType::BIGUINT64: return "BigUint64Array";
        default: return "TypedArray";
    }
}

size_t TypedArrayBase::get_bytes_per_element(ArrayType type) {
    switch (type) {
        case ArrayType::INT8:
        case ArrayType::UINT8:
        case ArrayType::UINT8_CLAMPED:
            return 1;
        case ArrayType::INT16:
        case ArrayType::UINT16:
            return 2;
        case ArrayType::INT32:
        case ArrayType::UINT32:
        case ArrayType::FLOAT32:
            return 4;
        case ArrayType::FLOAT64:
        case ArrayType::BIGINT64:
        case ArrayType::BIGUINT64:
            return 8;
        default:
            return 1;
    }
}

Value TypedArrayBase::get_element(uint32_t index) const {
    return get_element(static_cast<size_t>(index));
}

bool TypedArrayBase::set_element(uint32_t index, const Value& value) {
    return set_element(static_cast<size_t>(index), value);
}


template<typename T>
TypedArray<T>::TypedArray(ArrayType type, size_t length) 
    : TypedArrayBase(type, sizeof(T), length) {
}

template<typename T>
TypedArray<T>::TypedArray(ArrayType type, std::shared_ptr<ArrayBuffer> buffer) 
    : TypedArrayBase(type, sizeof(T), buffer) {
}

template<typename T>
TypedArray<T>::TypedArray(ArrayType type, std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t length)
    : TypedArrayBase(type, sizeof(T), buffer, byte_offset, length) {
}

template<typename T>
T TypedArray<T>::get_typed_element(size_t index) const {
    if (!check_bounds(index)) {
        return T{};
    }
    
    uint8_t* data = get_data_ptr();
    if (!data) {
        return T{};
    }
    
    T result;
    quanta_memcpy(&result, data + index * sizeof(T), sizeof(T));
    return result;
}

template<typename T>
bool TypedArray<T>::set_typed_element(size_t index, T value) {
    if (!check_bounds(index)) {
        return false;
    }
    
    uint8_t* data = get_data_ptr();
    if (!data) {
        return false;
    }
    
    quanta_memcpy(data + index * sizeof(T), &value, sizeof(T));
    return true;
}

template<typename T>
Value TypedArray<T>::get_element(size_t index) const {
    T value = get_typed_element(index);
    if constexpr (std::is_floating_point_v<T>) {
        return Value(static_cast<double>(value));
    } else if constexpr (std::is_signed_v<T>) {
        return Value(static_cast<double>(value));
    } else {
        return Value(static_cast<double>(value));
    }
}

template<typename T>
bool TypedArray<T>::set_element(size_t index, const Value& value) {
    if constexpr (std::is_floating_point_v<T>) {
        double num_val = value.to_number();
        if (std::isnan(num_val)) {
            return set_typed_element(index, T{});
        }
        return set_typed_element(index, static_cast<T>(num_val));
    } else if constexpr (std::is_signed_v<T>) {
        double num_val = value.to_number();
        if (std::isnan(num_val)) {
            return set_typed_element(index, T{});
        }
        int64_t int_val = static_cast<int64_t>(num_val);
        int64_t min_val = std::numeric_limits<T>::min();
        int64_t max_val = std::numeric_limits<T>::max();
        if (int_val < min_val) int_val = min_val;
        if (int_val > max_val) int_val = max_val;
        return set_typed_element(index, static_cast<T>(int_val));
    } else {
        double num_val = value.to_number();
        if (std::isnan(num_val)) {
            return set_typed_element(index, T{});
        }
        uint64_t uint_val = static_cast<uint64_t>(std::max(0.0, num_val));
        uint64_t max_val = std::numeric_limits<T>::max();
        if (uint_val > max_val) uint_val = max_val;
        return set_typed_element(index, static_cast<T>(uint_val));
    }
}


template class TypedArray<int8_t>;
template class TypedArray<uint8_t>;
template class TypedArray<int16_t>;
template class TypedArray<uint16_t>;
template class TypedArray<int32_t>;
template class TypedArray<uint32_t>;
template class TypedArray<float>;
template class TypedArray<double>;


bool Uint8ClampedArray::set_element(size_t index, const Value& value) {
    if (!check_bounds(index)) {
        return false;
    }
    
    double num_val = value.to_number();
    if (std::isnan(num_val)) {
        return set_typed_element(index, 0);
    }
    
    if (num_val < 0.0) {
        return set_typed_element(index, 0);
    } else if (num_val > 255.0) {
        return set_typed_element(index, 255);
    } else {
        return set_typed_element(index, static_cast<uint8_t>(std::round(num_val)));
    }
}


namespace TypedArrayFactory {

std::unique_ptr<TypedArrayBase> create_int8_array(size_t length) {
    return std::make_unique<Int8Array>(length);
}

std::unique_ptr<TypedArrayBase> create_uint8_array(size_t length) {
    return std::make_unique<Uint8Array>(length);
}

std::unique_ptr<TypedArrayBase> create_uint8_array_from_buffer(ArrayBuffer* buffer) {
    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {
    });
    return std::make_unique<Uint8Array>(shared_buffer);
}

std::unique_ptr<TypedArrayBase> create_uint8_clamped_array(size_t length) {
    return std::make_unique<Uint8ClampedArray>(length);
}

std::unique_ptr<TypedArrayBase> create_int16_array(size_t length) {
    return std::make_unique<Int16Array>(length);
}

std::unique_ptr<TypedArrayBase> create_uint16_array(size_t length) {
    return std::make_unique<Uint16Array>(length);
}

std::unique_ptr<TypedArrayBase> create_int32_array(size_t length) {
    return std::make_unique<Int32Array>(length);
}

std::unique_ptr<TypedArrayBase> create_uint32_array(size_t length) {
    return std::make_unique<Uint32Array>(length);
}

std::unique_ptr<TypedArrayBase> create_float32_array(size_t length) {
    return std::make_unique<Float32Array>(length);
}

std::unique_ptr<TypedArrayBase> create_float32_array_from_buffer(ArrayBuffer* buffer) {
    std::shared_ptr<ArrayBuffer> shared_buffer(buffer, [](ArrayBuffer*) {
    });
    return std::make_unique<Float32Array>(shared_buffer);
}

std::unique_ptr<TypedArrayBase> create_float64_array(size_t length) {
    return std::make_unique<Float64Array>(length);
}

std::unique_ptr<TypedArrayBase> create_from_buffer(TypedArrayBase::ArrayType type, 
                                                  std::shared_ptr<ArrayBuffer> buffer,
                                                  size_t byte_offset, 
                                                  size_t length) {
    switch (type) {
        case TypedArrayBase::ArrayType::INT8:
            return std::make_unique<Int8Array>(buffer, byte_offset, length);
        case TypedArrayBase::ArrayType::UINT8:
            return std::make_unique<Uint8Array>(buffer, byte_offset, length);
        case TypedArrayBase::ArrayType::UINT8_CLAMPED:
            return std::make_unique<Uint8ClampedArray>(buffer, byte_offset, length);
        case TypedArrayBase::ArrayType::INT16:
            return std::make_unique<Int16Array>(buffer, byte_offset, length);
        case TypedArrayBase::ArrayType::UINT16:
            return std::make_unique<Uint16Array>(buffer, byte_offset, length);
        case TypedArrayBase::ArrayType::INT32:
            return std::make_unique<Int32Array>(buffer, byte_offset, length);
        case TypedArrayBase::ArrayType::UINT32:
            return std::make_unique<Uint32Array>(buffer, byte_offset, length);
        case TypedArrayBase::ArrayType::FLOAT32:
            return std::make_unique<Float32Array>(buffer, byte_offset, length);
        case TypedArrayBase::ArrayType::FLOAT64:
            return std::make_unique<Float64Array>(buffer, byte_offset, length);
        default:
            throw std::invalid_argument("Unsupported TypedArray type");
    }
}

bool is_typed_array(const Object* obj) {
    return obj && obj->is_typed_array();
}

TypedArrayBase* as_typed_array(Object* obj) {
    return is_typed_array(obj) ? static_cast<TypedArrayBase*>(obj) : nullptr;
}

const TypedArrayBase* as_typed_array(const Object* obj) {
    return is_typed_array(obj) ? static_cast<const TypedArrayBase*>(obj) : nullptr;
}

}

}
