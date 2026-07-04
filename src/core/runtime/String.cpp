/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/String.h"
#include "quanta/core/gc/Heap.h"
#include <unordered_map>
#include <mutex>

namespace Quanta {

void* String::operator new(size_t size) {
    return Heap::active().allocate(size, CellKind::String);
}

void String::operator delete(void* p) noexcept {
    Heap::cell_free(p);
}


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
    const std::string& a = str();
    const std::string& b = other.str();
    if (hash_ && other.hash_ && hash_ != other.hash_) return false;
    return a == b;
}

String String::concat(const String& other) const {
    return String(str() + other.str());
}

String String::substring(size_t start, size_t length) const {
    return String(str().substr(start, length));
}

String String::intern(const std::string& s) {
    String result(s);
    result.interned_ = true;
    intern_cache_[s] = result.hash_;
    return result;
}

void String::calculate_hash() noexcept {
    hash_ = std::hash<std::string>{}(data_);
}

void String::collect_bytes(const String* node, std::string& out) {
    if (!node->is_cons_ || node->flat_) {
        out.append(node->data_);
    } else {
        collect_bytes(node->left_, out);
        collect_bytes(node->right_, out);
    }
}

void String::ensure_flat() const {
    std::string result;
    collect_bytes(this, result);
    data_  = std::move(result);
    flat_  = true;
    hash_  = std::hash<std::string>{}(data_);
}

static constexpr size_t CONS_THRESHOLD = 32;

String* String::make_concat(String* a, String* b) {
    if (!a || a->empty()) return b;
    if (!b || b->empty()) return a;
    // Flatten immediately only when BOTH sides are already flat and combined size is small.
    // Don't call str() on a cons node here — that would defeat the whole purpose.
    if (!a->is_cons_ && !b->is_cons_ && a->data_.size() + b->data_.size() <= CONS_THRESHOLD) {
        return new String(a->data_ + b->data_);
    }
    return new String(a, b);
}

namespace {
uint32_t decode_utf8_at(const std::string& s, size_t byte_pos, size_t* out_len) {
    unsigned char c = static_cast<unsigned char>(s[byte_pos]);
    if (c < 0x80) { *out_len = 1; return c; }
    if ((c & 0xE0) == 0xC0 && byte_pos + 1 < s.size()) {
        *out_len = 2;
        return ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[byte_pos + 1]) & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && byte_pos + 2 < s.size()) {
        *out_len = 3;
        return ((c & 0x0F) << 12) |
               ((static_cast<unsigned char>(s[byte_pos + 1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[byte_pos + 2]) & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && byte_pos + 3 < s.size()) {
        *out_len = 4;
        return ((c & 0x07) << 18) |
               ((static_cast<unsigned char>(s[byte_pos + 1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(s[byte_pos + 2]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[byte_pos + 3]) & 0x3F);
    }
    *out_len = 1;
    return c;
}
}

size_t utf16_length(const std::string& s) {
    size_t pos = 0, units = 0;
    while (pos < s.size()) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        units += (cp > 0xFFFF) ? 2 : 1;
        pos += len;
    }
    return units;
}

int32_t utf16_code_unit_at(const std::string& s, size_t index) {
    size_t pos = 0, units = 0;
    while (pos < s.size()) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        if (cp <= 0xFFFF) {
            if (units == index) return static_cast<int32_t>(cp);
            units += 1;
        } else {
            uint32_t v = cp - 0x10000;
            uint32_t hi = 0xD800 + (v >> 10);
            uint32_t lo = 0xDC00 + (v & 0x3FF);
            if (units == index) return static_cast<int32_t>(hi);
            if (units + 1 == index) return static_cast<int32_t>(lo);
            units += 2;
        }
        pos += len;
    }
    return -1;
}

int32_t utf16_code_point_at(const std::string& s, size_t index) {
    size_t pos = 0, units = 0;
    while (pos < s.size()) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        if (cp <= 0xFFFF) {
            if (units == index) return static_cast<int32_t>(cp);
            units += 1;
        } else {
            uint32_t v = cp - 0x10000;
            uint32_t lo = 0xDC00 + (v & 0x3FF);
            if (units == index) return static_cast<int32_t>(cp);
            if (units + 1 == index) return static_cast<int32_t>(lo);
            units += 2;
        }
        pos += len;
    }
    return -1;
}

// Returns the byte offset in s corresponding to the start of UTF-16 code unit `index`.
// If index >= utf16_length(s), returns s.size() (end of string).
size_t utf16_index_to_byte_pos(const std::string& s, size_t index) {
    size_t pos = 0, units = 0;
    while (pos < s.size()) {
        if (units == index) return pos;
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        units += (cp > 0xFFFF) ? 2 : 1;
        pos += len;
    }
    return s.size();
}

bool utf16_is_well_formed(const std::string& s) {
    size_t pos = 0;
    bool pending_high = false;
    while (pos < s.size()) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        bool is_high = cp >= 0xD800 && cp <= 0xDBFF;
        bool is_low = cp >= 0xDC00 && cp <= 0xDFFF;
        if (pending_high) {
            if (!is_low) return false;
            pending_high = false;
        } else if (is_low) {
            return false;
        } else if (is_high) {
            pending_high = true;
        }
        pos += len;
    }
    return !pending_high;
}

std::string utf16_to_well_formed(const std::string& s) {
    std::string result;
    size_t pos = 0;
    bool pending_high = false;
    size_t pending_start = 0, pending_len = 0;
    auto emit_replacement = [&]() { result += "\xEF\xBF\xBD"; };
    while (pos < s.size()) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        bool is_high = cp >= 0xD800 && cp <= 0xDBFF;
        bool is_low = cp >= 0xDC00 && cp <= 0xDFFF;

        if (pending_high) {
            if (is_low) {
                result.append(s, pending_start, pending_len);
                result.append(s, pos, len);
                pending_high = false;
                pos += len;
                continue;
            }
            emit_replacement();
            pending_high = false;
        }

        if (is_high) {
            pending_high = true;
            pending_start = pos;
            pending_len = len;
        } else if (is_low) {
            emit_replacement();
        } else {
            result.append(s, pos, len);
        }
        pos += len;
    }
    if (pending_high) emit_replacement();
    return result;
}

std::string encode_utf16_unit(uint32_t unit) {
    std::string r;
    if (unit <= 0x7F) {
        r += static_cast<char>(unit);
    } else if (unit <= 0x7FF) {
        r += static_cast<char>(0xC0 | (unit >> 6));
        r += static_cast<char>(0x80 | (unit & 0x3F));
    } else {
        r += static_cast<char>(0xE0 | (unit >> 12));
        r += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
        r += static_cast<char>(0x80 | (unit & 0x3F));
    }
    return r;
}

}
