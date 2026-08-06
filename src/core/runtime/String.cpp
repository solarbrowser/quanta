/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/String.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/core/gc/Heap.h"
#include <vector>
#include <cstring>

namespace Quanta {

namespace {

// WTF-8 keeps an unpaired surrogate as its own three-byte sequence: ED A0-AF xx
// for a high one, ED B0-BF xx for a low one. Concatenation can put a high one
// immediately before a low one, and that pair is a SINGLE code point, so it has
// to be stored as the one four-byte sequence it stands for. Left as two
// sequences it still reports the right length and the right code units, but
// string comparison is over the stored bytes -- so `hi + lo` did not equal the
// same character written any other way, and codePointAt and iteration both saw
// two characters where there is one.
bool high_surrogate_at(const std::string& s, size_t i) {
    unsigned char b0 = static_cast<unsigned char>(s[i]);
    unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    return b0 == 0xED && b1 >= 0xA0 && b1 <= 0xAF;
}

bool low_surrogate_at(const std::string& s, size_t i) {
    unsigned char b0 = static_cast<unsigned char>(s[i]);
    unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    return b0 == 0xED && b1 >= 0xB0 && b1 <= 0xBF;
}

uint32_t surrogate_at(const char* p) {
    return 0xD000u | ((static_cast<unsigned char>(p[1]) & 0x3F) << 6)
                   | (static_cast<unsigned char>(p[2]) & 0x3F);
}

void append_pair(std::string& out, uint32_t hi, uint32_t lo) {
    uint32_t cp = 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
}

// Every String's bytes are canonical, enforced on construction rather than at
// each of the many places that build one by appending (a template literal,
// Array.join, String.prototype.concat, replace, repeat...). Fixing those one at
// a time would leave whichever one nobody thought of still storing a split
// pair. Costs a memchr, and 0xED occurs in no ASCII text at all.
void canonicalize_pairs(std::string& s) {
    if (s.size() < 6 || std::memchr(s.data(), 0xED, s.size()) == nullptr) return;
    std::string out;
    out.reserve(s.size());
    bool merged = false;
    size_t i = 0;
    while (i < s.size()) {
        if (i + 6 <= s.size() && high_surrogate_at(s, i) && low_surrogate_at(s, i + 3)) {
            append_pair(out, surrogate_at(s.data() + i), surrogate_at(s.data() + i + 3));
            i += 6;
            merged = true;
        } else {
            out += s[i++];
        }
    }
    if (merged) s = std::move(out);
}

// The rope's flatten builds its buffer directly rather than through a
// constructor, and its pieces meet at seams the pieces themselves cannot see,
// so it joins as it goes instead of rescanning the result.
void append_joined(std::string& out, const std::string& add) {
    if (out.size() >= 3 && add.size() >= 3 &&
        high_surrogate_at(out, out.size() - 3) && low_surrogate_at(add, 0)) {
        uint32_t hi = surrogate_at(out.data() + out.size() - 3);
        out.resize(out.size() - 3);
        append_pair(out, hi, surrogate_at(add.data()));
        out.append(add, 3, std::string::npos);
        return;
    }
    out += add;
}

}  // namespace

void String::gc_trace(Visitor& v) const {
    if (!is_cons_) return;
    v.visit_string(left_);
    v.visit_string(right_);
}


void* String::operator new(size_t size) {
    return Heap::active().allocate(size, CellKind::String);
}

void String::operator delete(void* p) noexcept {
    Heap::cell_free(p);
}


String::String(const std::string& str) : data_(str) {
    canonicalize_pairs(data_);
}

String::String(std::string&& str) noexcept : data_(std::move(str)) {
    canonicalize_pairs(data_);
}

String::String(std::string_view sv) : data_(sv) {
    canonicalize_pairs(data_);
}

String::String(const char* str) : data_(str ? str : "") {
    canonicalize_pairs(data_);
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
    return result;
}

void String::calculate_hash() const noexcept {
    hash_ = std::hash<std::string>{}(data_);
}

void String::collect_bytes(const String* node, std::string& out) {
    // Explicit stack, not recursion. `s += x` in a loop builds a cons chain
    // one node deep per append, with nothing balancing it, so recursing once
    // per link put the rope's depth directly on the C stack and a few hundred
    // thousand appends -- ordinary code, and exactly what a code generator
    // that accumulates its output does -- overflowed it.
    std::vector<const String*> pending;
    pending.push_back(node);
    while (!pending.empty()) {
        const String* n = pending.back();
        pending.pop_back();
        if (!n->is_cons_ || n->flat_) {
            append_joined(out, n->data_);
        } else {
            // Right first: the stack pops left first, preserving order.
            pending.push_back(n->right_);
            pending.push_back(n->left_);
        }
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

bool String::is_ascii() const {
    if (ascii_) return ascii_ == 1;
    const std::string& s = str();
    bool plain = true;
    for (unsigned char c : s) {
        if (c >= 0x80) { plain = false; break; }
    }
    ascii_ = plain ? 1 : 2;
    if (plain && s.size() < UINT32_MAX) utf16_len_ = static_cast<uint32_t>(s.size());
    return plain;
}

size_t String::utf16_length() const {
    if (utf16_len_ != UINT32_MAX) return utf16_len_;
    if (is_ascii()) return utf16_len_;  // is_ascii() fills it in for this case
    size_t n = Quanta::utf16_length(str());
    if (n < UINT32_MAX) utf16_len_ = static_cast<uint32_t>(n);
    return n;
}

int32_t String::code_unit_at(size_t index) const {
    const std::string& s = str();
    if (is_ascii()) {
        if (index >= s.size()) return -1;
        return static_cast<int32_t>(static_cast<unsigned char>(s[index]));
    }
    return utf16_code_unit_at(s, index);
}

size_t String::byte_pos(size_t index) const {
    const std::string& s = str();
    if (is_ascii()) return index < s.size() ? index : s.size();
    return utf16_index_to_byte_pos(s, index);
}

std::string String::substring_utf16(size_t start, size_t end) const {
    const std::string& s = str();
    if (end <= start) return std::string();
    // Single-byte text has no pairs to split and no multi-byte sequences, so a
    // UTF-16 range is a byte range.
    if (is_ascii()) {
        size_t b = std::min(start, s.size());
        size_t e = std::min(end, s.size());
        return e > b ? s.substr(b, e - b) : std::string();
    }
    auto append_lone = [](std::string& out, uint32_t unit) {
        out += static_cast<char>(0xE0 | (unit >> 12));
        out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (unit & 0x3F));
    };
    std::string out;
    size_t units = 0, pos = 0;
    while (pos < s.size() && units < end) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        if (cp <= 0xFFFF) {
            if (units >= start) out.append(s, pos, len);
            units += 1;
        } else {
            uint32_t v = cp - 0x10000;
            bool take_hi = units >= start && units < end;
            bool take_lo = units + 1 >= start && units + 1 < end;
            if (take_hi && take_lo) out.append(s, pos, len);
            else if (take_hi) append_lone(out, 0xD800 + (v >> 10));
            else if (take_lo) append_lone(out, 0xDC00 + (v & 0x3FF));
            units += 2;
        }
        pos += len;
    }
    return out;
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

size_t utf16_index_from_byte_pos(const std::string& s, size_t byte_pos) {
    size_t pos = 0, units = 0;
    while (pos < s.size() && pos < byte_pos) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        units += (cp > 0xFFFF) ? 2 : 1;
        pos += len;
    }
    return units;
}

bool utf8_is_ascii(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0x80) return false;
    }
    return true;
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
