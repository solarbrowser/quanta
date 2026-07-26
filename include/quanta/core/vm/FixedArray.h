/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_FIXED_ARRAY_H
#define QUANTA_VM_FIXED_ARRAY_H

#include <cstdint>
#include <memory>
#include <vector>

namespace Quanta {

// Immutable-length pointer+count array, replacing std::vector's 24-byte
// header (ptr+size+capacity) with 16 (ptr+count) for fields that are built
// once (typically by BytecodeCompiler, via a std::vector builder) and never
// resized again after from() freezes them. Element CONTENTS may still
// mutate in place (see BytecodeChunk::feedback) -- only the length is fixed.
//
// operator[]/data()/begin()/end() are const-qualified but return non-const
// T*/T& -- same as std::unique_ptr<T[]>::operator[], which this wraps: a
// unique_ptr's constness never propagates to what it points to. This means,
// unlike the old `mutable std::vector<FeedbackSlot> feedback`, no `mutable`
// is needed to mutate elements through a const BytecodeChunk&.
template <typename T>
class FixedArray {
public:
    FixedArray() = default;

    static FixedArray from(std::vector<T> v) {
        FixedArray arr;
        arr.count_ = static_cast<uint32_t>(v.size());
        if (arr.count_ > 0) {
            arr.data_ = std::make_unique<T[]>(arr.count_);
            for (uint32_t i = 0; i < arr.count_; i++) {
                arr.data_[i] = std::move(v[i]);
            }
        }
        return arr;
    }

    // std::vector::assign(count, value)'s equivalent -- every element starts
    // as a copy of `value` (default-constructed T{} if omitted).
    static FixedArray filled(uint32_t count, const T& value = T{}) {
        FixedArray arr;
        arr.count_ = count;
        if (count > 0) {
            arr.data_ = std::make_unique<T[]>(count);
            for (uint32_t i = 0; i < count; i++) arr.data_[i] = value;
        }
        return arr;
    }

    uint32_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    T* data() const { return data_.get(); }
    T& operator[](size_t i) const { return data_[i]; }
    T* begin() const { return data_.get(); }
    T* end() const { return data_.get() + count_; }

private:
    std::unique_ptr<T[]> data_;
    uint32_t count_ = 0;
};

}  // namespace Quanta

#endif
