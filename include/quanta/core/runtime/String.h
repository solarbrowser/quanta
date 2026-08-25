/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_STRING_H
#define QUANTA_STRING_H

#include <new>
#include <utility>
#include <string>
#include <string_view>
#include <cstdint>

namespace Quanta {

class String {
    // Flat bytes and cons children are mutually exclusive -- a node is one or
    // the other, never both -- so they share storage. That is the difference
    // between a 64-byte cell and a 48-byte one, and a rope's nodes are cells
    // too, so an append loop pays it once per link. is_cons_ is the only
    // discriminant: every transition destroys one member and constructs the
    // other, which is why the copy/move/destroy below are all hand-written.
    union {
        mutable std::string data_;
        struct { String* left; String* right; } cons_;
        // A link that carries the rope's last few bytes itself instead of
        // pointing at a cell for them. `s += piece` in a loop appends a
        // handful of bytes at a time and a plain link costs a whole cell no
        // matter how little text it adds, so a rope built that way is mostly
        // pointers describing very little text. This form spends the same one
        // cell per append but only starts a new link once the bytes fill up,
        // so the live link count follows the TEXT rather than the number of
        // appends. Sized to fill the union the flat form already needs.
        struct { String* left; uint8_t len; char bytes[23]; } tail_;
    };
    mutable size_t hash_ = 0;
    // Bit-fields, not plain bools: ascii_ below was added after the comment
    // promising 64 bytes was written and pushed sizeof(String) to 72, which
    // rounds up to the 80-byte heap class. Packed into one byte together they
    // fit in the padding utf16_len_'s alignment leaves, so a string cell is
    // 64 again -- and a rope's cons nodes are strings too.
    bool interned_       : 1 = false;
    // "not flat": the bytes have to be collected from below. is_tail_ then
    // says which of the two link layouts this is.
    mutable bool is_cons_ : 1 = false;
    mutable bool is_tail_ : 1 = false;
    mutable uint8_t ascii_ : 2 = 0;
    // True while a thread-local side cache (the scan cursor, the decoded
    // units) names this cell. Not copied: a copy is a different cell and is in
    // no cache. Lets the destructor skip the cache scan for the strings that
    // were never in one, which is nearly all of them.
    mutable bool in_side_cache_ : 1 = false;
    // UTF-16 length, computed once. `.length` is a scan of the whole UTF-8
    // buffer, so reading it in a loop over a growing string is quadratic;
    // strings are immutable, so the answer never changes once known. Fits the
    // padding these three bools already left behind, so String stays 64 bytes.
    // UINT32_MAX means "not computed"; a string long enough to collide with
    // the sentinel simply recomputes.
    mutable uint32_t utf16_len_ = UINT32_MAX;
    // Whether every byte is < 0x80, computed once. When it is, a UTF-16 index
    // IS a byte index and the length IS the byte count, so every indexed read
    // becomes O(1) instead of a decode from the start of the string. 0 unknown,
    // 1 single-byte, 2 has multi-byte sequences.

    void ensure_flat() const;
    void destroy_payload() noexcept { forget_cursor(); if (!is_cons_) data_.~basic_string(); }
    // Where the last indexed read of THIS string stopped. See String.cpp: a
    // scan that walks a non-ASCII string in order would otherwise decode from
    // byte 0 on every read, and one such character anywhere in a file is
    // enough to put the whole file on that path.
    void seek_utf16(const std::string& s, size_t index, size_t& pos, size_t& units) const;
    void seek_utf16_byte(const std::string& s, size_t byte, size_t& pos, size_t& units) const;
    void park_utf16(size_t pos, size_t units) const;
    // Cheap for the overwhelming majority: an ASCII string is never on the
    // cursor, and ascii_ is already in this cell.
    void forget_cursor() const noexcept { if (in_side_cache_) drop_cursor(); }
    void drop_cursor() const noexcept;
    [[nodiscard]] bool still_cached_elsewhere() const noexcept;
    void copy_bits(const String& o) noexcept {
        hash_ = o.hash_; interned_ = o.interned_; is_cons_ = o.is_cons_;
        is_tail_ = o.is_tail_; ascii_ = o.ascii_; utf16_len_ = o.utf16_len_;
    }
    void copy_link(const String& o) noexcept {
        if (is_tail_) tail_ = o.tail_; else cons_ = o.cons_;
    }
    void copy_from(const String& o) {
        copy_bits(o);
        if (is_cons_) copy_link(o); else new (&data_) std::string(o.data_);
    }
    void move_from(String&& o) noexcept {
        copy_bits(o);
        if (is_cons_) copy_link(o); else new (&data_) std::string(std::move(o.data_));
    }
    void calculate_hash() const noexcept;
    // Charges the bytes this cell holds outside the cell heap to the
    // collector's pacing -- see the definition.
    void note_payload_bytes() const;
    static void collect_bytes(const String* node, std::string& out);
    // Bytes of a link's inline tail, empty for every other form.
    std::string_view inline_tail() const noexcept {
        return is_tail_ ? std::string_view(tail_.bytes, tail_.len) : std::string_view();
    }

public:
    // Bytes a link can carry inline; see the tail_ member above.
    static constexpr size_t kInlineTail = 23;

    // GC cell protocol (see Object.h): heap strings are String-kind cells;
    // stack/value instances never touch these. Rope cons nodes are cells too.
    static void* operator new(size_t size);
    static void  operator delete(void* p) noexcept;
    static void* operator new[](size_t) = delete;
    static void  operator delete[](void*) = delete;

    String() { new (&data_) std::string(); }
    explicit String(const std::string& str);
    void gc_trace(class Visitor& v) const;
    explicit String(std::string&& str) noexcept;
    explicit String(std::string_view sv);
    explicit String(const char* str);
    // Link nodes — created only via make_concat
    String(String* left, String* right) noexcept : cons_{left, right} { is_cons_ = true; }
    String(String* left, const char* bytes, size_t len) noexcept {
        tail_.left = left;
        tail_.len = static_cast<uint8_t>(len);
        for (size_t i = 0; i < len; ++i) tail_.bytes[i] = bytes[i];
        is_cons_ = true;
        is_tail_ = true;
    }

    ~String() { forget_cursor(); if (!is_cons_) data_.~basic_string(); }

    String(const String& o) { copy_from(o); }
    String(String&& o) noexcept { move_from(std::move(o)); }
    String& operator=(const String& o) {
        if (this != &o) { destroy_payload(); copy_from(o); }
        return *this;
    }
    String& operator=(String&& o) noexcept {
        if (this != &o) { destroy_payload(); move_from(std::move(o)); }
        return *this;
    }

    [[nodiscard]] const std::string& str()   const noexcept { if (is_cons_) ensure_flat(); return data_; }
    [[nodiscard]] const char*        c_str() const noexcept { return str().c_str(); }
    [[nodiscard]] size_t             length()const noexcept { return str().length(); }
    [[nodiscard]] size_t             size()  const noexcept { return str().size(); }
    [[nodiscard]] bool               empty() const noexcept { return !is_cons_ && data_.empty(); }
    // Lazily computed. Hashing is a full pass over the bytes and most strings
    // are never used as a key, so the constructors no longer pay it up front --
    // a regex handing back its subject on every match was hashing the whole
    // subject each time. Zero doubles as "not computed yet"; a string that
    // genuinely hashes to zero simply recomputes, which costs nothing else.
    [[nodiscard]] size_t hash() const noexcept {
        if (!hash_) { if (is_cons_) ensure_flat(); else calculate_hash(); }
        return hash_;
    }
    [[nodiscard]] bool               interned() const noexcept { return interned_; }

    // One shared cell per single-byte character, instead of a fresh 64-byte
    // cell every time one is produced. Splitting, scanning or indexing a
    // string yields these by the million and they are all equal to one of 128
    // values; strings are immutable, so sharing one is not observable.
    //
    // Cells belong to the heap they were allocated from, so the table is
    // dropped whenever the active heap changes (a realm switch) rather than
    // handing one heap's cell to another. Traced as a root: nothing else
    // refers to an entry that is currently unused.
    static String* single_char(unsigned char c);
    static void gc_trace_roots(class Visitor& v);
    // What JS calls .length: UTF-16 code units, not bytes. Cached -- see
    // utf16_len_. Prefer this over the free utf16_length(std::string) below
    // whenever a String* is at hand; the free function has nowhere to cache
    // and its callers usually had to copy the buffer to call it.
    [[nodiscard]] size_t utf16_length() const;
    // True when a UTF-16 index and a byte index are the same thing here.
    [[nodiscard]] bool is_ascii() const;
    // The UTF-16 code unit at `index`, or -1 past the end. O(1) for a
    // single-byte string, where the free utf16_code_unit_at below has to
    // decode from the start every time -- which turns the ordinary
    // `for (i...) s.charCodeAt(i)` scan of a source file into quadratic work.
    [[nodiscard]] int32_t code_unit_at(size_t index) const;
    // Byte offset of UTF-16 index `index`, clamped to the end.
    [[nodiscard]] size_t byte_pos(size_t index) const;
    // The inverse. What a builtin that searched in BYTES has to hand back,
    // since JS answers in units -- and, like byte_pos, resumed from the cursor
    // rather than counted from the start of the buffer.
    [[nodiscard]] size_t index_of_byte(size_t byte) const;
    // The bytes for UTF-16 code units [start, end). A boundary may fall
    // BETWEEN the halves of a surrogate pair -- `"\u{1F4A9}".slice(0, 1)` is
    // the leading surrogate alone -- which no byte offset can express, so this
    // emits the lone half rather than mapping to a position and cutting there.
    [[nodiscard]] std::string substring_utf16(size_t start, size_t end) const;
    // The whole cell decoded to UTF-16, remembered for this cell rather than
    // recomputed or re-recognised: a regex driven one exec per match hands the
    // same subject in over and over, and comparing the bytes to notice that is
    // itself a walk of the subject. `id` (never 0, never reused for different
    // text) lets a caller key its own derived tables on the same answer.
    [[nodiscard]] const std::u16string& utf16_units(uint64_t* id = nullptr) const;

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
// The bytes for UTF-16 code units [start, end) of an unowned buffer. Unlike a
// pair of utf16_index_to_byte_pos calls this can express a boundary that falls
// between the halves of a surrogate pair, which has no byte offset at all --
// see String::substring_utf16.
std::string utf16_substring(const std::string& utf8, size_t start, size_t end);
// The same walk, resumed from a boundary the caller already knows and leaving
// the boundary it stopped on behind -- see String::substring_utf16.
std::string utf16_substring_from(const std::string& utf8, size_t start, size_t end,
                                 size_t& pos, size_t& units);
// The inverse: how many UTF-16 units precede a byte position. What a builtin
// that searched in bytes has to hand back, since JS indices are units.
size_t utf16_index_from_byte_pos(const std::string& utf8, size_t byte_pos);
// True when every byte is below 0x80, in which case byte offsets and UTF-16
// indices coincide and neither conversion above has to run.
bool utf8_is_ascii(const std::string& utf8);
bool utf16_is_well_formed(const std::string& utf8);
std::string utf16_to_well_formed(const std::string& utf8);

}

#endif
