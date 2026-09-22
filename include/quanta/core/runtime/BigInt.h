/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <utility>

namespace Quanta {

// digits_'s own storage: a std::vector<uint32_t> put a heap allocation behind
// every single BigInt value, even `1n`, even a loop counter that never leaves
// one 32-bit digit -- two allocations (this vector's buffer and the BigInt
// cell itself) for every arithmetic result, freed again a moment later, which
// measured as the dominant cost of ordinary BigInt code (bitwise ops and
// small counters, not cryptography). Two digits -- up to a 64-bit magnitude,
// which covers a loop counter, a hash, an id, anything that fits in what
// `1n << 64n` does not -- live inline in the BigInt cell itself; only a
// magnitude past that spills to the heap, exactly where a vector always was.
// The public surface is the handful of std::vector operations BigInt.cpp
// actually calls, not a general-purpose container.
class SmallDigits {
public:
    static constexpr size_t kInline = 2;

    SmallDigits() = default;
    ~SmallDigits() { delete[] heap_; }

    SmallDigits(const SmallDigits& o) { assign_from(o.data(), o.size_); }
    SmallDigits(SmallDigits&& o) noexcept {
        if (o.heap_) {
            heap_ = o.heap_; capacity_ = o.capacity_; size_ = o.size_;
            o.heap_ = nullptr; o.size_ = 0; o.capacity_ = kInline;
        } else {
            std::memcpy(inline_, o.inline_, o.size_ * sizeof(uint32_t));
            size_ = o.size_;
        }
    }
    SmallDigits& operator=(const SmallDigits& o) {
        if (this != &o) assign_from(o.data(), o.size_);
        return *this;
    }
    SmallDigits& operator=(SmallDigits&& o) noexcept {
        if (this != &o) {
            delete[] heap_;
            heap_ = nullptr; capacity_ = kInline; size_ = 0;
            if (o.heap_) {
                heap_ = o.heap_; capacity_ = o.capacity_; size_ = o.size_;
                o.heap_ = nullptr; o.size_ = 0; o.capacity_ = kInline;
            } else {
                std::memcpy(inline_, o.inline_, o.size_ * sizeof(uint32_t));
                size_ = o.size_;
            }
        }
        return *this;
    }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    uint32_t& operator[](size_t i) { return data()[i]; }
    uint32_t operator[](size_t i) const { return data()[i]; }
    uint32_t& back() { return data()[size_ - 1]; }
    uint32_t back() const { return data()[size_ - 1]; }
    uint32_t* begin() { return data(); }
    uint32_t* end() { return data() + size_; }
    const uint32_t* begin() const { return data(); }
    const uint32_t* end() const { return data() + size_; }

    bool operator==(const SmallDigits& o) const {
        return size_ == o.size_ && std::memcmp(data(), o.data(), size_ * sizeof(uint32_t)) == 0;
    }

    void clear() { size_ = 0; }
    void pop_back() { --size_; }
    void push_back(uint32_t v) {
        if (size_ == capacity_) grow(capacity_ + 1);
        data()[size_++] = v;
    }
    void resize(size_t n, uint32_t v = 0) {
        if (n > capacity_) grow(n);
        for (size_t i = size_; i < n; i++) data()[i] = v;
        size_ = static_cast<uint32_t>(n);
    }
    // std::vector::assign(count, value): the previous content is discarded
    // outright, so unlike resize/push_back a spill here need not preserve it.
    void assign(size_t n, uint32_t v) {
        if (n > capacity_) {
            delete[] heap_;
            heap_ = new uint32_t[n];
            capacity_ = static_cast<uint32_t>(n);
        }
        uint32_t* d = data();
        for (size_t i = 0; i < n; i++) d[i] = v;
        size_ = static_cast<uint32_t>(n);
    }

private:
    uint32_t inline_[kInline] = {};
    uint32_t* heap_ = nullptr;
    uint32_t size_ = 0;
    uint32_t capacity_ = kInline;

    uint32_t* data() { return heap_ ? heap_ : inline_; }
    const uint32_t* data() const { return heap_ ? heap_ : inline_; }

    // Grows to hold at least `min_cap`, preserving the first size_ elements --
    // needed for resize()/push_back(), and harmless (just a few wasted copies
    // of about-to-be-overwritten data) for assign()'s own larger-capacity path.
    void grow(size_t min_cap) {
        size_t new_cap = capacity_ * 2;
        if (new_cap < min_cap) new_cap = min_cap;
        uint32_t* fresh = new uint32_t[new_cap];
        std::memcpy(fresh, data(), size_ * sizeof(uint32_t));
        delete[] heap_;
        heap_ = fresh;
        capacity_ = static_cast<uint32_t>(new_cap);
    }
    void assign_from(const uint32_t* src, size_t n) {
        if (n > capacity_) {
            delete[] heap_;
            heap_ = new uint32_t[n];
            capacity_ = static_cast<uint32_t>(n);
        } else if (heap_ && n <= kInline) {
            // Shrinking back into what the inline slots can hold: give up the
            // heap buffer rather than carry it for a value that no longer
            // needs it (a subtraction or a shift can shrink a result this way).
            delete[] heap_;
            heap_ = nullptr;
            capacity_ = kInline;
        }
        std::memcpy(data(), src, n * sizeof(uint32_t));
        size_ = static_cast<uint32_t>(n);
    }
};

class BigInt {
private:
    SmallDigits digits_;  // Little-endian: digits_[0] is least significant
    bool is_negative_;
    
    void normalize();
    void add_positive(const BigInt& other);
    void subtract_positive(const BigInt& other);
    void multiply_positive(const BigInt& other);
    int compare_absolute(const BigInt& other) const;

public:
    // GC cell protocol (see Object.h): only heap BigInts (the ones Values
    // point at) become cells; by-value temporaries stay on the stack.
    static void* operator new(size_t size);
    static void  operator delete(void* p) noexcept;
    static void* operator new[](size_t) = delete;
    static void  operator delete[](void*) = delete;

    BigInt();
    BigInt(int64_t value);
    BigInt(const std::string& str);
    BigInt(const BigInt& other) = default;
    BigInt(BigInt&& other) = default;
    
    BigInt& operator=(const BigInt& other) = default;
    BigInt& operator=(BigInt&& other) = default;
    
    BigInt operator+(const BigInt& other) const;
    BigInt operator-(const BigInt& other) const;
    BigInt operator*(const BigInt& other) const;
    BigInt operator/(const BigInt& other) const;
    BigInt operator%(const BigInt& other) const;
    
    BigInt& operator+=(const BigInt& other);
    BigInt& operator-=(const BigInt& other);
    BigInt& operator*=(const BigInt& other);
    BigInt& operator/=(const BigInt& other);
    BigInt& operator%=(const BigInt& other);
    
    BigInt operator-() const;
    BigInt operator+() const;
    
    bool operator==(const BigInt& other) const;
    bool operator!=(const BigInt& other) const;
    bool operator<(const BigInt& other) const;
    bool operator<=(const BigInt& other) const;
    bool operator>(const BigInt& other) const;
    bool operator>=(const BigInt& other) const;
    
    std::string to_string() const;
    double to_double() const;
    int64_t to_int64() const;
    bool to_boolean() const;
    
    bool is_zero() const;
    bool is_negative() const { return is_negative_; }
    
    static BigInt from_string(const std::string& str);
    // Exact conversion of a finite integral double (mantissa shift, no int64 overflow).
    static BigInt from_integral_double(double d);
    static BigInt pow(const BigInt& base, const BigInt& exponent);

    BigInt bitwise_not() const;
    BigInt bitwise_and(const BigInt& other) const;
    BigInt bitwise_or(const BigInt& other) const;
    BigInt bitwise_xor(const BigInt& other) const;
    BigInt left_shift(const BigInt& n) const;
    BigInt right_shift(const BigInt& n) const;
};

}
