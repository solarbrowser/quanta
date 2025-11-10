/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/data_view.h"
#include "../../include/Object.h"
#include <cstring>
#include <iostream>
#include <algorithm>

namespace Quanta {

DataView::DataView(uint8_t* buffer, size_t length, size_t offset)
    : buffer_(buffer), byte_length_(length), byte_offset_(offset), little_endian_(true) {
}

DataView::~DataView() {
    // Note: We don't own the buffer, so we don't delete it
}

int8_t DataView::get_int8(size_t byte_offset) const {
    if (!validate_offset(byte_offset, sizeof(int8_t))) {
        throw std::out_of_range("DataView: Invalid offset for getInt8");
    }
    return static_cast<int8_t>(buffer_[byte_offset_ + byte_offset]);
}

uint8_t DataView::get_uint8(size_t byte_offset) const {
    if (!validate_offset(byte_offset, sizeof(uint8_t))) {
        throw std::out_of_range("DataView: Invalid offset for getUint8");
    }
    return buffer_[byte_offset_ + byte_offset];
}

int16_t DataView::get_int16(size_t byte_offset, bool little_endian) const {
    if (!validate_offset(byte_offset, sizeof(int16_t))) {
        throw std::out_of_range("DataView: Invalid offset for getInt16");
    }

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;
    int16_t value;

    if (little_endian) {
        value = ptr[0] | (ptr[1] << 8);
    } else {
        value = (ptr[0] << 8) | ptr[1];
    }

    return value;
}

uint16_t DataView::get_uint16(size_t byte_offset, bool little_endian) const {
    if (!validate_offset(byte_offset, sizeof(uint16_t))) {
        throw std::out_of_range("DataView: Invalid offset for getUint16");
    }

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;
    uint16_t value;

    if (little_endian) {
        value = ptr[0] | (ptr[1] << 8);
    } else {
        value = (ptr[0] << 8) | ptr[1];
    }

    return value;
}

int32_t DataView::get_int32(size_t byte_offset, bool little_endian) const {
    if (!validate_offset(byte_offset, sizeof(int32_t))) {
        throw std::out_of_range("DataView: Invalid offset for getInt32");
    }

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;
    int32_t value;

    if (little_endian) {
        value = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    } else {
        value = (ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3];
    }

    return value;
}

uint32_t DataView::get_uint32(size_t byte_offset, bool little_endian) const {
    if (!validate_offset(byte_offset, sizeof(uint32_t))) {
        throw std::out_of_range("DataView: Invalid offset for getUint32");
    }

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;
    uint32_t value;

    if (little_endian) {
        value = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    } else {
        value = (ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3];
    }

    return value;
}

float DataView::get_float32(size_t byte_offset, bool little_endian) const {
    uint32_t int_value = get_uint32(byte_offset, little_endian);
    float float_value;
    std::memcpy(&float_value, &int_value, sizeof(float));
    return float_value;
}

double DataView::get_float64(size_t byte_offset, bool little_endian) const {
    if (!validate_offset(byte_offset, sizeof(double))) {
        throw std::out_of_range("DataView: Invalid offset for getFloat64");
    }

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;
    uint64_t int_value;

    if (little_endian) {
        int_value = static_cast<uint64_t>(ptr[0]) |
                   (static_cast<uint64_t>(ptr[1]) << 8) |
                   (static_cast<uint64_t>(ptr[2]) << 16) |
                   (static_cast<uint64_t>(ptr[3]) << 24) |
                   (static_cast<uint64_t>(ptr[4]) << 32) |
                   (static_cast<uint64_t>(ptr[5]) << 40) |
                   (static_cast<uint64_t>(ptr[6]) << 48) |
                   (static_cast<uint64_t>(ptr[7]) << 56);
    } else {
        int_value = (static_cast<uint64_t>(ptr[0]) << 56) |
                   (static_cast<uint64_t>(ptr[1]) << 48) |
                   (static_cast<uint64_t>(ptr[2]) << 40) |
                   (static_cast<uint64_t>(ptr[3]) << 32) |
                   (static_cast<uint64_t>(ptr[4]) << 24) |
                   (static_cast<uint64_t>(ptr[5]) << 16) |
                   (static_cast<uint64_t>(ptr[6]) << 8) |
                   static_cast<uint64_t>(ptr[7]);
    }

    double double_value;
    std::memcpy(&double_value, &int_value, sizeof(double));
    return double_value;
}

void DataView::set_int8(size_t byte_offset, int8_t value) {
    if (!validate_offset(byte_offset, sizeof(int8_t))) {
        throw std::out_of_range("DataView: Invalid offset for setInt8");
    }
    buffer_[byte_offset_ + byte_offset] = static_cast<uint8_t>(value);
}

void DataView::set_uint8(size_t byte_offset, uint8_t value) {
    if (!validate_offset(byte_offset, sizeof(uint8_t))) {
        throw std::out_of_range("DataView: Invalid offset for setUint8");
    }
    buffer_[byte_offset_ + byte_offset] = value;
}

void DataView::set_int16(size_t byte_offset, int16_t value, bool little_endian) {
    if (!validate_offset(byte_offset, sizeof(int16_t))) {
        throw std::out_of_range("DataView: Invalid offset for setInt16");
    }

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;

    if (little_endian) {
        ptr[0] = value & 0xFF;
        ptr[1] = (value >> 8) & 0xFF;
    } else {
        ptr[0] = (value >> 8) & 0xFF;
        ptr[1] = value & 0xFF;
    }
}

void DataView::set_uint16(size_t byte_offset, uint16_t value, bool little_endian) {
    if (!validate_offset(byte_offset, sizeof(uint16_t))) {
        throw std::out_of_range("DataView: Invalid offset for setUint16");
    }

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;

    if (little_endian) {
        ptr[0] = value & 0xFF;
        ptr[1] = (value >> 8) & 0xFF;
    } else {
        ptr[0] = (value >> 8) & 0xFF;
        ptr[1] = value & 0xFF;
    }
}

void DataView::set_int32(size_t byte_offset, int32_t value, bool little_endian) {
    if (!validate_offset(byte_offset, sizeof(int32_t))) {
        throw std::out_of_range("DataView: Invalid offset for setInt32");
    }

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;

    if (little_endian) {
        ptr[0] = value & 0xFF;
        ptr[1] = (value >> 8) & 0xFF;
        ptr[2] = (value >> 16) & 0xFF;
        ptr[3] = (value >> 24) & 0xFF;
    } else {
        ptr[0] = (value >> 24) & 0xFF;
        ptr[1] = (value >> 16) & 0xFF;
        ptr[2] = (value >> 8) & 0xFF;
        ptr[3] = value & 0xFF;
    }
}

void DataView::set_uint32(size_t byte_offset, uint32_t value, bool little_endian) {
    if (!validate_offset(byte_offset, sizeof(uint32_t))) {
        throw std::out_of_range("DataView: Invalid offset for setUint32");
    }

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;

    if (little_endian) {
        ptr[0] = value & 0xFF;
        ptr[1] = (value >> 8) & 0xFF;
        ptr[2] = (value >> 16) & 0xFF;
        ptr[3] = (value >> 24) & 0xFF;
    } else {
        ptr[0] = (value >> 24) & 0xFF;
        ptr[1] = (value >> 16) & 0xFF;
        ptr[2] = (value >> 8) & 0xFF;
        ptr[3] = value & 0xFF;
    }
}

void DataView::set_float32(size_t byte_offset, float value, bool little_endian) {
    uint32_t int_value;
    std::memcpy(&int_value, &value, sizeof(float));
    set_uint32(byte_offset, int_value, little_endian);
}

void DataView::set_float64(size_t byte_offset, double value, bool little_endian) {
    if (!validate_offset(byte_offset, sizeof(double))) {
        throw std::out_of_range("DataView: Invalid offset for setFloat64");
    }

    uint64_t int_value;
    std::memcpy(&int_value, &value, sizeof(double));

    uint8_t* ptr = buffer_ + byte_offset_ + byte_offset;

    if (little_endian) {
        ptr[0] = int_value & 0xFF;
        ptr[1] = (int_value >> 8) & 0xFF;
        ptr[2] = (int_value >> 16) & 0xFF;
        ptr[3] = (int_value >> 24) & 0xFF;
        ptr[4] = (int_value >> 32) & 0xFF;
        ptr[5] = (int_value >> 40) & 0xFF;
        ptr[6] = (int_value >> 48) & 0xFF;
        ptr[7] = (int_value >> 56) & 0xFF;
    } else {
        ptr[0] = (int_value >> 56) & 0xFF;
        ptr[1] = (int_value >> 48) & 0xFF;
        ptr[2] = (int_value >> 40) & 0xFF;
        ptr[3] = (int_value >> 32) & 0xFF;
        ptr[4] = (int_value >> 24) & 0xFF;
        ptr[5] = (int_value >> 16) & 0xFF;
        ptr[6] = (int_value >> 8) & 0xFF;
        ptr[7] = int_value & 0xFF;
    }
}

bool DataView::validate_offset(size_t offset, size_t type_size) const {
    return offset + type_size <= byte_length_;
}

void DataView::copy_from(const DataView& other, size_t src_offset, size_t dst_offset, size_t length) {
    if (!other.validate_offset(src_offset, length) || !validate_offset(dst_offset, length)) {
        throw std::out_of_range("DataView: Invalid offsets for copy operation");
    }

    std::memcpy(buffer_ + byte_offset_ + dst_offset,
                other.buffer_ + other.byte_offset_ + src_offset,
                length);
}

void DataView::fill(uint8_t value, size_t offset, size_t length) {
    if (offset + length > byte_length_) {
        length = byte_length_ - offset;
    }

    std::memset(buffer_ + byte_offset_ + offset, value, length);
}

// Static factory methods for JavaScript binding
Value DataView::create_data_view(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        // TODO: Throw TypeError
        return Value();
    }

    // In a real implementation, we would get ArrayBuffer from args[0]
    // For now, create a simple buffer
    size_t length = args.size() > 1 ? static_cast<size_t>(args[1].to_number()) : 1024;
    size_t offset = args.size() > 2 ? static_cast<size_t>(args[2].to_number()) : 0;

    uint8_t* buffer = new uint8_t[length];
    std::memset(buffer, 0, length);

    auto dataview = new DataView(buffer, length, offset);
    return Value(static_cast<Object*>(dataview));
}

void DataView::setup_data_view_prototype(Context& ctx) {
    // Set up DataView constructor and prototype
    // This would be called during engine initialization
}

} // namespace Quanta