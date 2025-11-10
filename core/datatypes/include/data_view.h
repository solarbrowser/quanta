/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "../../include/Value.h"
#include "../../include/Object.h"
#include <cstdint>

namespace Quanta {

/**
 * DataView implementation for JavaScript
 * Provides a low-level interface for reading and writing multiple number types
 */
class DataView : public Object {
private:
    uint8_t* buffer_;
    size_t byte_length_;
    size_t byte_offset_;
    bool is_little_endian_;

public:
    DataView(uint8_t* buffer, size_t length, size_t offset = 0);
    virtual ~DataView() = default;

    // Integer accessors
    int8_t get_int8(size_t byte_offset) const;
    uint8_t get_uint8(size_t byte_offset) const;
    int16_t get_int16(size_t byte_offset, bool little_endian = false) const;
    uint16_t get_uint16(size_t byte_offset, bool little_endian = false) const;
    int32_t get_int32(size_t byte_offset, bool little_endian = false) const;
    uint32_t get_uint32(size_t byte_offset, bool little_endian = false) const;

    // Float accessors
    float get_float32(size_t byte_offset, bool little_endian = false) const;
    double get_float64(size_t byte_offset, bool little_endian = false) const;

    // Setters
    void set_int8(size_t byte_offset, int8_t value);
    void set_uint8(size_t byte_offset, uint8_t value);
    void set_int16(size_t byte_offset, int16_t value, bool little_endian = false);
    void set_uint16(size_t byte_offset, uint16_t value, bool little_endian = false);
    void set_int32(size_t byte_offset, int32_t value, bool little_endian = false);
    void set_uint32(size_t byte_offset, uint32_t value, bool little_endian = false);
    void set_float32(size_t byte_offset, float value, bool little_endian = false);
    void set_float64(size_t byte_offset, double value, bool little_endian = false);

    // Properties
    size_t get_byte_length() const { return byte_length_; }
    size_t get_byte_offset() const { return byte_offset_; }
    const uint8_t* get_buffer() const { return buffer_; }

    // Static methods for JavaScript binding
    static Value dataview_constructor(Context& ctx, const std::vector<Value>& args);
    static Value dataview_get_int8(Context& ctx, const std::vector<Value>& args);
    static Value dataview_get_uint8(Context& ctx, const std::vector<Value>& args);
    static Value dataview_set_int8(Context& ctx, const std::vector<Value>& args);
    static Value dataview_set_uint8(Context& ctx, const std::vector<Value>& args);

    // Setup
    static void setup_dataview_prototype(Context& ctx);

private:
    void validate_offset(size_t offset, size_t type_size) const;
    template<typename T> T read_value(size_t offset, bool little_endian) const;
    template<typename T> void write_value(size_t offset, T value, bool little_endian);
};

} // namespace Quanta