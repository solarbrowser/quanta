/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace Quanta {

/**
 * BigInt implementation for arbitrarily large integers
 * Supports basic arithmetic operations and comparisons
 */
class BigInt {
private:
    std::vector<uint32_t> digits_;  // Little-endian: digits_[0] is least significant
    bool is_negative_;
    
    // Helper methods
    void normalize();
    void add_positive(const BigInt& other);
    void subtract_positive(const BigInt& other);
    void multiply_positive(const BigInt& other);
    int compare_absolute(const BigInt& other) const;

public:
    // Constructors
    BigInt();
    BigInt(int64_t value);
    BigInt(const std::string& str);
    BigInt(const BigInt& other) = default;
    BigInt(BigInt&& other) = default;
    
    // Assignment
    BigInt& operator=(const BigInt& other) = default;
    BigInt& operator=(BigInt&& other) = default;
    
    // Arithmetic operations
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
    
    // Unary operations
    BigInt operator-() const;
    BigInt operator+() const;
    
    // Comparison operations
    bool operator==(const BigInt& other) const;
    bool operator!=(const BigInt& other) const;
    bool operator<(const BigInt& other) const;
    bool operator<=(const BigInt& other) const;
    bool operator>(const BigInt& other) const;
    bool operator>=(const BigInt& other) const;
    
    // Conversion
    std::string to_string() const;
    double to_double() const;
    int64_t to_int64() const;
    bool to_boolean() const;
    
    // Utility
    bool is_zero() const;
    bool is_negative() const { return is_negative_; }
    
    // Static methods
    static BigInt from_string(const std::string& str);
    static BigInt pow(const BigInt& base, const BigInt& exponent);
};

} // namespace Quanta