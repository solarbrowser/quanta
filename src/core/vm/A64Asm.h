/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_A64ASM_H
#define QUANTA_VM_A64ASM_H

// The AArch64 half of the baseline JIT's assembler (the x86-64 half is X64Asm.h). It has
// the same interface on purpose: Jit.inc is written against these method names, registers
// and condition codes, and each method here is the A64 sequence that does what the x86-64
// one does. That keeps the code generator one piece of code instead of two.
//
// What carries over unchanged, and what does not:
//  * Registers keep their x86 names -- they are roles (RBX holds the Frame, R13 the
//    accumulator), not the hardware's. phys() says which A64 register each one is. The four
//    that C++ helpers take their arguments in (RDI, RSI, RDX, RCX) are x0-x3, the callee-saved
//    roles are x19-x24, and the rest are caller-saved temporaries.
//  * Condition codes keep the x86 meaning and numbering (Jit.inc indexes a table by them) and
//    are translated to A64's when a branch is emitted. An unsigned compare means the same on
//    both ("below" is carry clear on A64), and so does a floating-point one once NaN is
//    excluded, which the generated code always does first.
//  * Instructions with an x86 two-operand shape (add d, s) become the A64 three-operand form
//    with d twice. A memory operand or an immediate that A64 cannot encode goes through the
//    two scratch registers x16 (addresses, immediates) and x17 (data), which no role uses.
//  * What has no counterpart is spelled out where it appears: the carry flag a stub uses to
//    answer hit/miss (clc/stc), bt, cvttsd2si's 0x80000000 on overflow, and a call's result
//    arriving in x0 rather than RAX (every call here copies it across).
//
// Nothing here touches executable memory: the finished buffer is copied into a fresh
// mapping by the caller.

#include <cstdint>
#include <cstring>
#include <vector>

namespace Quanta {
namespace a64 {

enum Reg : uint8_t {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15
};
enum Xmm : uint8_t { XMM0 = 0, XMM1, XMM2, XMM3 };

// The x86 numbering, so a table indexed by Cond means the same thing here.
enum Cond : uint8_t {
    CC_O = 0, CC_NO = 1, CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7,
    CC_S = 8, CC_NS = 9, CC_P = 10, CC_NP = 11, CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15
};

struct Label {
    int32_t pos = -1;                 // bound position in the buffer, -1 while unbound
    std::vector<uint32_t> fixups;     // positions of branch instructions waiting for the bind
};

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

// The hardware register each role is. x16/x17 are the assembler's scratch, x18 is the
// platform register (reserved on Apple's), x29/x30 are the frame pointer and link register.
inline constexpr uint8_t kPhys[16] = {
    /* RAX */ 9,  /* RCX */ 3,  /* RDX */ 2,  /* RBX */ 19, /* RSP */ 31, /* RBP */ 24,
    /* RSI */ 1,  /* RDI */ 0,  /* R8  */ 10, /* R9  */ 11, /* R10 */ 12, /* R11 */ 13,
    /* R12 */ 20, /* R13 */ 21, /* R14 */ 22, /* R15 */ 23
};
inline constexpr uint8_t phys(Reg r) { return kPhys[r]; }

constexpr uint32_t kScratchAddr = 16;   // x16
constexpr uint32_t kScratchData = 17;   // x17
constexpr uint32_t kZr = 31;            // xzr/wzr (or sp, depending on the instruction)
constexpr uint32_t kLr = 30;
constexpr uint32_t kFpScratch = 30;     // d30

// ARM condition field for each x86 Cond (index by Cond). 0xFF: no counterpart (parity).
inline constexpr uint8_t kArmCond[16] = {
    /* O  */ 6,  /* NO */ 7,  /* B  */ 3,  /* AE */ 2,  /* E  */ 0,  /* NE */ 1,
    /* BE */ 9,  /* A  */ 8,  /* S  */ 4,  /* NS */ 5,  0xFF, 0xFF,
    /* L  */ 11, /* GE */ 10, /* LE */ 13, /* G  */ 12
};

class Assembler {
public:
    std::vector<uint8_t> buf;

    size_t size() const { return buf.size(); }
    // True once a branch could not reach its target; the code is then unusable.
    bool failed() const { return failed_; }

    void emit(uint32_t w) { for (int i = 0; i < 4; i++) buf.push_back(static_cast<uint8_t>(w >> (8 * i))); }

    // ---- labels --------------------------------------------------------
    void bind(Label& l) {
        l.pos = static_cast<int32_t>(buf.size());
        for (uint32_t at : l.fixups) patch_branch(at, l.pos);
        l.fixups.clear();
    }

    // ---- moves ---------------------------------------------------------
    void mov(Reg dst, Reg src) {
        if (dst == RSP || src == RSP) emit(0x91000000u | (phys(src) << 5) | phys(dst));   // add dst, src, #0
        else emit(0xAA0003E0u | (phys(src) << 16) | phys(dst));                           // orr dst, xzr, src
    }
    void mov32(Reg dst, Reg src) { emit(0x2A0003E0u | (phys(src) << 16) | phys(dst)); }
    void mov_imm64(Reg dst, uint64_t imm) { mov_imm_to(phys(dst), imm); }
    void mov_imm32(Reg dst, uint32_t imm) { mov_imm_to(phys(dst), imm); }   // zero-extends into the full register
    void mov_load(Reg dst, Mem m) { access(8, true, phys(dst), m); }
    void mov_store(Mem m, Reg src) { access(8, false, phys(src), m); }
    void mov_load32(Reg dst, Mem m) { access(4, true, phys(dst), m); }
    void mov_store32(Mem m, Reg src) { access(4, false, phys(src), m); }
    void mov_store8_imm(Mem m, uint8_t imm) { mov_imm_to(kScratchData, imm); access(1, false, kScratchData, m); }
    void mov_store32_imm(Mem m, uint32_t imm) { mov_imm_to(kScratchData, imm); access(4, false, kScratchData, m); }
    void movzx8_load(Reg dst, Mem m) { access(1, true, phys(dst), m); }
    void movzx8(Reg dst, Reg src) { emit(0x53001C00u | (phys(src) << 5) | phys(dst)); }   // uxtb dst, src
    void lea(Reg dst, Mem m) {
        const uint32_t d = phys(dst);
        if (m.index >= 0) {
            emit(0x8B000000u | (phys(static_cast<Reg>(m.index)) << 16) | (log2_scale(m.scale) << 10) |
                 (phys(m.base) << 5) | d);                                    // add d, base, index, lsl #s
            add_imm_to(d, d, m.disp);
        } else {
            add_imm_to(d, phys(m.base), m.disp);
        }
    }

    // ---- ALU (64-bit unless marked) -----------------------------------
    void add(Reg d, Reg s) { emit(0x8B000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void sub(Reg d, Reg s) { emit(0xCB000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void and_(Reg d, Reg s) { emit(0x8A000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void or_(Reg d, Reg s) { emit(0xAA000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void xor_(Reg d, Reg s) { emit(0xCA000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void cmp(Reg d, Reg s) { emit(0xEB00001Fu | (phys(s) << 16) | (phys(d) << 5)); }
    void test(Reg d, Reg s) { emit(0xEA00001Fu | (phys(s) << 16) | (phys(d) << 5)); }
    void add32(Reg d, Reg s) { emit(0x0B000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void sub32(Reg d, Reg s) { emit(0x4B000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void and32(Reg d, Reg s) { emit(0x0A000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void or32(Reg d, Reg s) { emit(0x2A000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void xor32(Reg d, Reg s) { emit(0x4A000000u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void cmp32(Reg d, Reg s) { emit(0x6B00001Fu | (phys(s) << 16) | (phys(d) << 5)); }
    void test32(Reg d, Reg s) { emit(0x6A00001Fu | (phys(s) << 16) | (phys(d) << 5)); }
    void add_imm(Reg d, int32_t imm) { add_imm_to(phys(d), phys(d), imm); }
    void sub_imm(Reg d, int32_t imm) { add_imm_to(phys(d), phys(d), -static_cast<int64_t>(imm)); }
    void or_imm(Reg d, int32_t imm) { logical_imm(0xAA000000u, d, static_cast<int64_t>(imm)); }
    void and_imm(Reg d, int32_t imm) { logical_imm(0x8A000000u, d, static_cast<int64_t>(imm)); }
    void cmp_imm(Reg d, int32_t imm) { cmp_imm_to(phys(d), imm, true); }
    void add32_imm(Reg d, int32_t imm) { add32_imm_to(phys(d), imm); }
    void and32_imm(Reg d, int32_t imm) { logical_imm(0x0A000000u, d, static_cast<int64_t>(static_cast<uint32_t>(imm))); }
    void sub32_imm(Reg d, int32_t imm) { add32_imm_to(phys(d), -static_cast<int64_t>(imm)); }
    void cmp32_imm(Reg d, int32_t imm) { cmp_imm_to(phys(d), imm, false); }
    void cmp_load(Reg a, Mem m) {
        access(8, true, kScratchData, m);
        emit(0xEB00001Fu | (kScratchData << 16) | (phys(a) << 5));
    }
    void cmp32_load(Reg a, Mem m) {
        access(4, true, kScratchData, m);
        emit(0x6B00001Fu | (kScratchData << 16) | (phys(a) << 5));
    }
    void cmp_mem_imm(Mem m, int32_t imm) {
        access(8, true, kScratchData, m);
        cmp_imm_to(kScratchData, imm, true);
    }
    void cmp32_mem_imm(Mem m, int32_t imm) {
        access(4, true, kScratchData, m);
        cmp_imm_to(kScratchData, imm, false);
    }
    void cmp8_mem_imm(Mem m, uint8_t imm) {
        access(1, true, kScratchData, m);
        cmp_imm_to(kScratchData, imm, false);
    }
    void sub32_mem_imm(Mem m, int32_t imm) {                                  // leaves the flags of the subtraction
        access(4, true, kScratchData, m);
        if (imm >= 0 && imm <= 4095) {
            emit(0x71000000u | (static_cast<uint32_t>(imm) << 10) | (kScratchData << 5) | kScratchData);   // subs w17, w17, #imm
        } else {
            mov_imm_to(kScratchAddr, static_cast<uint32_t>(imm));
            emit(0x6B000000u | (kScratchAddr << 16) | (kScratchData << 5) | kScratchData);
        }
        access(4, false, kScratchData, m);
    }
    void test8_mem_imm(Mem m, uint8_t imm) {
        access(1, true, kScratchData, m);
        mov_imm_to(kScratchAddr, imm);
        emit(0x6A00001Fu | (kScratchAddr << 16) | (kScratchData << 5));       // tst w17, w16
    }
    void imul32(Reg d, Reg s) { emit(0x1B007C00u | (phys(s) << 16) | (phys(d) << 5) | phys(d)); }
    void neg32(Reg r) { emit(0x4B0003E0u | (phys(r) << 16) | phys(r)); }
    void neg64(Reg r) { emit(0xCB0003E0u | (phys(r) << 16) | phys(r)); }
    void not32(Reg r) { emit(0x2A2003E0u | (phys(r) << 16) | phys(r)); }

    void shl_imm(Reg r, uint8_t n) { emit(0xD3400000u | (((64 - n) & 63u) << 16) | ((63u - n) << 10) | (phys(r) << 5) | phys(r)); }
    void shr_imm(Reg r, uint8_t n) { emit(0xD340FC00u | (static_cast<uint32_t>(n) << 16) | (phys(r) << 5) | phys(r)); }
    void sar_imm(Reg r, uint8_t n) { emit(0x9340FC00u | (static_cast<uint32_t>(n) << 16) | (phys(r) << 5) | phys(r)); }
    void shl32_imm(Reg r, uint8_t n) { emit(0x53000000u | (((32 - n) & 31u) << 16) | ((31u - n) << 10) | (phys(r) << 5) | phys(r)); }
    void shr32_imm(Reg r, uint8_t n) { emit(0x53007C00u | (static_cast<uint32_t>(n) << 16) | (phys(r) << 5) | phys(r)); }
    // The count is in RCX, as x86's cl; the hardware masks it to five bits, as JS does.
    void shl32_cl(Reg r) { emit(0x1AC02000u | (phys(RCX) << 16) | (phys(r) << 5) | phys(r)); }
    void shr32_cl(Reg r) { emit(0x1AC02400u | (phys(RCX) << 16) | (phys(r) << 5) | phys(r)); }
    void sar32_cl(Reg r) { emit(0x1AC02800u | (phys(RCX) << 16) | (phys(r) << 5) | phys(r)); }
    // x86's bt sets CF to the bit. The only reader branches on CC_B ("carry set"), which here
    // is "carry clear", so the flags are arranged for that: 0 - bit borrows exactly when set.
    void bt_imm(Reg r, uint8_t bit) {
        emit(0xD3400000u | (static_cast<uint32_t>(bit) << 16) | (static_cast<uint32_t>(bit) << 10) |
             (phys(r) << 5) | kScratchData);                                  // ubfx x17, r, #bit, #1
        emit(0xEB0003FFu | (kScratchData << 16));                             // cmp xzr, x17
    }

    void setcc(Cond c, Reg r) { if (arm(c) == 0xFF) { failed_ = true; return; } emit(0x1A9F07E0u | ((arm(c) ^ 1u) << 12) | phys(r)); }   // cset w, c
    void cmov(Cond c, Reg d, Reg s) {                                                    // csel d, s, d, c
        emit(0x9A800000u | (phys(d) << 16) | (arm(c) << 12) | (phys(s) << 5) | phys(d));
    }

    // ---- control flow --------------------------------------------------
    void jmp(Label& l) { branch(0x14000000u, l); }
    void jcc(Cond c, Label& l) {
        if (arm(c) == 0xFF) { failed_ = true; return; }     // parity: no A64 counterpart, and nothing here uses it
        branch(0x54000000u | arm(c), l);
    }
    void jmp_reg(Reg r) { emit(0xD61F0000u | (phys(r) << 5)); }
    void call_reg(Reg r) { emit(0xD63F0000u | (phys(r) << 5)); copy_result(); }
    void call_abs(const void* fn) {
        mov_imm_to(kScratchAddr, reinterpret_cast<uint64_t>(fn));
        emit(0xD63F0000u | (kScratchAddr << 5));
        copy_result();
    }
    void call_mem(Mem m) {
        access(8, true, kScratchAddr, m);
        emit(0xD63F0000u | (kScratchAddr << 5));
        copy_result();
    }
    void ret() { emit(0xD65F03C0u); }
    // x86's carry flag is how a stub says hit or miss. A64 returns flags just as well, and the
    // caller branches on CC_B, which is "carry clear": so a miss clears carry and a hit sets it.
    // (cmp xzr, xzr leaves C=1; cmn xzr, xzr leaves C=0.)
    void clc() { emit(0xEB1F03FFu); }
    void stc() { emit(0xAB1F03FFu); }
    void push(Reg r) { emit(0xF81F0FE0u | phys(r)); }   // str r, [sp, #-16]!  (sp stays 16-aligned)
    void pop(Reg r) { emit(0xF84107E0u | phys(r)); }    // ldr r, [sp], #16
    // The link register survives a nested call only if it is saved: a stub that calls out
    // brackets the call with these. No-ops on x86, where the return address is on the stack.
    void save_link() { emit(0xF81F0FE0u | kLr); }
    void restore_link() { emit(0xF84107E0u | kLr); }
    // The entry sequence of the generated code's one C-callable function: fp and lr, then
    // x19-x24 (the roles RBX, R12-R15 and RBP). 80 bytes, so sp stays 16-aligned for every
    // call the body makes. leave_frame undoes it and returns RAX as the function's result.
    void enter_frame() {
        pair(0xA9800000u, 29, 30, 31, -80);        // stp x29, x30, [sp, #-80]!
        pair(0xA9000000u, 19, 20, 31, 16);         // stp x19, x20, [sp, #16]
        pair(0xA9000000u, 21, 22, 31, 32);
        pair(0xA9000000u, 23, 24, 31, 48);
    }
    void leave_frame() {
        mov_x0_from_rax();
        pair(0xA9400000u, 19, 20, 31, 16);         // ldp x19, x20, [sp, #16]
        pair(0xA9400000u, 21, 22, 31, 32);
        pair(0xA9400000u, 23, 24, 31, 48);
        pair(0xA8C00000u, 29, 30, 31, 80);         // ldp x29, x30, [sp], #80
        ret();
    }
    void int3() { emit(0xD4200000u); }   // brk #0
    void ud2() { emit(0xD4200000u); }

    // ---- floating point ------------------------------------------------
    void movq_to_xmm(Xmm x, Reg r) { emit(0x9E670000u | (phys(r) << 5) | x); }     // fmov dX, r
    void movq_from_xmm(Reg r, Xmm x) { emit(0x9E660000u | (static_cast<uint32_t>(x) << 5) | phys(r)); }
    void addsd(Xmm d, Xmm s) { emit(0x1E602800u | (static_cast<uint32_t>(s) << 16) | (static_cast<uint32_t>(d) << 5) | d); }
    void subsd(Xmm d, Xmm s) { emit(0x1E603800u | (static_cast<uint32_t>(s) << 16) | (static_cast<uint32_t>(d) << 5) | d); }
    void mulsd(Xmm d, Xmm s) { emit(0x1E600800u | (static_cast<uint32_t>(s) << 16) | (static_cast<uint32_t>(d) << 5) | d); }
    void divsd(Xmm d, Xmm s) { emit(0x1E601800u | (static_cast<uint32_t>(s) << 16) | (static_cast<uint32_t>(d) << 5) | d); }
    void addsd_mem(Xmm d, Mem m) {
        access_fp(true, kFpScratch, m);
        emit(0x1E602800u | (kFpScratch << 16) | (static_cast<uint32_t>(d) << 5) | d);
    }
    void ucomisd(Xmm a, Xmm b) { emit(0x1E602000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 5)); }   // fcmp
    void cvtsi2sd32(Xmm d, Reg r) { emit(0x1E620000u | (phys(r) << 5) | d); }       // scvtf dD, wR
    void cvtsi2sd64(Xmm d, Reg r) { emit(0x9E620000u | (phys(r) << 5) | d); }       // scvtf dD, xR
    // x86's cvttsd2si answers 0x80000000 for anything outside int32; fcvtzs saturates. The
    // generated code tests for that value, so it is produced here: convert to 64 bits, and
    // if that is not the sign extension of its low half the result is out of range.
    void cvttsd2si32(Reg d, Xmm s) {
        emit(0x9E780000u | (static_cast<uint32_t>(s) << 5) | kScratchData);                              // fcvtzs x17, dS
        emit(0xEB20C000u | (kScratchData << 16) | (kScratchData << 5) | kZr);                             // cmp x17, w17, sxtw
        emit(0x52A00000u | (0x8000u << 5) | kScratchAddr);                                                // mov w16, #0x80000000
        emit(0x1A800000u | (kScratchAddr << 16) | (0u << 12) | (kScratchData << 5) | phys(d));            // csel wD, w17, w16, eq
    }

private:
    bool failed_ = false;

    static uint32_t arm(Cond c) { return kArmCond[c]; }
    static uint32_t log2_scale(uint8_t s) { return s == 8 ? 3 : s == 4 ? 2 : s == 2 ? 1 : 0; }

    void mov_x0_from_rax() { emit(0xAA0003E0u | (phys(RAX) << 16) | 0u); }   // mov x0, rax
    // stp/ldp of two 64-bit registers: base opcode picks pre/post-index or signed offset.
    void pair(uint32_t base, uint32_t rt, uint32_t rt2, uint32_t rn, int32_t byte_offset) {
        emit(base | ((static_cast<uint32_t>(byte_offset / 8) & 0x7Fu) << 15) | (rt2 << 10) | (rn << 5) | rt);
    }

    // A call's result is in x0; the generated code reads it from RAX.
    void copy_result() { emit(0xAA0003E0u | (0u << 16) | phys(RAX)); }   // mov rax, x0

    // movz/movk (or movn/movk, when more halfwords are all-ones than zero) for a constant. A 32-bit
    // one is the same with the high halves zero: the full register is written, so the upper
    // half is zero as it is after x86's mov r32, imm32.
    void mov_imm_to(uint32_t rd, uint64_t imm) {
        int nonzero = 0, not_ones = 0;
        for (int i = 0; i < 4; i++) {
            const uint32_t h = static_cast<uint32_t>((imm >> (16 * i)) & 0xFFFF);
            if (h != 0) nonzero++;
            if (h != 0xFFFF) not_ones++;
        }
        if (not_ones < nonzero) {
            int k = 0;
            while (k < 3 && ((imm >> (16 * k)) & 0xFFFF) == 0xFFFF) k++;
            const uint32_t hk = static_cast<uint32_t>((imm >> (16 * k)) & 0xFFFF);
            emit(0x92800000u | (static_cast<uint32_t>(k) << 21) | ((~hk & 0xFFFFu) << 5) | rd);     // movn
            for (int i = 0; i < 4; i++) {
                const uint32_t h = static_cast<uint32_t>((imm >> (16 * i)) & 0xFFFF);
                if (i == k || h == 0xFFFF) continue;
                emit(0xF2800000u | (static_cast<uint32_t>(i) << 21) | (h << 5) | rd);              // movk
            }
            return;
        }
        if (imm == 0) { emit(0xD2800000u | rd); return; }
        bool first = true;
        for (int i = 0; i < 4; i++) {
            const uint32_t h = static_cast<uint32_t>((imm >> (16 * i)) & 0xFFFF);
            if (h == 0) continue;
            emit((first ? 0xD2800000u : 0xF2800000u) | (static_cast<uint32_t>(i) << 21) | (h << 5) | rd);
            first = false;
        }
    }

    // rd = rn + imm, 64-bit; rn/rd may be sp.
    void add_imm_to(uint32_t rd, uint32_t rn, int64_t imm) {
        if (imm == 0 && rd == rn) return;
        const bool neg = imm < 0;
        const uint64_t mag = neg ? static_cast<uint64_t>(-imm) : static_cast<uint64_t>(imm);
        const uint32_t base = neg ? 0xD1000000u : 0x91000000u;
        if (mag <= 4095) {
            emit(base | (static_cast<uint32_t>(mag) << 10) | (rn << 5) | rd);
        } else if ((mag & 0xFFF) == 0 && (mag >> 12) <= 4095) {
            emit(base | (1u << 22) | (static_cast<uint32_t>(mag >> 12) << 10) | (rn << 5) | rd);
        } else {
            mov_imm_to(kScratchData, mag);
            // add/sub (extended register) so that sp is a legal operand
            emit((neg ? 0xCB200000u : 0x8B200000u) | (kScratchData << 16) | (3u << 13) | (rn << 5) | rd);   // uxtx
        }
    }
    void add32_imm_to(uint32_t rd, int64_t imm) {
        if (imm == 0) return;
        const bool neg = imm < 0;
        const uint64_t mag = neg ? static_cast<uint64_t>(-imm) : static_cast<uint64_t>(imm);
        if (mag <= 4095) {
            emit((neg ? 0x51000000u : 0x11000000u) | (static_cast<uint32_t>(mag) << 10) | (rd << 5) | rd);
        } else {
            mov_imm_to(kScratchData, mag);
            emit((neg ? 0x4B000000u : 0x0B000000u) | (kScratchData << 16) | (rd << 5) | rd);
        }
    }
    void cmp_imm_to(uint32_t rn, int32_t imm, bool wide) {
        const uint32_t sf = wide ? 0x80000000u : 0u;
        if (imm >= 0 && imm <= 4095) {
            emit(sf | 0x7100001Fu | (static_cast<uint32_t>(imm) << 10) | (rn << 5));      // subs zr, rn, #imm
        } else if (imm < 0 && imm >= -4095) {
            emit(sf | 0x3100001Fu | (static_cast<uint32_t>(-static_cast<int64_t>(imm)) << 10) | (rn << 5));   // adds zr, rn, #-imm
        } else {
            if (wide) mov_imm_to(kScratchAddr, static_cast<uint64_t>(static_cast<int64_t>(imm)));
            else mov_imm_to(kScratchAddr, static_cast<uint32_t>(imm));
            emit(sf | 0x6B00001Fu | (kScratchAddr << 16) | (rn << 5));
        }
    }
    // and/orr with an immediate: x86's sign-extends a 32-bit one for the 64-bit forms, so the
    // constant is built at full width and the register form used. (A64's own logical
    // immediates only cover bit patterns; none of the masks used here need to be fast to emit.)
    void logical_imm(uint32_t reg_form, Reg d, int64_t imm) {
        mov_imm_to(kScratchData, static_cast<uint64_t>(imm));
        emit(reg_form | (kScratchData << 16) | (phys(d) << 5) | phys(d));
    }

    // Loads and stores of 1, 4 or 8 bytes. `rt` is a hardware register number.
    void access(int size, bool load, uint32_t rt, const Mem& m) {
        uint32_t rn = phys(m.base);
        int64_t disp = m.disp;
        if (m.index >= 0) {
            const uint32_t rm = phys(static_cast<Reg>(m.index));
            if (disp == 0 && (m.scale == size || m.scale == 1)) {
                // [base, index{, lsl #scale}]
                const uint32_t s = (m.scale == 1 || size == 1) ? 0u : 1u;
                emit(reg_offset_op(size, load) | (rm << 16) | (3u << 13) | (s << 12) | (rn << 5) | rt);
                return;
            }
            emit(0x8B000000u | (rm << 16) | (log2_scale(m.scale) << 10) | (rn << 5) | kScratchAddr);   // add x16, base, index, lsl #s
            rn = kScratchAddr;
        }
        const int64_t scaled = disp / size;
        if (disp >= 0 && disp % size == 0 && scaled <= 4095) {
            emit(unsigned_op(size, load) | (static_cast<uint32_t>(scaled) << 10) | (rn << 5) | rt);
        } else if (disp >= -256 && disp <= 255) {
            emit(unscaled_op(size, load) | ((static_cast<uint32_t>(disp) & 0x1FFu) << 12) | (rn << 5) | rt);
        } else if (rn == kScratchAddr) {
            failed_ = true;                           // an index and a far displacement together: nothing emits one
        } else {
            mov_imm_to(kScratchAddr, static_cast<uint64_t>(disp));
            emit(reg_offset_op(size, load) | (kScratchAddr << 16) | (3u << 13) | (rn << 5) | rt);   // [base, x16]
        }
    }
    static uint32_t unsigned_op(int size, bool load) {
        switch (size) {
            case 8: return load ? 0xF9400000u : 0xF9000000u;
            case 4: return load ? 0xB9400000u : 0xB9000000u;
            default: return load ? 0x39400000u : 0x39000000u;
        }
    }
    static uint32_t unscaled_op(int size, bool load) {
        switch (size) {
            case 8: return load ? 0xF8400000u : 0xF8000000u;
            case 4: return load ? 0xB8400000u : 0xB8000000u;
            default: return load ? 0x38400000u : 0x38000000u;
        }
    }
    static uint32_t reg_offset_op(int size, bool load) {
        switch (size) {
            case 8: return load ? 0xF8600800u : 0xF8200800u;
            case 4: return load ? 0xB8600800u : 0xB8200800u;
            default: return load ? 0x38600800u : 0x38200800u;
        }
    }
    // d-register load (8 bytes) for addsd_mem.
    void access_fp(bool load, uint32_t rt, const Mem& m) {
        (void)load;
        uint32_t rn = phys(m.base);
        const int64_t disp = m.disp;
        if (m.index >= 0) {
            emit(0x8B000000u | (phys(static_cast<Reg>(m.index)) << 16) | (log2_scale(m.scale) << 10) | (rn << 5) | kScratchAddr);
            rn = kScratchAddr;
        }
        if (disp >= 0 && disp % 8 == 0 && disp / 8 <= 4095) {
            emit(0xFD400000u | (static_cast<uint32_t>(disp / 8) << 10) | (rn << 5) | rt);
        } else if (disp >= -256 && disp <= 255) {
            emit(0xFC400000u | ((static_cast<uint32_t>(disp) & 0x1FFu) << 12) | (rn << 5) | rt);
        } else if (rn == kScratchAddr) {
            failed_ = true;
        } else {
            mov_imm_to(kScratchAddr, static_cast<uint64_t>(disp));
            emit(0xFC600800u | (kScratchAddr << 16) | (3u << 13) | (rn << 5) | rt);                  // ldr d, [base, x16]
        }
    }

    // b / b.cond to a label: the displacement is in instructions, patched at bind if forward.
    void branch(uint32_t opcode, Label& l) {
        const uint32_t at = static_cast<uint32_t>(buf.size());
        emit(opcode);
        if (l.pos >= 0) patch_branch(at, l.pos);
        else l.fixups.push_back(at);
    }
    void patch_branch(uint32_t at, int32_t target) {
        uint32_t w;
        std::memcpy(&w, &buf[at], 4);
        const int64_t off = (static_cast<int64_t>(target) - static_cast<int64_t>(at)) / 4;
        if ((w & 0xFC000000u) == 0x14000000u) {                  // b
            if (off < -(1 << 25) || off >= (1 << 25)) { failed_ = true; return; }
            w = (w & 0xFC000000u) | (static_cast<uint32_t>(off) & 0x03FFFFFFu);
        } else {                                                 // b.cond
            if (off < -(1 << 18) || off >= (1 << 18)) { failed_ = true; return; }
            w = (w & 0xFF00001Fu) | ((static_cast<uint32_t>(off) & 0x7FFFFu) << 5);
        }
        std::memcpy(&buf[at], &w, 4);
    }
};

}  // namespace a64
}  // namespace Quanta

#endif
