/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The vector instructions that work on the stack alone (the loads and stores are the interpreter's). A vector is two slots, the low half
// first, which is the order the bytes of a lane have in memory.

#include "Simd.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Quanta::Wasm {

namespace {

struct V {
    uint8_t b[16];
};

template <class T>
inline T Get(const V& v, int i) {
    T x;
    std::memcpy(&x, v.b + i * sizeof(T), sizeof(T));
    return x;
}
template <class T>
inline void Set(V& v, int i, T x) {
    std::memcpy(v.b + i * sizeof(T), &x, sizeof(T));
}

inline V Load(const uint64_t* p) {
    V v;
    std::memcpy(v.b, p, 16);
    return v;
}
inline void Store(uint64_t* p, const V& v) { std::memcpy(p, v.b, 16); }

template <class T, class F>
V Un(const V& a, F f) {
    V r;
    for (size_t i = 0; i < 16 / sizeof(T); ++i) Set<T>(r, static_cast<int>(i), f(Get<T>(a, static_cast<int>(i))));
    return r;
}
template <class T, class F>
V Bin(const V& a, const V& b, F f) {
    V r;
    for (size_t i = 0; i < 16 / sizeof(T); ++i) Set<T>(r, static_cast<int>(i), f(Get<T>(a, static_cast<int>(i)), Get<T>(b, static_cast<int>(i))));
    return r;
}
// A comparison: each lane is all ones or all zeros, of the width of the lane.
template <class T, class M, class F>
V Cmp(const V& a, const V& b, F f) {
    V r;
    for (size_t i = 0; i < 16 / sizeof(T); ++i) Set<M>(r, static_cast<int>(i), f(Get<T>(a, static_cast<int>(i)), Get<T>(b, static_cast<int>(i))) ? static_cast<M>(~M(0)) : M(0));
    return r;
}
template <class T, class F>
V Shift(const V& a, uint32_t count, F f) {
    V r;
    const uint32_t c = count & (sizeof(T) * 8 - 1);
    for (size_t i = 0; i < 16 / sizeof(T); ++i) Set<T>(r, static_cast<int>(i), f(Get<T>(a, static_cast<int>(i)), c));
    return r;
}

template <class T>
T SatAdd(T a, T b) {
    T r;
    if (!__builtin_add_overflow(a, b, &r)) return r;
    return std::is_signed_v<T> ? (b < 0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max()) : std::numeric_limits<T>::max();
}
template <class T>
T SatSub(T a, T b) {
    T r;
    if (!__builtin_sub_overflow(a, b, &r)) return r;
    return std::is_signed_v<T> ? (b > 0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max()) : std::numeric_limits<T>::min();
}

template <class F>
F WMin(F a, F b) {
    if (a != a || b != b) return a + b;
    if (a == b) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}
template <class F>
F WMax(F a, F b) {
    if (a != a || b != b) return a + b;
    if (a == b) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
template <class F>
F PMin(F a, F b) { return b < a ? b : a; }
template <class F>
F PMax(F a, F b) { return a < b ? b : a; }

template <class F>
F AbsBits(F f);
template <>
float AbsBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); b &= 0x7FFFFFFFu; std::memcpy(&f, &b, 4); return f; }
template <>
double AbsBits(double f) { uint64_t b; std::memcpy(&b, &f, 8); b &= 0x7FFFFFFFFFFFFFFFull; std::memcpy(&f, &b, 8); return f; }
template <class F>
F NegBits(F f);
template <>
float NegBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); b ^= 0x80000000u; std::memcpy(&f, &b, 4); return f; }
template <>
double NegBits(double f) { uint64_t b; std::memcpy(&b, &f, 8); b ^= 0x8000000000000000ull; std::memcpy(&f, &b, 8); return f; }

template <class I, class F>
I SatTrunc(F f) {
    if (f != f) return 0;
    if (f <= static_cast<F>(std::numeric_limits<I>::min())) return std::numeric_limits<I>::min();
    if (f >= static_cast<F>(std::numeric_limits<I>::max())) return std::numeric_limits<I>::max();
    return static_cast<I>(f);
}

template <class Narrow, class Wide>
Narrow Clamp(Wide w) {
    if (w < static_cast<Wide>(std::numeric_limits<Narrow>::min())) return std::numeric_limits<Narrow>::min();
    if (w > static_cast<Wide>(std::numeric_limits<Narrow>::max())) return std::numeric_limits<Narrow>::max();
    return static_cast<Narrow>(w);
}

// narrow: the lanes of the first vector, then those of the second, each clamped to the narrower lane.
template <class Narrow, class Wide>
V Narrow2(const V& a, const V& b) {
    V r;
    const int n = static_cast<int>(16 / sizeof(Wide));
    for (int i = 0; i < n; ++i) Set<Narrow>(r, i, Clamp<Narrow, Wide>(Get<Wide>(a, i)));
    for (int i = 0; i < n; ++i) Set<Narrow>(r, n + i, Clamp<Narrow, Wide>(Get<Wide>(b, i)));
    return r;
}

template <class Wide, class Narrow>
V Extend(const V& a, bool high) {
    V r;
    const int n = static_cast<int>(16 / sizeof(Wide));
    for (int i = 0; i < n; ++i) Set<Wide>(r, i, static_cast<Wide>(Get<Narrow>(a, (high ? n : 0) + i)));
    return r;
}

template <class Wide, class Narrow>
V ExtMul(const V& a, const V& b, bool high) {
    V r;
    const int n = static_cast<int>(16 / sizeof(Wide));
    for (int i = 0; i < n; ++i) Set<Wide>(r, i, static_cast<Wide>(static_cast<Wide>(Get<Narrow>(a, (high ? n : 0) + i)) * static_cast<Wide>(Get<Narrow>(b, (high ? n : 0) + i))));
    return r;
}

template <class Wide, class Narrow>
V ExtAddPairwise(const V& a) {
    V r;
    const int n = static_cast<int>(16 / sizeof(Wide));
    for (int i = 0; i < n; ++i) Set<Wide>(r, i, static_cast<Wide>(static_cast<Wide>(Get<Narrow>(a, 2 * i)) + static_cast<Wide>(Get<Narrow>(a, 2 * i + 1))));
    return r;
}

template <class T>
uint32_t Bitmask(const V& a) {
    uint32_t m = 0;
    for (size_t i = 0; i < 16 / sizeof(T); ++i) if (Get<T>(a, static_cast<int>(i)) < 0) m |= 1u << i;
    return m;
}
template <class T>
uint32_t AllTrue(const V& a) {
    for (size_t i = 0; i < 16 / sizeof(T); ++i) if (Get<T>(a, static_cast<int>(i)) == 0) return 0;
    return 1;
}

V Splat(uint32_t sub, uint64_t x) {
    V r;
    switch (sub) {
        case 0x0F: for (int i = 0; i < 16; ++i) Set<uint8_t>(r, i, static_cast<uint8_t>(x)); break;
        case 0x10: for (int i = 0; i < 8; ++i) Set<uint16_t>(r, i, static_cast<uint16_t>(x)); break;
        case 0x11: case 0x13: for (int i = 0; i < 4; ++i) Set<uint32_t>(r, i, static_cast<uint32_t>(x)); break;
        default: for (int i = 0; i < 2; ++i) Set<uint64_t>(r, i, x); break;
    }
    return r;
}

V Const(uint64_t lo, uint64_t hi) {
    V r;
    std::memcpy(r.b, &lo, 8);
    std::memcpy(r.b + 8, &hi, 8);
    return r;
}

}  // namespace

uint32_t SimdImmediateWords(uint32_t sub) {
    if (sub >= 0x15 && sub <= 0x22) return 1;
    if (sub == 0x0D) return 4;
    return 0;
}

uint64_t* SimdExec(uint32_t sub, uint64_t* sp, const uint32_t* imm) {
    // The stack effect of the group the instruction is in.
    enum Shape { Unary, Binary, Ternary, ShiftOp, Test, SplatOp, Extract, Replace, ShuffleOp };
    Shape shape = Binary;
    if (sub == 0x0D) shape = ShuffleOp;
    else if (sub >= 0x0F && sub <= 0x14) shape = SplatOp;
    else if (sub >= 0x15 && sub <= 0x22) shape = ((sub == 0x17 || sub == 0x1A || sub == 0x1C || sub == 0x1E || sub == 0x20 || sub == 0x22) ? Replace : Extract);
    else if (sub == 0x4D || sub == 0x5E || sub == 0x5F || (sub >= 0x60 && sub <= 0x62) || (sub >= 0x67 && sub <= 0x6A) || sub == 0x74 || sub == 0x75 || sub == 0x7A || (sub >= 0x7C && sub <= 0x7F) ||
             sub == 0x80 || sub == 0x81 || (sub >= 0x87 && sub <= 0x8A) || sub == 0x94 || sub == 0xA0 || sub == 0xA1 || (sub >= 0xA7 && sub <= 0xAA) || sub == 0xC0 || sub == 0xC1 ||
             (sub >= 0xC7 && sub <= 0xCA) || sub == 0xE0 || sub == 0xE1 || sub == 0xE3 || sub == 0xEC || sub == 0xED || sub == 0xEF || (sub >= 0xF8 && sub <= 0xFF) || (sub >= 0x101 && sub <= 0x104))
        shape = Unary;
    else if (sub == 0x52 || (sub >= 0x105 && sub <= 0x10C) || sub == 0x113) shape = Ternary;
    else if ((sub >= 0x6B && sub <= 0x6D) || (sub >= 0x8B && sub <= 0x8D) || (sub >= 0xAB && sub <= 0xAD) || (sub >= 0xCB && sub <= 0xCD)) shape = ShiftOp;
    else if (sub == 0x53 || sub == 0x63 || sub == 0x64 || sub == 0x83 || sub == 0x84 || sub == 0xA3 || sub == 0xA4 || sub == 0xC3 || sub == 0xC4) shape = Test;

    V a, b, c, r;
    uint64_t* base = sp;
    uint32_t scalar = 0;
    uint64_t wide = 0;
    switch (shape) {
        case Unary: base = sp - 2; a = Load(base); break;
        case Binary: base = sp - 4; a = Load(base); b = Load(base + 2); break;
        case Ternary: base = sp - 6; a = Load(base); b = Load(base + 2); c = Load(base + 4); break;
        case ShiftOp: scalar = static_cast<uint32_t>(sp[-1]); base = sp - 3; a = Load(base); break;
        case Test: base = sp - 2; a = Load(base); break;
        case SplatOp: wide = sp[-1]; base = sp - 1; break;
        case Extract: base = sp - 2; a = Load(base); break;
        case Replace: wide = sp[-1]; base = sp - 3; a = Load(base); break;
        case ShuffleOp: base = sp - 4; a = Load(base); b = Load(base + 2); break;
    }
    const uint32_t lane = imm ? imm[0] : 0;
    bool produced = true;  // (false: the result is a scalar, in `wide`)
    wide = shape == SplatOp || shape == Replace ? wide : 0;
    bool scalarResult = false;

    switch (sub) {
        // ---- Shuffles and lanes ----
        case 0x0D: {
            uint8_t sel[16];
            std::memcpy(sel, imm, 16);
            uint8_t both[32];
            std::memcpy(both, a.b, 16);
            std::memcpy(both + 16, b.b, 16);
            for (int i = 0; i < 16; ++i) r.b[i] = both[sel[i] & 31];
            break;
        }
        case 0x0E: case 0x100:  // swizzle
            for (int i = 0; i < 16; ++i) r.b[i] = b.b[i] < 16 ? a.b[b.b[i]] : 0;
            break;
        case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: r = Splat(sub, wide); break;
        case 0x15: scalarResult = true; wide = static_cast<uint32_t>(static_cast<int32_t>(Get<int8_t>(a, lane))); break;
        case 0x16: scalarResult = true; wide = Get<uint8_t>(a, lane); break;
        case 0x18: scalarResult = true; wide = static_cast<uint32_t>(static_cast<int32_t>(Get<int16_t>(a, lane))); break;
        case 0x19: scalarResult = true; wide = Get<uint16_t>(a, lane); break;
        case 0x1B: case 0x1F: scalarResult = true; wide = Get<uint32_t>(a, lane); break;
        case 0x1D: case 0x21: scalarResult = true; wide = Get<uint64_t>(a, lane); break;
        case 0x17: r = a; Set<uint8_t>(r, lane, static_cast<uint8_t>(wide)); break;
        case 0x1A: r = a; Set<uint16_t>(r, lane, static_cast<uint16_t>(wide)); break;
        case 0x1C: case 0x20: r = a; Set<uint32_t>(r, lane, static_cast<uint32_t>(wide)); break;
        case 0x1E: case 0x22: r = a; Set<uint64_t>(r, lane, wide); break;

        // ---- Comparisons ----
        case 0x23: r = Cmp<uint8_t, uint8_t>(a, b, [](uint8_t x, uint8_t y) { return x == y; }); break;
        case 0x24: r = Cmp<uint8_t, uint8_t>(a, b, [](uint8_t x, uint8_t y) { return x != y; }); break;
        case 0x25: r = Cmp<int8_t, uint8_t>(a, b, [](int8_t x, int8_t y) { return x < y; }); break;
        case 0x26: r = Cmp<uint8_t, uint8_t>(a, b, [](uint8_t x, uint8_t y) { return x < y; }); break;
        case 0x27: r = Cmp<int8_t, uint8_t>(a, b, [](int8_t x, int8_t y) { return x > y; }); break;
        case 0x28: r = Cmp<uint8_t, uint8_t>(a, b, [](uint8_t x, uint8_t y) { return x > y; }); break;
        case 0x29: r = Cmp<int8_t, uint8_t>(a, b, [](int8_t x, int8_t y) { return x <= y; }); break;
        case 0x2A: r = Cmp<uint8_t, uint8_t>(a, b, [](uint8_t x, uint8_t y) { return x <= y; }); break;
        case 0x2B: r = Cmp<int8_t, uint8_t>(a, b, [](int8_t x, int8_t y) { return x >= y; }); break;
        case 0x2C: r = Cmp<uint8_t, uint8_t>(a, b, [](uint8_t x, uint8_t y) { return x >= y; }); break;
        case 0x2D: r = Cmp<uint16_t, uint16_t>(a, b, [](uint16_t x, uint16_t y) { return x == y; }); break;
        case 0x2E: r = Cmp<uint16_t, uint16_t>(a, b, [](uint16_t x, uint16_t y) { return x != y; }); break;
        case 0x2F: r = Cmp<int16_t, uint16_t>(a, b, [](int16_t x, int16_t y) { return x < y; }); break;
        case 0x30: r = Cmp<uint16_t, uint16_t>(a, b, [](uint16_t x, uint16_t y) { return x < y; }); break;
        case 0x31: r = Cmp<int16_t, uint16_t>(a, b, [](int16_t x, int16_t y) { return x > y; }); break;
        case 0x32: r = Cmp<uint16_t, uint16_t>(a, b, [](uint16_t x, uint16_t y) { return x > y; }); break;
        case 0x33: r = Cmp<int16_t, uint16_t>(a, b, [](int16_t x, int16_t y) { return x <= y; }); break;
        case 0x34: r = Cmp<uint16_t, uint16_t>(a, b, [](uint16_t x, uint16_t y) { return x <= y; }); break;
        case 0x35: r = Cmp<int16_t, uint16_t>(a, b, [](int16_t x, int16_t y) { return x >= y; }); break;
        case 0x36: r = Cmp<uint16_t, uint16_t>(a, b, [](uint16_t x, uint16_t y) { return x >= y; }); break;
        case 0x37: r = Cmp<uint32_t, uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x == y; }); break;
        case 0x38: r = Cmp<uint32_t, uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x != y; }); break;
        case 0x39: r = Cmp<int32_t, uint32_t>(a, b, [](int32_t x, int32_t y) { return x < y; }); break;
        case 0x3A: r = Cmp<uint32_t, uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x < y; }); break;
        case 0x3B: r = Cmp<int32_t, uint32_t>(a, b, [](int32_t x, int32_t y) { return x > y; }); break;
        case 0x3C: r = Cmp<uint32_t, uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x > y; }); break;
        case 0x3D: r = Cmp<int32_t, uint32_t>(a, b, [](int32_t x, int32_t y) { return x <= y; }); break;
        case 0x3E: r = Cmp<uint32_t, uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x <= y; }); break;
        case 0x3F: r = Cmp<int32_t, uint32_t>(a, b, [](int32_t x, int32_t y) { return x >= y; }); break;
        case 0x40: r = Cmp<uint32_t, uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x >= y; }); break;
        case 0x41: r = Cmp<float, uint32_t>(a, b, [](float x, float y) { return x == y; }); break;
        case 0x42: r = Cmp<float, uint32_t>(a, b, [](float x, float y) { return x != y; }); break;
        case 0x43: r = Cmp<float, uint32_t>(a, b, [](float x, float y) { return x < y; }); break;
        case 0x44: r = Cmp<float, uint32_t>(a, b, [](float x, float y) { return x > y; }); break;
        case 0x45: r = Cmp<float, uint32_t>(a, b, [](float x, float y) { return x <= y; }); break;
        case 0x46: r = Cmp<float, uint32_t>(a, b, [](float x, float y) { return x >= y; }); break;
        case 0x47: r = Cmp<double, uint64_t>(a, b, [](double x, double y) { return x == y; }); break;
        case 0x48: r = Cmp<double, uint64_t>(a, b, [](double x, double y) { return x != y; }); break;
        case 0x49: r = Cmp<double, uint64_t>(a, b, [](double x, double y) { return x < y; }); break;
        case 0x4A: r = Cmp<double, uint64_t>(a, b, [](double x, double y) { return x > y; }); break;
        case 0x4B: r = Cmp<double, uint64_t>(a, b, [](double x, double y) { return x <= y; }); break;
        case 0x4C: r = Cmp<double, uint64_t>(a, b, [](double x, double y) { return x >= y; }); break;
        case 0xD6: r = Cmp<int64_t, uint64_t>(a, b, [](int64_t x, int64_t y) { return x == y; }); break;
        case 0xD7: r = Cmp<int64_t, uint64_t>(a, b, [](int64_t x, int64_t y) { return x != y; }); break;
        case 0xD8: r = Cmp<int64_t, uint64_t>(a, b, [](int64_t x, int64_t y) { return x < y; }); break;
        case 0xD9: r = Cmp<int64_t, uint64_t>(a, b, [](int64_t x, int64_t y) { return x > y; }); break;
        case 0xDA: r = Cmp<int64_t, uint64_t>(a, b, [](int64_t x, int64_t y) { return x <= y; }); break;
        case 0xDB: r = Cmp<int64_t, uint64_t>(a, b, [](int64_t x, int64_t y) { return x >= y; }); break;

        // ---- Bits ----
        case 0x4D: r = Un<uint64_t>(a, [](uint64_t x) { return ~x; }); break;
        case 0x4E: r = Bin<uint64_t>(a, b, [](uint64_t x, uint64_t y) { return x & y; }); break;
        case 0x4F: r = Bin<uint64_t>(a, b, [](uint64_t x, uint64_t y) { return x & ~y; }); break;
        case 0x50: r = Bin<uint64_t>(a, b, [](uint64_t x, uint64_t y) { return x | y; }); break;
        case 0x51: r = Bin<uint64_t>(a, b, [](uint64_t x, uint64_t y) { return x ^ y; }); break;
        case 0x52: case 0x109: case 0x10A: case 0x10B: case 0x10C:  // bitselect: the bits of a where c is 1, of b where it is 0
            for (int i = 0; i < 2; ++i) Set<uint64_t>(r, i, (Get<uint64_t>(a, i) & Get<uint64_t>(c, i)) | (Get<uint64_t>(b, i) & ~Get<uint64_t>(c, i)));
            break;
        case 0x53: scalarResult = true; wide = (Get<uint64_t>(a, 0) | Get<uint64_t>(a, 1)) != 0; break;

        // ---- Conversions of floats ----
        case 0x5E:  // f32x4.demote_f64x2_zero
            Set<float>(r, 0, static_cast<float>(Get<double>(a, 0)));
            Set<float>(r, 1, static_cast<float>(Get<double>(a, 1)));
            Set<float>(r, 2, 0.f);
            Set<float>(r, 3, 0.f);
            break;
        case 0x5F:  // f64x2.promote_low_f32x4
            Set<double>(r, 0, static_cast<double>(Get<float>(a, 0)));
            Set<double>(r, 1, static_cast<double>(Get<float>(a, 1)));
            break;

        // ---- i8x16 ----
        case 0x60: r = Un<int8_t>(a, [](int8_t x) { return static_cast<int8_t>(x < 0 ? -x : x); }); break;
        case 0x61: r = Un<uint8_t>(a, [](uint8_t x) { return static_cast<uint8_t>(0 - x); }); break;
        case 0x62: r = Un<uint8_t>(a, [](uint8_t x) { return static_cast<uint8_t>(__builtin_popcount(x)); }); break;
        case 0x63: scalarResult = true; wide = AllTrue<uint8_t>(a); break;
        case 0x64: scalarResult = true; wide = Bitmask<int8_t>(a); break;
        case 0x65: r = Narrow2<int8_t, int16_t>(a, b); break;
        case 0x66: r = Narrow2<uint8_t, int16_t>(a, b); break;
        case 0x6B: r = Shift<uint8_t>(a, scalar, [](uint8_t x, uint32_t c) { return static_cast<uint8_t>(x << c); }); break;
        case 0x6C: r = Shift<int8_t>(a, scalar, [](int8_t x, uint32_t c) { return static_cast<int8_t>(x >> c); }); break;
        case 0x6D: r = Shift<uint8_t>(a, scalar, [](uint8_t x, uint32_t c) { return static_cast<uint8_t>(x >> c); }); break;
        case 0x6E: r = Bin<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + y); }); break;
        case 0x6F: r = Bin<int8_t>(a, b, [](int8_t x, int8_t y) { return SatAdd<int8_t>(x, y); }); break;
        case 0x70: r = Bin<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return SatAdd<uint8_t>(x, y); }); break;
        case 0x71: r = Bin<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x - y); }); break;
        case 0x72: r = Bin<int8_t>(a, b, [](int8_t x, int8_t y) { return SatSub<int8_t>(x, y); }); break;
        case 0x73: r = Bin<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return SatSub<uint8_t>(x, y); }); break;
        case 0x76: r = Bin<int8_t>(a, b, [](int8_t x, int8_t y) { return std::min(x, y); }); break;
        case 0x77: r = Bin<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return std::min(x, y); }); break;
        case 0x78: r = Bin<int8_t>(a, b, [](int8_t x, int8_t y) { return std::max(x, y); }); break;
        case 0x79: r = Bin<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return std::max(x, y); }); break;
        case 0x7B: r = Bin<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return static_cast<uint8_t>((static_cast<uint32_t>(x) + y + 1) >> 1); }); break;

        // ---- Floats: rounding ----
        case 0x67: r = Un<float>(a, [](float x) { return std::ceil(x); }); break;
        case 0x68: r = Un<float>(a, [](float x) { return std::floor(x); }); break;
        case 0x69: r = Un<float>(a, [](float x) { return std::trunc(x); }); break;
        case 0x6A: r = Un<float>(a, [](float x) { return std::nearbyint(x); }); break;
        case 0x74: r = Un<double>(a, [](double x) { return std::ceil(x); }); break;
        case 0x75: r = Un<double>(a, [](double x) { return std::floor(x); }); break;
        case 0x7A: r = Un<double>(a, [](double x) { return std::trunc(x); }); break;
        case 0x94: r = Un<double>(a, [](double x) { return std::nearbyint(x); }); break;

        // ---- i16x8 ----
        case 0x7C: r = ExtAddPairwise<int16_t, int8_t>(a); break;
        case 0x7D: r = ExtAddPairwise<uint16_t, uint8_t>(a); break;
        case 0x80: r = Un<int16_t>(a, [](int16_t x) { return static_cast<int16_t>(x < 0 ? -x : x); }); break;
        case 0x81: r = Un<uint16_t>(a, [](uint16_t x) { return static_cast<uint16_t>(0 - x); }); break;
        case 0x82: case 0x111:  // q15mulr_sat_s
            r = Bin<int16_t>(a, b, [](int16_t x, int16_t y) { return Clamp<int16_t, int32_t>((static_cast<int32_t>(x) * y + 0x4000) >> 15); });
            break;
        case 0x83: scalarResult = true; wide = AllTrue<uint16_t>(a); break;
        case 0x84: scalarResult = true; wide = Bitmask<int16_t>(a); break;
        case 0x85: r = Narrow2<int16_t, int32_t>(a, b); break;
        case 0x86: r = Narrow2<uint16_t, int32_t>(a, b); break;
        case 0x87: r = Extend<int16_t, int8_t>(a, false); break;
        case 0x88: r = Extend<int16_t, int8_t>(a, true); break;
        case 0x89: r = Extend<uint16_t, uint8_t>(a, false); break;
        case 0x8A: r = Extend<uint16_t, uint8_t>(a, true); break;
        case 0x8B: r = Shift<uint16_t>(a, scalar, [](uint16_t x, uint32_t c) { return static_cast<uint16_t>(x << c); }); break;
        case 0x8C: r = Shift<int16_t>(a, scalar, [](int16_t x, uint32_t c) { return static_cast<int16_t>(x >> c); }); break;
        case 0x8D: r = Shift<uint16_t>(a, scalar, [](uint16_t x, uint32_t c) { return static_cast<uint16_t>(x >> c); }); break;
        case 0x8E: r = Bin<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x + y); }); break;
        case 0x8F: r = Bin<int16_t>(a, b, [](int16_t x, int16_t y) { return SatAdd<int16_t>(x, y); }); break;
        case 0x90: r = Bin<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return SatAdd<uint16_t>(x, y); }); break;
        case 0x91: r = Bin<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x - y); }); break;
        case 0x92: r = Bin<int16_t>(a, b, [](int16_t x, int16_t y) { return SatSub<int16_t>(x, y); }); break;
        case 0x93: r = Bin<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return SatSub<uint16_t>(x, y); }); break;
        case 0x95: r = Bin<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>(x * y); }); break;
        case 0x96: r = Bin<int16_t>(a, b, [](int16_t x, int16_t y) { return std::min(x, y); }); break;
        case 0x97: r = Bin<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return std::min(x, y); }); break;
        case 0x98: r = Bin<int16_t>(a, b, [](int16_t x, int16_t y) { return std::max(x, y); }); break;
        case 0x99: r = Bin<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return std::max(x, y); }); break;
        case 0x9B: r = Bin<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return static_cast<uint16_t>((static_cast<uint32_t>(x) + y + 1) >> 1); }); break;
        case 0x9C: r = ExtMul<int16_t, int8_t>(a, b, false); break;
        case 0x9D: r = ExtMul<int16_t, int8_t>(a, b, true); break;
        case 0x9E: r = ExtMul<uint16_t, uint8_t>(a, b, false); break;
        case 0x9F: r = ExtMul<uint16_t, uint8_t>(a, b, true); break;

        // ---- i32x4 ----
        case 0x7E: r = ExtAddPairwise<int32_t, int16_t>(a); break;
        case 0x7F: r = ExtAddPairwise<uint32_t, uint16_t>(a); break;
        case 0xA0: r = Un<int32_t>(a, [](int32_t x) { return x < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(x)) : x; }); break;
        case 0xA1: r = Un<uint32_t>(a, [](uint32_t x) { return 0u - x; }); break;
        case 0xA3: scalarResult = true; wide = AllTrue<uint32_t>(a); break;
        case 0xA4: scalarResult = true; wide = Bitmask<int32_t>(a); break;
        case 0xA7: r = Extend<int32_t, int16_t>(a, false); break;
        case 0xA8: r = Extend<int32_t, int16_t>(a, true); break;
        case 0xA9: r = Extend<uint32_t, uint16_t>(a, false); break;
        case 0xAA: r = Extend<uint32_t, uint16_t>(a, true); break;
        case 0xAB: r = Shift<uint32_t>(a, scalar, [](uint32_t x, uint32_t c) { return x << c; }); break;
        case 0xAC: r = Shift<int32_t>(a, scalar, [](int32_t x, uint32_t c) { return x >> c; }); break;
        case 0xAD: r = Shift<uint32_t>(a, scalar, [](uint32_t x, uint32_t c) { return x >> c; }); break;
        case 0xAE: r = Bin<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x + y; }); break;
        case 0xB1: r = Bin<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x - y; }); break;
        case 0xB5: r = Bin<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x * y; }); break;
        case 0xB6: r = Bin<int32_t>(a, b, [](int32_t x, int32_t y) { return std::min(x, y); }); break;
        case 0xB7: r = Bin<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return std::min(x, y); }); break;
        case 0xB8: r = Bin<int32_t>(a, b, [](int32_t x, int32_t y) { return std::max(x, y); }); break;
        case 0xB9: r = Bin<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return std::max(x, y); }); break;
        case 0xBA:  // i32x4.dot_i16x8_s
            for (int i = 0; i < 4; ++i) {
                const int64_t sum = static_cast<int64_t>(Get<int16_t>(a, 2 * i)) * Get<int16_t>(b, 2 * i) + static_cast<int64_t>(Get<int16_t>(a, 2 * i + 1)) * Get<int16_t>(b, 2 * i + 1);
                Set<uint32_t>(r, i, static_cast<uint32_t>(sum));
            }
            break;
        case 0xBC: r = ExtMul<int32_t, int16_t>(a, b, false); break;
        case 0xBD: r = ExtMul<int32_t, int16_t>(a, b, true); break;
        case 0xBE: r = ExtMul<uint32_t, uint16_t>(a, b, false); break;
        case 0xBF: r = ExtMul<uint32_t, uint16_t>(a, b, true); break;

        // ---- i64x2 ----
        case 0xC0: r = Un<int64_t>(a, [](int64_t x) { return x < 0 ? static_cast<int64_t>(0ull - static_cast<uint64_t>(x)) : x; }); break;
        case 0xC1: r = Un<uint64_t>(a, [](uint64_t x) { return 0ull - x; }); break;
        case 0xC3: scalarResult = true; wide = AllTrue<uint64_t>(a); break;
        case 0xC4: scalarResult = true; wide = Bitmask<int64_t>(a); break;
        case 0xC7: r = Extend<int64_t, int32_t>(a, false); break;
        case 0xC8: r = Extend<int64_t, int32_t>(a, true); break;
        case 0xC9: r = Extend<uint64_t, uint32_t>(a, false); break;
        case 0xCA: r = Extend<uint64_t, uint32_t>(a, true); break;
        case 0xCB: r = Shift<uint64_t>(a, scalar, [](uint64_t x, uint32_t c) { return x << c; }); break;
        case 0xCC: r = Shift<int64_t>(a, scalar, [](int64_t x, uint32_t c) { return x >> c; }); break;
        case 0xCD: r = Shift<uint64_t>(a, scalar, [](uint64_t x, uint32_t c) { return x >> c; }); break;
        case 0xCE: r = Bin<uint64_t>(a, b, [](uint64_t x, uint64_t y) { return x + y; }); break;
        case 0xD1: r = Bin<uint64_t>(a, b, [](uint64_t x, uint64_t y) { return x - y; }); break;
        case 0xD5: r = Bin<uint64_t>(a, b, [](uint64_t x, uint64_t y) { return x * y; }); break;
        case 0xDC: r = ExtMul<int64_t, int32_t>(a, b, false); break;
        case 0xDD: r = ExtMul<int64_t, int32_t>(a, b, true); break;
        case 0xDE: r = ExtMul<uint64_t, uint32_t>(a, b, false); break;
        case 0xDF: r = ExtMul<uint64_t, uint32_t>(a, b, true); break;

        // ---- f32x4 ----
        case 0xE0: r = Un<float>(a, [](float x) { return AbsBits(x); }); break;
        case 0xE1: r = Un<float>(a, [](float x) { return NegBits(x); }); break;
        case 0xE3: r = Un<float>(a, [](float x) { return std::sqrt(x); }); break;
        case 0xE4: r = Bin<float>(a, b, [](float x, float y) { return x + y; }); break;
        case 0xE5: r = Bin<float>(a, b, [](float x, float y) { return x - y; }); break;
        case 0xE6: r = Bin<float>(a, b, [](float x, float y) { return x * y; }); break;
        case 0xE7: r = Bin<float>(a, b, [](float x, float y) { return x / y; }); break;
        case 0xE8: case 0x10D: r = Bin<float>(a, b, [](float x, float y) { return WMin(x, y); }); break;
        case 0xE9: case 0x10E: r = Bin<float>(a, b, [](float x, float y) { return WMax(x, y); }); break;
        case 0xEA: r = Bin<float>(a, b, [](float x, float y) { return PMin(x, y); }); break;
        case 0xEB: r = Bin<float>(a, b, [](float x, float y) { return PMax(x, y); }); break;

        // ---- f64x2 ----
        case 0xEC: r = Un<double>(a, [](double x) { return AbsBits(x); }); break;
        case 0xED: r = Un<double>(a, [](double x) { return NegBits(x); }); break;
        case 0xEF: r = Un<double>(a, [](double x) { return std::sqrt(x); }); break;
        case 0xF0: r = Bin<double>(a, b, [](double x, double y) { return x + y; }); break;
        case 0xF1: r = Bin<double>(a, b, [](double x, double y) { return x - y; }); break;
        case 0xF2: r = Bin<double>(a, b, [](double x, double y) { return x * y; }); break;
        case 0xF3: r = Bin<double>(a, b, [](double x, double y) { return x / y; }); break;
        case 0xF4: case 0x10F: r = Bin<double>(a, b, [](double x, double y) { return WMin(x, y); }); break;
        case 0xF5: case 0x110: r = Bin<double>(a, b, [](double x, double y) { return WMax(x, y); }); break;
        case 0xF6: r = Bin<double>(a, b, [](double x, double y) { return PMin(x, y); }); break;
        case 0xF7: r = Bin<double>(a, b, [](double x, double y) { return PMax(x, y); }); break;

        // ---- Conversions ----
        case 0xF8: case 0x101: for (int i = 0; i < 4; ++i) Set<int32_t>(r, i, SatTrunc<int32_t, float>(Get<float>(a, i))); break;
        case 0xF9: case 0x102: for (int i = 0; i < 4; ++i) Set<uint32_t>(r, i, SatTrunc<uint32_t, float>(Get<float>(a, i))); break;
        case 0xFA: for (int i = 0; i < 4; ++i) Set<float>(r, i, static_cast<float>(Get<int32_t>(a, i))); break;
        case 0xFB: for (int i = 0; i < 4; ++i) Set<float>(r, i, static_cast<float>(Get<uint32_t>(a, i))); break;
        case 0xFC: case 0x103:
            for (int i = 0; i < 2; ++i) Set<int32_t>(r, i, SatTrunc<int32_t, double>(Get<double>(a, i)));
            Set<int32_t>(r, 2, 0);
            Set<int32_t>(r, 3, 0);
            break;
        case 0xFD: case 0x104:
            for (int i = 0; i < 2; ++i) Set<uint32_t>(r, i, SatTrunc<uint32_t, double>(Get<double>(a, i)));
            Set<uint32_t>(r, 2, 0);
            Set<uint32_t>(r, 3, 0);
            break;
        case 0xFE: for (int i = 0; i < 2; ++i) Set<double>(r, i, static_cast<double>(Get<int32_t>(a, i))); break;
        case 0xFF: for (int i = 0; i < 2; ++i) Set<double>(r, i, static_cast<double>(Get<uint32_t>(a, i))); break;

        // ---- Relaxed ----
        case 0x105: for (int i = 0; i < 4; ++i) Set<float>(r, i, std::fma(Get<float>(a, i), Get<float>(b, i), Get<float>(c, i))); break;
        case 0x106: for (int i = 0; i < 4; ++i) Set<float>(r, i, std::fma(-Get<float>(a, i), Get<float>(b, i), Get<float>(c, i))); break;
        case 0x107: for (int i = 0; i < 2; ++i) Set<double>(r, i, std::fma(Get<double>(a, i), Get<double>(b, i), Get<double>(c, i))); break;
        case 0x108: for (int i = 0; i < 2; ++i) Set<double>(r, i, std::fma(-Get<double>(a, i), Get<double>(b, i), Get<double>(c, i))); break;
        case 0x112:  // i16x8.relaxed_dot_i8x16_i7x16_s
            for (int i = 0; i < 8; ++i) {
                const int32_t sum = static_cast<int32_t>(Get<int8_t>(a, 2 * i)) * Get<int8_t>(b, 2 * i) + static_cast<int32_t>(Get<int8_t>(a, 2 * i + 1)) * Get<int8_t>(b, 2 * i + 1);
                Set<int16_t>(r, i, Clamp<int16_t, int32_t>(sum));
            }
            break;
        case 0x113:  // i32x4.relaxed_dot_i8x16_i7x16_add_s
            for (int i = 0; i < 4; ++i) {
                int32_t sum = 0;
                for (int k = 0; k < 4; ++k) sum += static_cast<int32_t>(Get<int8_t>(a, 4 * i + k)) * Get<int8_t>(b, 4 * i + k);
                Set<int32_t>(r, i, static_cast<int32_t>(static_cast<uint32_t>(sum) + Get<uint32_t>(c, i)));
            }
            break;
        default: produced = false; break;
    }
    (void)produced;
    switch (shape) {
        case Test: case Extract:
            base[0] = scalarResult ? wide : 0;
            return base + 1;
        default:
            if (scalarResult) { base[0] = wide; return base + 1; }
            Store(base, r);
            return base + 2;
    }
}

}  // namespace Quanta::Wasm
