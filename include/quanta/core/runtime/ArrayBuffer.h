/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_ARRAY_BUFFER_H
#define QUANTA_ARRAY_BUFFER_H

#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Value.h"
#include <atomic>
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
public:
    // Refcounted byte storage, decoupled from the GC-cell wrapper. A
    // SharedArrayBuffer's store is referenced by wrapper cells in several
    // agents at once (each agent's heap owns its own wrapper, threads never
    // share cells); byte_length is atomic so grow() in one agent is visible
    // in every other. Growable stores allocate max_byte_length up front and
    // zero it, so growing never moves or exposes stale bytes.
    struct BackingStore {
        uint8_t* data = nullptr;
        std::atomic<size_t> byte_length{0};
        size_t max_byte_length = 0;
        bool growable = false;
        ~BackingStore();
    };

private:
    std::shared_ptr<BackingStore> store_;
    bool is_detached_;
    bool is_resizable_;

    std::vector<TypedArrayBase*> attached_views_;

    static constexpr size_t DEFAULT_ALIGNMENT = 16;

public:
    explicit ArrayBuffer(size_t byte_length);
    explicit ArrayBuffer(size_t byte_length, size_t max_byte_length);
    ArrayBuffer(const uint8_t* source, size_t byte_length);
    explicit ArrayBuffer(std::shared_ptr<BackingStore> store);

    ~ArrayBuffer() override;

    size_t byte_length() const {
        return (is_detached_ || !store_) ? 0 : store_->byte_length.load(std::memory_order_seq_cst);
    }
    size_t max_byte_length() const { return store_ ? store_->max_byte_length : 0; }
    bool is_detached() const { return is_detached_; }
    bool is_resizable() const { return is_resizable_; }

    uint8_t* data() { return (is_detached_ || !store_) ? nullptr : store_->data; }
    const uint8_t* data() const { return (is_detached_ || !store_) ? nullptr : store_->data; }

    const std::shared_ptr<BackingStore>& backing_store() const { return store_; }
    
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
    static std::shared_ptr<BackingStore> make_store(size_t byte_length,
                                                    size_t max_byte_length,
                                                    bool growable);
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
public:
    explicit SharedArrayBuffer(size_t byte_length);
    SharedArrayBuffer(size_t byte_length, size_t max_byte_length);
    // Wraps an existing store: how a second agent receives a SAB. The new
    // wrapper is a cell of the receiving agent's heap; only the store is shared.
    explicit SharedArrayBuffer(std::shared_ptr<BackingStore> store);

    // Monotonic: false when new_byte_length is below the current length
    // (spec: grow never shrinks), even if another agent grows concurrently.
    bool grow(size_t new_byte_length);

    bool is_shared_array_buffer() const override { return true; }
};

}

#endif
