/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/BigInt.h"
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


BigInt BigInt::operator/(const BigInt& other) const {
    bool result_negative = is_negative_ != other.is_negative_;

    BigInt abs_a(*this); abs_a.is_negative_ = false;
    BigInt abs_b(other); abs_b.is_negative_ = false;

    int cmp = abs_a.compare_absolute(abs_b);
    if (cmp < 0) return BigInt(0);
    if (cmp == 0) {
        BigInt one(1); one.is_negative_ = result_negative; return one;
    }

    size_t total_bits = abs_a.digits_.size() * 32;
    BigInt quotient(0);
    quotient.digits_.assign(abs_a.digits_.size(), 0);
    BigInt remainder(0);

    for (size_t i = total_bits; i > 0; --i) {
        size_t bit = i - 1;
        uint32_t carry = 0;
        for (auto& d : remainder.digits_) {
            uint64_t val = (uint64_t)d * 2 + carry;
            d = (uint32_t)(val & 0xFFFFFFFF);
            carry = (uint32_t)(val >> 32);
        }
        if (carry) remainder.digits_.push_back(carry);
        uint32_t a_bit = (abs_a.digits_[bit / 32] >> (bit % 32)) & 1;
        remainder.digits_[0] |= a_bit;

        if (remainder.compare_absolute(abs_b) >= 0) {
            remainder.subtract_positive(abs_b);
            quotient.digits_[bit / 32] |= (1u << (bit % 32));
        }
    }

    quotient.normalize();
    if (result_negative && !quotient.is_zero()) quotient.is_negative_ = true;
    return quotient;
}

BigInt BigInt::operator%(const BigInt& other) const {
    BigInt abs_a(*this); abs_a.is_negative_ = false;
    BigInt abs_b(other); abs_b.is_negative_ = false;

    int cmp = abs_a.compare_absolute(abs_b);
    if (cmp < 0) {
        BigInt r(*this);
        return r;
    }
    if (cmp == 0) return BigInt(0);

    // remainder = a - (a / b) * b using the same bit-shift loop
    size_t total_bits = abs_a.digits_.size() * 32;
    BigInt remainder(0);

    for (size_t i = total_bits; i > 0; --i) {
        size_t bit = i - 1;
        uint32_t carry = 0;
        for (auto& d : remainder.digits_) {
            uint64_t val = (uint64_t)d * 2 + carry;
            d = (uint32_t)(val & 0xFFFFFFFF);
            carry = (uint32_t)(val >> 32);
        }
        if (carry) remainder.digits_.push_back(carry);
        uint32_t a_bit = (abs_a.digits_[bit / 32] >> (bit % 32)) & 1;
        remainder.digits_[0] |= a_bit;

        if (remainder.compare_absolute(abs_b) >= 0) {
            remainder.subtract_positive(abs_b);
        }
    }

    remainder.normalize();
    if (is_negative_ && !remainder.is_zero()) remainder.is_negative_ = true;
    return remainder;
}

BigInt& BigInt::operator/=(const BigInt& other) {
    *this = *this / other;
    return *this;
}

BigInt& BigInt::operator%=(const BigInt& other) {
    *this = *this % other;
    return *this;
}

BigInt BigInt::pow(const BigInt& base, const BigInt& exp) {
    if (exp.is_zero()) return BigInt(1);
    if (base.is_zero()) return BigInt(0);

    size_t highest_bit = 0;
    for (size_t i = exp.digits_.size() * 32; i > 0; --i) {
        if ((exp.digits_[(i-1) / 32] >> ((i-1) % 32)) & 1) {
            highest_bit = i - 1;
            break;
        }
    }

    BigInt result(1);
    BigInt b = base;
    for (size_t i = 0; i <= highest_bit; ++i) {
        if ((exp.digits_[i / 32] >> (i % 32)) & 1) {
            result = result * b;
        }
        if (i < highest_bit) b = b * b;
    }
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

int64_t BigInt::to_int64() const {
    uint64_t result = 0;
    if (digits_.size() > 0) result |= static_cast<uint64_t>(digits_[0]);
    if (digits_.size() > 1) result |= static_cast<uint64_t>(digits_[1]) << 32;
    return is_negative_ ? -static_cast<int64_t>(result) : static_cast<int64_t>(result);
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

// Arbitrary-precision two's complement bitwise operations.
// Negative BigInt -A has infinite-precision two's complement = NOT(A-1).

BigInt BigInt::bitwise_not() const {
    // ~a = -(a+1) for positive; ~(-a) = a-1 for negative
    if (!is_negative_) {
        BigInt result = *this + BigInt(1);
        result.is_negative_ = true;
        result.normalize();
        return result;
    } else {
        BigInt mag = *this;
        mag.is_negative_ = false;
        return mag - BigInt(1);
    }
}

BigInt BigInt::bitwise_and(const BigInt& other) const {
    if (!is_negative_ && !other.is_negative_) {
        // (+A) & (+B) = A & B (magnitudes, min size)
        size_t sz = std::min(digits_.size(), other.digits_.size());
        BigInt result;
        result.digits_.assign(sz, 0);
        for (size_t i = 0; i < sz; i++)
            result.digits_[i] = digits_[i] & other.digits_[i];
        result.normalize();
        return result;
    } else if (is_negative_ && other.is_negative_) {
        // (-A) & (-B) = -(((A-1) | (B-1)) + 1)
        BigInt A = *this; A.is_negative_ = false;
        BigInt B = other; B.is_negative_ = false;
        BigInt a1 = A - BigInt(1);
        BigInt b1 = B - BigInt(1);
        size_t sz = std::max(a1.digits_.size(), b1.digits_.size());
        BigInt orr;
        orr.digits_.assign(sz, 0);
        for (size_t i = 0; i < sz; i++) {
            uint32_t av = (i < a1.digits_.size()) ? a1.digits_[i] : 0;
            uint32_t bv = (i < b1.digits_.size()) ? b1.digits_[i] : 0;
            orr.digits_[i] = av | bv;
        }
        orr.normalize();
        BigInt result = orr + BigInt(1);
        result.is_negative_ = true;
        result.normalize();
        return result;
    } else {
        // (+A) & (-B) = A & ~(B-1)  [beyond B-1's range, ~(B-1) is all 1s → just A's bits]
        const BigInt* pos = !is_negative_ ? this : &other;
        const BigInt* neg = !is_negative_ ? &other : this;
        BigInt neg_mag = *neg; neg_mag.is_negative_ = false;
        BigInt b1 = neg_mag - BigInt(1);
        BigInt result;
        result.digits_.assign(pos->digits_.size(), 0);
        for (size_t i = 0; i < pos->digits_.size(); i++) {
            uint32_t bv = (i < b1.digits_.size()) ? b1.digits_[i] : 0u;
            result.digits_[i] = pos->digits_[i] & ~bv;
        }
        result.normalize();
        return result;
    }
}

BigInt BigInt::bitwise_or(const BigInt& other) const {
    if (!is_negative_ && !other.is_negative_) {
        // (+A) | (+B) = A | B (max size)
        size_t sz = std::max(digits_.size(), other.digits_.size());
        BigInt result;
        result.digits_.assign(sz, 0);
        for (size_t i = 0; i < sz; i++) {
            uint32_t av = (i < digits_.size()) ? digits_[i] : 0;
            uint32_t bv = (i < other.digits_.size()) ? other.digits_[i] : 0;
            result.digits_[i] = av | bv;
        }
        result.normalize();
        return result;
    } else if (is_negative_ && other.is_negative_) {
        // (-A) | (-B) = -(((A-1) & (B-1)) + 1)
        BigInt A = *this; A.is_negative_ = false;
        BigInt B = other; B.is_negative_ = false;
        BigInt a1 = A - BigInt(1);
        BigInt b1 = B - BigInt(1);
        size_t sz = std::min(a1.digits_.size(), b1.digits_.size());
        BigInt andd;
        andd.digits_.assign(sz, 0);
        for (size_t i = 0; i < sz; i++)
            andd.digits_[i] = a1.digits_[i] & b1.digits_[i];
        andd.normalize();
        BigInt result = andd + BigInt(1);
        result.is_negative_ = true;
        result.normalize();
        return result;
    } else {
        // (+A) | (-B) = -((~A & (B-1)) + 1)  [~A has infinite 1s beyond A's range]
        const BigInt* pos = !is_negative_ ? this : &other;
        const BigInt* neg = !is_negative_ ? &other : this;
        BigInt neg_mag = *neg; neg_mag.is_negative_ = false;
        BigInt b1 = neg_mag - BigInt(1);
        size_t sz = b1.digits_.size();
        BigInt notandb;
        notandb.digits_.assign(sz, 0);
        for (size_t i = 0; i < sz; i++) {
            uint32_t av = (i < pos->digits_.size()) ? pos->digits_[i] : 0xFFFFFFFFu;
            notandb.digits_[i] = (~av) & b1.digits_[i];
        }
        notandb.normalize();
        BigInt result = notandb + BigInt(1);
        result.is_negative_ = true;
        result.normalize();
        return result;
    }
}

BigInt BigInt::bitwise_xor(const BigInt& other) const {
    if (!is_negative_ && !other.is_negative_) {
        // (+A) ^ (+B) = A ^ B (max size, positive)
        size_t sz = std::max(digits_.size(), other.digits_.size());
        BigInt result;
        result.digits_.assign(sz, 0);
        for (size_t i = 0; i < sz; i++) {
            uint32_t av = (i < digits_.size()) ? digits_[i] : 0;
            uint32_t bv = (i < other.digits_.size()) ? other.digits_[i] : 0;
            result.digits_[i] = av ^ bv;
        }
        result.normalize();
        return result;
    } else if (is_negative_ && other.is_negative_) {
        // (-A) ^ (-B) = (A-1) ^ (B-1)  [NOT(A-1) XOR NOT(B-1) = (A-1) XOR (B-1), positive]
        BigInt A = *this; A.is_negative_ = false;
        BigInt B = other; B.is_negative_ = false;
        BigInt a1 = A - BigInt(1);
        BigInt b1 = B - BigInt(1);
        size_t sz = std::max(a1.digits_.size(), b1.digits_.size());
        BigInt result;
        result.digits_.assign(sz, 0);
        for (size_t i = 0; i < sz; i++) {
            uint32_t av = (i < a1.digits_.size()) ? a1.digits_[i] : 0;
            uint32_t bv = (i < b1.digits_.size()) ? b1.digits_[i] : 0;
            result.digits_[i] = av ^ bv;
        }
        result.normalize();
        return result;
    } else {
        // (+A) ^ (-B) = -((A ^ (B-1)) + 1)  [negative result]
        const BigInt* pos = !is_negative_ ? this : &other;
        const BigInt* neg = !is_negative_ ? &other : this;
        BigInt neg_mag = *neg; neg_mag.is_negative_ = false;
        BigInt b1 = neg_mag - BigInt(1);
        size_t sz = std::max(pos->digits_.size(), b1.digits_.size());
        BigInt c;
        c.digits_.assign(sz, 0);
        for (size_t i = 0; i < sz; i++) {
            uint32_t av = (i < pos->digits_.size()) ? pos->digits_[i] : 0;
            uint32_t bv = (i < b1.digits_.size()) ? b1.digits_[i] : 0;
            c.digits_[i] = av ^ bv;
        }
        c.normalize();
        BigInt result = c + BigInt(1);
        result.is_negative_ = true;
        result.normalize();
        return result;
    }
}

BigInt BigInt::left_shift(const BigInt& n) const {
    if (n.is_zero()) return *this;
    if (n.is_negative_) return right_shift(-n);

    uint64_t shift = (uint64_t)n.digits_[0];
    if (n.digits_.size() > 1) shift |= (uint64_t)n.digits_[1] << 32;
    if (n.digits_.size() > 2) shift = UINT64_MAX;  // astronomically large

    size_t word_shift = (size_t)(shift / 32);
    size_t bit_shift  = (size_t)(shift % 32);

    BigInt result;
    result.is_negative_ = is_negative_;
    result.digits_.assign(digits_.size() + word_shift + 1, 0);

    for (size_t i = 0; i < digits_.size(); i++) {
        result.digits_[i + word_shift] |= digits_[i] << bit_shift;
        if (bit_shift > 0)
            result.digits_[i + word_shift + 1] |= digits_[i] >> (32 - bit_shift);
    }

    result.normalize();
    return result;
}

BigInt BigInt::right_shift(const BigInt& n) const {
    if (n.is_zero()) return *this;
    if (n.is_negative_) return left_shift(-n);

    if (n.digits_.size() > 2)
        return is_negative_ ? BigInt(-1) : BigInt(0);

    uint64_t shift = (uint64_t)n.digits_[0];
    if (n.digits_.size() > 1) shift |= (uint64_t)n.digits_[1] << 32;

    size_t word_shift = (size_t)(shift / 32);
    size_t bit_shift  = (size_t)(shift % 32);

    if (word_shift >= digits_.size())
        return is_negative_ ? BigInt(-1) : BigInt(0);

    // For negative: check if any bits are discarded (floor toward -inf)
    bool has_remainder = false;
    if (is_negative_) {
        for (size_t i = 0; i < word_shift && !has_remainder; i++)
            if (digits_[i]) has_remainder = true;
        if (!has_remainder && bit_shift > 0) {
            uint32_t mask = (1u << bit_shift) - 1;
            if (digits_[word_shift] & mask) has_remainder = true;
        }
    }

    size_t new_size = digits_.size() - word_shift;
    BigInt result;
    result.is_negative_ = is_negative_;
    result.digits_.assign(new_size, 0);

    for (size_t i = 0; i < new_size; i++) {
        result.digits_[i] = digits_[i + word_shift] >> bit_shift;
        if (bit_shift > 0 && i + word_shift + 1 < digits_.size())
            result.digits_[i] |= digits_[i + word_shift + 1] << (32 - bit_shift);
    }

    result.normalize();

    if (is_negative_ && has_remainder) {
        result.is_negative_ = false;
        result = result + BigInt(1);
        result.is_negative_ = true;
        result.normalize();
    }

    return result;
}

}
