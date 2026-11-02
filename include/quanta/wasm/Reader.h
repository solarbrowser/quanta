/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_READER_H
#define QUANTA_WASM_READER_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "quanta/wasm/Types.h"

namespace Quanta::Wasm {

// A cursor over bytes that is told at the first thing that is wrong, and after that reads zeros.
class Reader {
public:
    Reader(const uint8_t* data, size_t size, size_t base = 0) : begin_(data), pos_(data), end_(data + size), base_(base) {}

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    bool atEnd() const { return pos_ >= end_; }
    size_t remaining() const { return static_cast<size_t>(end_ - pos_); }
    // The offset in the whole module, for an error to name.
    size_t offset() const { return base_ + static_cast<size_t>(pos_ - begin_); }
    const uint8_t* cursor() const { return pos_; }

    void fail(const std::string& message) {
        if (error_.empty()) error_ = message + " @+" + std::to_string(offset());
    }

    uint8_t u8() {
        if (pos_ >= end_) { fail("unexpected end"); return 0; }
        return *pos_++;
    }
    uint8_t peek() {
        if (pos_ >= end_) { fail("unexpected end"); return 0; }
        return *pos_;
    }
    const uint8_t* bytes(size_t n) {
        if (remaining() < n) { fail("unexpected end"); pos_ = end_; return nullptr; }
        const uint8_t* at = pos_;
        pos_ += n;
        return at;
    }
    void skip(size_t n) { bytes(n); }

    uint32_t u32() {
        uint32_t result = 0;
        for (int i = 0; i < 5; ++i) {
            const uint8_t b = u8();
            if (!ok()) return 0;
            result |= static_cast<uint32_t>(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) {
                if (i == 4 && (b & 0x70)) { fail("integer too large"); return 0; }
                return result;
            }
            if (i == 4) { fail("integer representation too long"); return 0; }
        }
        return result;
    }
    int32_t s32() {
        uint32_t result = 0;
        for (int i = 0; i < 5; ++i) {
            const uint8_t b = u8();
            if (!ok()) return 0;
            result |= static_cast<uint32_t>(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) {
                if (i == 4) {
                    const uint8_t high = b & 0x78;  // (the bits above the 32nd must be the sign)
                    if (high != 0 && high != 0x78) { fail("integer too large"); return 0; }
                } else if (b & 0x40) {
                    result |= ~uint32_t(0) << (7 * (i + 1));
                }
                return static_cast<int32_t>(result);
            }
            if (i == 4) { fail("integer representation too long"); return 0; }
        }
        return static_cast<int32_t>(result);
    }
    // A 33-bit signed number: the index of a type, or a block type.
    int64_t s33() {
        uint64_t result = 0;
        for (int i = 0; i < 5; ++i) {
            const uint8_t b = u8();
            if (!ok()) return 0;
            result |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) {
                if (i == 4) {
                    const uint8_t high = b & 0x70;
                    if (high != 0 && high != 0x70) { fail("integer too large"); return 0; }
                    if (b & 0x10) result |= ~uint64_t(0) << 35;
                } else if (b & 0x40) {
                    result |= ~uint64_t(0) << (7 * (i + 1));
                }
                return static_cast<int64_t>(result);
            }
            if (i == 4) { fail("integer representation too long"); return 0; }
        }
        return static_cast<int64_t>(result);
    }
    uint64_t u64() {
        uint64_t result = 0;
        for (int i = 0; i < 10; ++i) {
            const uint8_t b = u8();
            if (!ok()) return 0;
            result |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) {
                if (i == 9 && (b & 0x7E)) { fail("integer too large"); return 0; }
                return result;
            }
            if (i == 9) { fail("integer representation too long"); return 0; }
        }
        return result;
    }
    int64_t s64() {
        uint64_t result = 0;
        for (int i = 0; i < 10; ++i) {
            const uint8_t b = u8();
            if (!ok()) return 0;
            result |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) {
                if (i == 9) {
                    if (b != 0x00 && b != 0x7F) { fail("integer too large"); return 0; }
                } else if (b & 0x40) {
                    result |= ~uint64_t(0) << (7 * (i + 1));
                }
                return static_cast<int64_t>(result);
            }
            if (i == 9) { fail("integer representation too long"); return 0; }
        }
        return static_cast<int64_t>(result);
    }
    uint32_t fixed32() {
        const uint8_t* p = bytes(4);
        uint32_t v = 0;
        if (p) std::memcpy(&v, p, 4);
        return v;
    }
    uint64_t fixed64() {
        const uint8_t* p = bytes(8);
        uint64_t v = 0;
        if (p) std::memcpy(&v, p, 8);
        return v;
    }

    // A heap type: an abstract one by its code, or the index of a defined type.
    HeapType heapType() {
        if (pos_ >= end_) { fail("unexpected end"); return 0; }
        switch (*pos_) {
            case 0x70: ++pos_; return Heap::Func;
            case 0x6F: ++pos_; return Heap::Extern;
            case 0x6E: ++pos_; return Heap::Any;
            case 0x6D: ++pos_; return Heap::Eq;
            case 0x6C: ++pos_; return Heap::I31;
            case 0x6B: ++pos_; return Heap::Struct;
            case 0x6A: ++pos_; return Heap::Array;
            case 0x69: ++pos_; return Heap::Exn;
            case 0x71: ++pos_; return Heap::None;
            case 0x72: ++pos_; return Heap::NoExtern;
            case 0x73: ++pos_; return Heap::NoFunc;
            case 0x74: ++pos_; return Heap::NoExn;
            default: break;
        }
        const int64_t index = s33();
        if (!ok()) return 0;
        if (index < 0 || index > 0x7FFFFFFF) { fail("malformed heap type"); return 0; }
        return static_cast<HeapType>(index);
    }

    ValType valType() {
        const uint8_t code = u8();
        if (!ok()) return ValType::Bottom();
        switch (code) {
            case 0x7F: return ValType::I32();
            case 0x7E: return ValType::I64();
            case 0x7D: return ValType::F32();
            case 0x7C: return ValType::F64();
            case 0x7B: return ValType::V128();
            case 0x63: { const HeapType h = heapType(); return ValType::Ref(true, h); }
            case 0x64: { const HeapType h = heapType(); return ValType::Ref(false, h); }
            case 0x70: return ValType::Ref(true, Heap::Func);
            case 0x6F: return ValType::Ref(true, Heap::Extern);
            case 0x6E: return ValType::Ref(true, Heap::Any);
            case 0x6D: return ValType::Ref(true, Heap::Eq);
            case 0x6C: return ValType::Ref(true, Heap::I31);
            case 0x6B: return ValType::Ref(true, Heap::Struct);
            case 0x6A: return ValType::Ref(true, Heap::Array);
            case 0x69: return ValType::Ref(true, Heap::Exn);
            case 0x71: return ValType::Ref(true, Heap::None);
            case 0x72: return ValType::Ref(true, Heap::NoExtern);
            case 0x73: return ValType::Ref(true, Heap::NoFunc);
            case 0x74: return ValType::Ref(true, Heap::NoExn);
            default: fail("malformed value type"); return ValType::Bottom();
        }
    }

private:
    const uint8_t* begin_;
    const uint8_t* pos_;
    const uint8_t* end_;
    size_t base_;
    std::string error_;
};

// Whether the bytes are well-formed UTF-8 (no surrogates, no overlong forms, nothing past U+10FFFF).
inline bool ValidUtf8(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = s[i];
        if (c < 0x80) { ++i; continue; }
        size_t extra;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { extra = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { extra = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { extra = 3; cp = c & 0x07; }
        else return false;
        if (i + extra >= n) return false;
        for (size_t k = 1; k <= extra; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (extra == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
        if (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
        i += extra + 1;
    }
    return true;
}

}  // namespace Quanta::Wasm

#endif
