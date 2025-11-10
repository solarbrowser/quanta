/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "../include/ArrayBuffer.h"
#include "../include/Context.h"
#include <algorithm>
#include <new>
#include <stdexcept>

// Platform-specific includes for memory allocation
#ifdef _WIN32
    #include <malloc.h>
#else
    #include <cstdlib>
#endif

// Manual memory functions to avoid linkage issues
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

//=============================================================================
// ArrayBuffer Implementation
//=============================================================================

ArrayBuffer::ArrayBuffer(size_t byte_length)
    : Object(ObjectType::ArrayBuffer), byte_length_(byte_length), 
      max_byte_length_(byte_length), is_detached_(false), is_resizable_(false) {
    allocate_buffer(byte_length);
    // initialize_properties(); // Disabled - properties set in Context.cpp lambda
}

ArrayBuffer::ArrayBuffer(size_t byte_length, size_t max_byte_length)
    : Object(ObjectType::ArrayBuffer), byte_length_(byte_length),
      max_byte_length_(max_byte_length), is_detached_(false), is_resizable_(true) {
    if (byte_length > max_byte_length) {
        throw std::invalid_argument("byte_length cannot exceed max_byte_length");
    }
    allocate_buffer(max_byte_length); // Allocate maximum size
    // initialize_properties(); // Disabled - properties set in Context.cpp lambda
}

ArrayBuffer::ArrayBuffer(const uint8_t* source, size_t byte_length)
    : Object(ObjectType::ArrayBuffer), byte_length_(byte_length),
      max_byte_length_(byte_length), is_detached_(false), is_resizable_(false) {
    allocate_buffer(byte_length);
    if (source && data_) {
        quanta_memcpy(data_.get(), source, byte_length);
    }
    // initialize_properties(); // Disabled - properties set in Context.cpp lambda
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
        // Allocate aligned memory for optimal performance
        uint8_t* raw_ptr = allocate_aligned(byte_length);
        data_ = std::unique_ptr<uint8_t[]>(raw_ptr);
        
        // Zero-initialize the buffer
        quanta_memset(data_.get(), 0, byte_length);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("ArrayBuffer allocation failed: out of memory");
    }
}

uint8_t* ArrayBuffer::allocate_aligned(size_t size, size_t alignment) {
    #ifdef _WIN32
        // Windows aligned allocation
        void* ptr = _aligned_malloc(size, alignment);
        if (!ptr) throw std::bad_alloc();
        return static_cast<uint8_t*>(ptr);
    #else
        // POSIX aligned allocation
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
    
    // Check for overflow
    if (offset + count < offset || offset + count > byte_length_) {
        return false;
    }
    
    return true;
}

std::unique_ptr<ArrayBuffer> ArrayBuffer::slice(size_t start, size_t end) const {
    if (is_detached_) {
        return nullptr;
    }
    
    // Handle negative indices and defaults
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
    
    // For simplicity, we don't actually resize the underlying buffer
    // Just update the logical length (real implementation would optimize this)
    byte_length_ = new_byte_length;
    
    // Update property
    set_property("byteLength", Value(static_cast<double>(byte_length_)));
    
    return true;
}

void ArrayBuffer::detach() {
    if (is_detached_) {
        return;
    }
    
    is_detached_ = true;
    detach_all_views();
    
    // Update properties
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
    // In a full implementation, this would notify all TypedArray views
    // that their buffer has been detached
    attached_views_.clear();
}

void ArrayBuffer::initialize_properties() {
    // Set up ArrayBuffer properties
    set_property("byteLength", Value(static_cast<double>(byte_length_)));
    set_property("maxByteLength", Value(static_cast<double>(max_byte_length_)));
    set_property("resizable", Value(is_resizable_));
}

Value ArrayBuffer::get_property(const std::string& key) const {
    // Override property access to return correct values from C++ members
    if (key == "byteLength") {
        return Value(static_cast<double>(byte_length_));
    } else if (key == "maxByteLength") {
        return Value(static_cast<double>(max_byte_length_));
    } else if (key == "resizable") {
        return Value(is_resizable_);
    } else if (key == "_isArrayBuffer") {
        return Value(true);
    }
    
    // Fall back to base class property access for other properties
    return Object::get_property(key);
}

std::string ArrayBuffer::to_string() const {
    return "[object ArrayBuffer]";
}

void ArrayBuffer::mark_references() const {
    // For now, just mark the base object properties
    // In a full GC implementation, this would mark references for garbage collection
    // Object::mark_references(); // Base class method not implemented
}

//=============================================================================
// JavaScript API Methods
//=============================================================================

Value ArrayBuffer::constructor(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        ctx.throw_type_error("ArrayBuffer constructor requires at least one argument");
        return Value();
    }
    
    if (!args[0].is_number()) {
        ctx.throw_type_error("ArrayBuffer size must be a number");
        return Value();
    }
    
    double length_double = args[0].as_number();
    if (length_double < 0 || length_double != std::floor(length_double)) {
        ctx.throw_range_error("ArrayBuffer size must be a non-negative integer");
        return Value();
    }
    
    size_t byte_length = static_cast<size_t>(length_double);
    
    // Check for maximum safe size (1GB for now)
    const size_t MAX_SAFE_SIZE = 1024 * 1024 * 1024;
    if (byte_length > MAX_SAFE_SIZE) {
        ctx.throw_range_error("ArrayBuffer size exceeds maximum allowed size");
        return Value();
    }
    
    try {
        // Handle resizable ArrayBuffer (experimental)
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
        
        // Standard fixed-size ArrayBuffer
        auto buffer = std::make_unique<ArrayBuffer>(byte_length);
        return Value(buffer.release());
    } catch (const std::exception& e) {
        ctx.throw_error(std::string("ArrayBuffer allocation failed: ") + e.what());
        return Value();
    }
}

Value ArrayBuffer::prototype_slice(Context& ctx, const std::vector<Value>& args) {
    // TODO: Implement proper this binding check
    // For now, assume 'this' is an ArrayBuffer
    return Value(); // Placeholder
}

Value ArrayBuffer::get_byteLength(Context& ctx, const std::vector<Value>& args) {
    // TODO: Implement proper this binding and property access
    return Value(); // Placeholder
}

Value ArrayBuffer::isView(Context& ctx, const std::vector<Value>& args) {
    if (args.empty()) {
        return Value(false);
    }
    
    if (!args[0].is_object()) {
        return Value(false);
    }
    
    // For now, return false to avoid segfault
    // TODO: Implement proper TypedArray detection when TypedArrays are implemented
    return Value(false);
}

//=============================================================================
// ArrayBufferFactory Implementation
//=============================================================================

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

} // namespace ArrayBufferFactory

//=============================================================================
// SharedArrayBuffer Implementation
//=============================================================================

SharedArrayBuffer::SharedArrayBuffer(size_t byte_length) 
    : ArrayBuffer(byte_length) {
    // Override the object type
    // TODO: This would need proper architecture changes to support ObjectType changes post-construction
}

Value SharedArrayBuffer::constructor(Context& ctx, const std::vector<Value>& args) {
    // For now, just create a regular ArrayBuffer
    // Full SharedArrayBuffer implementation would require threading support
    return ArrayBuffer::constructor(ctx, args);
}

} // namespace Quanta