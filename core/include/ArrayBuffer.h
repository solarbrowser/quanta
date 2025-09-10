/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ARRAY_BUFFER_H
#define QUANTA_ARRAY_BUFFER_H

#include "Object.h"
#include "Value.h"
#include <vector>
#include <memory>
#include <cstdint>

// Platform-specific includes for memory allocation
#ifdef _WIN32
    #include <malloc.h>
#else
    #include <cstdlib>
#endif

namespace Quanta {

// Forward declarations
class Context;

// Forward declaration for TypedArray base class (will be defined later)
class TypedArrayBase;

/**
 * ArrayBuffer implementation
 * Represents a fixed-length raw binary data buffer
 * 
 * Features:
 * - Efficient memory management with alignment
 * - Resizable buffer support (experimental)
 * - Shared buffer support for TypedArrays
 * - Memory protection and bounds checking
 * - Zero-copy operations where possible
 */
class ArrayBuffer : public Object {
private:
    // Raw buffer data - aligned for optimal performance
    std::unique_ptr<uint8_t[]> data_;
    size_t byte_length_;
    size_t max_byte_length_;    // For resizable buffers
    bool is_detached_;          // Buffer transfer state
    bool is_resizable_;         // Resizable buffer flag
    
    // Reference tracking for shared access
    std::vector<TypedArrayBase*> attached_views_;
    
    // Memory alignment for optimal performance
    static constexpr size_t DEFAULT_ALIGNMENT = 16;
    
public:
    // Constructors
    explicit ArrayBuffer(size_t byte_length);
    explicit ArrayBuffer(size_t byte_length, size_t max_byte_length); // Resizable
    ArrayBuffer(const uint8_t* source, size_t byte_length); // Copy from existing data
    
    // Destructor
    ~ArrayBuffer() override;
    
    // Core ArrayBuffer methods
    size_t byte_length() const { return is_detached_ ? 0 : byte_length_; }
    size_t max_byte_length() const { return max_byte_length_; }
    bool is_detached() const { return is_detached_; }
    bool is_resizable() const { return is_resizable_; }
    
    // Data access (with bounds checking)
    uint8_t* data() { return is_detached_ ? nullptr : data_.get(); }
    const uint8_t* data() const { return is_detached_ ? nullptr : data_.get(); }
    
    // Safe data access methods
    bool read_bytes(size_t offset, void* dest, size_t count) const;
    bool write_bytes(size_t offset, const void* src, size_t count);
    
    // Buffer operations
    std::unique_ptr<ArrayBuffer> slice(size_t start, size_t end = SIZE_MAX) const;
    bool resize(size_t new_byte_length); // For resizable buffers
    void detach(); // Transfer buffer ownership
    
    // Memory management
    static std::unique_ptr<ArrayBuffer> allocate(size_t byte_length);
    static std::unique_ptr<ArrayBuffer> allocate_resizable(size_t byte_length, size_t max_byte_length);
    
    // View management
    void register_view(TypedArrayBase* view);
    void unregister_view(TypedArrayBase* view);
    void detach_all_views();
    
    // JavaScript API methods
    static Value constructor(Context& ctx, const std::vector<Value>& args);
    static Value prototype_slice(Context& ctx, const std::vector<Value>& args);
    static Value prototype_resize(Context& ctx, const std::vector<Value>& args);
    static Value get_byteLength(Context& ctx, const std::vector<Value>& args);
    static Value get_maxByteLength(Context& ctx, const std::vector<Value>& args);
    static Value get_resizable(Context& ctx, const std::vector<Value>& args);
    
    // Static methods
    static Value isView(Context& ctx, const std::vector<Value>& args);
    
    // Property access override to fix broken property system
    Value get_property(const std::string& key) const override;
    
    // Utility methods
    std::string to_string() const;
    void mark_references() const;
    
    // Type checking
    bool is_array_buffer() const override { return true; }
    
private:
    // Internal helpers
    void allocate_buffer(size_t byte_length);
    static uint8_t* allocate_aligned(size_t size, size_t alignment = DEFAULT_ALIGNMENT);
    static void deallocate_aligned(uint8_t* ptr);
    bool check_bounds(size_t offset, size_t count) const;
    void initialize_properties();
};

/**
 * ArrayBuffer factory for creating optimized instances
 */
namespace ArrayBufferFactory {
    std::unique_ptr<ArrayBuffer> create(size_t byte_length);
    std::unique_ptr<ArrayBuffer> create_resizable(size_t byte_length, size_t max_byte_length);
    std::unique_ptr<ArrayBuffer> from_data(const uint8_t* data, size_t byte_length);
    std::unique_ptr<ArrayBuffer> from_string(const std::string& str);
    std::unique_ptr<ArrayBuffer> from_vector(const std::vector<uint8_t>& vec);
}

/**
 * SharedArrayBuffer implementation (for future Web Workers support)
 */
class SharedArrayBuffer : public ArrayBuffer {
private:
    // Shared memory implementation would go here
    // For now, inherits from ArrayBuffer
    
public:
    explicit SharedArrayBuffer(size_t byte_length);
    
    // SharedArrayBuffer-specific methods
    static Value constructor(Context& ctx, const std::vector<Value>& args);
    
    // Type checking
    bool is_shared_array_buffer() const override { return true; }
};

} // namespace Quanta

#endif // QUANTA_ARRAY_BUFFER_H