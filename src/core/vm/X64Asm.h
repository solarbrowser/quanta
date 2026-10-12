/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_X64ASM_H
#define QUANTA_VM_X64ASM_H

// A deliberately tiny x86-64 encoder: only the instruction forms the baseline
// JIT (Jit.inc) emits. It writes into a growable byte buffer and resolves
// forward/backward branches through Labels. Nothing here touches executable
// memory -- the finished buffer is copied into a fresh mapping by the caller.

#include <cstdint>
#include <cstring>
#include <vector>

namespace Quanta {
namespace x64 {

enum Reg : uint8_t {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15
};
enum Xmm : uint8_t { XMM0 = 0, XMM1, XMM2, XMM3 };

// Condition codes (the low nibble of Jcc/SETcc/CMOVcc).
enum Cond : uint8_t {
    CC_O = 0, CC_NO = 1, CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7,
    CC_S = 8, CC_NS = 9, CC_P = 10, CC_NP = 11, CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15
};

struct Label {
    int32_t pos = -1;                 // bound position in the buffer, -1 while unbound
    std::vector<uint32_t> fixups;     // positions of rel32 fields waiting for the bind
};

// [base + index*scale + disp]; index < 0 means no index.
struct Mem {
    Reg base;
    int8_t index = -1;
    uint8_t scale = 1;   // 1, 2, 4, 8
    int32_t disp = 0;
};
inline Mem mem(Reg base, int32_t disp = 0) { return Mem{base, -1, 1, disp}; }
inline Mem mem_idx(Reg base, Reg index, uint8_t scale, int32_t disp = 0) {
    return Mem{base, static_cast<int8_t>(index), scale, disp};
}

class Assembler {
public:
    std::vector<uint8_t> buf;

    size_t size() const { return buf.size(); }

    void byte(uint8_t b) { buf.push_back(b); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) buf.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; i++) buf.push_back(static_cast<uint8_t>(v >> (8 * i))); }

    // ---- labels --------------------------------------------------------
    void bind(Label& l) {
        l.pos = static_cast<int32_t>(buf.size());
        for (uint32_t at : l.fixups) patch32(at, static_cast<int32_t>(l.pos) - static_cast<int32_t>(at + 4));
        l.fixups.clear();
    }
    void patch32(uint32_t at, int32_t v) { std::memcpy(&buf[at], &v, 4); }

    // ---- moves ---------------------------------------------------------
    void mov(Reg dst, Reg src) { rex_rr(true, src, dst); byte(0x89); modrm_rr(src, dst); }
    void mov32(Reg dst, Reg src) { rex_rr(false, src, dst); byte(0x89); modrm_rr(src, dst); }
    void mov_imm64(Reg dst, uint64_t imm) {
        if (imm <= 0xFFFFFFFFull) { mov_imm32(dst, static_cast<uint32_t>(imm)); return; }
        rex(true, 0, 0, dst >> 3); byte(0xB8 + (dst & 7)); u64(imm);
    }
    void mov_imm32(Reg dst, uint32_t imm) {   // zero-extends into the full register
        if (dst >> 3) byte(0x41);
        byte(0xB8 + (dst & 7)); u32(imm);
    }
    void mov_load(Reg dst, Mem m) { rex_rm(true, dst, m); byte(0x8B); modrm_mem(dst, m); }
    void mov_store(Mem m, Reg src) { rex_rm(true, src, m); byte(0x89); modrm_mem(src, m); }
    void mov_load32(Reg dst, Mem m) { rex_rm(false, dst, m); byte(0x8B); modrm_mem(dst, m); }
    void mov_store32(Mem m, Reg src) { rex_rm(false, src, m); byte(0x89); modrm_mem(src, m); }
    void mov_store8_imm(Mem m, uint8_t imm) { rex_rm(false, RAX, m); byte(0xC6); modrm_mem(RAX, m); byte(imm); }
    void mov_store32_imm(Mem m, uint32_t imm) { rex_rm(false, RAX, m); byte(0xC7); modrm_mem(RAX, m); u32(imm); }
    void movzx8_load(Reg dst, Mem m) { rex_rm(false, dst, m); byte(0x0F); byte(0xB6); modrm_mem(dst, m); }
    void movzx8(Reg dst, Reg src) {          // movzx dst32, src8
        // A REX prefix (even an empty one) is what makes SIL/DIL etc. addressable.
        byte(static_cast<uint8_t>(0x40 | ((dst >> 3) << 2) | (src >> 3)));
        byte(0x0F); byte(0xB6); modrm_rr(dst, src);
    }
    void lea(Reg dst, Mem m) { rex_rm(true, dst, m); byte(0x8D); modrm_mem(dst, m); }

    // ---- ALU (64-bit unless marked) -----------------------------------
    void add(Reg d, Reg s) { alu_rr(0x01, d, s); }
    void sub(Reg d, Reg s) { alu_rr(0x29, d, s); }
    void and_(Reg d, Reg s) { alu_rr(0x21, d, s); }
    void or_(Reg d, Reg s) { alu_rr(0x09, d, s); }
    void xor_(Reg d, Reg s) { alu_rr(0x31, d, s); }
    void cmp(Reg d, Reg s) { alu_rr(0x39, d, s); }
    void test(Reg d, Reg s) { alu_rr(0x85, d, s); }
    void add32(Reg d, Reg s) { alu_rr32(0x01, d, s); }
    void sub32(Reg d, Reg s) { alu_rr32(0x29, d, s); }
    void and32(Reg d, Reg s) { alu_rr32(0x21, d, s); }
    void or32(Reg d, Reg s) { alu_rr32(0x09, d, s); }
    void xor32(Reg d, Reg s) { alu_rr32(0x31, d, s); }
    void cmp32(Reg d, Reg s) { alu_rr32(0x39, d, s); }
    void test32(Reg d, Reg s) { alu_rr32(0x85, d, s); }
    void add_imm(Reg d, int32_t imm) { alu_ri(0, d, imm, true); }
    void or_imm(Reg d, int32_t imm) { alu_ri(1, d, imm, true); }
    void and_imm(Reg d, int32_t imm) { alu_ri(4, d, imm, true); }
    void sub_imm(Reg d, int32_t imm) { alu_ri(5, d, imm, true); }
    void cmp_imm(Reg d, int32_t imm) { alu_ri(7, d, imm, true); }
    void add32_imm(Reg d, int32_t imm) { alu_ri(0, d, imm, false); }
    void and32_imm(Reg d, int32_t imm) { alu_ri(4, d, imm, false); }
    void sub32_imm(Reg d, int32_t imm) { alu_ri(5, d, imm, false); }
    void cmp32_imm(Reg d, int32_t imm) { alu_ri(7, d, imm, false); }
    void cmp_load(Reg a, Mem m) { rex_rm(true, a, m); byte(0x3B); modrm_mem(a, m); }       // cmp a, [m]
    void cmp32_load(Reg a, Mem m) { rex_rm(false, a, m); byte(0x3B); modrm_mem(a, m); }
    void cmp_mem_imm(Mem m, int32_t imm) {                                                   // cmp qword [m], imm32
        rex_rm(true, RAX, m); byte(0x81); modrm_mem(static_cast<Reg>(7), m); u32(static_cast<uint32_t>(imm));
    }
    void cmp32_mem_imm(Mem m, int32_t imm) {                                                 // cmp dword [m], imm32
        rex_rm(false, RAX, m); byte(0x81); modrm_mem(static_cast<Reg>(7), m); u32(static_cast<uint32_t>(imm));
    }
    void cmp8_mem_imm(Mem m, uint8_t imm) {
        rex_rm(false, RAX, m); byte(0x80); modrm_mem(static_cast<Reg>(7), m); byte(imm);
    }
    void sub32_mem_imm(Mem m, int32_t imm) {                                                 // sub dword [m], imm32
        rex_rm(false, RAX, m); byte(0x81); modrm_mem(static_cast<Reg>(5), m); u32(static_cast<uint32_t>(imm));
    }
    void test8_mem_imm(Mem m, uint8_t imm) {
        rex_rm(false, RAX, m); byte(0xF6); modrm_mem(RAX, m); byte(imm);
    }
    void imul32(Reg d, Reg s) { rex_rr(false, d, s); byte(0x0F); byte(0xAF); modrm_rr(d, s); }
    void neg32(Reg r) { if (r >> 3) byte(0x41); byte(0xF7); modrm_rr(static_cast<Reg>(3), r); }
    void neg64(Reg r) { rex(true, 0, 0, r >> 3); byte(0xF7); modrm_rr(static_cast<Reg>(3), r); }
    void not32(Reg r) { if (r >> 3) byte(0x41); byte(0xF7); modrm_rr(static_cast<Reg>(2), r); }
    void cdq() { byte(0x99); }
    void idiv32(Reg r) { if (r >> 3) byte(0x41); byte(0xF7); modrm_rr(static_cast<Reg>(7), r); }

    void shl_imm(Reg r, uint8_t n) { shift_imm(4, r, n, true); }
    void shr_imm(Reg r, uint8_t n) { shift_imm(5, r, n, true); }
    void sar_imm(Reg r, uint8_t n) { shift_imm(7, r, n, true); }
    void shl32_imm(Reg r, uint8_t n) { shift_imm(4, r, n, false); }
    void shr32_imm(Reg r, uint8_t n) { shift_imm(5, r, n, false); }
    void shl32_cl(Reg r) { if (r >> 3) byte(0x41); byte(0xD3); modrm_rr(static_cast<Reg>(4), r); }
    void shr32_cl(Reg r) { if (r >> 3) byte(0x41); byte(0xD3); modrm_rr(static_cast<Reg>(5), r); }
    void sar32_cl(Reg r) { if (r >> 3) byte(0x41); byte(0xD3); modrm_rr(static_cast<Reg>(7), r); }
    void bt_imm(Reg r, uint8_t bit) {                                                        // bt r64, imm8 -> CF
        rex(true, 0, 0, r >> 3); byte(0x0F); byte(0xBA); modrm_rr(static_cast<Reg>(4), r); byte(bit);
    }

    void setcc(Cond c, Reg r) {              // setcc r8 (REX forces SIL/DIL, not DH/BH)
        byte(static_cast<uint8_t>(0x40 | (r >> 3))); byte(0x0F); byte(0x90 + c); modrm_rr(RAX, r);
    }
    void cmov(Cond c, Reg d, Reg s) { rex_rr(true, d, s); byte(0x0F); byte(0x40 + c); modrm_rr(d, s); }

    // ---- control flow --------------------------------------------------
    void jmp(Label& l) { byte(0xE9); rel32(l); }
    void jcc(Cond c, Label& l) { byte(0x0F); byte(0x80 + c); rel32(l); }
    void jmp_reg(Reg r) { if (r >> 3) byte(0x41); byte(0xFF); modrm_rr(static_cast<Reg>(4), r); }
    void call_reg(Reg r) { if (r >> 3) byte(0x41); byte(0xFF); modrm_rr(static_cast<Reg>(2), r); }
    void call_abs(const void* fn) { mov_imm64(RAX, reinterpret_cast<uint64_t>(fn)); call_reg(RAX); }
    void ret() { byte(0xC3); }
    void clc() { byte(0xF8); }
    void stc() { byte(0xF9); }
    void call_mem(Mem m) { rex_rm(false, RAX, m); byte(0xFF); modrm_mem(static_cast<Reg>(2), m); }
    void push(Reg r) { if (r >> 3) byte(0x41); byte(0x50 + (r & 7)); }
    void pop(Reg r) { if (r >> 3) byte(0x41); byte(0x58 + (r & 7)); }
    void int3() { byte(0xCC); }
    void ud2() { byte(0x0F); byte(0x0B); }

    // ---- SSE2 ----------------------------------------------------------
    void movq_to_xmm(Xmm x, Reg r) { byte(0x66); rex(true, x >> 3, 0, r >> 3); byte(0x0F); byte(0x6E); modrm_rr(static_cast<Reg>(x), r); }
    void movq_from_xmm(Reg r, Xmm x) { byte(0x66); rex(true, x >> 3, 0, r >> 3); byte(0x0F); byte(0x7E); modrm_rr(static_cast<Reg>(x), r); }
    void addsd(Xmm d, Xmm s) { sse(0xF2, 0x58, d, s); }
    void subsd(Xmm d, Xmm s) { sse(0xF2, 0x5C, d, s); }
    void mulsd(Xmm d, Xmm s) { sse(0xF2, 0x59, d, s); }
    void divsd(Xmm d, Xmm s) { sse(0xF2, 0x5E, d, s); }
    void ucomisd(Xmm a, Xmm b) { byte(0x66); byte(0x0F); byte(0x2E); modrm_rr(static_cast<Reg>(a), static_cast<Reg>(b)); }
    void cvtsi2sd32(Xmm d, Reg r) { byte(0xF2); if (r >> 3) byte(0x41); byte(0x0F); byte(0x2A); modrm_rr(static_cast<Reg>(d), r); }
    void cvtsi2sd64(Xmm d, Reg r) { byte(0xF2); rex(true, 0, 0, r >> 3); byte(0x0F); byte(0x2A); modrm_rr(static_cast<Reg>(d), r); }
    void cvttsd2si32(Reg d, Xmm s) { byte(0xF2); if (d >> 3) byte(0x44); byte(0x0F); byte(0x2C); modrm_rr(d, static_cast<Reg>(s)); }

private:
    void rex(bool w, int r, int x, int b) {
        uint8_t v = static_cast<uint8_t>(0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (b ? 1 : 0));
        if (v != 0x40 || w) byte(v);
    }
    // reg field = `reg`, rm = register `rm`.
    void rex_rr(bool w, Reg reg, Reg rm) { rex(w, reg >> 3, 0, rm >> 3); }
    void rex_rm(bool w, Reg reg, const Mem& m) {
        rex(w, reg >> 3, m.index >= 0 ? (m.index >> 3) : 0, m.base >> 3);
    }
    void modrm_rr(Reg reg, Reg rm) { byte(static_cast<uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7))); }
    void modrm_mem(Reg reg, const Mem& m) {
        const uint8_t r = static_cast<uint8_t>((reg & 7) << 3);
        const bool need_sib = m.index >= 0 || (m.base & 7) == 4;
        uint8_t mod;
        if (m.disp == 0 && (m.base & 7) != 5) mod = 0x00;
        else if (m.disp >= -128 && m.disp <= 127) mod = 0x40;
        else mod = 0x80;
        if (need_sib) {
            byte(static_cast<uint8_t>(mod | r | 4));
            uint8_t ss = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
            uint8_t idx = m.index >= 0 ? static_cast<uint8_t>(m.index & 7) : 4;
            byte(static_cast<uint8_t>((ss << 6) | (idx << 3) | (m.base & 7)));
        } else {
            byte(static_cast<uint8_t>(mod | r | (m.base & 7)));
        }
        if (mod == 0x40) byte(static_cast<uint8_t>(m.disp));
        else if (mod == 0x80) u32(static_cast<uint32_t>(m.disp));
    }
    void alu_rr(uint8_t opcode, Reg d, Reg s) { rex_rr(true, s, d); byte(opcode); modrm_rr(s, d); }
    void alu_rr32(uint8_t opcode, Reg d, Reg s) { rex_rr(false, s, d); byte(opcode); modrm_rr(s, d); }
    void alu_ri(uint8_t ext, Reg d, int32_t imm, bool w) {
        rex(w, 0, 0, d >> 3);
        if (imm >= -128 && imm <= 127) { byte(0x83); modrm_rr(static_cast<Reg>(ext), d); byte(static_cast<uint8_t>(imm)); }
        else { byte(0x81); modrm_rr(static_cast<Reg>(ext), d); u32(static_cast<uint32_t>(imm)); }
    }
    void shift_imm(uint8_t ext, Reg r, uint8_t n, bool w) {
        rex(w, 0, 0, r >> 3); byte(0xC1); modrm_rr(static_cast<Reg>(ext), r); byte(n);
    }
    void sse(uint8_t prefix, uint8_t op, Xmm d, Xmm s) {
        byte(prefix); byte(0x0F); byte(op); modrm_rr(static_cast<Reg>(d), static_cast<Reg>(s));
    }
    void rel32(Label& l) {
        if (l.pos >= 0) {
            u32(static_cast<uint32_t>(l.pos - static_cast<int32_t>(buf.size() + 4)));
        } else {
            l.fixups.push_back(static_cast<uint32_t>(buf.size()));
            u32(0);
        }
    }
};

}  // namespace x64
}  // namespace Quanta

#endif
