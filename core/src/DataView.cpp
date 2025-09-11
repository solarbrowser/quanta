/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "DataView.h"
#include "ArrayBuffer.h"
#include "Context.h"
#include "Error.h"
#include <algorithm>
#include <sstream>
#include <cmath>

// Custom memory functions to avoid cstring linkage issues on Windows
static void quanta_memcpy(void* dest, const void* src, size_t count) {
    const char* s = static_cast<const char*>(src);
    char* d = static_cast<char*>(dest);
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
}

namespace Quanta {

//=============================================================================
// DataView Implementation
//=============================================================================

DataView::DataView(std::shared_ptr<ArrayBuffer> buffer)
    : Object(ObjectType::DataView), buffer_(buffer) {
    if (!buffer_) {
        throw std::invalid_argument("ArrayBuffer cannot be null");
    }
    if (buffer_->is_detached()) {
        throw std::runtime_error("Cannot construct DataView from detached ArrayBuffer");
    }
    
    byte_offset_ = 0;
    byte_length_ = buffer_->byte_length();
    
    // Add methods directly to this instance - DISABLED due to compilation issues
    // setup_methods();
}

DataView::DataView(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset)
    : Object(ObjectType::DataView), buffer_(buffer), byte_offset_(byte_offset) {
    if (!buffer_) {
        throw std::invalid_argument("ArrayBuffer cannot be null");
    }
    if (buffer_->is_detached()) {
        throw std::runtime_error("Cannot construct DataView from detached ArrayBuffer");
    }
    if (byte_offset > buffer_->byte_length()) {
        throw std::range_error("DataView byte offset exceeds ArrayBuffer size");
    }
    
    byte_length_ = buffer_->byte_length() - byte_offset_;
    // setup_methods();
}

DataView::DataView(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t byte_length)
    : Object(ObjectType::DataView), buffer_(buffer), byte_offset_(byte_offset), byte_length_(byte_length) {
    if (!buffer_) {
        throw std::invalid_argument("ArrayBuffer cannot be null");
    }
    if (buffer_->is_detached()) {
        throw std::runtime_error("Cannot construct DataView from detached ArrayBuffer");
    }
    if (byte_offset + byte_length > buffer_->byte_length()) {
        throw std::range_error("DataView extends beyond ArrayBuffer bounds");
    }
    // setup_methods();
}

bool DataView::validate_offset(size_t offset, size_t size) const {
    if (buffer_->is_detached()) {
        return false;
    }
    return offset + size <= byte_length_;
}

uint8_t* DataView::get_data_ptr() const {
    if (!buffer_ || buffer_->is_detached()) {
        return nullptr;
    }
    return buffer_->data() + byte_offset_;
}

// Endianness conversion utilities
uint16_t DataView::swap_bytes_16(uint16_t value) const {
    return ((value >> 8) & 0xFF) | ((value & 0xFF) << 8);
}

uint32_t DataView::swap_bytes_32(uint32_t value) const {
    return ((value >> 24) & 0xFF) |
           ((value >> 8) & 0xFF00) |
           ((value & 0xFF00) << 8) |
           ((value & 0xFF) << 24);
}

uint64_t DataView::swap_bytes_64(uint64_t value) const {
    return ((value >> 56) & 0xFF) |
           ((value >> 40) & 0xFF00) |
           ((value >> 24) & 0xFF0000) |
           ((value >> 8) & 0xFF000000) |
           ((value & 0xFF000000) << 8) |
           ((value & 0xFF0000) << 24) |
           ((value & 0xFF00) << 40) |
           ((value & 0xFF) << 56);
}

// Template implementation for reading values
template<typename T>
T DataView::read_value(size_t offset, bool little_endian) const {
    if (!validate_offset(offset, sizeof(T))) {
        return T{};
    }
    
    uint8_t* data = get_data_ptr();
    if (!data) {
        return T{};
    }
    
    T result;
    quanta_memcpy(&result, data + offset, sizeof(T));
    
    // Handle endianness conversion for multi-byte types
    if constexpr (sizeof(T) > 1) {
        bool is_big_endian = false; // Assume little-endian host for now
        if (little_endian != is_big_endian) {
            // Need to swap bytes
            if constexpr (sizeof(T) == 2) {
                uint16_t* ptr = reinterpret_cast<uint16_t*>(&result);
                *ptr = swap_bytes_16(*ptr);
            } else if constexpr (sizeof(T) == 4) {
                uint32_t* ptr = reinterpret_cast<uint32_t*>(&result);
                *ptr = swap_bytes_32(*ptr);
            } else if constexpr (sizeof(T) == 8) {
                uint64_t* ptr = reinterpret_cast<uint64_t*>(&result);
                *ptr = swap_bytes_64(*ptr);
            }
        }
    }
    
    return result;
}

template<typename T>
bool DataView::write_value(size_t offset, T value, bool little_endian) {
    if (!validate_offset(offset, sizeof(T))) {
        return false;
    }
    
    uint8_t* data = get_data_ptr();
    if (!data) {
        return false;
    }
    
    // Handle endianness conversion for multi-byte types
    if constexpr (sizeof(T) > 1) {
        bool is_big_endian = false; // Assume little-endian host for now
        if (little_endian != is_big_endian) {
            // Need to swap bytes
            if constexpr (sizeof(T) == 2) {
                uint16_t* ptr = reinterpret_cast<uint16_t*>(&value);
                *ptr = swap_bytes_16(*ptr);
            } else if constexpr (sizeof(T) == 4) {
                uint32_t* ptr = reinterpret_cast<uint32_t*>(&value);
                *ptr = swap_bytes_32(*ptr);
            } else if constexpr (sizeof(T) == 8) {
                uint64_t* ptr = reinterpret_cast<uint64_t*>(&value);
                *ptr = swap_bytes_64(*ptr);
            }
        }
    }
    
    quanta_memcpy(data + offset, &value, sizeof(T));
    return true;
}

// 8-bit getters
Value DataView::get_int8(size_t offset) const {
    return Value(static_cast<double>(read_value<int8_t>(offset, false)));
}

Value DataView::get_uint8(size_t offset) const {
    return Value(static_cast<double>(read_value<uint8_t>(offset, false)));
}

// 16-bit getters
Value DataView::get_int16(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<int16_t>(offset, little_endian)));
}

Value DataView::get_uint16(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<uint16_t>(offset, little_endian)));
}

// 32-bit getters
Value DataView::get_int32(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<int32_t>(offset, little_endian)));
}

Value DataView::get_uint32(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<uint32_t>(offset, little_endian)));
}

// Float getters
Value DataView::get_float32(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<float>(offset, little_endian)));
}

Value DataView::get_float64(size_t offset, bool little_endian) const {
    return Value(read_value<double>(offset, little_endian));
}

// 8-bit setters
bool DataView::set_int8(size_t offset, int8_t value) {
    return write_value<int8_t>(offset, value, false);
}

bool DataView::set_uint8(size_t offset, uint8_t value) {
    return write_value<uint8_t>(offset, value, false);
}

// 16-bit setters
bool DataView::set_int16(size_t offset, int16_t value, bool little_endian) {
    return write_value<int16_t>(offset, value, little_endian);
}

bool DataView::set_uint16(size_t offset, uint16_t value, bool little_endian) {
    return write_value<uint16_t>(offset, value, little_endian);
}

// 32-bit setters
bool DataView::set_int32(size_t offset, int32_t value, bool little_endian) {
    return write_value<int32_t>(offset, value, little_endian);
}

bool DataView::set_uint32(size_t offset, uint32_t value, bool little_endian) {
    return write_value<uint32_t>(offset, value, little_endian);
}

// Float setters
bool DataView::set_float32(size_t offset, float value, bool little_endian) {
    return write_value<float>(offset, value, little_endian);
}

bool DataView::set_float64(size_t offset, double value, bool little_endian) {
    return write_value<double>(offset, value, little_endian);
}

Value DataView::get_property(const std::string& key) const {
    // Handle DataView properties
    if (key == "buffer") {
        return Value(buffer_.get());
    }
    if (key == "byteLength") {
        return Value(static_cast<double>(byte_length_));
    }
    if (key == "byteOffset") {
        return Value(static_cast<double>(byte_offset_));
    }
    
    // Delegate to base class
    return Object::get_property(key);
}

std::string DataView::to_string() const {
    return "[object DataView]";
}

// Static constructor
Value DataView::constructor(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("DataView constructor requires at least one argument");
        return Value();
    }
    
    if (!args[0].is_object()) {
        ctx.throw_type_error("DataView constructor requires an ArrayBuffer");
        return Value();
    }
    
    Object* buffer_obj = args[0].as_object();
    if (!buffer_obj->is_array_buffer()) {
        ctx.throw_type_error("DataView constructor requires an ArrayBuffer");
        return Value();
    }
    
    ArrayBuffer* array_buffer = static_cast<ArrayBuffer*>(buffer_obj);
    auto shared_buffer = std::shared_ptr<ArrayBuffer>(array_buffer, [](ArrayBuffer*) {
        // Don't delete - ArrayBuffer is managed elsewhere
    });
    
    try {
        std::unique_ptr<DataView> dataview;
        
        if (args.size() == 1) {
            dataview = std::make_unique<DataView>(shared_buffer);
        } else if (args.size() == 2) {
            size_t byte_offset = static_cast<size_t>(args[1].to_number());
            dataview = std::make_unique<DataView>(shared_buffer, byte_offset);
        } else {
            size_t byte_offset = static_cast<size_t>(args[1].to_number());
            size_t byte_length = static_cast<size_t>(args[2].to_number());
            dataview = std::make_unique<DataView>(shared_buffer, byte_offset, byte_length);
        }
        
        return Value(dataview.release());
    } catch (const std::exception& e) {
        ctx.throw_error(std::string("DataView creation failed: ") + e.what());
        return Value();
    }
}

//=============================================================================
// DataViewFactory Implementation
//=============================================================================

namespace DataViewFactory {

std::unique_ptr<DataView> create(std::shared_ptr<ArrayBuffer> buffer) {
    return std::make_unique<DataView>(buffer);
}

std::unique_ptr<DataView> create(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset) {
    return std::make_unique<DataView>(buffer, byte_offset);
}

std::unique_ptr<DataView> create(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset, size_t byte_length) {
    return std::make_unique<DataView>(buffer, byte_offset, byte_length);
}

bool is_data_view(const Object* obj) {
    return obj && obj->is_data_view();
}

DataView* as_data_view(Object* obj) {
    return is_data_view(obj) ? static_cast<DataView*>(obj) : nullptr;
}

const DataView* as_data_view(const Object* obj) {
    return is_data_view(obj) ? static_cast<const DataView*>(obj) : nullptr;
}

} // namespace DataViewFactory

//=============================================================================
// DataView Instance Setup
//=============================================================================

void DataView::setup_methods() {
    // DISABLED - methods are now registered in Context.cpp prototype
    /*
    // Helper lambda to get DataView from this binding
    auto get_dataview = [](Context& ctx) -> DataView* {
        Object* this_obj = ctx.get_this_binding();
        if (!this_obj || !this_obj->is_data_view()) {
            return nullptr;
        }
        return static_cast<DataView*>(this_obj);
    };
    
    // Create bound methods for this instance
    auto get_int8_fn = ObjectFactory::create_native_function("getInt8", 
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("getInt8 requires offset argument");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("getInt8 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            return dataview->get_int8(offset);
        });
    set_property("getInt8", Value(get_int8_fn.release()));
    
    auto get_uint8_fn = ObjectFactory::create_native_function("getUint8",
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("getUint8 requires offset argument");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("getUint8 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            return dataview->get_uint8(offset);
        });
    set_property("getUint8", Value(get_uint8_fn.release()));
    
    auto get_int32_fn = ObjectFactory::create_native_function("getInt32",
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("getInt32 requires offset argument");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("getInt32 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
            return dataview->get_int32(offset, little_endian);
        });
    set_property("getInt32", Value(get_int32_fn.release()));
    
    auto get_float32_fn = ObjectFactory::create_native_function("getFloat32",
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("getFloat32 requires offset argument");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("getFloat32 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
            return dataview->get_float32(offset, little_endian);
        });
    set_property("getFloat32", Value(get_float32_fn.release()));
    
    auto get_float64_fn = ObjectFactory::create_native_function("getFloat64",
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                ctx.throw_type_error("getFloat64 requires offset argument");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("getFloat64 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
            return dataview->get_float64(offset, little_endian);
        });
    set_property("getFloat64", Value(get_float64_fn.release()));
    
    auto set_int8_fn = ObjectFactory::create_native_function("setInt8",
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_type_error("setInt8 requires offset and value arguments");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("setInt8 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            int8_t value = static_cast<int8_t>(args[1].to_number());
            dataview->set_int8(offset, value);
            return Value();
        });
    set_property("setInt8", Value(set_int8_fn.release()));
    
    auto set_uint8_fn = ObjectFactory::create_native_function("setUint8",
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_type_error("setUint8 requires offset and value arguments");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("setUint8 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            uint8_t value = static_cast<uint8_t>(args[1].to_number());
            dataview->set_uint8(offset, value);
            return Value();
        });
    set_property("setUint8", Value(set_uint8_fn.release()));
    
    auto set_int32_fn = ObjectFactory::create_native_function("setInt32",
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_type_error("setInt32 requires offset and value arguments");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("setInt32 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            int32_t value = static_cast<int32_t>(args[1].to_number());
            bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
            dataview->set_int32(offset, value, little_endian);
            return Value();
        });
    set_property("setInt32", Value(set_int32_fn.release()));
    
    auto set_float32_fn = ObjectFactory::create_native_function("setFloat32",
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_type_error("setFloat32 requires offset and value arguments");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("setFloat32 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            float value = static_cast<float>(args[1].to_number());
            bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
            dataview->set_float32(offset, value, little_endian);
            return Value();
        });
    set_property("setFloat32", Value(set_float32_fn.release()));
    
    auto set_float64_fn = ObjectFactory::create_native_function("setFloat64",
        [get_dataview](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_type_error("setFloat64 requires offset and value arguments");
                return Value();
            }
            
            DataView* dataview = get_dataview(ctx);
            if (!dataview) {
                ctx.throw_type_error("setFloat64 called on non-DataView object");
                return Value();
            }
            
            size_t offset = static_cast<size_t>(args[0].to_number());
            double value = args[1].to_number();
            bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
            dataview->set_float64(offset, value, little_endian);
            return Value();
        });
    set_property("setFloat64", Value(set_float64_fn.release()));
    */
}

//=============================================================================
// DataView JavaScript Method Implementations
//=============================================================================

DataView* DataView::get_this_dataview(Context& ctx) {
    // In a real implementation, we'd get 'this' from the context
    // For now, this is a placeholder
    return nullptr;
}

Value DataView::js_get_int8(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("DataView.getInt8 requires offset argument");
        return Value();
    }
    
    // This would normally get 'this' from context binding
    // For now, return placeholder
    return Value(0.0);
}

Value DataView::js_get_uint8(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("DataView.getUint8 requires offset argument");
        return Value();
    }
    
    // Get 'this' binding (the DataView object)
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("getUint8 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    return dataview->get_uint8(offset);
}

Value DataView::js_get_int16(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("DataView.getInt16 requires offset argument");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("getInt16 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    return dataview->get_int16(offset, little_endian);
}

Value DataView::js_get_uint16(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("DataView.getUint16 requires offset argument");
        return Value();
    }
    return Value(0.0);
}

Value DataView::js_get_int32(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("DataView.getInt32 requires offset argument");
        return Value();
    }
    return Value(0.0);
}

Value DataView::js_get_uint32(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("DataView.getUint32 requires offset argument");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("getUint32 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    return dataview->get_uint32(offset, little_endian);
}

Value DataView::js_get_float32(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("DataView.getFloat32 requires offset argument");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("getFloat32 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    return dataview->get_float32(offset, little_endian);
}

Value DataView::js_get_float64(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("DataView.getFloat64 requires offset argument");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("getFloat64 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    return dataview->get_float64(offset, little_endian);
}

Value DataView::js_set_int8(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("DataView.setInt8 requires offset and value arguments");
        return Value();
    }
    return Value();
}

Value DataView::js_set_uint8(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("DataView.setUint8 requires offset and value arguments");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("setUint8 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    uint8_t value = static_cast<uint8_t>(args[1].to_number());
    dataview->set_uint8(offset, value);
    return Value();
}

Value DataView::js_set_int16(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("DataView.setInt16 requires offset and value arguments");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("setInt16 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    int16_t value = static_cast<int16_t>(args[1].to_number());
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    dataview->set_int16(offset, value, little_endian);
    return Value();
}

Value DataView::js_set_uint16(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("DataView.setUint16 requires offset and value arguments");
        return Value();
    }
    return Value();
}

Value DataView::js_set_int32(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("DataView.setInt32 requires offset and value arguments");
        return Value();
    }
    return Value();
}

Value DataView::js_set_uint32(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("DataView.setUint32 requires offset and value arguments");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("setUint32 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    uint32_t value = static_cast<uint32_t>(args[1].to_number());
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    dataview->set_uint32(offset, value, little_endian);
    return Value();
}

Value DataView::js_set_float32(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("DataView.setFloat32 requires offset and value arguments");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("setFloat32 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    float value = static_cast<float>(args[1].to_number());
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    dataview->set_float32(offset, value, little_endian);
    return Value();
}

Value DataView::js_set_float64(Context& ctx, const std::vector<Value>& args) {
    if (args.size() < 2) {
        ctx.throw_type_error("DataView.setFloat64 requires offset and value arguments");
        return Value();
    }
    
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error("setFloat64 called on non-DataView object");
        return Value();
    }
    
    DataView* dataview = static_cast<DataView*>(this_obj);
    size_t offset = static_cast<size_t>(args[0].to_number());
    double value = args[1].to_number();
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    dataview->set_float64(offset, value, little_endian);
    return Value();
}

} // namespace Quanta