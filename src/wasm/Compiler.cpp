/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Validation of the code of a function, and the making of the code it is run from. The two are one pass: the validator knows at each
// instruction what is on the stack, and the instruction is made to say so.

#include <algorithm>
#include <cstring>

#include "quanta/wasm/Reader.h"
#include "Internal.h"

namespace Quanta::Wasm {

namespace {

constexpr size_t kNone = static_cast<size_t>(-1);

struct SlotCount {
    uint32_t n = 0, r = 0;
};

SlotCount Slots(const ValType& t) {
    switch (t.kind) {
        case ValType::Kind::Ref: return {0, 1};
        case ValType::Kind::V128: return {2, 0};
        case ValType::Kind::Bottom: return {0, 0};
        default: return {1, 0};
    }
}

struct Ctl {
    enum class Kind : uint8_t { Func, Block, Loop, If, Else, TryTable } kind = Kind::Block;
    std::vector<ValType> params, results;
    size_t height = 0;  // the types of the operand stack below this block's own
    uint32_t baseN = 0, baseR = 0;
    bool unreachable = false;
    bool deadEntry = false;  // it is in code that is not run, which is not emitted
    size_t loopStart = 0;
    std::vector<size_t> fixups;  // the words that take the place of the end of it
    size_t ifFixup = kNone;      // the target of the jump of an if that is not taken
    size_t elseFixup = kNone;    // the jump from the end of the then part
    size_t initMark = 0;
};

class FuncCompiler {
public:
    FuncCompiler(ModuleContext& cx, const uint8_t* code, size_t size, size_t base, CompiledFunction& out, bool constMode, uint32_t globalLimit)
        : cx_(cx), m_(cx.module), r_(code, size, base), out_(out), constMode_(constMode), globalLimit_(globalLimit) {}

    bool Function(uint32_t index, std::string& error) {
        const FuncType& sig = TypeRegistry::Get(cx_.funcSigs[index]).func;
        out_ = CompiledFunction{};
        out_.type = cx_.funcSigs[index];
        for (const ValType& p : sig.params) {
            locals_.push_back(p);
            inited_.push_back(true);
            Assign(p, true);
        }
        const FuncBody& body = m_.bodies[index - m_.importedFuncs];
        for (const ValType& l : body.locals) {
            if (l.IsRef() && l.heap >= 0 && static_cast<size_t>(l.heap) >= m_.types.size()) return Fail(error, "unknown type");
            const ValType t = cx_.Canon(l);
            locals_.push_back(t);
            inited_.push_back(Defaultable(t));
            Assign(t, false);
        }
        for (const ValType& p : sig.params) {
            const SlotCount s = Slots(p);
            out_.paramsN += s.n;
            out_.paramsR += s.r;
        }
        for (const ValType& res : sig.results) {
            const SlotCount s = Slots(res);
            out_.resultsN += s.n;
            out_.resultsR += s.r;
        }
        Ctl f;
        f.kind = Ctl::Kind::Func;
        f.results = sig.results;
        ctls_.push_back(std::move(f));
        return Run(error);
    }

    bool ConstExpr(const ValType& expected, std::string& error) {
        out_ = CompiledFunction{};
        Ctl f;
        f.kind = Ctl::Kind::Func;
        f.results = {expected};
        ctls_.push_back(std::move(f));
        const SlotCount s = Slots(expected);
        out_.resultsN = s.n;
        out_.resultsR = s.r;
        return Run(error);
    }

private:
    ModuleContext& cx_;
    const Module& m_;
    Reader r_;
    CompiledFunction& out_;
    bool constMode_;
    uint32_t globalLimit_;
    std::vector<ValType> locals_;
    std::vector<bool> inited_;       // (a local of a type that has no default has to be set before it is read)
    std::vector<uint32_t> initLog_;  // the ones that were set since each block began, for the end of it to forget
    std::vector<uint32_t> localSlot_;
    std::vector<ValType> vals_;
    std::vector<Ctl> ctls_;
    uint32_t hN_ = 0, hR_ = 0;
    std::string err_;
    uint32_t localsN_ = 0, localsR_ = 0;
    uint32_t paramSlotsN_ = 0, paramSlotsR_ = 0;
    bool inParams_ = true;

    bool Fail(std::string& error, const std::string& message) {
        error = message;
        return false;
    }

    // ---- Locals ----

    void Assign(const ValType& t, bool param) {
        const SlotCount s = Slots(t);
        if (t.IsRef()) {
            localSlot_.push_back(localsR_);
            localsR_ += s.r;
        } else {
            localSlot_.push_back(localsN_);
            localsN_ += s.n;
        }
        if (!param) {
            out_.localsN += s.n;
            out_.localsR += s.r;
        }
    }

    static bool Defaultable(const ValType& t) { return !(t.IsRef() && !t.nullable); }

    // ---- The code ----

    void Emit(uint32_t w) { out_.code.push_back(w); }
    void Emit(uint32_t a, uint32_t b) { Emit(a); Emit(b); }
    size_t Here() const { return out_.code.size(); }

    bool Dead() const { return ctls_.back().unreachable || ctls_.back().deadEntry; }

    // ---- The types on the stack ----

    bool Error(const std::string& message) {
        if (err_.empty()) err_ = message + " @+" + std::to_string(r_.offset());
        return false;
    }

    void Push(const ValType& t) {
        vals_.push_back(t);
        if (!Dead()) {
            const SlotCount s = Slots(t);
            hN_ += s.n;
            hR_ += s.r;
            out_.maxN = std::max(out_.maxN, hN_);
            out_.maxR = std::max(out_.maxR, hR_);
        }
    }

    // The type that is taken off if it is what is expected (an unreachable stack gives whatever is asked for).
    bool Pop(const ValType& expected, ValType* actual = nullptr) {
        Ctl& c = ctls_.back();
        if (vals_.size() == c.height) {
            if (c.unreachable) {
                if (actual) *actual = ValType::Bottom();
                return true;
            }
            return Error("type mismatch: not enough operands");
        }
        const ValType t = vals_.back();
        vals_.pop_back();
        if (!Dead()) {
            const SlotCount s = Slots(t);
            hN_ -= s.n;
            hR_ -= s.r;
        }
        if (!expected.IsBottom() && !TypeRegistry::Matches(t, expected)) return Error("type mismatch");
        if (actual) *actual = t;
        return true;
    }

    bool PopAny(ValType& t) {
        Ctl& c = ctls_.back();
        if (vals_.size() == c.height) {
            if (c.unreachable) { t = ValType::Bottom(); return true; }
            return Error("type mismatch: not enough operands");
        }
        t = vals_.back();
        vals_.pop_back();
        if (!Dead()) {
            const SlotCount s = Slots(t);
            hN_ -= s.n;
            hR_ -= s.r;
        }
        return true;
    }

    bool PopVals(const std::vector<ValType>& types) {
        for (size_t i = types.size(); i-- > 0;) {
            if (!Pop(types[i])) return false;
        }
        return true;
    }
    void PushVals(const std::vector<ValType>& types) {
        for (const ValType& t : types) Push(t);
    }

    void SetUnreachable() {
        Ctl& c = ctls_.back();
        vals_.resize(c.height);
        c.unreachable = true;
    }

    // ---- Control ----

    SlotCount SlotsOf(const std::vector<ValType>& types) const {
        SlotCount total;
        for (const ValType& t : types) {
            const SlotCount s = Slots(t);
            total.n += s.n;
            total.r += s.r;
        }
        return total;
    }

    // The types a branch to the label gives or takes.
    const std::vector<ValType>& LabelTypes(const Ctl& c) const { return c.kind == Ctl::Kind::Loop ? c.params : c.results; }

    bool ReadBlockType(std::vector<ValType>& params, std::vector<ValType>& results) {
        const uint8_t b = r_.peek();
        if (!r_.ok()) return Error(r_.error());
        if (b == 0x40) {
            r_.u8();
            return true;
        }
        const bool valtype = (b >= 0x7B && b <= 0x7F) || (b >= 0x69 && b <= 0x74) || b == 0x63 || b == 0x64;
        if (valtype) {
            const ValType t = r_.valType();
            if (!r_.ok()) return Error(r_.error());
            if (!ValidValType(t)) return false;
            results.push_back(cx_.Canon(t));
            return true;
        }
        const int64_t index = r_.s33();
        if (!r_.ok()) return Error(r_.error());
        if (index < 0 || static_cast<uint64_t>(index) >= m_.types.size()) return Error("unknown type");
        const CanonType& t = TypeRegistry::Get(cx_.typeIds[static_cast<size_t>(index)]);
        if (t.kind != TypeDef::Kind::Func) return Error("block type is not a function type");
        params = t.func.params;
        results = t.func.results;
        return true;
    }

    bool ValidValType(const ValType& t) {
        if (t.IsRef() && t.heap >= 0 && static_cast<size_t>(t.heap) >= m_.types.size()) return Error("unknown type");
        return true;
    }

    void BranchWords(const Ctl& target, uint32_t keepN, uint32_t keepR) {
        const uint32_t dropN = hN_ - target.baseN - keepN;
        const uint32_t dropR = hR_ - target.baseR - keepR;
        Emit(keepN);
        Emit(keepR);
        Emit(dropN);
        Emit(dropR);
    }

    // The target word of a branch to the label: known now for a loop, patched later for the others.
    void EmitTarget(Ctl& target) {
        if (target.kind == Ctl::Kind::Loop) {
            Emit(static_cast<uint32_t>(target.loopStart));
        } else {
            target.fixups.push_back(Here());
            Emit(0);
        }
    }

    bool Label(uint32_t depth, Ctl*& target) {
        if (depth >= ctls_.size()) return Error("unknown label");
        target = &ctls_[ctls_.size() - 1 - depth];
        return true;
    }

    bool BeginBlock(Ctl::Kind kind) {
        std::vector<ValType> params, results;
        if (!ReadBlockType(params, results)) return false;
        if (kind == Ctl::Kind::If) {
            if (!Pop(ValType::I32())) return false;
            if (!Dead()) {
                // (The jump that skips the then part.)
            }
        }
        const bool deadEntry = Dead();
        size_t jz = kNone;
        if (kind == Ctl::Kind::If && !deadEntry) {
            Emit(Op::kJz);
            jz = Here();
            Emit(0);
        }
        if (!PopVals(params)) return false;
        Ctl c;
        c.kind = kind;
        c.params = params;
        c.results = results;
        c.height = vals_.size();
        c.deadEntry = deadEntry;
        c.baseN = hN_;
        c.baseR = hR_;
        c.ifFixup = jz;
        c.initMark = initLog_.size();
        c.loopStart = Here();
        ctls_.push_back(std::move(c));
        PushVals(params);
        return true;
    }

    bool End(std::string& error) {
        Ctl& c = ctls_.back();
        if (!PopVals(c.results)) return false;
        if (vals_.size() != c.height) return Error("type mismatch: values left on the stack at the end of a block");
        if (c.kind == Ctl::Kind::If) {
            // (An if with no else passes its parameters on: they have to be what it gives.)
            if (c.params != c.results) return Error("type mismatch: if without else has other results than parameters");
        }
        Ctl done = std::move(c);
        ctls_.pop_back();
        ForgetInits(done.initMark);
        if (done.kind == Ctl::Kind::Func) {
            if (!done.deadEntry) {
                const SlotCount s = SlotsOf(done.results);
                const size_t returnAt = Here();
                Emit(Op::kReturn);
                Emit(s.n);
                Emit(s.r);
                for (size_t f : done.fixups) out_.code[f] = static_cast<uint32_t>(returnAt);
            }
            if (!r_.atEnd()) return Error("operators remaining after end of function");
            (void)error;
            return true;
        }
        if (!done.deadEntry) {
            if (done.ifFixup != kNone) out_.code[done.ifFixup] = static_cast<uint32_t>(Here());
            if (done.elseFixup != kNone) out_.code[done.elseFixup] = static_cast<uint32_t>(Here());
            for (size_t f : done.fixups) out_.code[f] = static_cast<uint32_t>(Here());
        }
        PushValsAfterBlock(done);
        return true;
    }

    // What the block gives is on the stack of the code around it; its height is what the block began at, and the results.
    void PushValsAfterBlock(const Ctl& done) {
        if (!Dead()) {
            hN_ = done.baseN;
            hR_ = done.baseR;
        }
        PushVals(done.results);
    }

    bool Else() {
        Ctl& c = ctls_.back();
        if (c.kind != Ctl::Kind::If) return Error("else does not belong to an if");
        if (!PopVals(c.results)) return false;
        if (vals_.size() != c.height) return Error("type mismatch: values left on the stack at the else");
        if (!c.deadEntry) {
            if (!c.unreachable) {
                Emit(Op::kJmp);
                c.elseFixup = Here();
                Emit(0);
            }
            if (c.ifFixup != kNone) out_.code[c.ifFixup] = static_cast<uint32_t>(Here());
            c.ifFixup = kNone;
        }
        ForgetInits(c.initMark);
        c.kind = Ctl::Kind::Else;
        c.unreachable = false;
        vals_.resize(c.height);
        if (!c.deadEntry) {
            hN_ = c.baseN;
            hR_ = c.baseR;
        }
        PushVals(c.params);
        return true;
    }

    void ForgetInits(size_t mark) {
        while (initLog_.size() > mark) {
            inited_[initLog_.back()] = false;
            initLog_.pop_back();
        }
    }
    bool Br(bool conditional, bool onNull = false, bool onNonNull = false);
    bool CheckTop(const std::vector<ValType>& types);

    // ---- The instructions ----

    bool Run(std::string& error) {
        while (r_.ok() && !ctls_.empty()) {
            if (r_.atEnd()) {
                r_.fail("function body must end with \"end\" opcode");
                break;
            }
            if (!Instruction(error)) {
                if (error.empty()) error = err_;
                return false;
            }
        }
        if (!r_.ok()) { error = r_.error(); return false; }
        if (!ctls_.empty()) { error = "function body must end with \"end\" opcode"; return false; }
        if (!err_.empty()) { error = err_; return false; }
        return true;
    }

    bool Instruction(std::string& error);
    bool Numeric(uint8_t op);
    bool MemoryAccess(uint8_t op);
    bool Prefixed(uint8_t prefix);
    bool MiscInstruction(uint32_t n);
    bool MemArg(uint32_t naturalAlign, uint32_t& memory, uint64_t& offset, bool& is64);
    bool Call(uint32_t index, bool tail);
    bool CallIndirect(bool tail);
    bool CallRef(bool tail);
    bool ConstInstruction(uint8_t op);

};

// ---- Branches ----

// Whether the top of the stack is what the types say, with nothing taken off (an unreachable stack has whatever is wanted).
bool FuncCompiler::CheckTop(const std::vector<ValType>& types) {
    const Ctl& c = ctls_.back();
    for (size_t k = 0; k < types.size(); ++k) {
        const size_t fromTop = types.size() - 1 - k;
        if (vals_.size() < c.height + fromTop + 1) {
            if (c.unreachable) continue;
            return Error("type mismatch: not enough operands");
        }
        if (!TypeRegistry::Matches(vals_[vals_.size() - 1 - fromTop], types[k])) return Error("type mismatch");
    }
    return true;
}

bool FuncCompiler::Br(bool conditional, bool onNull, bool onNonNull) {
    const uint32_t depth = r_.u32();
    if (!r_.ok()) return Error(r_.error());
    Ctl* target = nullptr;
    if (!Label(depth, target)) return false;
    const std::vector<ValType> types = LabelTypes(*target);
    if (onNull || onNonNull) {
        // A reference is on top: it is the one that is tested.
        ValType ref;
        if (!PopAny(ref)) return false;
        if (!ref.IsRef() && !ref.IsBottom()) return Error("type mismatch: a reference was expected");
        const SlotCount keep = SlotsOf(types);
        if (onNull) {
            // (If it is null, it is taken off and the branch is taken with the values below it; if not, it stays.)
            if (!Dead()) {
                Emit(Op::kBrOnNull);
                EmitTarget(*target);
                BranchWords(*target, keep.n, keep.r);
            }
            if (!PopVals(types)) return false;
            PushVals(types);
            Push(ref.IsBottom() ? ref : ValType::Ref(false, ref.heap));
        } else {
            if (types.empty() || !types.back().IsRef()) return Error("type mismatch: br_on_non_null needs a label that takes a reference");
            const std::vector<ValType> prefix(types.begin(), types.end() - 1);
            if (!ref.IsBottom() && !TypeRegistry::Matches(ValType::Ref(false, ref.heap), types.back())) return Error("type mismatch");
            // (If it is not null, it stays and the branch is taken with it; if it is, it is taken off.)
            if (!Dead()) {
                Emit(Op::kBrOnNonNull);
                EmitTarget(*target);
                Emit(keep.n);
                Emit(keep.r);
                Emit(hN_ - target->baseN - keep.n);
                Emit(hR_ + 1 - target->baseR - keep.r);
            }
            if (!PopVals(prefix)) return false;
            PushVals(prefix);
        }
        return true;
    }
    const SlotCount keep = SlotsOf(types);
    if (conditional) {
        if (!Pop(ValType::I32())) return false;
        if (!Dead()) {
            Emit(Op::kBrIf);
            EmitTarget(*target);
            BranchWords(*target, keep.n, keep.r);
        }
        if (!PopVals(types)) return false;
        PushVals(types);
        return true;
    }
    if (!Dead()) {
        Emit(Op::kBr);
        EmitTarget(*target);
        BranchWords(*target, keep.n, keep.r);
    }
    if (!PopVals(types)) return false;
    SetUnreachable();
    return true;
}

// ---- Memory ----

bool FuncCompiler::MemArg(uint32_t naturalAlign, uint32_t& memory, uint64_t& offset, bool& is64) {
    uint32_t align = r_.u32();
    if (!r_.ok()) return Error(r_.error());
    memory = 0;
    if (align & 0x40) {
        align &= ~0x40u;
        memory = r_.u32();
        if (!r_.ok()) return Error(r_.error());
    }
    if (align > 63 || align > naturalAlign) return Error("alignment must not be larger than natural");
    if (memory >= cx_.memories.size()) return Error("unknown memory");
    is64 = cx_.memories[memory].limits.is64;
    offset = is64 ? r_.u64() : r_.u32();
    if (!r_.ok()) return Error(r_.error());
    return true;
}

bool FuncCompiler::MemoryAccess(uint8_t op) {
    struct Info { uint8_t align; ValType type; bool store; };
    ValType t;
    uint32_t align;
    bool store = false;
    switch (op) {
        case 0x28: t = ValType::I32(); align = 2; break;
        case 0x29: t = ValType::I64(); align = 3; break;
        case 0x2A: t = ValType::F32(); align = 2; break;
        case 0x2B: t = ValType::F64(); align = 3; break;
        case 0x2C: case 0x2D: t = ValType::I32(); align = 0; break;
        case 0x2E: case 0x2F: t = ValType::I32(); align = 1; break;
        case 0x30: case 0x31: t = ValType::I64(); align = 0; break;
        case 0x32: case 0x33: t = ValType::I64(); align = 1; break;
        case 0x34: case 0x35: t = ValType::I64(); align = 2; break;
        case 0x36: t = ValType::I32(); align = 2; store = true; break;
        case 0x37: t = ValType::I64(); align = 3; store = true; break;
        case 0x38: t = ValType::F32(); align = 2; store = true; break;
        case 0x39: t = ValType::F64(); align = 3; store = true; break;
        case 0x3A: t = ValType::I32(); align = 0; store = true; break;
        case 0x3B: t = ValType::I32(); align = 1; store = true; break;
        case 0x3C: t = ValType::I64(); align = 0; store = true; break;
        case 0x3D: t = ValType::I64(); align = 1; store = true; break;
        default: t = ValType::I64(); align = 2; store = true; break;  // 0x3E
    }
    uint32_t memory;
    uint64_t offset;
    bool is64;
    if (!MemArg(align, memory, offset, is64)) return false;
    const ValType address = is64 ? ValType::I64() : ValType::I32();
    if (store) {
        if (!Pop(t) || !Pop(address)) return false;
    } else {
        if (!Pop(address)) return false;
        Push(t);
    }
    if (!Dead()) {
        Emit(op);
        Emit(memory);
        Emit(static_cast<uint32_t>(offset));
        Emit(static_cast<uint32_t>(offset >> 32));
    }
    return true;
}

// ---- Numbers ----

bool FuncCompiler::Numeric(uint8_t op) {
    ValType a, b, out;
    int arity = 1;
    const ValType i32 = ValType::I32(), i64 = ValType::I64(), f32 = ValType::F32(), f64 = ValType::F64();
    if (op == 0x45) { a = i32; out = i32; }
    else if (op >= 0x46 && op <= 0x4F) { a = b = i32; out = i32; arity = 2; }
    else if (op == 0x50) { a = i64; out = i32; }
    else if (op >= 0x51 && op <= 0x5A) { a = b = i64; out = i32; arity = 2; }
    else if (op >= 0x5B && op <= 0x60) { a = b = f32; out = i32; arity = 2; }
    else if (op >= 0x61 && op <= 0x66) { a = b = f64; out = i32; arity = 2; }
    else if (op >= 0x67 && op <= 0x69) { a = i32; out = i32; }
    else if (op >= 0x6A && op <= 0x78) { a = b = i32; out = i32; arity = 2; }
    else if (op >= 0x79 && op <= 0x7B) { a = i64; out = i64; }
    else if (op >= 0x7C && op <= 0x8A) { a = b = i64; out = i64; arity = 2; }
    else if (op >= 0x8B && op <= 0x91) { a = f32; out = f32; }
    else if (op >= 0x92 && op <= 0x98) { a = b = f32; out = f32; arity = 2; }
    else if (op >= 0x99 && op <= 0x9F) { a = f64; out = f64; }
    else if (op >= 0xA0 && op <= 0xA6) { a = b = f64; out = f64; arity = 2; }
    else {
        switch (op) {
            case 0xA7: a = i64; out = i32; break;
            case 0xA8: case 0xA9: a = f32; out = i32; break;
            case 0xAA: case 0xAB: a = f64; out = i32; break;
            case 0xAC: case 0xAD: a = i32; out = i64; break;
            case 0xAE: case 0xAF: a = f32; out = i64; break;
            case 0xB0: case 0xB1: a = f64; out = i64; break;
            case 0xB2: case 0xB3: a = i32; out = f32; break;
            case 0xB4: case 0xB5: a = i64; out = f32; break;
            case 0xB6: a = f64; out = f32; break;
            case 0xB7: case 0xB8: a = i32; out = f64; break;
            case 0xB9: case 0xBA: a = i64; out = f64; break;
            case 0xBB: a = f32; out = f64; break;
            case 0xBC: a = f32; out = i32; break;
            case 0xBD: a = f64; out = i64; break;
            case 0xBE: a = i32; out = f32; break;
            case 0xBF: a = i64; out = f64; break;
            case 0xC0: case 0xC1: a = i32; out = i32; break;
            case 0xC2: case 0xC3: case 0xC4: a = i64; out = i64; break;
            default: return Error("invalid opcode");
        }
    }
    if (arity == 2 && !Pop(b)) return false;
    if (!Pop(a)) return false;
    Push(out);
    if (!Dead()) Emit(op);
    return true;
}

// ---- Calls ----

bool FuncCompiler::Call(uint32_t index, bool tail) {
    if (index >= cx_.funcSigs.size()) return Error("unknown function");
    const FuncType& sig = TypeRegistry::Get(cx_.funcSigs[index]).func;
    if (tail) {
        const std::vector<ValType>& mine = ctls_.front().results;
        if (sig.results.size() != mine.size()) return Error("type mismatch: return_call results do not match the function's");
        for (size_t i = 0; i < mine.size(); ++i) {
            if (!TypeRegistry::Matches(sig.results[i], mine[i])) return Error("type mismatch: return_call results do not match the function's");
        }
    }
    if (!PopVals(sig.params)) return false;
    if (!Dead()) {
        Emit(tail ? Op::kReturnCall : Op::kCall);
        Emit(index);
    }
    if (tail) {
        SetUnreachable();
    } else {
        PushVals(sig.results);
    }
    return true;
}

bool FuncCompiler::CallIndirect(bool tail) {
    const uint32_t typeIndex = r_.u32();
    const uint32_t table = r_.u32();
    if (!r_.ok()) return Error(r_.error());
    if (typeIndex >= m_.types.size()) return Error("unknown type");
    if (table >= cx_.tables.size()) return Error("unknown table");
    const TableType& tt = cx_.tables[table];
    if (!tt.elem.IsRef() || !TypeRegistry::HeapSubtype(tt.elem.heap, Heap::Func)) return Error("type mismatch: call_indirect needs a table of functions");
    const uint32_t canon = cx_.typeIds[typeIndex];
    const CanonType& t = TypeRegistry::Get(canon);
    if (t.kind != TypeDef::Kind::Func) return Error("call_indirect type is not a function type");
    if (tail) {
        const std::vector<ValType>& mine = ctls_.front().results;
        if (t.func.results.size() != mine.size()) return Error("type mismatch: return_call_indirect results do not match the function's");
        for (size_t i = 0; i < mine.size(); ++i) {
            if (!TypeRegistry::Matches(t.func.results[i], mine[i])) return Error("type mismatch: return_call_indirect results do not match the function's");
        }
    }
    if (!Pop(tt.limits.is64 ? ValType::I64() : ValType::I32())) return false;
    if (!PopVals(t.func.params)) return false;
    if (!Dead()) {
        Emit(tail ? Op::kReturnCallIndirect : Op::kCallIndirect);
        Emit(canon);
        Emit(table);
    }
    if (tail) SetUnreachable();
    else PushVals(t.func.results);
    return true;
}

bool FuncCompiler::CallRef(bool tail) {
    const uint32_t typeIndex = r_.u32();
    if (!r_.ok()) return Error(r_.error());
    if (typeIndex >= m_.types.size()) return Error("unknown type");
    const uint32_t canon = cx_.typeIds[typeIndex];
    const CanonType& t = TypeRegistry::Get(canon);
    if (t.kind != TypeDef::Kind::Func) return Error("call_ref type is not a function type");
    if (tail) {
        const std::vector<ValType>& mine = ctls_.front().results;
        if (t.func.results.size() != mine.size()) return Error("type mismatch: return_call_ref results do not match the function's");
        for (size_t i = 0; i < mine.size(); ++i) {
            if (!TypeRegistry::Matches(t.func.results[i], mine[i])) return Error("type mismatch: return_call_ref results do not match the function's");
        }
    }
    if (!Pop(ValType::Ref(true, static_cast<HeapType>(canon)))) return false;
    if (!PopVals(t.func.params)) return false;
    if (!Dead()) {
        Emit(tail ? Op::kReturnCallRef : Op::kCallRef);
        Emit(canon);
    }
    if (tail) SetUnreachable();
    else PushVals(t.func.results);
    return true;
}

// ---- The rest ----

bool FuncCompiler::ConstInstruction(uint8_t op) {
    switch (op) {
        case 0x41: {
            const int32_t v = r_.s32();
            if (!r_.ok()) return Error(r_.error());
            Push(ValType::I32());
            if (!Dead()) Emit(Op::kConstN, static_cast<uint32_t>(v));
            return true;
        }
        case 0x42: {
            const int64_t v = r_.s64();
            if (!r_.ok()) return Error(r_.error());
            Push(ValType::I64());
            if (!Dead()) {
                Emit(Op::kConstW);
                Emit(static_cast<uint32_t>(static_cast<uint64_t>(v)));
                Emit(static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32));
            }
            return true;
        }
        case 0x43: {
            const uint32_t v = r_.fixed32();
            if (!r_.ok()) return Error(r_.error());
            Push(ValType::F32());
            if (!Dead()) Emit(Op::kConstN, v);
            return true;
        }
        default: {
            const uint64_t v = r_.fixed64();
            if (!r_.ok()) return Error(r_.error());
            Push(ValType::F64());
            if (!Dead()) {
                Emit(Op::kConstW);
                Emit(static_cast<uint32_t>(v));
                Emit(static_cast<uint32_t>(v >> 32));
            }
            return true;
        }
    }
}

bool FuncCompiler::Prefixed(uint8_t prefix) {
    const uint32_t n = r_.u32();
    if (!r_.ok()) return Error(r_.error());
    if (prefix == 0xFC) return MiscInstruction(n);
    return Error("invalid opcode");
}

// 0xFC: the saturating conversions and the bulk memory and table instructions.
bool FuncCompiler::MiscInstruction(uint32_t n) {
    const ValType i32 = ValType::I32(), i64 = ValType::I64();
    if (n <= 7) {
        ValType from, to;
        switch (n) {
            case 0: case 1: from = ValType::F32(); to = i32; break;
            case 2: case 3: from = ValType::F64(); to = i32; break;
            case 4: case 5: from = ValType::F32(); to = i64; break;
            default: from = ValType::F64(); to = i64; break;
        }
        if (!Pop(from)) return false;
        Push(to);
        if (!Dead()) Emit(Op::kFcBase + n);
        return true;
    }
    const auto memoryAddress = [&](uint32_t memory) { return cx_.memories[memory].limits.is64 ? i64 : i32; };
    const auto tableAddress = [&](uint32_t table) { return cx_.tables[table].limits.is64 ? i64 : i32; };
    switch (n) {
        case 8: {  // memory.init
            const uint32_t data = r_.u32();
            const uint32_t memory = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (!m_.hasDataCount) return Error("data count section required");
            if (data >= m_.dataCount) return Error("unknown data segment");
            if (memory >= cx_.memories.size()) return Error("unknown memory");
            if (!Pop(i32) || !Pop(i32) || !Pop(memoryAddress(memory))) return false;
            if (!Dead()) { Emit(Op::kFcBase + n); Emit(data, memory); }
            return true;
        }
        case 9: {  // data.drop
            const uint32_t data = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (!m_.hasDataCount) return Error("data count section required");
            if (data >= m_.dataCount) return Error("unknown data segment");
            if (!Dead()) Emit(Op::kFcBase + n, data);
            return true;
        }
        case 10: {  // memory.copy
            const uint32_t dst = r_.u32();
            const uint32_t src = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (dst >= cx_.memories.size() || src >= cx_.memories.size()) return Error("unknown memory");
            const ValType length = cx_.memories[dst].limits.is64 && cx_.memories[src].limits.is64 ? i64 : i32;
            if (!Pop(length) || !Pop(memoryAddress(src)) || !Pop(memoryAddress(dst))) return false;
            if (!Dead()) { Emit(Op::kFcBase + n); Emit(dst, src); }
            return true;
        }
        case 11: {  // memory.fill
            const uint32_t memory = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (memory >= cx_.memories.size()) return Error("unknown memory");
            if (!Pop(memoryAddress(memory)) || !Pop(i32) || !Pop(memoryAddress(memory))) return false;
            if (!Dead()) Emit(Op::kFcBase + n, memory);
            return true;
        }
        case 12: {  // table.init
            const uint32_t elem = r_.u32();
            const uint32_t table = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (elem >= m_.elems.size()) return Error("unknown elem segment");
            if (table >= cx_.tables.size()) return Error("unknown table");
            if (!TypeRegistry::Matches(cx_.elemTypes[elem], cx_.tables[table].elem)) return Error("type mismatch: element type does not match the table's");
            if (!Pop(i32) || !Pop(i32) || !Pop(tableAddress(table))) return false;
            if (!Dead()) { Emit(Op::kFcBase + n); Emit(elem, table); }
            return true;
        }
        case 13: {  // elem.drop
            const uint32_t elem = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (elem >= m_.elems.size()) return Error("unknown elem segment");
            if (!Dead()) Emit(Op::kFcBase + n, elem);
            return true;
        }
        case 14: {  // table.copy
            const uint32_t dst = r_.u32();
            const uint32_t src = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (dst >= cx_.tables.size() || src >= cx_.tables.size()) return Error("unknown table");
            if (!TypeRegistry::Matches(cx_.tables[src].elem, cx_.tables[dst].elem)) return Error("type mismatch: table element types differ");
            const ValType length = cx_.tables[dst].limits.is64 && cx_.tables[src].limits.is64 ? i64 : i32;
            if (!Pop(length) || !Pop(tableAddress(src)) || !Pop(tableAddress(dst))) return false;
            if (!Dead()) { Emit(Op::kFcBase + n); Emit(dst, src); }
            return true;
        }
        case 15: {  // table.grow
            const uint32_t table = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (table >= cx_.tables.size()) return Error("unknown table");
            if (!Pop(tableAddress(table)) || !Pop(cx_.tables[table].elem)) return false;
            Push(tableAddress(table));
            if (!Dead()) Emit(Op::kFcBase + n, table);
            return true;
        }
        case 16: {  // table.size
            const uint32_t table = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (table >= cx_.tables.size()) return Error("unknown table");
            Push(tableAddress(table));
            if (!Dead()) Emit(Op::kFcBase + n, table);
            return true;
        }
        case 17: {  // table.fill
            const uint32_t table = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (table >= cx_.tables.size()) return Error("unknown table");
            if (!Pop(tableAddress(table)) || !Pop(cx_.tables[table].elem) || !Pop(tableAddress(table))) return false;
            if (!Dead()) Emit(Op::kFcBase + n, table);
            return true;
        }
        default: return Error("invalid opcode");
    }
}

bool FuncCompiler::Instruction(std::string& error) {
    const uint8_t op = r_.u8();
    if (!r_.ok()) return Error(r_.error());
    if (constMode_) {
        // (The instructions a constant expression may have: the others are not valid there.)
        const bool allowed = op == 0x0B || op == 0x41 || op == 0x42 || op == 0x43 || op == 0x44 || op == 0x23 || op == 0xD0 || op == 0xD2 || op == 0x6A || op == 0x6B ||
                             op == 0x6C || op == 0x7C || op == 0x7D || op == 0x7E || op == 0xFB || op == 0xFD;
        if (!allowed) return Error("constant expression required");
    }
    switch (op) {
        case 0x00:
            if (!Dead()) Emit(Op::kUnreachable);
            SetUnreachable();
            return true;
        case 0x01: return true;
        case 0x02: return BeginBlock(Ctl::Kind::Block);
        case 0x03: return BeginBlock(Ctl::Kind::Loop);
        case 0x04: return BeginBlock(Ctl::Kind::If);
        case 0x05: return Else();
        case 0x0B: return End(error);
        case 0x0C: return Br(false);
        case 0x0D: return Br(true);
        case 0x0E: {
            const uint32_t count = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (count > r_.remaining()) return Error("br_table is too large");
            std::vector<uint32_t> depths(count + 1);
            for (uint32_t i = 0; i <= count; ++i) depths[i] = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (!Pop(ValType::I32())) return false;
            Ctl* def = nullptr;
            if (!Label(depths[count], def)) return false;
            const size_t arity = LabelTypes(*def).size();
            if (!Dead()) {
                Emit(Op::kBrTable);
                Emit(count);
            }
            for (uint32_t i = 0; i <= count; ++i) {
                Ctl* target = nullptr;
                if (!Label(depths[i], target)) return false;
                const std::vector<ValType> types = LabelTypes(*target);
                if (types.size() != arity) return Error("type mismatch: br_table targets take different numbers of values");
                if (!CheckTop(types)) return false;
                if (!Dead()) {
                    const SlotCount keep = SlotsOf(types);
                    EmitTarget(*target);
                    BranchWords(*target, keep.n, keep.r);
                }
            }
            if (!PopVals(LabelTypes(*def))) return false;
            SetUnreachable();
            return true;
        }
        case 0x0F: {
            const std::vector<ValType>& results = ctls_.front().results;
            const SlotCount keep = SlotsOf(results);
            if (!Dead()) {
                Emit(Op::kReturn);
                Emit(keep.n);
                Emit(keep.r);
            }
            if (!PopVals(results)) return false;
            SetUnreachable();
            return true;
        }
        case 0x10: {
            const uint32_t f = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            return Call(f, false);
        }
        case 0x11: return CallIndirect(false);
        case 0x12: {
            const uint32_t f = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            return Call(f, true);
        }
        case 0x13: return CallIndirect(true);
        case 0x14: return CallRef(false);
        case 0x15: return CallRef(true);
        case 0x1A: {
            ValType t;
            if (!PopAny(t)) return false;
            if (!Dead()) {
                if (t.IsRef()) Emit(Op::kDropR);
                else if (t.kind == ValType::Kind::V128) Emit(Op::kDropV);
                else Emit(Op::kDropN);
            }
            return true;
        }
        case 0x1B: case 0x1C: {
            ValType t;
            if (op == 0x1C) {
                const uint32_t n = r_.u32();
                if (!r_.ok()) return Error(r_.error());
                if (n != 1) return Error("invalid result arity");
                const ValType declared = r_.valType();
                if (!r_.ok()) return Error(r_.error());
                if (!ValidValType(declared)) return false;
                t = cx_.Canon(declared);
                if (!Pop(ValType::I32()) || !Pop(t) || !Pop(t)) return false;
            } else {
                if (!Pop(ValType::I32())) return false;
                ValType a, b;
                if (!PopAny(a) || !PopAny(b)) return false;
                if (a.IsRef() || b.IsRef()) return Error("type mismatch: select needs a type for references");
                if (!a.IsBottom() && !b.IsBottom() && a != b) return Error("type mismatch: the operands of select");
                t = a.IsBottom() ? b : a;
            }
            Push(t);
            if (!Dead()) {
                if (t.IsRef()) Emit(Op::kSelectR);
                else if (t.kind == ValType::Kind::V128) Emit(Op::kSelectV);
                else Emit(Op::kSelectN);
            }
            return true;
        }
        case 0x20: case 0x21: case 0x22: {
            const uint32_t index = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (index >= locals_.size()) return Error("unknown local");
            const ValType t = locals_[index];
            const uint32_t slot = localSlot_[index];
            if (op == 0x20) {
                if (!inited_[index]) return Error("uninitialized local");
                Push(t);
            } else {
                if (!Pop(t)) return false;
                if (!inited_[index]) {
                    inited_[index] = true;
                    initLog_.push_back(index);
                }
                if (op == 0x22) Push(t);
            }
            if (!Dead()) {
                const int kind = t.IsRef() ? 2 : t.kind == ValType::Kind::V128 ? 1 : 0;
                const uint32_t base = op == 0x20 ? Op::kLocalGetN : op == 0x21 ? Op::kLocalSetN : Op::kLocalTeeN;
                Emit(base + kind, slot);
            }
            return true;
        }
        case 0x23: case 0x24: {
            const uint32_t index = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (index >= cx_.globals.size()) return Error("unknown global");
            const GlobalType& g = cx_.globals[index];
            if (op == 0x23) {
                if (constMode_ && (index >= globalLimit_ || g.mut)) return Error("constant expression required");
                Push(g.type);
            } else {
                if (!g.mut) return Error("global is immutable");
                if (!Pop(g.type)) return false;
            }
            if (!Dead()) {
                const int kind = g.type.IsRef() ? 2 : g.type.kind == ValType::Kind::V128 ? 1 : 0;
                Emit((op == 0x23 ? Op::kGlobalGetN : Op::kGlobalSetN) + kind, index);
            }
            return true;
        }
        case 0x25: case 0x26: {
            const uint32_t table = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (table >= cx_.tables.size()) return Error("unknown table");
            const TableType& tt = cx_.tables[table];
            const ValType address = tt.limits.is64 ? ValType::I64() : ValType::I32();
            if (op == 0x25) {
                if (!Pop(address)) return false;
                Push(tt.elem);
                if (!Dead()) Emit(Op::kTableGet, table);
            } else {
                if (!Pop(tt.elem) || !Pop(address)) return false;
                if (!Dead()) Emit(Op::kTableSet, table);
            }
            return true;
        }
        case 0x3F: case 0x40: {
            const uint32_t memory = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (memory >= cx_.memories.size()) return Error("unknown memory");
            const ValType address = cx_.memories[memory].limits.is64 ? ValType::I64() : ValType::I32();
            if (op == 0x40 && !Pop(address)) return false;
            Push(address);
            if (!Dead()) Emit(op == 0x3F ? Op::kMemorySize : Op::kMemoryGrow, memory);
            return true;
        }
        case 0x41: case 0x42: case 0x43: case 0x44: return ConstInstruction(op);
        case 0xD0: {
            const HeapType h = r_.heapType();
            if (!r_.ok()) return Error(r_.error());
            if (h >= 0 && static_cast<size_t>(h) >= m_.types.size()) return Error("unknown type");
            const HeapType canon = h >= 0 ? static_cast<HeapType>(cx_.typeIds[static_cast<size_t>(h)]) : h;
            Push(ValType::Ref(true, canon));
            if (!Dead()) Emit(Op::kRefNull);
            return true;
        }
        case 0xD1: {
            ValType t;
            if (!PopAny(t)) return false;
            if (!t.IsRef() && !t.IsBottom()) return Error("type mismatch: ref.is_null needs a reference");
            Push(ValType::I32());
            if (!Dead()) Emit(Op::kRefIsNull);
            return true;
        }
        case 0xD2: {
            const uint32_t f = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (f >= cx_.funcSigs.size()) return Error("unknown function");
            if (!constMode_ && !cx_.declared[f]) return Error("undeclared reference to function");
            Push(ValType::Ref(false, static_cast<HeapType>(cx_.funcSigs[f])));
            if (!Dead()) Emit(Op::kRefFunc, f);
            return true;
        }
        case 0xD3: {
            const ValType eq = ValType::Ref(true, Heap::Eq);
            if (!Pop(eq) || !Pop(eq)) return false;
            Push(ValType::I32());
            if (!Dead()) Emit(Op::kRefEq);
            return true;
        }
        case 0xD4: {
            ValType t;
            if (!PopAny(t)) return false;
            if (!t.IsRef() && !t.IsBottom()) return Error("type mismatch: ref.as_non_null needs a reference");
            Push(t.IsBottom() ? t : ValType::Ref(false, t.heap));
            if (!Dead()) Emit(Op::kRefAsNonNull);
            return true;
        }
        case 0xD5: return Br(false, true, false);
        case 0xD6: return Br(false, false, true);
        case 0xFC: case 0xFB: case 0xFD: return Prefixed(op);
        default:
            if (op >= 0x28 && op <= 0x3E) return MemoryAccess(op);
            if (op >= 0x45 && op <= 0xC4) return Numeric(op);
            return Error("invalid opcode " + std::to_string(op));
    }
}

}  // namespace

bool CompileFunction(ModuleContext& cx, uint32_t funcIndex, CompiledFunction& out, std::string& error) {
    const FuncBody& body = cx.module.bodies[funcIndex - cx.module.importedFuncs];
    FuncCompiler compiler(cx, cx.module.bytes.data() + body.code.offset, body.code.size, body.code.offset, out, false, 0);
    return compiler.Function(funcIndex, error);
}

bool CompileConstExpr(ModuleContext& cx, const ByteRange& range, const ValType& expected, uint32_t globalLimit, CompiledFunction& out, std::string& error) {
    FuncCompiler compiler(cx, cx.module.bytes.data() + range.offset, range.size, range.offset, out, true, globalLimit);
    return compiler.ConstExpr(expected, error);
}

}  // namespace Quanta::Wasm
