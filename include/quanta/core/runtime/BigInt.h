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



class BigInt {
private:
    std::vector<uint32_t> digits_;  // Little-endian: digits_[0] is least significant
    bool is_negative_;
    
    void normalize();
    void add_positive(const BigInt& other);
    void subtract_positive(const BigInt& other);
    void multiply_positive(const BigInt& other);
    int compare_absolute(const BigInt& other) const;

public:
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
    static BigInt pow(const BigInt& base, const BigInt& exponent);
};

}
