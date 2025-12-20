/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ARRAY_BUFFER_H
#define QUANTA_ARRAY_BUFFER_H

#include "quanta/Object.h"
#include "quanta/Value.h"
#include <vector>
#include <memory>
#include <cstdint>

#ifdef _WIN32
    #include <malloc.h>
#else
    #include <cstdlib>
#endif

namespace Quanta {

class Context;

class TypedArrayBase;

class ArrayBuffer : public Object {
private:
    std::unique_ptr<uint8_t[]> data_;
    size_t byte_length_;
    size_t max_byte_length_;
    bool is_detached_;
    bool is_resizable_;
    
    std::vector<TypedArrayBase*> attached_views_;
    
    static constexpr size_t DEFAULT_ALIGNMENT = 16;
    
public:
    explicit ArrayBuffer(size_t byte_length);
    explicit ArrayBuffer(size_t byte_length, size_t max_byte_length);
    ArrayBuffer(const uint8_t* source, size_t byte_length);
    
    ~ArrayBuffer() override;
    
    size_t byte_length() const { return is_detached_ ? 0 : byte_length_; }
    size_t max_byte_length() const { return max_byte_length_; }
    bool is_detached() const { return is_detached_; }
    bool is_resizable() const { return is_resizable_; }
    
    uint8_t* data() { return is_detached_ ? nullptr : data_.get(); }
    const uint8_t* data() const { return is_detached_ ? nullptr : data_.get(); }
    
    bool read_bytes(size_t offset, void* dest, size_t count) const;
    bool write_bytes(size_t offset, const void* src, size_t count);
    
    std::unique_ptr<ArrayBuffer> slice(size_t start, size_t end = SIZE_MAX) const;
    bool resize(size_t new_byte_length);
    void detach();
    
    static std::unique_ptr<ArrayBuffer> allocate(size_t byte_length);
    static std::unique_ptr<ArrayBuffer> allocate_resizable(size_t byte_length, size_t max_byte_length);
    
    void register_view(TypedArrayBase* view);
    void unregister_view(TypedArrayBase* view);
    void detach_all_views();
    
    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value prototype_slice(Context& ctx, const std::vector<Value>& args);
    static Value prototype_resize(Context& ctx, const std::vector<Value>& args);
    static Value get_byteLength(Context& ctx, const std::vector<Value>& args);
    static Value get_maxByteLength(Context& ctx, const std::vector<Value>& args);
    static Value get_resizable(Context& ctx, const std::vector<Value>& args);
    
    static Value isView(Context& ctx, const std::vector<Value>& args);
    
    Value get_property(const std::string& key) const override;
    
    std::string to_string() const;
    void mark_references() const;
    
    bool is_array_buffer() const override { return true; }
    
private:
    void allocate_buffer(size_t byte_length);
    static uint8_t* allocate_aligned(size_t size, size_t alignment = DEFAULT_ALIGNMENT);
    static void deallocate_aligned(uint8_t* ptr);
    bool check_bounds(size_t offset, size_t count) const;
    void initialize_properties();
};

/**
 * ArrayBuffer factory for creating instances
 */
namespace ArrayBufferFactory {
    std::unique_ptr<ArrayBuffer> create(size_t byte_length);
    std::unique_ptr<ArrayBuffer> create_resizable(size_t byte_length, size_t max_byte_length);
    std::unique_ptr<ArrayBuffer> from_data(const uint8_t* data, size_t byte_length);
    std::unique_ptr<ArrayBuffer> from_string(const std::string& str);
    std::unique_ptr<ArrayBuffer> from_vector(const std::vector<uint8_t>& vec);
}


class SharedArrayBuffer : public ArrayBuffer {
private:
    
public:
    explicit SharedArrayBuffer(size_t byte_length);
    
    static Value constructor(Context& ctx, const std::vector<Value>& args);
    
    bool is_shared_array_buffer() const override { return true; }
};

}

#endif
