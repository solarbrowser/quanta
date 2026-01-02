/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/engine/Context.h"
#include <algorithm>
#include <new>
#include <stdexcept>

#ifdef _WIN32
    #include <malloc.h>
#else
    #include <cstdlib>
#endif

extern "C" {
    static void* quanta_memcpy(void* dest, const void* src, size_t n) {
        char* d = (char*)dest;
        const char* s = (const char*)src;
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
        return dest;
    }
    
    static void* quanta_memset(void* s, int c, size_t n) {
        unsigned char* p = (unsigned char*)s;
        for (size_t i = 0; i < n; i++) {
            p[i] = (unsigned char)c;
        }
        return s;
    }
}

namespace Quanta {


ArrayBuffer::ArrayBuffer(size_t byte_length)
    : Object(ObjectType::ArrayBuffer), byte_length_(byte_length), 
      max_byte_length_(byte_length), is_detached_(false), is_resizable_(false) {
    allocate_buffer(byte_length);
}

ArrayBuffer::ArrayBuffer(size_t byte_length, size_t max_byte_length)
    : Object(ObjectType::ArrayBuffer), byte_length_(byte_length),
      max_byte_length_(max_byte_length), is_detached_(false), is_resizable_(true) {
    if (byte_length > max_byte_length) {
        throw std::invalid_argument("byte_length cannot exceed max_byte_length");
    }
    allocate_buffer(max_byte_length);
}

ArrayBuffer::ArrayBuffer(const uint8_t* source, size_t byte_length)
    : Object(ObjectType::ArrayBuffer), byte_length_(byte_length),
      max_byte_length_(byte_length), is_detached_(false), is_resizable_(false) {
    allocate_buffer(byte_length);
    if (source && data_) {
        quanta_memcpy(data_.get(), source, byte_length);
    }
}

ArrayBuffer::~ArrayBuffer() {
    detach_all_views();
}

void ArrayBuffer::allocate_buffer(size_t byte_length) {
    if (byte_length == 0) {
        data_ = nullptr;
        return;
    }
    
    try {
        uint8_t* raw_ptr = allocate_aligned(byte_length);
        data_ = std::unique_ptr<uint8_t[]>(raw_ptr);
        
        quanta_memset(data_.get(), 0, byte_length);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("ArrayBuffer allocation failed: out of memory");
    }
}

uint8_t* ArrayBuffer::allocate_aligned(size_t size, size_t alignment) {
    #ifdef _WIN32
        void* ptr = _aligned_malloc(size, alignment);
        if (!ptr) throw std::bad_alloc();
        return static_cast<uint8_t*>(ptr);
    #else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, alignment, size) != 0) {
            throw std::bad_alloc();
        }
        return static_cast<uint8_t*>(ptr);
    #endif
}

void ArrayBuffer::deallocate_aligned(uint8_t* ptr) {
    if (ptr) {
        #ifdef _WIN32
            _aligned_free(ptr);
        #else
            free(ptr);
        #endif
    }
}

bool ArrayBuffer::read_bytes(size_t offset, void* dest, size_t count) const {
    if (!check_bounds(offset, count) || !dest || is_detached_) {
        return false;
    }
    
    quanta_memcpy(dest, data_.get() + offset, count);
    return true;
}

bool ArrayBuffer::write_bytes(size_t offset, const void* src, size_t count) {
    if (!check_bounds(offset, count) || !src || is_detached_) {
        return false;
    }
    
    quanta_memcpy(data_.get() + offset, src, count);
    return true;
}

bool ArrayBuffer::check_bounds(size_t offset, size_t count) const {
    if (is_detached_ || offset > byte_length_) {
        return false;
    }
    
    if (offset + count < offset || offset + count > byte_length_) {
        return false;
    }
    
    return true;
}

std::unique_ptr<ArrayBuffer> ArrayBuffer::slice(size_t start, size_t end) const {
    if (is_detached_) {
        return nullptr;
    }
    
    if (end == SIZE_MAX) {
        end = byte_length_;
    }
    
    start = std::min(start, byte_length_);
    end = std::min(end, byte_length_);
    
    if (start >= end) {
        return std::make_unique<ArrayBuffer>(0);
    }
    
    size_t slice_length = end - start;
    return std::make_unique<ArrayBuffer>(data_.get() + start, slice_length);
}

bool ArrayBuffer::resize(size_t new_byte_length) {
    if (!is_resizable_ || is_detached_) {
        return false;
    }
    
    if (new_byte_length > max_byte_length_) {
        return false;
    }
    
    byte_length_ = new_byte_length;
    
    set_property("byteLength", Value(static_cast<double>(byte_length_)));
    
    return true;
}

void ArrayBuffer::detach() {
    if (is_detached_) {
        return;
    }
    
    is_detached_ = true;
    detach_all_views();
    
    set_property("byteLength", Value(0.0));
}

void ArrayBuffer::register_view(TypedArrayBase* view) {
    if (view && std::find(attached_views_.begin(), attached_views_.end(), view) == attached_views_.end()) {
        attached_views_.push_back(view);
    }
}

void ArrayBuffer::unregister_view(TypedArrayBase* view) {
    attached_views_.erase(
        std::remove(attached_views_.begin(), attached_views_.end(), view),
        attached_views_.end()
    );
}

void ArrayBuffer::detach_all_views() {
    attached_views_.clear();
}

void ArrayBuffer::initialize_properties() {
    set_property("byteLength", Value(static_cast<double>(byte_length_)));
    set_property("maxByteLength", Value(static_cast<double>(max_byte_length_)));
    set_property("resizable", Value(is_resizable_));
}

Value ArrayBuffer::get_property(const std::string& key) const {
    if (key == "byteLength") {
        return Value(static_cast<double>(byte_length_));
    } else if (key == "maxByteLength") {
        return Value(static_cast<double>(max_byte_length_));
    } else if (key == "resizable") {
        return Value(is_resizable_);
    } else if (key == "_isArrayBuffer") {
        return Value(true);
    }
    
    return Object::get_property(key);
}

std::string ArrayBuffer::to_string() const {
    return "[object ArrayBuffer]";
}

void ArrayBuffer::mark_references() const {
}


Value ArrayBuffer::constructor(Context& ctx, const std::vector<Value>& args) {
    double length_double = 0.0;

    if (!args.empty()) {
        if (!args[0].is_number()) {
            ctx.throw_type_error("ArrayBuffer size must be a number");
            return Value();
        }
        length_double = args[0].as_number();
    }

    if (std::isnan(length_double) || std::isinf(length_double)) {
        ctx.throw_range_error("ArrayBuffer size must be a non-negative integer");
        return Value();
    }

    if (length_double < 0 || length_double != std::floor(length_double)) {
        ctx.throw_range_error("ArrayBuffer size must be a non-negative integer");
        return Value();
    }
    
    size_t byte_length = static_cast<size_t>(length_double);
    
    const size_t MAX_SAFE_SIZE = 1024 * 1024 * 1024;
    if (byte_length > MAX_SAFE_SIZE) {
        ctx.throw_range_error("ArrayBuffer size exceeds allowed size");
        return Value();
    }
    
    try {
        if (args.size() > 1 && args[1].is_object()) {
            Object* options = args[1].as_object();
            Value max_byte_length_val = options->get_property("maxByteLength");
            
            if (!max_byte_length_val.is_undefined()) {
                if (!max_byte_length_val.is_number()) {
                    ctx.throw_type_error("maxByteLength must be a number");
                    return Value();
                }
                
                size_t max_byte_length = static_cast<size_t>(max_byte_length_val.as_number());
                auto buffer = std::make_unique<ArrayBuffer>(byte_length, max_byte_length);
                return Value(buffer.release());
            }
        }
        
        auto buffer = std::make_unique<ArrayBuffer>(byte_length);
        return Value(buffer.release());
    } catch (const std::exception& e) {
        ctx.throw_error(std::string("ArrayBuffer allocation failed: ") + e.what());
        return Value();
    }
}

Value ArrayBuffer::prototype_slice(Context& ctx, const std::vector<Value>& args) {
    Value this_val = ctx.get_binding("this");
    if (!this_val.is_object()) {
        ctx.throw_error("ArrayBuffer.prototype.slice called on non-object");
        return Value();
    }

    Object* this_obj = this_val.as_object();

    Value byte_length_val = this_obj->get_property("byteLength");
    if (!byte_length_val.is_number()) {
        ctx.throw_error("Invalid ArrayBuffer");
        return Value();
    }

    size_t byte_length = static_cast<size_t>(byte_length_val.as_number());

    int32_t start = 0;
    if (!args.empty()) {
        double start_arg = args[0].to_number();
        start = start_arg < 0 ? std::max(0, static_cast<int32_t>(byte_length) + static_cast<int32_t>(start_arg))
                               : std::min(static_cast<size_t>(start_arg), byte_length);
    }

    int32_t end = byte_length;
    if (args.size() > 1 && !args[1].is_undefined()) {
        double end_arg = args[1].to_number();
        end = end_arg < 0 ? std::max(0, static_cast<int32_t>(byte_length) + static_cast<int32_t>(end_arg))
                          : std::min(static_cast<size_t>(end_arg), byte_length);
    }

    size_t new_length = std::max(0, end - start);

    auto new_buffer = std::make_unique<ArrayBuffer>(new_length);


    return Value(new_buffer.release());
}

Value ArrayBuffer::get_byteLength(Context& ctx, const std::vector<Value>& args) {
    (void)args;
    Value this_val = ctx.get_binding("this");
    if (!this_val.is_object()) {
        return Value(0.0);
    }

    Object* this_obj = this_val.as_object();

    if (this_obj->has_property("byteLength")) {
        return this_obj->get_property("byteLength");
    }

    return Value(0.0);
}

Value ArrayBuffer::isView(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(false);
    }
    
    if (!args[0].is_object()) {
        return Value(false);
    }
    
    return Value(false);
}


namespace ArrayBufferFactory {

std::unique_ptr<ArrayBuffer> create(size_t byte_length) {
    return std::make_unique<ArrayBuffer>(byte_length);
}

std::unique_ptr<ArrayBuffer> create_resizable(size_t byte_length, size_t max_byte_length) {
    return std::make_unique<ArrayBuffer>(byte_length, max_byte_length);
}

std::unique_ptr<ArrayBuffer> from_data(const uint8_t* data, size_t byte_length) {
    return std::make_unique<ArrayBuffer>(data, byte_length);
}

std::unique_ptr<ArrayBuffer> from_string(const std::string& str) {
    return std::make_unique<ArrayBuffer>(reinterpret_cast<const uint8_t*>(str.data()), str.length());
}

std::unique_ptr<ArrayBuffer> from_vector(const std::vector<uint8_t>& vec) {
    return std::make_unique<ArrayBuffer>(vec.data(), vec.size());
}

}


SharedArrayBuffer::SharedArrayBuffer(size_t byte_length) 
    : ArrayBuffer(byte_length) {
}

Value SharedArrayBuffer::constructor(Context& ctx, const std::vector<Value>& args) {
    return ArrayBuffer::constructor(ctx, args);
}

}
