/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_STRING_H
#define QUANTA_STRING_H

#include <string>
#include <string_view>

namespace Quanta {

class String {
private:
    std::string data_;
    size_t hash_ = 0;
    bool interned_ = false;

public:
    String() = default;
    explicit String(const std::string& str);
    explicit String(std::string&& str) noexcept;
    explicit String(std::string_view sv);
    explicit String(const char* str);
    String(const String&) = default;
    String(String&&) noexcept = default;

    String& operator=(const String&) = default;
    String& operator=(String&&) noexcept = default;

    [[nodiscard]] const std::string& str()    const noexcept { return data_; }
    [[nodiscard]] const char*        c_str()  const noexcept { return data_.c_str(); }
    [[nodiscard]] size_t             length() const noexcept { return data_.length(); }
    [[nodiscard]] size_t             size()   const noexcept { return data_.size(); }
    [[nodiscard]] bool               empty()  const noexcept { return data_.empty(); }
    [[nodiscard]] size_t             hash()   const noexcept { return hash_; }
    [[nodiscard]] bool               interned() const noexcept { return interned_; }

    bool operator==(const String& other) const noexcept;
    bool operator!=(const String& other) const noexcept { return !(*this == other); }
    bool operator< (const String& other) const noexcept { return data_ < other.data_; }

    [[nodiscard]] String concat(const String& other) const;
    [[nodiscard]] String substring(size_t start, size_t length = std::string::npos) const;

    static String intern(const std::string& str);

private:
    void calculate_hash() noexcept;
};

}

#endif
