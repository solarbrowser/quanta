/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/String.h"
#include <unordered_map>
#include <mutex>

namespace Quanta {

// Intern cache: string content → weak ownership signal (just a set of interned contents)
static std::unordered_map<std::string, size_t> intern_cache_;

String::String(const std::string& str) : data_(str) {
    calculate_hash();
}

String::String(std::string&& str) noexcept : data_(std::move(str)) {
    calculate_hash();
}

String::String(std::string_view sv) : data_(sv) {
    calculate_hash();
}

String::String(const char* str) : data_(str ? str : "") {
    calculate_hash();
}

bool String::operator==(const String& other) const noexcept {
    if (this == &other) return true;
    if (hash_ != other.hash_) return false;
    return data_ == other.data_;
}

String String::concat(const String& other) const {
    return String(data_ + other.data_);
}

String String::substring(size_t start, size_t length) const {
    return String(data_.substr(start, length));
}

String String::intern(const std::string& str) {
    String result(str);
    result.interned_ = true;
    intern_cache_[str] = result.hash_;
    return result;
}

void String::calculate_hash() noexcept {
    hash_ = std::hash<std::string>{}(data_);
}

}
