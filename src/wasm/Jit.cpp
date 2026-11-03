/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The code generator of Jit.h. Registers while generated code runs:
//   rbx  the frame (the slots of the function)        r12  the base of memory 0       r13  its size in bytes
//   r14  the JitCtx                                    rax rcx rdx  scratch
// A function returns 0 when it went well, 1 when something it called stopped (JitCtx::status says how), and 16 + a TrapKind for a trap of its own.

#include "quanta/wasm/Jit.h"

#if QUANTA_WASM_JIT

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../core/vm/JitPlatform.h"
#include "../core/vm/X64Asm.h"

namespace Quanta::Wasm {

namespace {

using namespace Quanta::x64;
namespace platform = Quanta::VM::jit::platform;

constexpr uint32_t kRetOk = 0, kRetStatus = 1, kRetTrap = 16;

uint64_t Clz32(uint64_t a) { const uint32_t x = static_cast<uint32_t>(a); return x ? __builtin_clz(x) : 32; }
uint64_t Ctz32(uint64_t a) { const uint32_t x = static_cast<uint32_t>(a); return x ? __builtin_ctz(x) : 32; }
uint64_t Pop32(uint64_t a) { return __builtin_popcount(static_cast<uint32_t>(a)); }
uint64_t Clz64(uint64_t a) { return a ? __builtin_clzll(a) : 64; }
uint64_t Ctz64(uint64_t a) { return a ? __builtin_ctzll(a) : 64; }
uint64_t Pop64(uint64_t a) { return __builtin_popcountll(a); }


// ---- floats: what is not worth the instructions ----
float F32(uint64_t v) { float f; const uint32_t b = static_cast<uint32_t>(v); std::memcpy(&f, &b, 4); return f; }
double F64(uint64_t v) { double d; std::memcpy(&d, &v, 8); return d; }
uint64_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
uint64_t Bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
template <class F> F MinOf(F a, F b) { if (a != a || b != b) return a + b; if (a == b) return std::signbit(a) ? a : b; return a < b ? a : b; }
template <class F> F MaxOf(F a, F b) { if (a != a || b != b) return a + b; if (a == b) return std::signbit(a) ? b : a; return a > b ? a : b; }
uint64_t Ceil32(uint64_t a) { return Bits(std::ceil(F32(a))); }
uint64_t Floor32(uint64_t a) { return Bits(std::floor(F32(a))); }
uint64_t Trunc32(uint64_t a) { return Bits(std::trunc(F32(a))); }
uint64_t Nearest32(uint64_t a) { return Bits(std::nearbyint(F32(a))); }
uint64_t Ceil64(uint64_t a) { return Bits(std::ceil(F64(a))); }
uint64_t Floor64(uint64_t a) { return Bits(std::floor(F64(a))); }
uint64_t Trunc64(uint64_t a) { return Bits(std::trunc(F64(a))); }
uint64_t Nearest64(uint64_t a) { return Bits(std::nearbyint(F64(a))); }
uint64_t Min32(uint64_t a, uint64_t b) { return Bits(MinOf(F32(a), F32(b))); }
uint64_t Max32(uint64_t a, uint64_t b) { return Bits(MaxOf(F32(a), F32(b))); }
uint64_t Min64(uint64_t a, uint64_t b) { return Bits(MinOf(F64(a), F64(b))); }
uint64_t Max64(uint64_t a, uint64_t b) { return Bits(MaxOf(F64(a), F64(b))); }
uint64_t F32FromU64(uint64_t a) { return Bits(static_cast<float>(a)); }
uint64_t F64FromU64(uint64_t a) { return Bits(static_cast<double>(a)); }

template <class I, class F> I Sat(F f, F lo, F hi, I imin, I imax) { if (f != f) return 0; if (f <= lo) return imin; if (f >= hi) return imax; return static_cast<I>(f); }
uint64_t SatI32F32S(uint64_t a) { return static_cast<uint32_t>(Sat<int32_t, float>(F32(a), -2147483648.0f, 2147483648.0f, INT32_MIN, INT32_MAX)); }
uint64_t SatI32F32U(uint64_t a) { return Sat<uint32_t, float>(F32(a), 0.0f, 4294967296.0f, 0u, 0xFFFFFFFFu); }
uint64_t SatI32F64S(uint64_t a) { return static_cast<uint32_t>(Sat<int32_t, double>(F64(a), -2147483648.0, 2147483648.0, INT32_MIN, INT32_MAX)); }
uint64_t SatI32F64U(uint64_t a) { return Sat<uint32_t, double>(F64(a), 0.0, 4294967296.0, 0u, 0xFFFFFFFFu); }
uint64_t SatI64F32S(uint64_t a) { return static_cast<uint64_t>(Sat<int64_t, float>(F32(a), -9223372036854775808.0f, 9223372036854775808.0f, INT64_MIN, INT64_MAX)); }
uint64_t SatI64F32U(uint64_t a) { return Sat<uint64_t, float>(F32(a), 0.0f, 18446744073709551616.0f, 0ull, ~0ull); }
uint64_t SatI64F64S(uint64_t a) { return static_cast<uint64_t>(Sat<int64_t, double>(F64(a), -9223372036854775808.0, 9223372036854775808.0, INT64_MIN, INT64_MAX)); }
uint64_t SatI64F64U(uint64_t a) { return Sat<uint64_t, double>(F64(a), 0.0, 18446744073709551616.0, 0ull, ~0ull); }

// A conversion that can trap: the value in rax, and in rdx 0 or the trap (TrapKind + 1).
struct Converted { uint64_t value; uint64_t trap; };
constexpr uint64_t kInvalid = static_cast<uint64_t>(TrapKind::InvalidConversion) + 1, kOverflow = static_cast<uint64_t>(TrapKind::IntegerOverflow) + 1;
Converted I32TruncF32S(uint64_t a) { const float f = F32(a); if (f != f) return {0, kInvalid}; if (!(f >= -2147483648.0f && f < 2147483648.0f)) return {0, kOverflow}; return {static_cast<uint32_t>(static_cast<int32_t>(f)), 0}; }
Converted I32TruncF32U(uint64_t a) { const float f = F32(a); if (f != f) return {0, kInvalid}; if (!(f > -1.0f && f < 4294967296.0f)) return {0, kOverflow}; return {static_cast<uint32_t>(f), 0}; }
Converted I32TruncF64S(uint64_t a) { const double f = F64(a); if (f != f) return {0, kInvalid}; if (!(f > -2147483649.0 && f < 2147483648.0)) return {0, kOverflow}; return {static_cast<uint32_t>(static_cast<int32_t>(f)), 0}; }
Converted I32TruncF64U(uint64_t a) { const double f = F64(a); if (f != f) return {0, kInvalid}; if (!(f > -1.0 && f < 4294967296.0)) return {0, kOverflow}; return {static_cast<uint32_t>(f), 0}; }
Converted I64TruncF32S(uint64_t a) { const float f = F32(a); if (f != f) return {0, kInvalid}; if (!(f >= -9223372036854775808.0f && f < 9223372036854775808.0f)) return {0, kOverflow}; return {static_cast<uint64_t>(static_cast<int64_t>(f)), 0}; }
Converted I64TruncF32U(uint64_t a) { const float f = F32(a); if (f != f) return {0, kInvalid}; if (!(f > -1.0f && f < 18446744073709551616.0f)) return {0, kOverflow}; return {static_cast<uint64_t>(f), 0}; }
Converted I64TruncF64S(uint64_t a) { const double f = F64(a); if (f != f) return {0, kInvalid}; if (!(f >= -9223372036854775808.0 && f < 9223372036854775808.0)) return {0, kOverflow}; return {static_cast<uint64_t>(static_cast<int64_t>(f)), 0}; }
Converted I64TruncF64U(uint64_t a) { const double f = F64(a); if (f != f) return {0, kInvalid}; if (!(f > -1.0 && f < 18446744073709551616.0)) return {0, kOverflow}; return {static_cast<uint64_t>(f), 0}; }

const size_t kLoOffset = [] { GlobalObject* g = nullptr; return static_cast<size_t>(reinterpret_cast<char*>(&g->lo) - reinterpret_cast<char*>(g)); }();

const bool kStats = std::getenv("QUANTA_WASM_JIT_STATS") != nullptr;

// The size in bytes of the loads and stores 0x28 to 0x3E.
const uint8_t kSize[] = {4, 8, 4, 8, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4, 4, 8, 4, 8, 1, 2, 1, 2, 4};

enum Form { kPlain, kImmediate, kJumpIfSlot, kJumpIfImmediate, kAbsolute, kImmediateAbsolute };

struct Decoded {
    Form form;
    uint32_t base;  // the opcode without what was added to it
};

// The numeric and memory instructions come in forms, told by the number (see Code.h).
bool Decode(uint32_t op, Decoded& d) {
    if (op >= Op::kImmAbs + 0x28 && op <= Op::kImmAbs + 0x3E) { d = {kImmediateAbsolute, op - Op::kImmAbs}; return true; }
    if (op >= Op::kAbs + 0x28 && op <= Op::kAbs + 0x3E) { d = {kAbsolute, op - Op::kAbs}; return true; }
    if (op >= Op::kJumpIfImm && op < Op::kJumpIfImm + 0x100) { d = {kJumpIfImmediate, op - Op::kJumpIfImm}; return true; }
    if (op >= Op::kJumpIf && op < Op::kJumpIf + 0x100) { d = {kJumpIfSlot, op - Op::kJumpIf}; return true; }
    if (op >= Op::kImm && op < Op::kImm + 0x100) { d = {kImmediate, op - Op::kImm}; return true; }
    if (op < 0x100) { d = {kPlain, op}; return true; }
    return false;
}

bool IsCompare32(uint32_t b) { return b >= 0x46 && b <= 0x4F; }
bool IsCompare64(uint32_t b) { return b >= 0x51 && b <= 0x5A; }
bool IsBinary32(uint32_t b) { return b >= 0x6A && b <= 0x78; }
bool IsBinary64(uint32_t b) { return b >= 0x7C && b <= 0x8A; }
bool IsFloatCompare(uint32_t b) { return b >= 0x5B && b <= 0x66; }
bool IsFloatUnary(uint32_t b) { return (b >= 0x8B && b <= 0x91) || (b >= 0x99 && b <= 0x9F) || (b >= 0xA8 && b <= 0xB1) || (b >= 0xB2 && b <= 0xBF); }
bool IsFloatBinary(uint32_t b) { return (b >= 0x92 && b <= 0x98) || (b >= 0xA0 && b <= 0xA6); }
bool IsDivision(uint32_t b) { return (b >= 0x6D && b <= 0x70) || (b >= 0x7F && b <= 0x82); }

Cond ConditionOf(uint32_t index) {  // index from the first comparison of a family: eq ne lt_s lt_u gt_s gt_u le_s le_u ge_s ge_u
    static const Cond kConditions[] = {CC_E, CC_NE, CC_L, CC_B, CC_G, CC_A, CC_LE, CC_BE, CC_GE, CC_AE};
    return kConditions[index];
}

struct LocalJump { uint32_t at; uint32_t target; };            // a rel32 at `at` waiting for the native position of word `target`
struct BrTable { uint32_t lea; std::vector<uint32_t> targets; };  // the rel32 of a lea waiting for a table of offsets that is put at the end

class Generator {
public:
    Generator(const CompiledFunction& f, InstanceObject* inst) : f_(f), inst_(inst), words_(f.code) {}

    bool Run(std::string& why) {
        if (f_.paramsR || f_.localsR || f_.maxR || f_.resultsR || !f_.tries.empty()) { why = "references or handlers"; return false; }
        if (!Scan(why)) return false;
        Prologue();
        const size_t n = words_.size();
        native_.assign(n + 1, -1);
        for (size_t i = 0; i < n;) {
            native_[i] = static_cast<int32_t>(a_.size());
            const uint32_t len = Emit(i);
            if (!len) { why = "op " + Hex(words_[i]) + " at " + std::to_string(i); return false; }
            i += len;
        }
        native_[n] = static_cast<int32_t>(a_.size());
        Epilogue();
        return Finish();
    }

    std::vector<uint8_t>& Code() { return a_.buf; }
    bool usesMemory = false;
    bool hasLoop = false;  // a jump that goes back

    // Whether the function jumps back anywhere (without making code).
    bool LoopsOnly() { std::string why; return Scan(why) && hasLoop; }

private:
    static std::string Hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%x", v); return b; }

    // The length of an instruction, from its number; 0 if the JIT does not do it.
    uint32_t Length(size_t i) const {
        const uint32_t op = words_[i];
        if (op >= 0x340 && op < 0x400) {
            switch (op) {
                case Op::kUnreachable: return 1;
                case Op::kJmp: return 2;
                case Op::kJzSlot: case Op::kJnzSlot: return 3;
                case Op::kCopyN: return 3;
                case Op::kConstSlot: return 4;
                case Op::kSelectSlot: return 5;
                case Op::kGlobalGetS: case Op::kGlobalSetS: return 3;
                case Op::kRet: case Op::kRet1: return 3;
                case Op::kBrTableS: return i + 2 < words_.size() ? 4 + words_[i + 2] : 0;
                case Op::kCallR: return 3;
                case Op::kCallIndirectR: return 5;
                case Op::kSyncSp: return 2;
                case Op::kEntry: case Op::kJitEnter: return 1;
                default: return 0;
            }
        }
        if (op >= Op::kFcBase && op <= Op::kFcBase + 7) return 3;   // the saturating truncations
        Decoded d;
        if (!Decode(op, d)) return 0;
        const uint32_t b = d.base;
        switch (d.form) {
            case kAbsolute: case kImmediateAbsolute: return 6;
            case kJumpIfSlot: case kJumpIfImmediate: return IsCompare32(b) || IsCompare64(b) ? 4 : 0;
            case kImmediate:
                if (b >= 0x36 && b <= 0x3E) return 6;
                return IsCompare32(b) || IsCompare64(b) || ((IsBinary32(b) || IsBinary64(b)) && !IsDivision(b)) ? 4 : 0;
            case kPlain:
                if (b >= 0x28 && b <= 0x3E) return 6;
                if (b == 0x45 || b == 0x50 || (b >= 0x67 && b <= 0x69) || (b >= 0x79 && b <= 0x7B)) return 3;
                if (b == 0xA7 || b == 0xAC || b == 0xAD || (b >= 0xC0 && b <= 0xC4) || IsFloatUnary(b)) return 3;
                if (IsFloatCompare(b) || IsFloatBinary(b)) return 4;
                return IsCompare32(b) || IsCompare64(b) || IsBinary32(b) || IsBinary64(b) ? 4 : 0;
        }
        return 0;
    }

    bool Scan(std::string& why) {
        for (size_t i = 0; i < words_.size();) {
            const uint32_t len = Length(i);
            if (!len || i + len > words_.size()) { why = "op " + Hex(words_[i]) + " at " + std::to_string(i); return false; }
            const uint32_t op = words_[i];
            Decoded d{kPlain, 0};
            if ((op < 0x340 || op >= 0x400) && Decode(op, d) && d.base >= 0x28 && d.base <= 0x3E) {
                if (words_[i + 3] != 0) { why = "memory other than 0"; return false; }
                usesMemory = true;
            }
            // (The targets of the jumps: the word of the target is before the instruction for a loop.)
            const uint32_t jumpWord = op == Op::kJmp ? 1 : (op == Op::kJzSlot || op == Op::kJnzSlot) ? 2 : 0;
            if (jumpWord && words_[i + jumpWord] <= i) hasLoop = true;
            if (op >= Op::kJumpIf && op < Op::kJumpIfImm + 0x100 && words_[i + 3] <= i) hasLoop = true;
            if (op == Op::kBrTableS) for (uint32_t k = 0; k <= words_[i + 2]; ++k) if (words_[i + 3 + k] <= i) hasLoop = true;
            i += len;
        }
        return true;
    }

    // ---- small pieces ----
    Mem Slot(uint32_t slot) const { return mem(RBX, static_cast<int32_t>(slot * 8)); }
    void Load(Reg r, uint32_t slot, bool w) { if (w) a_.mov_load(r, Slot(slot)); else a_.mov_load32(r, Slot(slot)); }
    void Store(uint32_t slot, Reg r) { a_.mov_store(Slot(slot), r); }
    void LoadImm64(Reg r, uint64_t v) { if (v <= 0xFFFFFFFFull) a_.mov_imm32(r, static_cast<uint32_t>(v)); else a_.mov_imm64(r, v); }

    void JumpTo(uint32_t target) { a_.byte(0xE9); jumps_.push_back({static_cast<uint32_t>(a_.size()), target}); a_.u32(0); }
    void JumpIf(Cond c, uint32_t target) { a_.byte(0x0F); a_.byte(static_cast<uint8_t>(0x80 + c)); jumps_.push_back({static_cast<uint32_t>(a_.size()), target}); a_.u32(0); }
    Label& TrapLabel(TrapKind k) { return traps_[static_cast<size_t>(k)]; }
    void TrapIf(Cond c, TrapKind k) { a_.jcc(c, TrapLabel(k)); }
    void CallAbs(const void* fn) { a_.call_abs(fn); }

    void Prologue() {
        a_.push(RBP); a_.push(RBX); a_.push(R12); a_.push(R13); a_.push(R14); a_.push(R15);
        a_.sub_imm(RSP, 8);
        a_.mov(RBX, RDI);
        a_.mov(R14, RSI);
        if (usesMemory) {
            a_.mov_load(R12, mem(R14, 0));
            a_.mov_load(R13, mem(R14, 8));
        }
        // (The locals that are not parameters begin as zero.)
        const uint32_t first = f_.paramsN, count = f_.localsN;
        if (count <= 8) {
            for (uint32_t i = 0; i < count; ++i) a_.mov_store64_imm32(Slot(first + i), 0);
        } else {
            a_.lea(RDI, Slot(first));
            a_.mov_imm32(RCX, count);
            a_.xor32(RAX, RAX);
            a_.byte(0xF3); a_.byte(0x48); a_.byte(0xAB);  // rep stosq
        }
    }

    void Epilogue() {
        a_.bind(epilogue_);
        a_.add_imm(RSP, 8);
        a_.pop(R15); a_.pop(R14); a_.pop(R13); a_.pop(R12); a_.pop(RBX); a_.pop(RBP);
        a_.ret();
        for (size_t k = 0; k < traps_.size(); ++k) {
            if (traps_[k].fixups.empty()) continue;
            a_.bind(traps_[k]);
            a_.mov_imm32(RAX, kRetTrap + static_cast<uint32_t>(k));
            a_.jmp(epilogue_);
        }
    }

    void ReturnOk() { a_.xor32(RAX, RAX); a_.jmp(epilogue_); }

    bool Finish() {
        if (a_.buf.empty()) return false;
        // The jumps between instructions.
        for (const LocalJump& j : jumps_) {
            if (j.target >= native_.size() || native_[j.target] < 0) return false;
            a_.patch32(j.at, native_[j.target] - static_cast<int32_t>(j.at + 4));
        }
        // The tables of the br_tables: offsets from the start of each table.
        for (const BrTable& t : tables_) {
            while (a_.size() % 4) a_.byte(0xCC);
            const int32_t tableAt = static_cast<int32_t>(a_.size());
            a_.patch32(t.lea, tableAt - static_cast<int32_t>(t.lea + 4));
            for (uint32_t target : t.targets) {
                if (target >= native_.size() || native_[target] < 0) return false;
                a_.u32(static_cast<uint32_t>(native_[target] - tableAt));
            }
        }
        return true;
    }

    // ---- the instructions ----
    uint32_t Emit(size_t i) {
        const uint32_t len = Length(i);
        if (!len) return 0;
        const uint32_t op = words_[i];
        const uint32_t* p = &words_[i + 1];
        if (op >= 0x340 && op < 0x400) return EmitSpecial(op, p, len) ? len : 0;
        if (op >= Op::kFcBase && op <= Op::kFcBase + 7) {
            static const void* const kFns[] = {reinterpret_cast<const void*>(&SatI32F32S), reinterpret_cast<const void*>(&SatI32F32U), reinterpret_cast<const void*>(&SatI32F64S), reinterpret_cast<const void*>(&SatI32F64U),
                                               reinterpret_cast<const void*>(&SatI64F32S), reinterpret_cast<const void*>(&SatI64F32U), reinterpret_cast<const void*>(&SatI64F64S), reinterpret_cast<const void*>(&SatI64F64U)};
            Helper(kFns[op - Op::kFcBase], p[0], p[1], -1);
            return len;
        }
        Decoded d;
        Decode(op, d);
        if (d.base >= 0x28 && d.base <= 0x3E) return EmitMemory(d, p) ? len : 0;
        return EmitNumeric(d, p) ? len : 0;
    }

    bool EmitSpecial(uint32_t op, const uint32_t* p, uint32_t len) {
        switch (op) {
            case Op::kUnreachable: a_.jmp(TrapLabel(TrapKind::Unreachable)); return true;
            case Op::kJmp: JumpTo(p[0]); return true;
            case Op::kJzSlot: a_.cmp32_mem_imm(Slot(p[0]), 0); JumpIf(CC_E, p[1]); return true;
            case Op::kJnzSlot: a_.cmp32_mem_imm(Slot(p[0]), 0); JumpIf(CC_NE, p[1]); return true;
            case Op::kCopyN: Load(RAX, p[1], true); Store(p[0], RAX); return true;
            case Op::kConstSlot: {
                const uint64_t v = static_cast<uint64_t>(p[1]) | (static_cast<uint64_t>(p[2]) << 32);
                if (static_cast<int64_t>(v) >= INT32_MIN && static_cast<int64_t>(v) <= INT32_MAX) a_.mov_store64_imm32(Slot(p[0]), static_cast<int32_t>(v));
                else { a_.mov_imm64(RAX, v); Store(p[0], RAX); }
                return true;
            }
            case Op::kSelectSlot:
                Load(RAX, p[2], true);
                Load(RCX, p[1], true);
                a_.cmp32_mem_imm(Slot(p[3]), 0);
                a_.cmov(CC_NE, RAX, RCX);
                Store(p[0], RAX);
                return true;
            case Op::kGlobalGetS:
                a_.mov_load(RAX, mem(R14, 24));
                a_.mov_load(RAX, mem(RAX, static_cast<int32_t>(p[1] * 8)));
                a_.mov_load(RAX, mem(RAX, static_cast<int32_t>(kLoOffset)));
                Store(p[0], RAX);
                return true;
            case Op::kGlobalSetS:
                Load(RCX, p[1], true);
                a_.mov_load(RAX, mem(R14, 24));
                a_.mov_load(RAX, mem(RAX, static_cast<int32_t>(p[0] * 8)));
                a_.mov_store(mem(RAX, static_cast<int32_t>(kLoOffset)), RCX);
                return true;
            case Op::kRet: ReturnOk(); return true;
            case Op::kRet1: Load(RAX, p[0], true); a_.mov_store(mem(RBX, 0), RAX); ReturnOk(); return true;
            case Op::kBrTableS: {
                const uint32_t count = p[1];
                a_.mov_load32(RAX, Slot(p[0]));
                a_.cmp32_imm(RAX, static_cast<int32_t>(count));
                Label inRange;
                a_.jcc(CC_BE, inRange);
                a_.mov_imm32(RAX, count);
                a_.bind(inRange);
                // lea rcx,[rip + table]; movsxd rax,[rcx + rax*4]; add rax,rcx; jmp rax
                a_.byte(0x48); a_.byte(0x8D); a_.byte(0x0D);
                BrTable t;
                t.lea = static_cast<uint32_t>(a_.size());
                a_.u32(0);
                for (uint32_t k = 0; k <= count; ++k) t.targets.push_back(p[2 + k]);
                tables_.push_back(std::move(t));
                a_.movsxd_load(RAX, mem_idx(RCX, RAX, 4, 0));
                a_.add(RAX, RCX);
                a_.jmp_reg(RAX);
                (void)len;
                return true;
            }
            case Op::kCallR:
                a_.mov(RDI, R14);
                a_.lea(RSI, Slot(p[1]));
                a_.mov_imm32(RDX, p[0]);
                CallAbs(reinterpret_cast<const void*>(&JitCallHelper));
                a_.test32(RAX, RAX);
                a_.jcc(CC_NE, epilogue_);
                if (usesMemory) {
                    a_.mov_load(R12, mem(R14, 0));
                    a_.mov_load(R13, mem(R14, 8));
                }
                return true;
            case Op::kCallIndirectR:  // type table firstArgumentSlot indexSlot
                a_.mov(RDI, R14);
                a_.lea(RSI, Slot(p[2]));
                a_.mov_imm32(RDX, p[0]);
                a_.mov_imm32(RCX, p[1]);
                a_.mov_load(R8, Slot(p[3]));
                CallAbs(reinterpret_cast<const void*>(&JitCallIndirectHelper));
                a_.test32(RAX, RAX);
                a_.jcc(CC_NE, epilogue_);
                if (usesMemory) {
                    a_.mov_load(R12, mem(R14, 0));
                    a_.mov_load(R13, mem(R14, 8));
                }
                return true;
            case Op::kEntry: case Op::kJitEnter: return true;
            case Op::kSyncSp: return true;  // (only what works from the stack pointer needs it, and the JIT does none of that)
            default: return false;
        }
    }

    bool EmitNumeric(const Decoded& d, const uint32_t* p) {
        const uint32_t b = d.base;
        const bool imm = d.form == kImmediate || d.form == kJumpIfImmediate;

        if (b == 0x45 || b == 0x50) {
            const bool w = b == 0x50;
            if (w) a_.cmp_mem_imm(Slot(p[1]), 0); else a_.cmp32_mem_imm(Slot(p[1]), 0);
            a_.setcc(CC_E, RAX);
            a_.movzx8(RAX, RAX);
            Store(p[0], RAX);
            return true;
        }
        if ((b >= 0x67 && b <= 0x69) || (b >= 0x79 && b <= 0x7B)) {
            static const void* const kFns[] = {reinterpret_cast<const void*>(&Clz32), reinterpret_cast<const void*>(&Ctz32), reinterpret_cast<const void*>(&Pop32),
                                               reinterpret_cast<const void*>(&Clz64), reinterpret_cast<const void*>(&Ctz64), reinterpret_cast<const void*>(&Pop64)};
            Load(RDI, p[1], true);
            CallAbs(kFns[b < 0x79 ? b - 0x67 : 3 + (b - 0x79)]);
            Store(p[0], RAX);
            return true;
        }
        switch (b) {
            case 0xA7: case 0xAD: a_.mov_load32(RAX, Slot(p[1])); Store(p[0], RAX); return true;
            case 0xAC: case 0xC4: a_.movsxd_load(RAX, Slot(p[1])); Store(p[0], RAX); return true;
            case 0xC0: a_.movsx8_load(RAX, Slot(p[1])); Store(p[0], RAX); return true;
            case 0xC1: a_.movsx16_load(RAX, Slot(p[1])); Store(p[0], RAX); return true;
            case 0xC2: a_.movsx8_load64(RAX, Slot(p[1])); Store(p[0], RAX); return true;
            case 0xC3: a_.movsx16_load64(RAX, Slot(p[1])); Store(p[0], RAX); return true;
            default: break;
        }

        if (IsFloatCompare(b) || IsFloatUnary(b) || IsFloatBinary(b)) return EmitFloat(b, p);

        if (IsCompare32(b) || IsCompare64(b)) {
            const bool w = IsCompare64(b);
            const Cond cc = ConditionOf(b - (w ? 0x51 : 0x46));
            const bool jump = d.form == kJumpIfSlot || d.form == kJumpIfImmediate;
            const uint32_t aSlot = jump ? p[0] : p[1];
            const uint32_t second = jump ? p[1] : p[2];
            Load(RAX, aSlot, w);
            if (imm) { if (w) a_.cmp_imm(RAX, static_cast<int32_t>(second)); else a_.cmp32_imm(RAX, static_cast<int32_t>(second)); }
            else a_.alu_rm(0x3B, RAX, Slot(second), w);
            if (jump) { JumpIf(cc, p[2]); return true; }
            a_.setcc(cc, RAX);
            a_.movzx8(RAX, RAX);
            Store(p[0], RAX);
            return true;
        }

        const bool w = IsBinary64(b);
        const uint32_t k = b - (w ? 0x7C : 0x6A);  // add sub mul div_s div_u rem_s rem_u and or xor shl shr_s shr_u rotl rotr
        if (IsDivision(b)) return EmitDivision(k, w, p);
        Load(RAX, p[1], w);
        const uint32_t second = p[2];
        switch (k) {
            case 0: case 1: case 7: case 8: case 9: {
                static const uint8_t kRm[] = {0x03, 0x2B, 0, 0, 0, 0, 0, 0x23, 0x0B, 0x33};
                if (imm) {
                    const int32_t v = static_cast<int32_t>(second);
                    switch (k) {
                        case 0: if (w) a_.add_imm(RAX, v); else a_.add32_imm(RAX, v); break;
                        case 1: if (w) a_.sub_imm(RAX, v); else a_.sub32_imm(RAX, v); break;
                        case 7: if (w) a_.and_imm(RAX, v); else a_.and32_imm(RAX, v); break;
                        case 8: if (w) a_.or_imm(RAX, v); else { a_.mov_imm32(RCX, second); a_.or32(RAX, RCX); } break;
                        default: if (w) { a_.mov_imm64(RCX, static_cast<uint64_t>(static_cast<int64_t>(v))); a_.xor_(RAX, RCX); } else { a_.mov_imm32(RCX, second); a_.xor32(RAX, RCX); } break;
                    }
                } else a_.alu_rm(kRm[k], RAX, Slot(second), w);
                break;
            }
            case 2:
                if (imm) { LoadImm64(RCX, w ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(second))) : second); if (w) a_.imul64(RAX, RCX); else a_.imul32(RAX, RCX); }
                else a_.imul_rm(RAX, Slot(second), w);
                break;
            default: {  // shl shr_s shr_u rotl rotr
                static const uint8_t kExt[] = {4, 7, 5, 0, 1};
                const uint8_t ext = kExt[k - 10];
                if (imm) a_.shift_imm_n(ext, RAX, static_cast<uint8_t>(second & (w ? 63 : 31)), w);
                else { a_.mov_load32(RCX, Slot(second)); a_.shift_cl(ext, RAX, w); }
                break;
            }
        }
        Store(p[0], RAX);
        return true;
    }

    // The helpers take their numbers in rdi and rsi and give theirs in rax.
    void Helper(const void* fn, uint32_t dst, uint32_t a, int64_t b) {
        Load(RDI, a, true);
        if (b >= 0) Load(RSI, static_cast<uint32_t>(b), true);
        CallAbs(fn);
        Store(dst, RAX);
    }

    // cvtsi2ss/sd keep the rest of the register: without clearing it first, each conversion waits for what was in it before.
    void ConvertFrom(uint8_t prefix, uint8_t op, Xmm d, Reg r, bool w) {
        a_.sse_op(0, 0x57, d, d);   // xorps
        a_.sse_gpr(prefix, op, d, r, w);
    }

    bool EmitFloat(uint32_t b, const uint32_t* p) {
        const auto double64 = [&](uint32_t base) { return b >= base; };
        if (IsFloatCompare(b)) {
            const bool d = b >= 0x61;
            const uint32_t k = b - (d ? 0x61 : 0x5B);  // eq ne lt gt le ge
            const uint8_t pre = d ? 0x66 : 0;
            const auto ld = [&](Xmm x, uint32_t slot) { if (d) a_.movsd_load(x, Slot(slot)); else a_.movss_load(x, Slot(slot)); };
            if (k == 2 || k == 4) { ld(XMM0, p[2]); ld(XMM1, p[1]); }   // (a < b is b > a)
            else { ld(XMM0, p[1]); ld(XMM1, p[2]); }
            a_.sse_op(pre, 0x2E, XMM0, XMM1);
            switch (k) {
                case 0: a_.setcc(CC_E, RAX); a_.setcc(CC_NP, RCX); a_.and32(RAX, RCX); break;
                case 1: a_.setcc(CC_NE, RAX); a_.setcc(CC_P, RCX); a_.or32(RAX, RCX); break;
                case 2: case 3: a_.setcc(CC_A, RAX); break;
                default: a_.setcc(CC_AE, RAX); break;   // le ge
            }
            a_.movzx8(RAX, RAX);
            Store(p[0], RAX);
            return true;
        }
        if (IsFloatBinary(b)) {
            const bool d = b >= 0xA0;
            const uint32_t k = b - (d ? 0xA0 : 0x92);  // add sub mul div min max copysign
            if (k == 4 || k == 5) { Helper(reinterpret_cast<const void*>(d ? (k == 4 ? &Min64 : &Max64) : (k == 4 ? &Min32 : &Max32)), p[0], p[1], p[2]); return true; }
            if (k == 6) {
                if (d) {
                    a_.mov_imm64(RCX, 0x7FFFFFFFFFFFFFFFull); a_.mov_imm64(RDX, 0x8000000000000000ull);
                    Load(RAX, p[1], true); a_.and_(RAX, RCX);
                    Load(RCX, p[2], true); a_.and_(RCX, RDX); a_.or_(RAX, RCX);
                } else {
                    Load(RAX, p[1], false); a_.and32_imm(RAX, 0x7FFFFFFF);
                    Load(RCX, p[2], false); a_.and32_imm(RCX, INT32_MIN); a_.or32(RAX, RCX);
                }
                Store(p[0], RAX);
                return true;
            }
            static const uint8_t kOps[] = {0x58, 0x5C, 0x59, 0x5E};
            if (d) { a_.movsd_load(XMM0, Slot(p[1])); a_.movsd_load(XMM1, Slot(p[2])); }
            else { a_.movss_load(XMM0, Slot(p[1])); a_.movss_load(XMM1, Slot(p[2])); }
            a_.sse_op(d ? 0xF2 : 0xF3, kOps[k], XMM0, XMM1);
            if (d) a_.movq_from_xmm(RAX, XMM0); else a_.movd_from_xmm(RAX, XMM0);
            Store(p[0], RAX);
            return true;
        }
        // Unary, and the conversions.
        const uint32_t src = p[1], dst = p[0];
        switch (b) {
            case 0x8B: Load(RAX, src, false); a_.and32_imm(RAX, 0x7FFFFFFF); Store(dst, RAX); return true;   // f32.abs
            case 0x8C: Load(RAX, src, false); a_.mov_imm32(RCX, 0x80000000u); a_.xor32(RAX, RCX); Store(dst, RAX); return true;  // f32.neg
            case 0x99: a_.mov_imm64(RCX, 0x7FFFFFFFFFFFFFFFull); Load(RAX, src, true); a_.and_(RAX, RCX); Store(dst, RAX); return true;
            case 0x9A: a_.mov_imm64(RCX, 0x8000000000000000ull); Load(RAX, src, true); a_.xor_(RAX, RCX); Store(dst, RAX); return true;
            case 0x8D: Helper(reinterpret_cast<const void*>(&Ceil32), dst, src, -1); return true;
            case 0x8E: Helper(reinterpret_cast<const void*>(&Floor32), dst, src, -1); return true;
            case 0x8F: Helper(reinterpret_cast<const void*>(&Trunc32), dst, src, -1); return true;
            case 0x90: Helper(reinterpret_cast<const void*>(&Nearest32), dst, src, -1); return true;
            case 0x9B: Helper(reinterpret_cast<const void*>(&Ceil64), dst, src, -1); return true;
            case 0x9C: Helper(reinterpret_cast<const void*>(&Floor64), dst, src, -1); return true;
            case 0x9D: Helper(reinterpret_cast<const void*>(&Trunc64), dst, src, -1); return true;
            case 0x9E: Helper(reinterpret_cast<const void*>(&Nearest64), dst, src, -1); return true;
            case 0x91: a_.movss_load(XMM0, Slot(src)); a_.sse_op(0xF3, 0x51, XMM0, XMM0); a_.movd_from_xmm(RAX, XMM0); Store(dst, RAX); return true;
            case 0x9F: a_.movsd_load(XMM0, Slot(src)); a_.sse_op(0xF2, 0x51, XMM0, XMM0); a_.movq_from_xmm(RAX, XMM0); Store(dst, RAX); return true;
            case 0xB2: Load(RAX, src, false); ConvertFrom(0xF3, 0x2A, XMM0, RAX, false); a_.movd_from_xmm(RAX, XMM0); Store(dst, RAX); return true;  // f32.convert_i32_s
            case 0xB3: Load(RAX, src, false); ConvertFrom(0xF3, 0x2A, XMM0, RAX, true); a_.movd_from_xmm(RAX, XMM0); Store(dst, RAX); return true;   // _u (zero-extended)
            case 0xB4: Load(RAX, src, true); ConvertFrom(0xF3, 0x2A, XMM0, RAX, true); a_.movd_from_xmm(RAX, XMM0); Store(dst, RAX); return true;
            case 0xB5: Helper(reinterpret_cast<const void*>(&F32FromU64), dst, src, -1); return true;
            case 0xB6: a_.movsd_load(XMM0, Slot(src)); a_.sse_op(0xF2, 0x5A, XMM0, XMM0); a_.movd_from_xmm(RAX, XMM0); Store(dst, RAX); return true;   // demote
            case 0xB7: Load(RAX, src, false); ConvertFrom(0xF2, 0x2A, XMM0, RAX, false); a_.movq_from_xmm(RAX, XMM0); Store(dst, RAX); return true;
            case 0xB8: Load(RAX, src, false); ConvertFrom(0xF2, 0x2A, XMM0, RAX, true); a_.movq_from_xmm(RAX, XMM0); Store(dst, RAX); return true;
            case 0xB9: Load(RAX, src, true); ConvertFrom(0xF2, 0x2A, XMM0, RAX, true); a_.movq_from_xmm(RAX, XMM0); Store(dst, RAX); return true;
            case 0xBA: Helper(reinterpret_cast<const void*>(&F64FromU64), dst, src, -1); return true;
            case 0xBB: a_.movss_load(XMM0, Slot(src)); a_.sse_op(0xF3, 0x5A, XMM0, XMM0); a_.movq_from_xmm(RAX, XMM0); Store(dst, RAX); return true;   // promote
            case 0xBC: Load(RAX, src, false); Store(dst, RAX); return true;   // (the reinterpretations move bits)
            case 0xBD: case 0xBE: case 0xBF: Load(RAX, src, true); Store(dst, RAX); return true;
            default: break;
        }
        if (b >= 0xA8 && b <= 0xB1 && b != 0xAC && b != 0xAD) {   // the truncations, which can trap
            static Converted (*const kFns[])(uint64_t) = {I32TruncF32S, I32TruncF32U, I32TruncF64S, I32TruncF64U, nullptr, nullptr, I64TruncF32S, I64TruncF32U, I64TruncF64S, I64TruncF64U};
            Load(RDI, src, true);
            CallAbs(reinterpret_cast<const void*>(kFns[b - 0xA8]));
            Label ok;
            a_.test(RDX, RDX);
            a_.jcc(CC_E, ok);
            a_.mov32(RAX, RDX);
            a_.add32_imm(RAX, static_cast<int32_t>(kRetTrap - 1));
            a_.jmp(epilogue_);
            a_.bind(ok);
            Store(dst, RAX);
            return true;
        }
        (void)double64;
        return false;
    }

    bool EmitDivision(uint32_t k, bool w, const uint32_t* p) {
        const bool isSigned = k == 3 || k == 5, isRem = k >= 5;
        Load(RAX, p[1], w);
        Load(RCX, p[2], w);
        if (w) a_.test(RCX, RCX); else a_.test32(RCX, RCX);
        TrapIf(CC_E, TrapKind::DivideByZero);
        Label done;
        if (isSigned) {
            Label normal;
            if (w) a_.cmp_imm(RCX, -1); else a_.cmp32_imm(RCX, -1);
            a_.jcc(CC_NE, normal);
            if (isRem) {
                a_.xor32(RAX, RAX);
                a_.jmp(done);
            } else {
                if (w) { a_.mov_imm64(RDX, 0x8000000000000000ull); a_.cmp(RAX, RDX); }
                else a_.cmp32_imm(RAX, INT32_MIN);
                TrapIf(CC_E, TrapKind::IntegerOverflow);
            }
            a_.bind(normal);
            if (w) a_.cqo(); else a_.cdq();
            a_.unary_f7(7, RCX, w);
        } else {
            a_.xor32(RDX, RDX);
            a_.unary_f7(6, RCX, w);
        }
        if (isRem) a_.mov(RAX, RDX);
        if (!w) a_.mov32(RAX, RAX);
        a_.bind(done);
        Store(p[0], RAX);
        return true;
    }

    bool EmitMemory(const Decoded& d, const uint32_t* p) {
        const uint32_t b = d.base;
        const uint32_t size = kSize[b - 0x28];
        const bool isStore = b >= 0x36;
        const bool absolute = d.form == kAbsolute || d.form == kImmediateAbsolute;
        const bool immediateValue = d.form == kImmediate || d.form == kImmediateAbsolute;
        const uint64_t off = static_cast<uint64_t>(p[3]) | (static_cast<uint64_t>(p[4]) << 32);
        const bool is64 = inst_->memories.empty() ? false : inst_->memories[0]->type.limits.is64;

        if (isStore) {
            if (immediateValue) LoadImm64(RDX, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(p[0]))));
            else Load(RDX, p[0], true);
        }
        Mem at = mem(R12, 0);
        if (absolute) {
            if (off + size < off) { a_.jmp(TrapLabel(TrapKind::MemoryOutOfBounds)); return true; }
            LoadImm64(RCX, off + size);
            a_.cmp(RCX, R13);
            TrapIf(CC_A, TrapKind::MemoryOutOfBounds);
            if (off <= 0x7FFFFFF0ull) at = mem(R12, static_cast<int32_t>(off));
            else { LoadImm64(RAX, off); at = mem_idx(R12, RAX, 1, 0); }
        } else if (!is64 && off + size <= 0x7FFFFFFFull) {
            a_.mov_load32(RAX, Slot(p[1]));
            a_.lea(RCX, mem(RAX, static_cast<int32_t>(off + size)));
            a_.cmp(RCX, R13);
            TrapIf(CC_A, TrapKind::MemoryOutOfBounds);
            at = mem_idx(R12, RAX, 1, static_cast<int32_t>(off));
        } else {
            if (is64) a_.mov_load(RAX, Slot(p[1])); else a_.mov_load32(RAX, Slot(p[1]));
            if (off) {
                LoadImm64(RCX, off);
                a_.add(RAX, RCX);
                TrapIf(CC_B, TrapKind::MemoryOutOfBounds);
            }
            a_.mov(RCX, RAX);
            a_.add_imm(RCX, static_cast<int32_t>(size));
            TrapIf(CC_B, TrapKind::MemoryOutOfBounds);
            a_.cmp(RCX, R13);
            TrapIf(CC_A, TrapKind::MemoryOutOfBounds);
            at = mem_idx(R12, RAX, 1, 0);
        }

        if (isStore) {
            switch (size) {
                case 1: a_.mov_store8(at, RDX); break;
                case 2: a_.mov_store16(at, RDX); break;
                case 4: a_.mov_store32(at, RDX); break;
                default: a_.mov_store(at, RDX); break;
            }
            return true;
        }
        switch (b) {
            case 0x28: case 0x2A: case 0x35: a_.mov_load32(RAX, at); break;
            case 0x29: case 0x2B: a_.mov_load(RAX, at); break;
            case 0x2C: a_.movsx8_load(RAX, at); break;
            case 0x2D: case 0x31: a_.movzx8_load(RAX, at); break;
            case 0x2E: a_.movsx16_load(RAX, at); break;
            case 0x2F: case 0x33: a_.movzx16_load(RAX, at); break;
            case 0x30: a_.movsx8_load64(RAX, at); break;
            case 0x32: a_.movsx16_load64(RAX, at); break;
            default: a_.movsxd_load(RAX, at); break;  // 0x34
        }
        Store(p[0], RAX);
        return true;
    }

    const CompiledFunction& f_;
    InstanceObject* inst_;
    const std::vector<uint32_t>& words_;
    Assembler a_;
    Label epilogue_;
    std::vector<Label> traps_ = std::vector<Label>(static_cast<size_t>(TrapKind::kCount));
    std::vector<int32_t> native_;
    std::vector<LocalJump> jumps_;
    std::vector<BrTable> tables_;
};

}  // namespace

bool JitCompile(const CompiledFunction& function, InstanceObject* instance) {
    function.jitFailed = true;  // (until it is not)
    Generator g(function, instance);
    std::string why;
    if (!g.Run(why)) {
        if (kStats) std::fprintf(stderr, "[wasm jit] refused: %s\n", why.c_str());
        return false;
    }
    std::vector<uint8_t>& code = g.Code();
    const size_t page = platform::page_size();
    const size_t size = (code.size() + page - 1) / page * page;
    void* memory = platform::map_writable(size);
    if (!memory) return false;
    std::memcpy(memory, code.data(), code.size());
    if (!platform::make_executable(memory, size)) { platform::unmap(memory, size); return false; }
    WasmJitCode* jit = new WasmJitCode();
    jit->memory = memory;
    jit->size = size;
    jit->usesMemory = g.usesMemory;
    jit->entry = reinterpret_cast<uint32_t (*)(uint64_t*, JitCtx*)>(memory);
    function.jit = jit;
    function.jitFailed = false;
    if (kStats) std::fprintf(stderr, "[wasm jit] compiled %zu words -> %zu bytes\n", function.code.size(), code.size());
    return true;
}

void JitConsider(const CompiledFunction& function, InstanceObject* instance) {
    static const bool disabled = [] { const char* e = std::getenv("QUANTA_WASM_JIT"); return e && e[0] == '0'; }();
    static const uint32_t threshold = [] { const char* e = std::getenv("QUANTA_WASM_JIT_THRESHOLD"); return e ? static_cast<uint32_t>(std::atoi(e)) : 8u; }();
    if (disabled) { function.jitFailed = true; return; }
    if (function.jitFailed || function.jit) return;
    if (++function.calls < threshold) {
        // (A function with a loop is worth it on its first call: it may well be the only one, and run for a long time.)
        if (function.calls == 1 && function.paramsR == 0 && function.localsR == 0 && function.maxR == 0 && function.tries.empty()) {
            Generator g(function, instance);
            if (!g.LoopsOnly()) return;
        } else return;
    }
    JitCompile(function, instance);
}

}  // namespace Quanta::Wasm

#else

namespace Quanta::Wasm {
bool JitCompile(const CompiledFunction&, InstanceObject*) { return false; }
void JitConsider(const CompiledFunction&, InstanceObject*) {}
}  // namespace Quanta::Wasm

#endif
