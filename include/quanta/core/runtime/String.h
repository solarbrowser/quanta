/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_STRING_H
#define QUANTA_STRING_H

#include <string>
#include <string_view>
#include <cstdint>

namespace Quanta {

class String {
    mutable std::string data_;
    String* left_  = nullptr;
    String* right_ = nullptr;
    mutable size_t hash_ = 0;
    bool interned_       = false;
    bool is_cons_        = false;
    mutable bool flat_   = false; // true when data_ holds the materialized bytes

    void ensure_flat() const;
    void calculate_hash() noexcept;
    static void collect_bytes(const String* node, std::string& out);

public:
    // GC cell protocol (see Object.h): heap strings are String-kind cells;
    // stack/value instances never touch these. Rope cons nodes are cells too.
    static void* operator new(size_t size);
    static void  operator delete(void* p) noexcept;
    static void* operator new[](size_t) = delete;
    static void  operator delete[](void*) = delete;

    String() = default;
    explicit String(const std::string& str);
    explicit String(std::string&& str) noexcept;
    explicit String(std::string_view sv);
    explicit String(const char* str);
    // Cons node — created only via make_concat
    String(String* left, String* right) noexcept
        : left_(left), right_(right), is_cons_(true), flat_(false) {}

    String(const String&) = default;
    String(String&&) noexcept = default;
    String& operator=(const String&) = default;
    String& operator=(String&&) noexcept = default;

    [[nodiscard]] const std::string& str()   const noexcept { if (is_cons_ && !flat_) ensure_flat(); return data_; }
    [[nodiscard]] const char*        c_str() const noexcept { return str().c_str(); }
    [[nodiscard]] size_t             length()const noexcept { return str().length(); }
    [[nodiscard]] size_t             size()  const noexcept { return str().size(); }
    [[nodiscard]] bool               empty() const noexcept { return !is_cons_ && data_.empty(); }
    [[nodiscard]] size_t             hash()  const noexcept { if (!hash_) ensure_flat(); return hash_; }
    [[nodiscard]] bool               interned() const noexcept { return interned_; }

    bool operator==(const String& other) const noexcept;
    bool operator!=(const String& other) const noexcept { return !(*this == other); }
    bool operator< (const String& other) const noexcept { return str() < other.str(); }

    // O(1) when both sides are large; flattens lazily on first read
    static String* make_concat(String* a, String* b);

    // Value-returning helpers kept for backwards compat with non-pointer callers
    [[nodiscard]] String concat(const String& other) const;
    [[nodiscard]] String substring(size_t start, size_t length = std::string::npos) const;

    static String intern(const std::string& str);
};

size_t utf16_length(const std::string& utf8);
int32_t utf16_code_unit_at(const std::string& utf8, size_t utf16_index);
int32_t utf16_code_point_at(const std::string& utf8, size_t utf16_index);
std::string encode_utf16_unit(uint32_t unit);
size_t utf16_index_to_byte_pos(const std::string& utf8, size_t index);
bool utf16_is_well_formed(const std::string& utf8);
std::string utf16_to_well_formed(const std::string& utf8);

}

#endif
