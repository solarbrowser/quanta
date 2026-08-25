/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "quanta/core/runtime/String.h"

namespace Quanta {
// Defined in RegExp.cpp; declared here so a string can decode itself without
// pulling in the regex engine's headers.
std::u16string wtf8_to_utf16(const std::string& s);
}
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
bool high_surrogate_at(std::string_view s, size_t i) {
    unsigned char b0 = static_cast<unsigned char>(s[i]);
    unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
    return b0 == 0xED && b1 >= 0xA0 && b1 <= 0xAF;
}

bool low_surrogate_at(std::string_view s, size_t i) {
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
void append_joined(std::string& out, std::string_view add) {
    if (out.size() >= 3 && add.size() >= 3 &&
        high_surrogate_at(out, out.size() - 3) && low_surrogate_at(add, 0)) {
        uint32_t hi = surrogate_at(out.data() + out.size() - 3);
        out.resize(out.size() - 3);
        append_pair(out, hi, surrogate_at(add.data()));
        out.append(add.substr(3));
        return;
    }
    out += add;
}

}  // namespace

void String::gc_trace(Visitor& v) const {
    if (!is_cons_) return;
    if (is_tail_) { v.visit_string(tail_.left); return; }
    v.visit_string(cons_.left);
    v.visit_string(cons_.right);
}


namespace {
// Owner is tracked so a cell is never handed to a heap that did not allocate
// it: HeapScope swaps the active heap for the duration of a realm's call, and
// a cell from the previous heap would be swept by a collector that never
// traced it.
struct SingleCharCache {
    Heap* owner = nullptr;
    String* cells[128] = {};
};
thread_local SingleCharCache g_single_char;
}  // namespace

String* String::single_char(unsigned char c) {
    if (c >= 128) return nullptr;
    Heap* active = Heap::active_or_null();
    if (!active) return nullptr;
    if (g_single_char.owner != active) {
        // The old heap's cells are simply forgotten: they are ordinary cells
        // there and its own collector reclaims them.
        for (String*& cell : g_single_char.cells) cell = nullptr;
        g_single_char.owner = active;
    }
    String*& slot = g_single_char.cells[c];
    if (!slot) slot = new String(std::string(1, static_cast<char>(c)));
    return slot;
}

void String::gc_trace_roots(Visitor& v) {
    if (g_single_char.owner != Heap::active_or_null()) return;
    for (String* cell : g_single_char.cells)
        if (cell) v.visit_string(cell);
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
    // A link's inline bytes have no node of their own, so the entry says which
    // of the two the node is being visited for -- its subtree, or the bytes it
    // carries. Order is left subtree first, then those bytes.
    struct Item { const String* node; bool tail_bytes; };
    std::vector<Item> pending;
    pending.push_back({node, false});
    while (!pending.empty()) {
        Item it = pending.back();
        pending.pop_back();
        if (it.tail_bytes) {
            append_joined(out, it.node->inline_tail());
        } else if (!it.node->is_cons_) {
            append_joined(out, it.node->data_);
        } else if (it.node->is_tail_) {
            pending.push_back({it.node, true});
            pending.push_back({it.node->tail_.left, false});
        } else {
            // Right first: the stack pops left first, preserving order.
            pending.push_back({it.node->cons_.right, false});
            pending.push_back({it.node->cons_.left, false});
        }
    }
}

void String::ensure_flat() const {
    std::string result;
    collect_bytes(this, result);
    // Hand the union over: the children are plain pointers with nothing to
    // destroy, and the bytes have to be constructed in their place before
    // anything reads data_.
    is_cons_ = false;
    is_tail_ = false;
    new (&data_) std::string(std::move(result));
    // The node holds its own bytes now, so it is an ordinary flat string and
    // its children are nobody's business. Keeping is_cons_ set left gc_trace
    // visiting them, which pinned the whole tree behind a rope that had
    // already been collapsed -- for an append loop that is every node ever
    // built.
    hash_  = std::hash<std::string>{}(data_);
}

static constexpr size_t CONS_THRESHOLD = 32;

// The bytes an append would put in the rope's last link. Built on the stack:
// the limit is above the length a std::string keeps in its own storage, so
// going through one would put a heap allocation on the hottest append path.
struct TailBuf { char bytes[String::kInlineTail]; size_t len; };

static bool merged_tail(std::string_view head, const std::string& add, TailBuf& out) {
    const size_t n = head.size() + add.size();
    if (n > String::kInlineTail) return false;
    std::memcpy(out.bytes, head.data(), head.size());
    std::memcpy(out.bytes + head.size(), add.data(), add.size());
    out.len = n;
    // A surrogate pair split across the seam is one code point and has to be
    // stored as the one sequence it stands for. Nothing but a 0xED byte can
    // begin half of one, so the rewrite cannot be needed without one.
    if (std::memchr(out.bytes, 0xED, n) == nullptr) return true;
    std::string joined(out.bytes, n);
    canonicalize_pairs(joined);
    if (joined.size() > String::kInlineTail) return false;
    std::memcpy(out.bytes, joined.data(), joined.size());
    out.len = joined.size();
    return true;
}

String* String::make_concat(String* a, String* b) {
    if (!a || a->empty()) return b;
    if (!b || b->empty()) return a;
    // Flatten immediately only when BOTH sides are already flat and combined size is small.
    // Don't call str() on a cons node here — that would defeat the whole purpose.
    if (!a->is_cons_ && !b->is_cons_ && a->data_.size() + b->data_.size() <= CONS_THRESHOLD) {
        return new String(a->data_ + b->data_);
    }
    // Put a small append in the last link rather than growing the rope by one.
    // `a` is untouched -- it is immutable and may be shared, so this builds a
    // second rope over the same left subtree, spelling the same bytes.
    if (a->is_cons_ && !b->is_cons_) {
        TailBuf merged;
        if (a->is_tail_) {
            if (merged_tail(a->inline_tail(), b->data_, merged))
                return new String(a->tail_.left, merged.bytes, merged.len);
        } else if (!a->cons_.right->is_cons_) {
            if (merged_tail(a->cons_.right->data_, b->data_, merged))
                return new String(a->cons_.left, merged.bytes, merged.len);
        }
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

// Cursors live per thread, not per cell: a per-String field would cost every
// string cell 16 bytes (48 is a heap size class, 56 rounds up to 64), and the
// scans that matter walk only a handful of strings. A few ways rather than one,
// because a lexer alternates between its source and the tokens it cuts out of
// it, and with a single way each token read evicted the source's position --
// putting the next read back at byte 0. Dropped by ~String, so a way can never
// name a freed cell.
namespace {
constexpr size_t kCursorWays = 4;
struct Cursor {
    const String* owner = nullptr;
    size_t units = 0;
    size_t bytes = 0;
    uint64_t used = 0;
};
thread_local Cursor g_cursors[kCursorWays];
thread_local uint64_t g_cursor_tick = 0;

Cursor* find_cursor(const String* s) {
    for (Cursor& c : g_cursors) if (c.owner == s) return &c;
    return nullptr;
}
}

// Decoded subjects, kept for the cell they came from and on the same terms as
// the cursors above. A few ways because a program can drive more than one
// subject at a time, bounded because the units cost twice the bytes they
// decode.
namespace {
constexpr size_t kUnitWays = 2;
constexpr size_t kUnitsMinBytes = 1024;
constexpr size_t kUnitsMaxBytes = 24u << 20;
struct UnitWay {
    const String* owner = nullptr;
    std::u16string units;
    uint64_t id = 0;
    uint64_t used = 0;
};
thread_local UnitWay g_units[kUnitWays];
thread_local uint64_t g_units_tick = 0;
thread_local uint64_t g_units_next_id = 0;
}

void String::drop_cursor() const noexcept {
    for (Cursor& c : g_cursors) if (c.owner == this) { c.owner = nullptr; c.used = 0; }
    for (UnitWay& w : g_units) {
        if (w.owner != this) continue;
        w.owner = nullptr;
        w.used = 0;
        w.id = 0;
        w.units.clear();
        w.units.shrink_to_fit();
    }
    in_side_cache_ = false;
}

const std::u16string& String::utf16_units(uint64_t* id) const {
    const std::string& s = str();
    for (UnitWay& w : g_units) {
        if (w.owner != this) continue;
        w.used = ++g_units_tick;
        if (id) *id = w.id;
        return w.units;
    }
    // Short subjects are not worth a way: decoding one is cheap and the
    // programs that match against many different short strings would evict a
    // long subject that is being walked.
    static thread_local std::u16string scratch;
    if (s.size() < kUnitsMinBytes || s.size() > kUnitsMaxBytes) {
        scratch = wtf8_to_utf16(s);
        if (id) *id = 0;
        return scratch;
    }
    UnitWay* victim = &g_units[0];
    for (UnitWay& w : g_units) if (w.used < victim->used) victim = &w;
    // Cleared before asking, so the way being taken does not answer for itself.
    const String* evicted = victim->owner;
    victim->owner = nullptr;
    if (evicted) evicted->in_side_cache_ = evicted->still_cached_elsewhere();
    victim->owner = this;
    victim->units = wtf8_to_utf16(s);
    victim->id = ++g_units_next_id;
    victim->used = ++g_units_tick;
    in_side_cache_ = true;
    if (id) *id = victim->id;
    return victim->units;
}

bool String::still_cached_elsewhere() const noexcept {
    for (const Cursor& c : g_cursors) if (c.owner == this) return true;
    for (const UnitWay& w : g_units) if (w.owner == this) return true;
    return false;
}

void String::seek_utf16(const std::string& s, size_t index, size_t& pos, size_t& units) const {
    units = 0;
    pos = 0;
    Cursor* cur = find_cursor(this);
    if (!cur) return;
    if (cur->units <= index) {
        units = cur->units;
        pos = cur->bytes;
        return;
    }
    // Backwards too, because the two readers of a source string disagree about
    // direction: a lexer walks forward with charCodeAt and then slices the token
    // it just read, which starts behind where the walk stopped. Forward-only
    // left every one of those slices decoding from byte 0.
    size_t back = cur->units - index;
    if (back > index) return;  // rewinding would cost more than starting over
    size_t p = cur->bytes, u = cur->units;
    while (u > index && p > 0) {
        // A continuation byte is 10xxxxxx; the character starts at the first
        // byte that is not one.
        do { --p; } while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80);
        size_t len;
        uint32_t cp = decode_utf8_at(s, p, &len);
        u -= (cp > 0xFFFF) ? 2 : 1;
    }
    units = u;
    pos = p;
}

// The same rewind, but aimed at a byte offset instead of a unit index.
void String::seek_utf16_byte(const std::string& s, size_t byte, size_t& pos, size_t& units) const {
    units = 0;
    pos = 0;
    Cursor* cur = find_cursor(this);
    if (!cur) return;
    if (cur->bytes <= byte) {
        units = cur->units;
        pos = cur->bytes;
        return;
    }
    if (cur->bytes - byte > byte) return;
    size_t p = cur->bytes, u = cur->units;
    while (p > byte && p > 0) {
        do { --p; } while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80);
        size_t len;
        uint32_t cp = decode_utf8_at(s, p, &len);
        u -= (cp > 0xFFFF) ? 2 : 1;
    }
    units = u;
    pos = p;
}

size_t String::index_of_byte(size_t byte) const {
    const std::string& s = str();
    if (is_ascii()) return byte < s.size() ? byte : s.size();
    if (byte >= s.size()) return utf16_length();
    size_t pos, units;
    seek_utf16_byte(s, byte, pos, units);
    while (pos < byte && pos < s.size()) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        units += (cp > 0xFFFF) ? 2 : 1;
        pos += len;
    }
    park_utf16(pos, units);
    return units;
}

void String::park_utf16(size_t pos, size_t units) const {
    Cursor* cur = find_cursor(this);
    if (!cur) {
        cur = &g_cursors[0];
        for (Cursor& c : g_cursors) if (c.used < cur->used) cur = &c;
        const String* evicted = cur->owner;
        cur->owner = nullptr;
        if (evicted) evicted->in_side_cache_ = evicted->still_cached_elsewhere();
        cur->owner = this;
        in_side_cache_ = true;
    }
    cur->bytes = pos;
    cur->units = units;
    cur->used = ++g_cursor_tick;
}

int32_t String::code_unit_at(size_t index) const {
    const std::string& s = str();
    if (is_ascii()) {
        if (index >= s.size()) return -1;
        return static_cast<int32_t>(static_cast<unsigned char>(s[index]));
    }
    size_t pos, units;
    seek_utf16(s, index, pos, units);
    while (pos < s.size()) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        if (cp <= 0xFFFF) {
            if (units == index) { park_utf16(pos, units); return static_cast<int32_t>(cp); }
            units += 1;
        } else {
            uint32_t v = cp - 0x10000;
            // Parked at the pair's own start, which is the position both of
            // its units answer from.
            if (units == index) { park_utf16(pos, units); return static_cast<int32_t>(0xD800 + (v >> 10)); }
            if (units + 1 == index) { park_utf16(pos, units); return static_cast<int32_t>(0xDC00 + (v & 0x3FF)); }
            units += 2;
        }
        pos += len;
    }
    return -1;
}

size_t String::byte_pos(size_t index) const {
    const std::string& s = str();
    if (is_ascii()) return index < s.size() ? index : s.size();
    size_t pos, units;
    seek_utf16(s, index, pos, units);
    while (pos < s.size()) {
        if (units == index) { park_utf16(pos, units); return pos; }
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        units += (cp > 0xFFFF) ? 2 : 1;
        pos += len;
    }
    return s.size();
}

std::string String::substring_utf16(size_t start, size_t end) const {
    const std::string& s = str();
    if (end <= start) return std::string();
    // Single-byte text has no pairs to split and no multi-byte sequences, so a
    // UTF-16 range is a byte range. Cached here; the free function below has to
    // discover it.
    if (is_ascii()) {
        size_t b = std::min(start, s.size());
        size_t e = std::min(end, s.size());
        return e > b ? s.substr(b, e - b) : std::string();
    }
    size_t pos, units;
    seek_utf16(s, start, pos, units);
    std::string out = utf16_substring_from(s, start, end, pos, units);
    if (pos <= s.size()) park_utf16(pos, units);
    return out;
}

std::string utf16_substring(const std::string& s, size_t start, size_t end) {
    size_t pos = 0, units = 0;
    return utf16_substring_from(s, start, end, pos, units);
}

// `pos`/`units` come in naming any character boundary at or before `start` and
// go out naming the boundary the walk stopped on, so a caller holding a cursor
// can hand its position in and get the advanced one back.
std::string utf16_substring_from(const std::string& s, size_t start, size_t end,
                                 size_t& pos, size_t& units) {
    if (end <= start) return std::string();
    auto append_lone = [](std::string& out, uint32_t unit) {
        out += static_cast<char>(0xE0 | (unit >> 12));
        out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (unit & 0x3F));
    };
    // Find the byte range first and copy it in one piece. Appending character
    // by character costs an append per character of the RESULT, which for the
    // `s.slice(i)` that hands back most of a source file is the whole file --
    // and the two forms differ only when an endpoint falls between the halves
    // of a surrogate pair, which no byte range can express.
    size_t byte_start = std::string::npos;
    size_t split_start = std::string::npos, split_end = std::string::npos;
    while (pos < s.size() && units < end) {  // stops at `end`, not at the end of s
        size_t len;
        uint32_t cp = decode_utf8_at(s, pos, &len);
        size_t width = (cp > 0xFFFF) ? 2u : 1u;
        if (byte_start == std::string::npos) {
            if (units == start) byte_start = pos;
            // `start` names the low half of this pair: the leading half is not
            // part of the answer, so the copy cannot begin at this character.
            else if (width == 2 && units + 1 == start) { split_start = pos; byte_start = pos + len; }
        }
        if (width == 2 && units + 1 == end) split_end = pos;  // `end` splits this pair
        units += width;
        pos += len;
    }
    if (byte_start == std::string::npos) byte_start = pos;
    size_t byte_end = (split_end == std::string::npos) ? pos : split_end;
    std::string out;
    if (split_start != std::string::npos) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, split_start, &len);
        append_lone(out, 0xDC00 + ((cp - 0x10000) & 0x3FF));
    }
    if (byte_end > byte_start) out.append(s, byte_start, byte_end - byte_start);
    if (split_end != std::string::npos) {
        size_t len;
        uint32_t cp = decode_utf8_at(s, split_end, &len);
        append_lone(out, 0xD800 + ((cp - 0x10000) >> 10));
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
