/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_DATAVIEW_H
#define QUANTA_DATAVIEW_H

#include "quanta/Object.h"
#include "quanta/ArrayBuffer.h"
#include "quanta/Value.h"
#include <memory>
#include <cstdint>

namespace Quanta {

class Context;
class ArrayBuffer;

/**
 * DataView provides a flexible interface for reading and writing 
 * multi-byte numeric data at arbitrary offsets in ArrayBuffers
 */
class DataView : public Object {
private:
    std::shared_ptr<ArrayBuffer> buffer_;
    size_t byte_offset_;
    size_t byte_length_;
    
    bool validate_offset(size_t offset, size_t size) const;
    uint8_t* get_data_ptr() const;
    
    template<typename T>
    T read_value(size_t offset, bool little_endian) const;
    
    template<typename T>
    bool write_value(size_t offset, T value, bool little_endian);
    
    uint16_t swap_bytes_16(uint16_t value) const;
    uint32_t swap_bytes_32(uint32_t value) const;
    uint64_t swap_bytes_64(uint64_t value) const;
    
public:
    explicit DataView(std::shared_ptr<ArrayBuffer> buffer);
    DataView(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset);
    DataView(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t byte_length);
    
    virtual ~DataView() = default;
    
    ArrayBuffer* buffer() const { return buffer_.get(); }
    size_t byte_offset() const { return byte_offset_; }
    size_t byte_length() const { return byte_length_; }
    
    bool is_data_view() const override { return true; }
    std::string get_type_name() const { return "DataView"; }
    
    Value get_int8(size_t offset) const;
    Value get_uint8(size_t offset) const;
    
    Value get_int16(size_t offset, bool little_endian = false) const;
    Value get_uint16(size_t offset, bool little_endian = false) const;
    
    Value get_int32(size_t offset, bool little_endian = false) const;
    Value get_uint32(size_t offset, bool little_endian = false) const;
    
    Value get_float32(size_t offset, bool little_endian = false) const;
    Value get_float64(size_t offset, bool little_endian = false) const;
    
    bool set_int8(size_t offset, int8_t value);
    bool set_uint8(size_t offset, uint8_t value);
    
    bool set_int16(size_t offset, int16_t value, bool little_endian = false);
    bool set_uint16(size_t offset, uint16_t value, bool little_endian = false);
    
    bool set_int32(size_t offset, int32_t value, bool little_endian = false);
    bool set_uint32(size_t offset, uint32_t value, bool little_endian = false);
    
    bool set_float32(size_t offset, float value, bool little_endian = false);
    bool set_float64(size_t offset, double value, bool little_endian = false);
    
    Value get_property(const std::string& key) const override;
    
    std::string to_string() const;
    
    static Value constructor(Context& ctx, const std::vector<Value>& args);
    
    static Value js_get_int8(Context& ctx, const std::vector<Value>& args);
    static Value js_get_uint8(Context& ctx, const std::vector<Value>& args);
    static Value js_get_int16(Context& ctx, const std::vector<Value>& args);
    static Value js_get_uint16(Context& ctx, const std::vector<Value>& args);
    static Value js_get_int32(Context& ctx, const std::vector<Value>& args);
    static Value js_get_uint32(Context& ctx, const std::vector<Value>& args);
    static Value js_get_float32(Context& ctx, const std::vector<Value>& args);
    static Value js_get_float64(Context& ctx, const std::vector<Value>& args);
    
    static Value js_set_int8(Context& ctx, const std::vector<Value>& args);
    static Value js_set_uint8(Context& ctx, const std::vector<Value>& args);
    static Value js_set_int16(Context& ctx, const std::vector<Value>& args);
    static Value js_set_uint16(Context& ctx, const std::vector<Value>& args);
    static Value js_set_int32(Context& ctx, const std::vector<Value>& args);
    static Value js_set_uint32(Context& ctx, const std::vector<Value>& args);
    static Value js_set_float32(Context& ctx, const std::vector<Value>& args);
    static Value js_set_float64(Context& ctx, const std::vector<Value>& args);

private:
    void setup_methods();
    
    static DataView* get_this_dataview(Context& ctx);
};

/**
 * DataView utility functions
 */
namespace DataViewFactory {
    std::unique_ptr<DataView> create(std::shared_ptr<ArrayBuffer> buffer);
    std::unique_ptr<DataView> create(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset);
    std::unique_ptr<DataView> create(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t byte_length);
    
    bool is_data_view(const Object* obj);
    DataView* as_data_view(Object* obj);
    const DataView* as_data_view(const Object* obj);
}

}

#endif
