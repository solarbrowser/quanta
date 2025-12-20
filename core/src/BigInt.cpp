/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/BigInt.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <climits>

namespace Quanta {


BigInt::BigInt() : is_negative_(false) {
    digits_.push_back(0);
}

BigInt::BigInt(int64_t value) : is_negative_(value < 0) {
    uint64_t abs_value = is_negative_ ? -value : value;
    
    if (abs_value == 0) {
        digits_.push_back(0);
        is_negative_ = false;
    } else {
        while (abs_value > 0) {
            digits_.push_back(abs_value & 0xFFFFFFFF);
            abs_value >>= 32;
        }
    }
}

BigInt::BigInt(const std::string& str) : is_negative_(false) {
    if (str.empty()) {
        digits_.push_back(0);
        return;
    }
    
    size_t start = 0;
    if (str[0] == '-') {
        is_negative_ = true;
        start = 1;
    } else if (str[0] == '+') {
        start = 1;
    }
    
    if (start >= str.length()) {
        digits_.push_back(0);
        return;
    }

    int base = 10;
    if (start + 1 < str.length() && str[start] == '0') {
        char prefix = str[start + 1];
        if (prefix == 'x' || prefix == 'X') {
            base = 16;
            start += 2;
        } else if (prefix == 'o' || prefix == 'O') {
            base = 8;
            start += 2;
        } else if (prefix == 'b' || prefix == 'B') {
            base = 2;
            start += 2;
        }
    }

    digits_.push_back(0);

    for (size_t i = start; i < str.length(); ++i) {
        char c = str[i];

        if (c == '_') {
            continue;
        }

        int digit_value;
        if (c >= '0' && c <= '9') {
            digit_value = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit_value = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit_value = c - 'A' + 10;
        } else {
            throw std::invalid_argument("Invalid BigInt string: " + str);
        }

        if (digit_value >= base) {
            throw std::invalid_argument("Invalid BigInt string: " + str);
        }

        uint64_t carry = digit_value;
        for (auto& digit : digits_) {
            uint64_t temp = static_cast<uint64_t>(digit) * base + carry;
            digit = temp & 0xFFFFFFFF;
            carry = temp >> 32;
        }

        while (carry > 0) {
            digits_.push_back(static_cast<uint32_t>(carry & 0xFFFFFFFFULL));
            carry = carry >> 32;
        }
    }
    
    normalize();
}


void BigInt::normalize() {
    while (digits_.size() > 1 && digits_.back() == 0) {
        digits_.pop_back();
    }
    
    if (digits_.size() == 1 && digits_[0] == 0) {
        is_negative_ = false;
    }
}

int BigInt::compare_absolute(const BigInt& other) const {
    if (digits_.size() != other.digits_.size()) {
        return digits_.size() < other.digits_.size() ? -1 : 1;
    }
    
    for (size_t i = digits_.size(); i > 0; --i) {
        if (digits_[i-1] != other.digits_[i-1]) {
            return digits_[i-1] < other.digits_[i-1] ? -1 : 1;
        }
    }
    
    return 0;
}


BigInt BigInt::operator+(const BigInt& other) const {
    BigInt result = *this;
    result += other;
    return result;
}

BigInt& BigInt::operator+=(const BigInt& other) {
    if (is_negative_ == other.is_negative_) {
        add_positive(other);
    } else {
        int cmp = compare_absolute(other);
        if (cmp == 0) {
            digits_.clear();
            digits_.push_back(0);
            is_negative_ = false;
        } else if (cmp > 0) {
            subtract_positive(other);
        } else {
            BigInt temp = other;
            temp.subtract_positive(*this);
            *this = std::move(temp);
        }
    }
    return *this;
}

void BigInt::add_positive(const BigInt& other) {
    uint32_t carry = 0;
    size_t max_size = std::max(digits_.size(), other.digits_.size());
    
    digits_.resize(max_size, 0);
    
    for (size_t i = 0; i < max_size; ++i) {
        uint64_t sum = static_cast<uint64_t>(digits_[i]) + carry;
        if (i < other.digits_.size()) {
            sum += other.digits_[i];
        }
        
        digits_[i] = sum & 0xFFFFFFFF;
        carry = sum >> 32;
    }
    
    if (carry > 0) {
        digits_.push_back(carry);
    }
}

BigInt BigInt::operator-(const BigInt& other) const {
    BigInt result = *this;
    result -= other;
    return result;
}

BigInt& BigInt::operator-=(const BigInt& other) {
    return *this += (-other);
}

void BigInt::subtract_positive(const BigInt& other) {
    uint32_t borrow = 0;
    
    for (size_t i = 0; i < digits_.size(); ++i) {
        uint64_t sub = borrow;
        if (i < other.digits_.size()) {
            sub += other.digits_[i];
        }
        
        if (digits_[i] >= sub) {
            digits_[i] -= sub;
            borrow = 0;
        } else {
            digits_[i] = (0x100000000ULL + digits_[i]) - sub;
            borrow = 1;
        }
    }
    
    normalize();
}

BigInt BigInt::operator*(const BigInt& other) const {
    BigInt result;
    result.digits_.clear();
    result.digits_.resize(digits_.size() + other.digits_.size(), 0);
    result.is_negative_ = is_negative_ != other.is_negative_;
    
    for (size_t i = 0; i < digits_.size(); ++i) {
        uint32_t carry = 0;
        for (size_t j = 0; j < other.digits_.size(); ++j) {
            uint64_t prod = static_cast<uint64_t>(digits_[i]) * other.digits_[j] + 
                           result.digits_[i + j] + carry;
            result.digits_[i + j] = prod & 0xFFFFFFFF;
            carry = prod >> 32;
        }
        result.digits_[i + other.digits_.size()] += carry;
    }
    
    result.normalize();
    return result;
}


BigInt BigInt::operator-() const {
    if (is_zero()) return *this;
    
    BigInt result = *this;
    result.is_negative_ = !is_negative_;
    return result;
}

BigInt BigInt::operator+() const {
    return *this;
}


bool BigInt::operator==(const BigInt& other) const {
    return is_negative_ == other.is_negative_ && digits_ == other.digits_;
}

bool BigInt::operator!=(const BigInt& other) const {
    return !(*this == other);
}

bool BigInt::operator<(const BigInt& other) const {
    if (is_negative_ != other.is_negative_) {
        return is_negative_ && !other.is_negative_;
    }
    
    int cmp = compare_absolute(other);
    return is_negative_ ? cmp > 0 : cmp < 0;
}

bool BigInt::operator<=(const BigInt& other) const {
    return *this < other || *this == other;
}

bool BigInt::operator>(const BigInt& other) const {
    return !(*this <= other);
}

bool BigInt::operator>=(const BigInt& other) const {
    return !(*this < other);
}


std::string BigInt::to_string() const {
    if (is_zero()) return "0";
    
    std::string result;
    BigInt temp = *this;
    temp.is_negative_ = false;
    
    while (!temp.is_zero()) {
        uint32_t remainder = 0;
        for (size_t i = temp.digits_.size(); i > 0; --i) {
            uint64_t dividend = (static_cast<uint64_t>(remainder) << 32) + temp.digits_[i-1];
            temp.digits_[i-1] = dividend / 10;
            remainder = dividend % 10;
        }
        
        result = char('0' + remainder) + result;
        temp.normalize();
    }
    
    if (is_negative_) {
        result = "-" + result;
    }
    
    return result;
}

double BigInt::to_double() const {
    if (is_zero()) return 0.0;
    
    double result = 0.0;
    double base = 1.0;
    
    for (uint32_t digit : digits_) {
        result += digit * base;
        base *= 4294967296.0;
    }
    
    return is_negative_ ? -result : result;
}

bool BigInt::to_boolean() const {
    return !is_zero();
}

bool BigInt::is_zero() const {
    return digits_.size() == 1 && digits_[0] == 0;
}


BigInt BigInt::from_string(const std::string& str) {
    return BigInt(str);
}

}
