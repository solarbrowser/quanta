/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/DataView.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/BigInt.h"
#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/Error.h"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <type_traits>

static void quanta_memcpy(void* dest, const void* src, size_t count) {
    const char* s = static_cast<const char*>(src);
    char* d = static_cast<char*>(dest);
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
}

namespace Quanta {


DataView::DataView(std::shared_ptr<ArrayBuffer> buffer)
    : Object(ObjectType::DataView), buffer_(buffer), length_tracking_(true) {
    if (!buffer_) {
        throw std::invalid_argument("ArrayBuffer cannot be null");
    }
    if (buffer_->is_detached()) {
        throw std::runtime_error("Cannot construct DataView from detached ArrayBuffer");
    }

    byte_offset_ = 0;
    byte_length_ = buffer_->byte_length();

}

DataView::DataView(std::shared_ptr<ArrayBuffer> buffer, size_t byte_offset)
    : Object(ObjectType::DataView), buffer_(buffer), byte_offset_(byte_offset), length_tracking_(true) {
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
}

bool DataView::is_out_of_bounds() const {
    if (!buffer_ || buffer_->is_detached()) {
        return true;
    }
    size_t buf_len = buffer_->byte_length();
    if (byte_offset_ > buf_len) {
        return true;
    }
    if (!length_tracking_ && byte_offset_ + byte_length_ > buf_len) {
        return true;
    }
    return false;
}

size_t DataView::current_byte_length() const {
    if (length_tracking_) {
        return buffer_->byte_length() - byte_offset_;
    }
    return byte_length_;
}

bool DataView::validate_offset(size_t offset, size_t size) const {
    if (is_out_of_bounds()) {
        return false;
    }
    return offset + size <= current_byte_length();
}

uint8_t* DataView::get_data_ptr() const {
    if (!buffer_ || buffer_->is_detached()) {
        return nullptr;
    }
    return buffer_->data() + byte_offset_;
}

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
    
    if constexpr (sizeof(T) > 1) {
        bool is_host_big_endian = false;
        if (little_endian == is_host_big_endian) {
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
    
    if constexpr (sizeof(T) > 1) {
        bool is_host_big_endian = false;
        if (little_endian == is_host_big_endian) {
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

Value DataView::get_int8(size_t offset) const {
    return Value(static_cast<double>(read_value<int8_t>(offset, false)));
}

Value DataView::get_uint8(size_t offset) const {
    return Value(static_cast<double>(read_value<uint8_t>(offset, false)));
}

Value DataView::get_int16(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<int16_t>(offset, little_endian)));
}

Value DataView::get_uint16(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<uint16_t>(offset, little_endian)));
}

Value DataView::get_int32(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<int32_t>(offset, little_endian)));
}

Value DataView::get_uint32(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<uint32_t>(offset, little_endian)));
}

Value DataView::get_float32(size_t offset, bool little_endian) const {
    return Value(static_cast<double>(read_value<float>(offset, little_endian)));
}

Value DataView::get_float64(size_t offset, bool little_endian) const {
    return Value(read_value<double>(offset, little_endian));
}

bool DataView::set_int8(size_t offset, int8_t value) {
    return write_value<int8_t>(offset, value, false);
}

bool DataView::set_uint8(size_t offset, uint8_t value) {
    return write_value<uint8_t>(offset, value, false);
}

bool DataView::set_int16(size_t offset, int16_t value, bool little_endian) {
    return write_value<int16_t>(offset, value, little_endian);
}

bool DataView::set_uint16(size_t offset, uint16_t value, bool little_endian) {
    return write_value<uint16_t>(offset, value, little_endian);
}

bool DataView::set_int32(size_t offset, int32_t value, bool little_endian) {
    return write_value<int32_t>(offset, value, little_endian);
}

bool DataView::set_uint32(size_t offset, uint32_t value, bool little_endian) {
    return write_value<uint32_t>(offset, value, little_endian);
}

bool DataView::set_float32(size_t offset, float value, bool little_endian) {
    return write_value<float>(offset, value, little_endian);
}

bool DataView::set_float64(size_t offset, double value, bool little_endian) {
    return write_value<double>(offset, value, little_endian);
}

Value DataView::get_property(const std::string& key) const {
    if (key == "buffer") {
        return Value(buffer_.get());
    }
    if (key == "byteLength") {
        if (is_out_of_bounds()) {
            if (Object::current_context_) Object::current_context_->throw_type_error("DataView is out of bounds of its buffer");
            return Value();
        }
        return Value(static_cast<double>(current_byte_length()));
    }
    if (key == "byteOffset") {
        if (is_out_of_bounds()) {
            if (Object::current_context_) Object::current_context_->throw_type_error("DataView is out of bounds of its buffer");
            return Value();
        }
        return Value(static_cast<double>(byte_offset_));
    }

    return Object::get_property(key);
}

std::string DataView::to_string() const {
    return "[object DataView]";
}

namespace {

// ToIndex: NaN/+-0 -> 0, throws RangeError outside [0, 2^53-1], TypeError for Symbol/BigInt.
double dataview_to_index(Context& ctx, const Value& v) {
    if (v.is_symbol() || v.is_bigint()) {
        ctx.throw_type_error("Cannot convert a Symbol or BigInt value to a DataView index");
        return 0;
    }
    double number = v.to_number();
    if (ctx.has_exception()) return 0;
    if (std::isnan(number) || number == 0) return 0;
    double integer = (number < 0) ? std::ceil(number) : std::floor(number);
    if (integer < 0 || integer > 9007199254740991.0) {
        ctx.throw_range_error("Invalid DataView byte offset or length");
        return 0;
    }
    return integer;
}

}

Value DataView::constructor(Context& ctx, const std::vector<Value>& args) {
    if (args.empty() || !args[0].is_object()) {
        ctx.throw_type_error("DataView constructor requires an ArrayBuffer");
        return Value();
    }

    Object* buffer_obj = args[0].as_object();
    if (!buffer_obj->is_array_buffer()) {
        ctx.throw_type_error("DataView constructor requires an ArrayBuffer");
        return Value();
    }

    ArrayBuffer* array_buffer = static_cast<ArrayBuffer*>(buffer_obj);

    double offset = dataview_to_index(ctx, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();

    if (array_buffer->is_detached()) {
        ctx.throw_type_error("Cannot construct DataView with a detached ArrayBuffer");
        return Value();
    }
    if (offset > static_cast<double>(array_buffer->byte_length())) {
        ctx.throw_range_error("DataView byte offset exceeds ArrayBuffer size");
        return Value();
    }

    bool has_length = args.size() > 2 && !args[2].is_undefined();
    double length = 0;
    if (has_length) {
        length = dataview_to_index(ctx, args[2]);
        if (ctx.has_exception()) return Value();
        if (offset + length > static_cast<double>(array_buffer->byte_length())) {
            ctx.throw_range_error("DataView extends beyond ArrayBuffer bounds");
            return Value();
        }
    }

    auto shared_buffer = std::shared_ptr<ArrayBuffer>(array_buffer, [](ArrayBuffer*) {
    });

    try {
        std::unique_ptr<DataView> dataview;
        if (has_length) {
            dataview = std::make_unique<DataView>(shared_buffer, static_cast<size_t>(offset), static_cast<size_t>(length));
        } else {
            dataview = std::make_unique<DataView>(shared_buffer, static_cast<size_t>(offset));
        }

        return Value(dataview.release());
    } catch (const std::exception& e) {
        ctx.throw_range_error(e.what());
        return Value();
    }
}


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

}


namespace {

bool dataview_check_in_bounds(Context& ctx, DataView* dv, double offset, size_t element_size) {
    if (dv->is_out_of_bounds()) {
        ctx.throw_type_error("DataView is out of bounds of its buffer");
        return false;
    }
    double view_size = static_cast<double>(dv->current_byte_length());
    if (offset + static_cast<double>(element_size) > view_size) {
        ctx.throw_range_error("Offset is outside the bounds of the DataView");
        return false;
    }
    return true;
}

DataView* dataview_require_this(Context& ctx, const char* method_name) {
    Object* this_obj = ctx.get_this_binding();
    if (!this_obj || !this_obj->is_data_view()) {
        ctx.throw_type_error(std::string(method_name) + " called on non-DataView object");
        return nullptr;
    }
    return static_cast<DataView*>(this_obj);
}

// ES SetValueInBuffer's numeric conversions use modular wraparound, not clamping.
template<typename T>
T dataview_to_int_modular(double num_val) {
    if (std::isnan(num_val) || std::isinf(num_val) || num_val == 0.0) {
        return T{0};
    }
    double truncated = std::trunc(num_val);
    constexpr int bits = sizeof(T) * 8;
    double mod = std::pow(2.0, bits);
    double mod_val = std::fmod(truncated, mod);
    if (mod_val < 0) mod_val += mod;
    if constexpr (std::is_signed_v<T>) {
        double half = mod / 2.0;
        if (mod_val >= half) mod_val -= mod;
    }
    return static_cast<T>(static_cast<int64_t>(mod_val));
}

// ToBigInt(value): objects go through ToPrimitive (valueOf/toString) first, then re-coerced.
Value dataview_to_bigint(Context& ctx, const Value& value) {
    if (value.is_bigint()) return value;
    if (value.is_boolean()) return Value(new BigInt(value.as_boolean() ? 1 : 0));
    if (value.is_string()) {
        try {
            return Value(new BigInt(value.to_string()));
        } catch (const std::exception&) {
            ctx.throw_syntax_error("Cannot convert string to BigInt");
            return Value();
        }
    }
    if (value.is_object()) {
        Object* obj = value.as_object();
        Value valueOf_fn = obj->get_property("valueOf");
        if (valueOf_fn.is_function()) {
            Value prim = valueOf_fn.as_function()->call(ctx, {}, value);
            if (ctx.has_exception()) return Value();
            if (!prim.is_object() && !prim.is_function()) {
                return dataview_to_bigint(ctx, prim);
            }
        }
        Value toString_fn = obj->get_property("toString");
        if (toString_fn.is_function()) {
            Value prim = toString_fn.as_function()->call(ctx, {}, value);
            if (ctx.has_exception()) return Value();
            if (!prim.is_object() && !prim.is_function()) {
                return dataview_to_bigint(ctx, prim);
            }
        }
        ctx.throw_type_error("Cannot convert object to BigInt");
        return Value();
    }
    ctx.throw_type_error("Cannot convert value to BigInt");
    return Value();
}

}

Value DataView::js_get_int8(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getInt8");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    if (!dataview_check_in_bounds(ctx, dv, offset, 1)) return Value();
    return dv->get_int8(static_cast<size_t>(offset));
}

Value DataView::js_get_uint8(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getUint8");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    if (!dataview_check_in_bounds(ctx, dv, offset, 1)) return Value();
    return dv->get_uint8(static_cast<size_t>(offset));
}

Value DataView::js_get_int16(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getInt16");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 2)) return Value();
    return dv->get_int16(static_cast<size_t>(offset), little_endian);
}

Value DataView::js_get_uint16(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getUint16");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 2)) return Value();
    return dv->get_uint16(static_cast<size_t>(offset), little_endian);
}

Value DataView::js_get_int32(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getInt32");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 4)) return Value();
    return dv->get_int32(static_cast<size_t>(offset), little_endian);
}

Value DataView::js_get_uint32(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getUint32");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 4)) return Value();
    return dv->get_uint32(static_cast<size_t>(offset), little_endian);
}

namespace {

double half_bits_to_double(uint16_t h) {
    int sign = (h >> 15) & 1;
    int exp = (h >> 10) & 0x1F;
    int mant = h & 0x3FF;
    double result;
    if (exp == 31) {
        result = mant ? std::numeric_limits<double>::quiet_NaN()
                      : std::numeric_limits<double>::infinity();
    } else if (exp == 0) {
        result = std::ldexp(static_cast<double>(mant), -24);
    } else {
        result = std::ldexp(1.0 + mant / 1024.0, exp - 15);
    }
    return sign ? -result : result;
}

uint16_t double_to_half_bits(double x) {
    if (std::isnan(x)) return 0x7E00;
    uint16_t sign = std::signbit(x) ? 0x8000 : 0;
    double a = std::fabs(x);
    // Past the max-half/infinity midpoint (65504 + 16) everything rounds to Infinity.
    if (a >= 65520.0) return sign | 0x7C00;
    if (a < std::ldexp(1.0, -14)) {
        // Subnormal quantum 2^-24; a result of 1024 lands on the smallest
        // normal encoding naturally. Power-of-two scaling keeps one rounding.
        return sign | static_cast<uint16_t>(std::nearbyint(std::ldexp(a, 24)));
    }
    int e;
    std::frexp(a, &e);
    --e;
    int mant = static_cast<int>(std::nearbyint(std::ldexp(a, 10 - e)));
    if (mant == 2048) { mant = 1024; ++e; }
    if (e > 15) return sign | 0x7C00;
    return sign | static_cast<uint16_t>(((e + 15) << 10) | (mant - 1024));
}

}

Value DataView::js_get_float16(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getFloat16");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 2)) return Value();
    uint16_t bits = static_cast<uint16_t>(dv->get_uint16(static_cast<size_t>(offset), little_endian).to_number());
    return Value(half_bits_to_double(bits));
}

Value DataView::js_set_float16(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setFloat16");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value _val = args.size() > 1 ? args[1] : Value();
    if (_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
    double num = _val.to_number();
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 2)) return Value();
    dv->set_uint16(static_cast<size_t>(offset), double_to_half_bits(num), little_endian);
    return Value();
}

Value DataView::js_get_float32(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getFloat32");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 4)) return Value();
    return dv->get_float32(static_cast<size_t>(offset), little_endian);
}

Value DataView::js_get_float64(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getFloat64");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 1) ? args[1].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 8)) return Value();
    return dv->get_float64(static_cast<size_t>(offset), little_endian);
}

Value DataView::js_set_int8(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setInt8");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value _val = args.size() > 1 ? args[1] : Value();
    if (_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
    double num = _val.to_number();
    if (ctx.has_exception()) return Value();
    if (!dataview_check_in_bounds(ctx, dv, offset, 1)) return Value();
    dv->set_int8(static_cast<size_t>(offset), dataview_to_int_modular<int8_t>(num));
    return Value();
}

Value DataView::js_set_uint8(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setUint8");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value _val = args.size() > 1 ? args[1] : Value();
    if (_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
    double num = _val.to_number();
    if (ctx.has_exception()) return Value();
    if (!dataview_check_in_bounds(ctx, dv, offset, 1)) return Value();
    dv->set_uint8(static_cast<size_t>(offset), dataview_to_int_modular<uint8_t>(num));
    return Value();
}

Value DataView::js_set_int16(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setInt16");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value _val = args.size() > 1 ? args[1] : Value();
    if (_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
    double num = _val.to_number();
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 2)) return Value();
    dv->set_int16(static_cast<size_t>(offset), dataview_to_int_modular<int16_t>(num), little_endian);
    return Value();
}

Value DataView::js_set_uint16(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setUint16");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value _val = args.size() > 1 ? args[1] : Value();
    if (_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
    double num = _val.to_number();
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 2)) return Value();
    dv->set_uint16(static_cast<size_t>(offset), dataview_to_int_modular<uint16_t>(num), little_endian);
    return Value();
}

Value DataView::js_set_int32(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setInt32");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value _val = args.size() > 1 ? args[1] : Value();
    if (_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
    double num = _val.to_number();
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 4)) return Value();
    dv->set_int32(static_cast<size_t>(offset), dataview_to_int_modular<int32_t>(num), little_endian);
    return Value();
}

Value DataView::js_set_uint32(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setUint32");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value _val = args.size() > 1 ? args[1] : Value();
    if (_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
    double num = _val.to_number();
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 4)) return Value();
    dv->set_uint32(static_cast<size_t>(offset), dataview_to_int_modular<uint32_t>(num), little_endian);
    return Value();
}

Value DataView::js_set_float32(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setFloat32");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value _val = args.size() > 1 ? args[1] : Value();
    if (_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
    double num = _val.to_number();
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 4)) return Value();
    dv->set_float32(static_cast<size_t>(offset), static_cast<float>(num), little_endian);
    return Value();
}

Value DataView::js_set_float64(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setFloat64");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value _val = args.size() > 1 ? args[1] : Value();
    if (_val.is_symbol()) { ctx.throw_type_error("Cannot convert a Symbol value to a number"); return Value(); }
    double num = _val.to_number();
    if (ctx.has_exception()) return Value();
    bool little_endian = (args.size() > 2) ? args[2].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 8)) return Value();
    dv->set_float64(static_cast<size_t>(offset), num, little_endian);
    return Value();
}

Value DataView::js_get_bigint64(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getBigInt64");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    bool le = (args.size() > 1) ? args[1].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 8)) return Value();
    int64_t val = static_cast<int64_t>(dv->read_value<uint64_t>(static_cast<size_t>(offset), le));
    return Value(new BigInt(val));
}

Value DataView::js_set_bigint64(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setBigInt64");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value big = dataview_to_bigint(ctx, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    bool le = (args.size() > 2) ? args[2].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 8)) return Value();
    int64_t val = big.as_bigint()->to_int64();
    dv->write_value<uint64_t>(static_cast<size_t>(offset), static_cast<uint64_t>(val), le);
    return Value();
}

Value DataView::js_get_biguint64(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "getBigUint64");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    bool le = (args.size() > 1) ? args[1].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 8)) return Value();
    uint64_t val = dv->read_value<uint64_t>(static_cast<size_t>(offset), le);
    return Value(new BigInt(std::to_string(val)));
}

Value DataView::js_set_biguint64(Context& ctx, const std::vector<Value>& args) {
    DataView* dv = dataview_require_this(ctx, "setBigUint64");
    if (!dv) return Value();
    double offset = dataview_to_index(ctx, args.empty() ? Value() : args[0]);
    if (ctx.has_exception()) return Value();
    Value big = dataview_to_bigint(ctx, args.size() > 1 ? args[1] : Value());
    if (ctx.has_exception()) return Value();
    bool le = (args.size() > 2) ? args[2].to_boolean() : false;
    if (!dataview_check_in_bounds(ctx, dv, offset, 8)) return Value();
    uint64_t val = static_cast<uint64_t>(big.as_bigint()->to_int64());
    dv->write_value<uint64_t>(static_cast<size_t>(offset), val, le);
    return Value();
}

}
