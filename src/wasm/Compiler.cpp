/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Validation of the code of a function, and the making of the code it is run from. The two are one pass: the validator knows at each
// instruction what is on the stack, and the instruction is made to say so.

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "quanta/wasm/Reader.h"
#include "quanta/wasm/Internal.h"

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
    int tryIndex = -1;  // (a try_table: its place in the function's list)
    std::vector<std::pair<size_t, size_t>> catchFixups;  // the catch clauses (try, clause) that go to the end of this block
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
        nBase_ = out_.paramsN + out_.localsN;
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
    // What each value of vals_ is: in the slot of its height (Stack), or not there yet because it is only a local that was read or a constant
    // (it is put there when something needs it to be, see Materialize).
    struct Operand {
        enum class Kind : uint8_t { Stack, Local, Const } kind = Kind::Stack;
        uint32_t src = 0;   // (a Local: its slot)
        uint32_t home = 0;  // the slot of its height
        uint64_t value = 0; // (a Const)
    };
    std::vector<Operand> opnds_;
    Operand lastPopped_;
    uint32_t nBase_ = 0;      // where the operands begin in the numbers of the frame (above the parameters and locals)
    uint32_t spHeight_ = 0;   // the height the stack pointer is at when the code that is made runs (what the stack instructions work from)
    bool touchedSp_ = false;
    size_t lastEnd_ = kNone, lastDstAt_ = kNone;  // the end of the last instruction on numbers and the word of its destination
    uint32_t lastDst_ = 0;
    uint32_t lastOp_ = 0, lastA_ = 0, lastB_ = 0;  // what the last instruction on numbers was and its operands (a constant or a slot)
    size_t lastStart_ = kNone;
    bool lastImm_ = false;
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

    void Push(const ValType& t) { PushOperand(t, Operand{}); }

    void PushOperand(const ValType& t, Operand o) {
        vals_.push_back(t);
        o.home = nBase_ + hN_;
        if (o.kind == Operand::Kind::Stack) o.src = o.home;
        opnds_.push_back(o);
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
        lastPopped_ = opnds_.back();
        opnds_.pop_back();
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
        lastPopped_ = opnds_.back();
        opnds_.pop_back();
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
        opnds_.resize(c.height);
        c.unreachable = true;
    }

    // ---- Register form ----

    // A value that is only a local that was read, or a constant, is put in the slot of its height.
    void MaterializeEntry(size_t i) {
        Operand& o = opnds_[i];
        if (o.kind == Operand::Kind::Local) {
            if (o.src != o.home) {
                Emit(Op::kCopyN, o.home);
                Emit(o.src);
            }
        } else if (o.kind == Operand::Kind::Const) {
            Emit(Op::kConstSlot, o.home);
            Emit(static_cast<uint32_t>(o.value), static_cast<uint32_t>(o.value >> 32));
        }
        o.kind = Operand::Kind::Stack;
        o.src = o.home;
    }

    static constexpr uint32_t kUnknownSp = 0xFFFFFFFFu;

    // What is not in its slot is put there.
    void Spill() {
        if (Dead()) return;
        for (size_t i = ctls_.back().height; i < opnds_.size(); ++i) {
            if (opnds_[i].kind != Operand::Kind::Stack) MaterializeEntry(i);
        }
    }

    // Before an instruction that works from the stack pointer: what is not in its slot is put there, and the stack pointer is brought to the height.
    void Materialize() {
        touchedSp_ = true;
        if (Dead()) return;
        Spill();
        if (spHeight_ != hN_) {
            Emit(Op::kSyncSp, hN_);
            spHeight_ = hN_;
        }
    }

    // The slot a value can be read from by an instruction on numbers.
    uint32_t SlotOf(const Operand& o) {
        if (o.kind == Operand::Kind::Local) return o.src;
        if (o.kind == Operand::Kind::Const) {
            Emit(Op::kConstSlot, o.home);
            Emit(static_cast<uint32_t>(o.value), static_cast<uint32_t>(o.value >> 32));
            return o.home;
        }
        return o.home;
    }

    // Values that were read from the local are put in their slots before it is written to.
    void SpillLocal(uint32_t slot) {
        for (size_t i = ctls_.back().height; i < opnds_.size(); ++i) {
            if (opnds_[i].kind == Operand::Kind::Local && opnds_[i].src == slot) MaterializeEntry(i);
        }
    }

    // Whether the second operand of the instruction can be a constant in the instruction itself.
    static bool ImmediateCapable(uint32_t op) {
        return (op >= 0x46 && op <= 0x4F) || (op >= 0x6A && op <= 0x6C) || (op >= 0x71 && op <= 0x78) || (op >= 0x51 && op <= 0x5A) || (op >= 0x7C && op <= 0x7E) || (op >= 0x83 && op <= 0x8A);
    }
    static bool Is64(uint32_t op) { return (op >= 0x51 && op <= 0x5A) || (op >= 0x7C && op <= 0x8A); }

    // An instruction on numbers: its operands (already taken off) are slots, and the result is in the slot of the height it has afterwards.
    void EmitRegister(uint32_t op, int arity, const Operand& a, const Operand& b) {
        bool immediate = false;
        uint32_t number = 0;
        static const bool noImm = std::getenv("QUANTA_WASM_NO_IMM") != nullptr;
        if (!noImm && arity == 2 && b.kind == Operand::Kind::Const && ImmediateCapable(op)) {
            if (!Is64(op)) {
                immediate = true;
                number = static_cast<uint32_t>(b.value);
            } else if (static_cast<int64_t>(b.value) >= INT32_MIN && static_cast<int64_t>(b.value) <= INT32_MAX) {
                immediate = true;
                number = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(b.value)));
            }
        }
        const uint32_t sa = SlotOf(a);
        const uint32_t sb = arity == 2 && !immediate ? SlotOf(b) : 0;
        const uint32_t dst = nBase_ + hN_;
        lastStart_ = Here();
        Emit(immediate ? op + Op::kImm : op);
        lastDstAt_ = Here();
        Emit(dst);
        Emit(sa);
        if (arity == 2) Emit(immediate ? number : sb);
        lastDst_ = dst;
        lastOp_ = op;
        lastA_ = sa;
        lastB_ = immediate ? number : sb;
        lastImm_ = immediate;
        lastEnd_ = Here();
    }

    static uint32_t InvertCompare(uint32_t op) {
        switch (op) {
            case 0x46: return 0x47; case 0x47: return 0x46;
            case 0x48: return 0x4E; case 0x4E: return 0x48;
            case 0x49: return 0x4F; case 0x4F: return 0x49;
            case 0x4A: return 0x4C; case 0x4C: return 0x4A;
            case 0x4B: return 0x4D; case 0x4D: return 0x4B;
            case 0x51: return 0x52; case 0x52: return 0x51;
            case 0x53: return 0x59; case 0x59: return 0x53;
            case 0x54: return 0x5A; case 0x5A: return 0x54;
            case 0x55: return 0x57; case 0x57: return 0x55;
            case 0x56: return 0x58; case 0x58: return 0x56;
            default: return op;
        }
    }

    // A jump on a condition that was just made by a comparison (or by i32.eqz) is the comparison and the jump in one instruction: the one that made the condition
    // is taken away and what is emitted instead is everything of the jump but its target, which the caller emits.
    bool FuseCondition(const Operand& condition, bool jumpIfTrue) {
        static const bool off = std::getenv("QUANTA_WASM_NO_FUSE") != nullptr;
        if (off) return false;
        if (condition.kind != Operand::Kind::Stack || lastEnd_ != Here() || lastDst_ != condition.home) return false;
        const bool compare32 = lastOp_ >= 0x46 && lastOp_ <= 0x4F, compare64 = lastOp_ >= 0x51 && lastOp_ <= 0x5A;
        if (compare32 || compare64) {
            const uint32_t op = jumpIfTrue ? lastOp_ : InvertCompare(lastOp_);
            const uint32_t a = lastA_, b = lastB_;
            const bool immediate = lastImm_;
            out_.code.resize(lastStart_);
            Emit((immediate ? Op::kJumpIfImm : Op::kJumpIf) + op);
            Emit(a);
            Emit(b);
            lastEnd_ = kNone;
            return true;
        }
        if (lastOp_ == 0x45) {
            const uint32_t a = lastA_;
            out_.code.resize(lastStart_);
            Emit(jumpIfTrue ? Op::kJzSlot : Op::kJnzSlot);
            Emit(a);
            lastEnd_ = kNone;
            return true;
        }
        return false;
    }

    // The results are moved to the first slots of the frame, where the caller has them, and the function returns.
    void EmitReturn(uint32_t firstResultSlot, const std::vector<ValType>& results) {
        const SlotCount keep = SlotsOf(results);
        for (uint32_t i = 0; i < keep.n; ++i) {
            if (firstResultSlot + i != i) {
                Emit(Op::kCopyN, i);
                Emit(firstResultSlot + i);
            }
        }
        Emit(Op::kRet);
        Emit(keep.n);
        Emit(keep.r);
    }

    // A branch to the label, with the values it takes moved to where the label has them. `condition` is a slot with a number, for the ones that are taken only if it is not 0.
    void EmitBranch(Ctl& target, const SlotCount& keep, bool conditional, const Operand& condition) {
        struct Move { uint32_t dst, src; };
        std::vector<Move> moves;
        for (uint32_t i = 0; i < keep.n; ++i) {
            const uint32_t src = nBase_ + hN_ - keep.n + i, dst = nBase_ + target.baseN + i;
            if (src != dst) moves.push_back({dst, src});
        }
        const uint32_t dropR = hR_ - target.baseR - keep.r;
        size_t skip = kNone;
        if (conditional) {
            if (moves.empty() && dropR == 0) {
                if (!FuseCondition(condition, true)) {
                    const uint32_t slot = SlotOf(condition);
                    Emit(Op::kJnzSlot);
                    Emit(slot);
                }
                EmitTarget(target);
                return;
            }
            const uint32_t slot = SlotOf(condition);
            Emit(Op::kJzSlot);
            Emit(slot);
            skip = Here();
            Emit(0);
        }
        for (const Move& m : moves) {
            Emit(Op::kCopyN, m.dst);
            Emit(m.src);
        }
        if (dropR != 0) {
            Emit(Op::kBr);
            EmitTarget(target);
            Emit(0);
            Emit(keep.r);
            Emit(0);
            Emit(dropR);
        } else {
            Emit(Op::kJmp);
            EmitTarget(target);
        }
        if (skip != kNone) out_.code[skip] = static_cast<uint32_t>(Here());
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

    struct RawCatch {
        uint8_t kind = 0;
        uint32_t tag = 0, label = 0;
    };

    bool BeginBlock(Ctl::Kind kind) {
        std::vector<ValType> params, results;
        if (!ReadBlockType(params, results)) return false;
        std::vector<RawCatch> rawCatches;
        if (kind == Ctl::Kind::TryTable) {
            const uint32_t count = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (count > r_.remaining()) return Error("too many catch clauses");
            for (uint32_t i = 0; i < count; ++i) {
                RawCatch c;
                c.kind = r_.u8();
                if (!r_.ok()) return Error(r_.error());
                if (c.kind > 3) return Error("invalid catch kind");
                if (c.kind <= 1) c.tag = r_.u32();
                c.label = r_.u32();
                if (!r_.ok()) return Error(r_.error());
                rawCatches.push_back(c);
            }
        }
        Operand condition;
        if (kind == Ctl::Kind::If) {
            if (!Pop(ValType::I32())) return false;
            condition = lastPopped_;
            if (!Dead()) {
                // (The jump that skips the then part.)
            }
        }
        const bool deadEntry = Dead();
        if (!deadEntry) Spill();
        size_t jz = kNone;
        if (kind == Ctl::Kind::If && !deadEntry) {
            if (!FuseCondition(condition, false)) {
                const uint32_t slot = SlotOf(condition);
                Emit(Op::kJzSlot);
                Emit(slot);
            }
            jz = Here();
            Emit(0);
        }
        if (!PopVals(params)) return false;
        int tryIndex = -1;
        if (kind == Ctl::Kind::TryTable) {
            TryEntry entry;
            entry.start = static_cast<uint32_t>(Here());
            entry.baseN = hN_;
            entry.baseR = hR_;
            for (const RawCatch& rc : rawCatches) {
                if (rc.label >= ctls_.size()) return Error("unknown label");
                const size_t targetIndex = ctls_.size() - 1 - rc.label;
                std::vector<ValType> pushed;
                if (rc.kind <= 1) {
                    if (rc.tag >= cx_.tagSigs.size()) return Error("unknown tag");
                    pushed = TypeRegistry::Get(cx_.tagSigs[rc.tag]).func.params;
                }
                if (rc.kind & 1) pushed.push_back(ValType::Ref(false, Heap::Exn));
                const std::vector<ValType>& wanted = LabelTypes(ctls_[targetIndex]);
                if (wanted.size() != pushed.size()) return Error("type mismatch: a catch clause gives other values than its label takes");
                for (size_t k = 0; k < pushed.size(); ++k) {
                    if (!TypeRegistry::Matches(pushed[k], wanted[k])) return Error("type mismatch: a catch clause gives other values than its label takes");
                }
                CatchClause clause;
                clause.kind = rc.kind;
                clause.tag = rc.tag;
                if (!deadEntry) {
                    clause.dropN = entry.baseN - ctls_[targetIndex].baseN;
                    clause.dropR = entry.baseR - ctls_[targetIndex].baseR;
                    if (ctls_[targetIndex].kind == Ctl::Kind::Loop) clause.target = static_cast<uint32_t>(ctls_[targetIndex].loopStart);
                }
                entry.catches.push_back(clause);
            }
            if (!deadEntry) {
                tryIndex = static_cast<int>(out_.tries.size());
                for (size_t k = 0; k < rawCatches.size(); ++k) {
                    Ctl& target = ctls_[ctls_.size() - 1 - rawCatches[k].label];
                    if (target.kind != Ctl::Kind::Loop) target.catchFixups.push_back({static_cast<size_t>(tryIndex), k});
                }
                out_.tries.push_back(std::move(entry));
            }
        }
        Ctl c;
        c.tryIndex = tryIndex;
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
        lastEnd_ = kNone;
        if (kind == Ctl::Kind::Loop) spHeight_ = kUnknownSp;  // (a jump comes back to it)
        return true;
    }

    bool End(std::string& error) {
        if (!Dead()) Spill();
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
        if (done.tryIndex >= 0) out_.tries[static_cast<size_t>(done.tryIndex)].end = static_cast<uint32_t>(Here());
        if (done.kind == Ctl::Kind::Func) {
            if (!done.deadEntry) {
                const size_t returnAt = Here();
                EmitReturn(nBase_, done.results);  // (the values of a branch to the function are where the first of them is: the base of the operands)
                for (size_t f : done.fixups) out_.code[f] = static_cast<uint32_t>(returnAt);
                for (const auto& cf : done.catchFixups) out_.tries[cf.first].catches[cf.second].target = static_cast<uint32_t>(returnAt);
            }
            if (!r_.atEnd()) return Error("operators remaining after end of function");
            (void)error;
            return true;
        }
        if (!done.deadEntry) {
            if (done.ifFixup != kNone) out_.code[done.ifFixup] = static_cast<uint32_t>(Here());
            if (done.elseFixup != kNone) out_.code[done.elseFixup] = static_cast<uint32_t>(Here());
            for (size_t f : done.fixups) out_.code[f] = static_cast<uint32_t>(Here());
            for (const auto& cf : done.catchFixups) out_.tries[cf.first].catches[cf.second].target = static_cast<uint32_t>(Here());
        }
        PushValsAfterBlock(done);
        spHeight_ = kUnknownSp;  // (the ways in come from other places)
        lastEnd_ = kNone;        // (and so what was made last is not the only way to what comes next)
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
        if (!Dead()) Spill();
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
        spHeight_ = kUnknownSp;
        lastEnd_ = kNone;
        c.kind = Ctl::Kind::Else;
        c.unreachable = false;
        vals_.resize(c.height);
        opnds_.resize(c.height);
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
    bool InstructionBody(std::string& error);
    bool Numeric(uint8_t op);
    bool MemoryAccess(uint8_t op);
    bool Prefixed(uint8_t prefix);
    bool MiscInstruction(uint32_t n);
    bool GcInstruction(uint32_t n);
    bool SimdInstruction(uint32_t n);
    bool ReadHeap(HeapType& canon);
    bool ReadTypeIndex(TypeDef::Kind kind, uint32_t& canon, uint32_t& local);
    uint32_t ElementInfo(const CanonType& t) const;
    bool SameHierarchy(HeapType a, HeapType b) const { return TypeRegistry::TopOf(a) == TypeRegistry::TopOf(b); }
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
        const Operand condition = lastPopped_;
        if (!Dead()) {
            Spill();
            EmitBranch(*target, keep, true, condition);
        }
        if (!PopVals(types)) return false;
        PushVals(types);
        return true;
    }
    if (!Dead()) {
        Spill();
        EmitBranch(*target, keep, false, Operand{});
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
        if (!Pop(t)) return false;
        const Operand value = lastPopped_;
        if (!Pop(address)) return false;
        const Operand where = lastPopped_;
        if (!Dead()) {
            const uint32_t sv = SlotOf(value), sw = SlotOf(where);
            Emit(op);
            Emit(sv);
            Emit(sw);
            Emit(memory);
            Emit(static_cast<uint32_t>(offset));
            Emit(static_cast<uint32_t>(offset >> 32));
            lastEnd_ = kNone;
        }
    } else {
        if (!Pop(address)) return false;
        const Operand where = lastPopped_;
        if (!Dead()) {
            const uint32_t sw = SlotOf(where);
            const uint32_t dst = nBase_ + hN_;
            Emit(op);
            lastDstAt_ = Here();
            Emit(dst);
            Emit(sw);
            Emit(memory);
            Emit(static_cast<uint32_t>(offset));
            Emit(static_cast<uint32_t>(offset >> 32));
            lastDst_ = dst;
            lastOp_ = 0;  // (a load is not a comparison that a jump can be made of)
            lastEnd_ = Here();
        }
        Push(t);
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
    Operand ob;
    if (arity == 2) {
        if (!Pop(b)) return false;
        ob = lastPopped_;
    }
    if (!Pop(a)) return false;
    const Operand oa = lastPopped_;
    if (!Dead()) EmitRegister(op, arity, oa, ob);
    Push(out);
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
    if (!Dead()) Spill();
    if (!PopVals(sig.params)) return false;
    if (!Dead()) {
        const uint32_t firstArgument = nBase_ + hN_;
        if (tail) {
            // (The arguments go to the first slots of the frame, which is the one the callee has.)
            const SlotCount params = SlotsOf(sig.params);
            for (uint32_t i = 0; i < params.n; ++i) {
                if (firstArgument + i != i) {
                    Emit(Op::kCopyN, i);
                    Emit(firstArgument + i);
                }
            }
            Emit(Op::kReturnCallR, index);
        } else {
            Emit(Op::kCallR, index);
            Emit(firstArgument);
        }
        spHeight_ = kUnknownSp;
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
    const Operand index = lastPopped_;
    if (!tail && !Dead()) Spill();
    if (!PopVals(t.func.params)) return false;
    if (!Dead()) {
        if (tail) {
            Emit(Op::kReturnCallIndirect);
            Emit(canon);
            Emit(table);
        } else {
            const uint32_t slot = SlotOf(index);
            Emit(Op::kCallIndirectR);
            Emit(canon);
            Emit(table);
            Emit(nBase_ + hN_);
            Emit(slot);
            spHeight_ = kUnknownSp;
        }
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
    if (!tail && !Dead()) Spill();
    if (!PopVals(t.func.params)) return false;
    if (!Dead()) {
        if (tail) {
            Emit(Op::kReturnCallRef);
            Emit(canon);
        } else {
            Emit(Op::kCallRefR);
            Emit(canon);
            Emit(nBase_ + hN_);
            spHeight_ = kUnknownSp;
        }
    }
    if (tail) SetUnreachable();
    else PushVals(t.func.results);
    return true;
}

// ---- The rest ----

bool FuncCompiler::ConstInstruction(uint8_t op) {
    Operand o;
    o.kind = Operand::Kind::Const;
    switch (op) {
        case 0x41: {
            const int32_t v = r_.s32();
            if (!r_.ok()) return Error(r_.error());
            o.value = static_cast<uint32_t>(v);
            PushOperand(ValType::I32(), o);
            return true;
        }
        case 0x42: {
            const int64_t v = r_.s64();
            if (!r_.ok()) return Error(r_.error());
            o.value = static_cast<uint64_t>(v);
            PushOperand(ValType::I64(), o);
            return true;
        }
        case 0x43: {
            const uint32_t v = r_.fixed32();
            if (!r_.ok()) return Error(r_.error());
            o.value = v;
            PushOperand(ValType::F32(), o);
            return true;
        }
        default: {
            const uint64_t v = r_.fixed64();
            if (!r_.ok()) return Error(r_.error());
            o.value = v;
            PushOperand(ValType::F64(), o);
            return true;
        }
    }
}

bool FuncCompiler::Prefixed(uint8_t prefix) {
    const uint32_t n = r_.u32();
    if (!r_.ok()) return Error(r_.error());
    if (prefix == 0xFC) return MiscInstruction(n);
    if (prefix == 0xFD) return SimdInstruction(n);
    if (prefix == 0xFB) {
        if (constMode_ && !(n == 0x00 || n == 0x01 || n == 0x06 || n == 0x07 || n == 0x08 || n == 0x1A || n == 0x1B || n == 0x1C)) return Error("constant expression required");
        return GcInstruction(n);
    }
    return Error("invalid opcode");
}

// ---- The GC instructions ----

bool FuncCompiler::ReadHeap(HeapType& canon) {
    const HeapType h = r_.heapType();
    if (!r_.ok()) return Error(r_.error());
    if (h >= 0 && static_cast<size_t>(h) >= m_.types.size()) return Error("unknown type");
    canon = h >= 0 ? static_cast<HeapType>(cx_.typeIds[static_cast<size_t>(h)]) : h;
    return true;
}

bool FuncCompiler::ReadTypeIndex(TypeDef::Kind kind, uint32_t& canon, uint32_t& local) {
    local = r_.u32();
    if (!r_.ok()) return Error(r_.error());
    if (local >= m_.types.size()) return Error("unknown type");
    canon = cx_.typeIds[local];
    if (TypeRegistry::Get(canon).kind != kind) return Error("type is not of the kind the instruction takes");
    return true;
}

// What an access to an element of an array is told: its size, whether it is a reference, and, if it is packed, how it is read.
uint32_t FuncCompiler::ElementInfo(const CanonType& t) const {
    uint32_t info = t.elemBytes;
    if (t.elemIsRef) info |= 0x100;
    return info;
}

static bool StorageDefaultable(const StorageType& t) { return t.pack != StorageType::Pack::None || !(t.val.IsRef() && !t.val.nullable); }

bool FuncCompiler::GcInstruction(uint32_t n) {
    const ValType i32 = ValType::I32();
    uint32_t id, local;
    switch (n) {
        case 0x00: case 0x01: {  // struct.new, struct.new_default
            if (!ReadTypeIndex(TypeDef::Kind::Struct, id, local)) return false;
            const CanonType& t = TypeRegistry::Get(id);
            if (n == 0x01) {
                for (const FieldType& f : t.str.fields) if (!StorageDefaultable(f.type)) return Error("struct.new_default of a type with a field that has no default");
            } else {
                for (size_t k = t.str.fields.size(); k-- > 0;) if (!Pop(t.str.fields[k].type.Unpacked())) return false;
            }
            Push(ValType::Ref(false, static_cast<HeapType>(id)));
            if (!Dead()) Emit(Op::kFbBase + n, id);
            return true;
        }
        case 0x02: case 0x03: case 0x04: case 0x05: {  // struct.get, _s, _u, struct.set
            if (!ReadTypeIndex(TypeDef::Kind::Struct, id, local)) return false;
            const uint32_t field = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            const CanonType& t = TypeRegistry::Get(id);
            if (field >= t.str.fields.size()) return Error("unknown field");
            const FieldType& f = t.str.fields[field];
            const bool packed = f.type.pack != StorageType::Pack::None;
            if (n == 0x02 && packed) return Error("struct.get of a packed field needs the signed or unsigned form");
            if ((n == 0x03 || n == 0x04) && !packed) return Error("struct.get_s and struct.get_u are for packed fields");
            uint32_t flags = t.fieldIsRef[field] ? 1 : 0;
            if (f.type.pack == StorageType::Pack::I8) flags |= 2;
            if (f.type.pack == StorageType::Pack::I16) flags |= 4;
            if (n == 0x03) flags |= 8;
            if (f.type.val.kind == ValType::Kind::V128 && !packed) flags |= 16;
            if (n == 0x05) {
                if (!f.mut) return Error("struct.set of an immutable field");
                if (!Pop(f.type.Unpacked()) || !Pop(ValType::Ref(true, static_cast<HeapType>(id)))) return false;
            } else {
                if (!Pop(ValType::Ref(true, static_cast<HeapType>(id)))) return false;
                Push(f.type.Unpacked());
            }
            if (!Dead()) { Emit(Op::kFbBase + n); Emit(t.fieldSlot[field], flags); }
            return true;
        }
        case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: {  // array.new, _default, _fixed, _data, _elem
            if (!ReadTypeIndex(TypeDef::Kind::Array, id, local)) return false;
            const CanonType& t = TypeRegistry::Get(id);
            const StorageType& e = t.arr.element.type;
            uint32_t extra = 0;
            bool hasExtra = false;
            if (n == 0x06) {
                if (!Pop(i32) || !Pop(e.Unpacked())) return false;
            } else if (n == 0x07) {
                if (!StorageDefaultable(e)) return Error("array.new_default of a type that has no default");
                if (!Pop(i32)) return false;
            } else if (n == 0x08) {
                extra = r_.u32();
                hasExtra = true;
                if (!r_.ok()) return Error(r_.error());
                if (extra > r_.remaining() + 100000) return Error("array.new_fixed is too large");
                for (uint32_t k = 0; k < extra; ++k) if (!Pop(e.Unpacked())) return false;
            } else if (n == 0x09) {
                extra = r_.u32();
                hasExtra = true;
                if (!r_.ok()) return Error(r_.error());
                if (e.pack == StorageType::Pack::None && e.val.IsRef()) return Error("array.new_data of an array of references");
                if (!m_.hasDataCount) return Error("data count section required");
                if (extra >= m_.dataCount) return Error("unknown data segment");
                if (!Pop(i32) || !Pop(i32)) return false;
            } else {
                extra = r_.u32();
                hasExtra = true;
                if (!r_.ok()) return Error(r_.error());
                if (extra >= m_.elems.size()) return Error("unknown elem segment");
                if (!(e.pack == StorageType::Pack::None && e.val.IsRef())) return Error("array.new_elem of an array that is not of references");
                if (!TypeRegistry::Matches(cx_.elemTypes[extra], e.val)) return Error("type mismatch: the elements of the segment are not what the array has");
                if (!Pop(i32) || !Pop(i32)) return false;
            }
            Push(ValType::Ref(false, static_cast<HeapType>(id)));
            if (!Dead()) { Emit(Op::kFbBase + n, id); if (hasExtra) Emit(extra); }
            return true;
        }
        case 0x0B: case 0x0C: case 0x0D: case 0x0E: {  // array.get, _s, _u, array.set
            if (!ReadTypeIndex(TypeDef::Kind::Array, id, local)) return false;
            const CanonType& t = TypeRegistry::Get(id);
            const FieldType& f = t.arr.element;
            const bool packed = f.type.pack != StorageType::Pack::None;
            if (n == 0x0B && packed) return Error("array.get of a packed element needs the signed or unsigned form");
            if ((n == 0x0C || n == 0x0D) && !packed) return Error("array.get_s and array.get_u are for packed elements");
            uint32_t info = ElementInfo(t);
            if (n == 0x0C) info |= 0x200;
            if (n == 0x0E) {
                if (!f.mut) return Error("array.set of an immutable array");
                if (!Pop(f.type.Unpacked()) || !Pop(i32) || !Pop(ValType::Ref(true, static_cast<HeapType>(id)))) return false;
            } else {
                if (!Pop(i32) || !Pop(ValType::Ref(true, static_cast<HeapType>(id)))) return false;
                Push(f.type.Unpacked());
            }
            if (!Dead()) Emit(Op::kFbBase + n, info);
            return true;
        }
        case 0x0F: {  // array.len
            if (!Pop(ValType::Ref(true, Heap::Array))) return false;
            Push(i32);
            if (!Dead()) Emit(Op::kFbBase + n);
            return true;
        }
        case 0x10: {  // array.fill
            if (!ReadTypeIndex(TypeDef::Kind::Array, id, local)) return false;
            const CanonType& t = TypeRegistry::Get(id);
            if (!t.arr.element.mut) return Error("array.fill of an immutable array");
            if (!Pop(i32) || !Pop(t.arr.element.type.Unpacked()) || !Pop(i32) || !Pop(ValType::Ref(true, static_cast<HeapType>(id)))) return false;
            if (!Dead()) Emit(Op::kFbBase + n, ElementInfo(t));
            return true;
        }
        case 0x11: {  // array.copy
            uint32_t id2, local2;
            if (!ReadTypeIndex(TypeDef::Kind::Array, id, local)) return false;
            if (!ReadTypeIndex(TypeDef::Kind::Array, id2, local2)) return false;
            const CanonType& dst = TypeRegistry::Get(id);
            const CanonType& src = TypeRegistry::Get(id2);
            if (!dst.arr.element.mut) return Error("array.copy into an immutable array");
            if (!TypeRegistry::Matches(src.arr.element.type, dst.arr.element.type)) return Error("type mismatch: array.copy between arrays of other elements");
            if (!Pop(i32) || !Pop(i32) || !Pop(ValType::Ref(true, static_cast<HeapType>(id2))) || !Pop(i32) || !Pop(ValType::Ref(true, static_cast<HeapType>(id)))) return false;
            if (!Dead()) { Emit(Op::kFbBase + n); Emit(ElementInfo(dst), ElementInfo(src)); }
            return true;
        }
        case 0x12: case 0x13: {  // array.init_data, array.init_elem
            if (!ReadTypeIndex(TypeDef::Kind::Array, id, local)) return false;
            const uint32_t segment = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            const CanonType& t = TypeRegistry::Get(id);
            const StorageType& e = t.arr.element.type;
            if (!t.arr.element.mut) return Error("array.init of an immutable array");
            const bool isRef = e.pack == StorageType::Pack::None && e.val.IsRef();
            if (n == 0x12) {
                if (isRef) return Error("array.init_data of an array of references");
                if (!m_.hasDataCount) return Error("data count section required");
                if (segment >= m_.dataCount) return Error("unknown data segment");
            } else {
                if (!isRef) return Error("array.init_elem of an array that is not of references");
                if (segment >= m_.elems.size()) return Error("unknown elem segment");
                if (!TypeRegistry::Matches(cx_.elemTypes[segment], e.val)) return Error("type mismatch: the elements of the segment are not what the array has");
            }
            if (!Pop(i32) || !Pop(i32) || !Pop(i32) || !Pop(ValType::Ref(true, static_cast<HeapType>(id)))) return false;
            if (!Dead()) { Emit(Op::kFbBase + n); Emit(ElementInfo(t), segment); }
            return true;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: {  // ref.test, ref.cast
            HeapType ht;
            if (!ReadHeap(ht)) return false;
            const bool nullable = n == 0x15 || n == 0x17;
            ValType v;
            if (!PopAny(v)) return false;
            if (!v.IsRef() && !v.IsBottom()) return Error("type mismatch: a reference was expected");
            if (v.IsRef() && !SameHierarchy(v.heap, ht)) return Error("type mismatch: the type is not in the hierarchy of the reference");
            if (n <= 0x15) Push(i32);
            else Push(ValType::Ref(nullable, ht));
            if (!Dead()) { Emit(Op::kFbBase + n); Emit(static_cast<uint32_t>(ht), nullable ? 1 : 0); }
            return true;
        }
        case 0x18: case 0x19: {  // br_on_cast, br_on_cast_fail
            const uint8_t flags = r_.u8();
            const uint32_t depth = r_.u32();
            HeapType ht1, ht2;
            if (!r_.ok()) return Error(r_.error());
            if (flags > 3) return Error("invalid cast flags");
            if (!ReadHeap(ht1) || !ReadHeap(ht2)) return false;
            Ctl* target = nullptr;
            if (!Label(depth, target)) return false;
            const ValType rt1 = ValType::Ref(flags & 1, ht1);
            const ValType rt2 = ValType::Ref(flags & 2, ht2);
            if (!TypeRegistry::Matches(rt2, rt1)) return Error("type mismatch: br_on_cast to a type that is not a subtype of the one it takes");
            const std::vector<ValType> types = LabelTypes(*target);
            if (types.empty() || !types.back().IsRef()) return Error("type mismatch: br_on_cast needs a label that takes a reference");
            const ValType diff = ValType::Ref(rt1.nullable && !rt2.nullable, ht1);
            const ValType& branches = n == 0x18 ? rt2 : diff;
            if (!TypeRegistry::Matches(branches, types.back())) return Error("type mismatch: what br_on_cast branches with is not what the label takes");
            std::vector<ValType> prefix(types.begin(), types.end() - 1);
            const SlotCount keep = SlotsOf(types);
            if (!Dead()) {
                Emit(Op::kFbBase + n);
                EmitTarget(*target);
                Emit(keep.n);
                Emit(keep.r);
                Emit(hN_ - target->baseN - keep.n);
                Emit(hR_ - target->baseR - keep.r);
                Emit(static_cast<uint32_t>(ht2), (flags & 2) ? 1 : 0);
            }
            if (!Pop(rt1)) return false;
            if (!PopVals(prefix)) return false;
            PushVals(prefix);
            Push(n == 0x18 ? diff : rt2);
            return true;
        }
        case 0x1A: case 0x1B: {  // any.convert_extern, extern.convert_any
            const HeapType from = n == 0x1A ? Heap::Extern : Heap::Any;
            ValType v;
            if (!PopAny(v)) return false;
            if (!v.IsRef() && !v.IsBottom()) return Error("type mismatch: a reference was expected");
            if (v.IsRef() && !TypeRegistry::Matches(ValType::Ref(v.nullable, v.heap), ValType::Ref(true, from))) return Error("type mismatch: the conversion takes a reference of the other kind");
            Push(ValType::Ref(v.IsBottom() ? true : v.nullable, n == 0x1A ? Heap::Any : Heap::Extern));
            if (!Dead()) Emit(Op::kFbBase + n);
            return true;
        }
        case 0x1C: {  // ref.i31
            if (!Pop(i32)) return false;
            Push(ValType::Ref(false, Heap::I31));
            if (!Dead()) Emit(Op::kFbBase + n);
            return true;
        }
        case 0x1D: case 0x1E: {  // i31.get_s, i31.get_u
            if (!Pop(ValType::Ref(true, Heap::I31))) return false;
            Push(i32);
            if (!Dead()) Emit(Op::kFbBase + n);
            return true;
        }
        default: return Error("invalid opcode");
    }
}

// ---- The vector instructions ----

bool FuncCompiler::SimdInstruction(uint32_t n) {
    const ValType v = ValType::V128(), i32 = ValType::I32(), i64 = ValType::I64(), f32 = ValType::F32(), f64 = ValType::F64();
    if (constMode_ && n != 0x0C) return Error("constant expression required");
    // Loads and stores.
    const auto memory = [&](uint32_t align, bool lane, uint32_t lanes, uint32_t& memIndex, uint64_t& offset, uint32_t& laneIndex, bool& is64) {
        if (!MemArg(align, memIndex, offset, is64)) return false;
        if (lane) {
            laneIndex = r_.u8();
            if (!r_.ok()) return Error(r_.error());
            if (laneIndex >= lanes) return Error("invalid lane index");
        }
        return true;
    };
    if (n <= 0x0B || (n >= 0x54 && n <= 0x5D)) {
        uint32_t align = 4, lanes = 0;
        bool store = false, lane = false;
        switch (n) {
            case 0x00: align = 4; break;
            case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: align = 3; break;
            case 0x07: align = 0; break;
            case 0x08: align = 1; break;
            case 0x09: align = 2; break;
            case 0x0A: align = 3; break;
            case 0x0B: align = 4; store = true; break;
            case 0x54: align = 0; lane = true; lanes = 16; break;
            case 0x55: align = 1; lane = true; lanes = 8; break;
            case 0x56: align = 2; lane = true; lanes = 4; break;
            case 0x57: align = 3; lane = true; lanes = 2; break;
            case 0x58: align = 0; lane = true; lanes = 16; store = true; break;
            case 0x59: align = 1; lane = true; lanes = 8; store = true; break;
            case 0x5A: align = 2; lane = true; lanes = 4; store = true; break;
            case 0x5B: align = 3; lane = true; lanes = 2; store = true; break;
            case 0x5C: align = 2; break;
            default: align = 3; break;  // 0x5D
        }
        uint32_t memIndex = 0, laneIndex = 0;
        uint64_t offset = 0;
        bool is64 = false;
        if (!memory(align, lane, lanes, memIndex, offset, laneIndex, is64)) return false;
        const ValType address = is64 ? i64 : i32;
        if (store) {
            if (!Pop(v) || !Pop(address)) return false;
        } else if (lane) {
            if (!Pop(v) || !Pop(address)) return false;
            Push(v);
        } else {
            if (!Pop(address)) return false;
            Push(v);
        }
        if (!Dead()) {
            Emit(Op::kFdBase + n);
            Emit(memIndex);
            Emit(static_cast<uint32_t>(offset));
            Emit(static_cast<uint32_t>(offset >> 32));
            if (lane) Emit(laneIndex);
        }
        return true;
    }
    if (n == 0x0C) {
        const uint8_t* bytes = r_.bytes(16);
        if (!r_.ok()) return Error(r_.error());
        Push(v);
        if (!Dead()) {
            Emit(Op::kConstV);
            for (int i = 0; i < 4; ++i) {
                uint32_t w;
                std::memcpy(&w, bytes + i * 4, 4);
                Emit(w);
            }
        }
        return true;
    }
    if (n == 0x0D) {  // i8x16.shuffle
        const uint8_t* lanes = r_.bytes(16);
        if (!r_.ok()) return Error(r_.error());
        for (int i = 0; i < 16; ++i) if (lanes[i] >= 32) return Error("invalid lane index");
        if (!Pop(v) || !Pop(v)) return false;
        Push(v);
        if (!Dead()) {
            Emit(Op::kFdBase + n);
            for (int i = 0; i < 4; ++i) {
                uint32_t w;
                std::memcpy(&w, lanes + i * 4, 4);
                Emit(w);
            }
        }
        return true;
    }
    if (n >= 0x15 && n <= 0x22) {  // extract_lane, replace_lane
        const uint32_t lane = r_.u8();
        if (!r_.ok()) return Error(r_.error());
        uint32_t lanes;
        ValType scalar;
        switch (n) {
            case 0x15: case 0x16: case 0x17: lanes = 16; scalar = i32; break;
            case 0x18: case 0x19: case 0x1A: lanes = 8; scalar = i32; break;
            case 0x1B: case 0x1C: lanes = 4; scalar = i32; break;
            case 0x1D: case 0x1E: lanes = 2; scalar = i64; break;
            case 0x1F: case 0x20: lanes = 4; scalar = f32; break;
            default: lanes = 2; scalar = f64; break;
        }
        if (lane >= lanes) return Error("invalid lane index");
        const bool replace = n == 0x17 || n == 0x1A || n == 0x1C || n == 0x1E || n == 0x20 || n == 0x22;
        if (replace) {
            if (!Pop(scalar) || !Pop(v)) return false;
            Push(v);
        } else {
            if (!Pop(v)) return false;
            Push(scalar);
        }
        if (!Dead()) Emit(Op::kFdBase + n, lane);
        return true;
    }
    if (n >= 0x0F && n <= 0x14) {  // splat
        const ValType scalar = n == 0x12 ? i64 : n == 0x13 ? f32 : n == 0x14 ? f64 : i32;
        if (!Pop(scalar)) return false;
        Push(v);
        if (!Dead()) Emit(Op::kFdBase + n);
        return true;
    }
    // The rest take vectors (and one scalar for a shift) and give a vector or a number.
    const auto in = [&](std::initializer_list<ValType> types, ValType out) {
        for (auto it = types.end(); it != types.begin();) {
            --it;
            if (!Pop(*it)) return false;
        }
        Push(out);
        if (!Dead()) Emit(Op::kFdBase + n);
        return true;
    };
    const bool shift = (n >= 0x6B && n <= 0x6D) || (n >= 0x8B && n <= 0x8D) || (n >= 0xAB && n <= 0xAD) || (n >= 0xCB && n <= 0xCD);
    const bool test = n == 0x53 || n == 0x63 || n == 0x64 || n == 0x83 || n == 0x84 || n == 0xA3 || n == 0xA4 || n == 0xC3 || n == 0xC4;
    const bool ternary = n == 0x52 || (n >= 0x105 && n <= 0x10C) || n == 0x113;
    const bool unary = n == 0x5E || n == 0x5F || n == 0x4D || (n >= 0x60 && n <= 0x62) || (n >= 0x67 && n <= 0x6A) || n == 0x74 || n == 0x75 || n == 0x7A || (n >= 0x7C && n <= 0x7F) || n == 0x80 ||
                       n == 0x81 || (n >= 0x87 && n <= 0x8A) || n == 0x94 || n == 0xA0 || n == 0xA1 || (n >= 0xA7 && n <= 0xAA) || n == 0xC0 || n == 0xC1 || (n >= 0xC7 && n <= 0xCA) || n == 0xE0 ||
                       n == 0xE1 || n == 0xE3 || n == 0xEC || n == 0xED || n == 0xEF || (n >= 0xF8 && n <= 0xFF) || (n >= 0x101 && n <= 0x104);
    const bool binary = (n >= 0x0E && n <= 0x0E) || (n >= 0x23 && n <= 0x4C) || (n >= 0x4E && n <= 0x51) || n == 0x65 || n == 0x66 || (n >= 0x6E && n <= 0x73) || (n >= 0x76 && n <= 0x79) || n == 0x7B ||
                        n == 0x82 || n == 0x85 || n == 0x86 || (n >= 0x8E && n <= 0x93) || (n >= 0x95 && n <= 0x99) || (n >= 0x9B && n <= 0x9F) || n == 0xAE || n == 0xB1 || (n >= 0xB5 && n <= 0xBA) ||
                        (n >= 0xBC && n <= 0xBF) || n == 0xCE || n == 0xD1 || (n >= 0xD5 && n <= 0xDF) || (n >= 0xE4 && n <= 0xEB) || (n >= 0xF0 && n <= 0xF7) || n == 0x100 || (n >= 0x10D && n <= 0x112);
    if (shift) return in({v, i32}, v);
    if (test) return in({v}, i32);
    if (ternary) return in({v, v, v}, v);
    if (unary) return in({v}, v);
    if (binary) return in({v, v}, v);
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
        const Operand oa = lastPopped_;
        if (!Dead()) EmitRegister(Op::kFcBase + n, 1, oa, Operand{});
        Push(to);
        return true;
    }
    Materialize();  // (the rest work from the stack pointer)
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
    touchedSp_ = false;
    const uint8_t first = r_.peek();
    if (!r_.ok()) return Error(r_.error());
    // (The instructions on numbers, the constants and the locals do not work from the stack pointer: they are in register form. The locals and
    // the 0xFC group say for themselves which of them are not.)
    const bool registerForm = (first >= 0x45 && first <= 0xC4) || (first >= 0x41 && first <= 0x44) || (first >= 0x20 && first <= 0x24) || first == 0xFC || (first >= 0x28 && first <= 0x3E) ||
                              first == 0x1A || first == 0x1B || first == 0x1C;
    // (The control instructions and the calls say for themselves what they need: the stack pointer is not it.)
    const bool control = first == 0x02 || first == 0x03 || first == 0x04 || first == 0x05 || first == 0x0B || first == 0x0C || first == 0x0D || first == 0x0F || first == 0x10 || first == 0x11 ||
                         first == 0x12 || first == 0x14;
    if (!registerForm && !control) Materialize();
    const bool ok = InstructionBody(error);
    if (ok && touchedSp_ && !Dead()) spHeight_ = hN_;
    return ok;
}

bool FuncCompiler::InstructionBody(std::string& error) {
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
        case 0x08: {
            const uint32_t tag = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (tag >= cx_.tagSigs.size()) return Error("unknown tag");
            if (!PopVals(TypeRegistry::Get(cx_.tagSigs[tag]).func.params)) return false;
            if (!Dead()) Emit(Op::kThrow, tag);
            SetUnreachable();
            return true;
        }
        case 0x0A: {
            if (!Pop(ValType::Ref(true, Heap::Exn))) return false;
            if (!Dead()) Emit(Op::kThrowRef);
            SetUnreachable();
            return true;
        }
        case 0x1F: return BeginBlock(Ctl::Kind::TryTable);
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
                Spill();
                EmitReturn(nBase_ + hN_ - keep.n, results);
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
            // (A number has no slot of its own that has to be given up; a reference is on the stack of references.)
            if (!Dead() && t.IsRef()) Emit(Op::kDropR);
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
            } else if (vals_.size() >= ctls_.back().height + 3) {
                t = vals_[vals_.size() - 2];
            }
            if (t.kind == ValType::Kind::V128) Materialize();
            Operand c, v1, v2;
            if (op == 0x1C) {
                if (!Pop(ValType::I32())) return false;
                c = lastPopped_;
                if (!Pop(t)) return false;
                v2 = lastPopped_;
                if (!Pop(t)) return false;
                v1 = lastPopped_;
            } else {
                if (!Pop(ValType::I32())) return false;
                c = lastPopped_;
                ValType a, b;
                if (!PopAny(a)) return false;
                v2 = lastPopped_;
                if (!PopAny(b)) return false;
                v1 = lastPopped_;
                if (a.IsRef() || b.IsRef()) return Error("type mismatch: select needs a type for references");
                if (!a.IsBottom() && !b.IsBottom() && a != b) return Error("type mismatch: the operands of select");
                t = a.IsBottom() ? b : a;
            }
            if (!Dead()) {
                if (t.IsRef()) {
                    const uint32_t slot = SlotOf(c);
                    Emit(Op::kSelectRSlot);
                    Emit(slot);
                } else if (t.kind == ValType::Kind::V128) {
                    Emit(Op::kSelectV);
                } else {
                    const uint32_t sc = SlotOf(c), s2 = SlotOf(v2), s1 = SlotOf(v1);
                    const uint32_t dst = nBase_ + hN_;
                    Emit(Op::kSelectSlot);
                    Emit(dst);
                    Emit(s1);
                    Emit(s2);
                    Emit(sc);
                    lastEnd_ = kNone;
                }
            }
            Push(t);
            return true;
        }
        case 0x20: case 0x21: case 0x22: {
            const uint32_t index = r_.u32();
            if (!r_.ok()) return Error(r_.error());
            if (index >= locals_.size()) return Error("unknown local");
            const ValType t = locals_[index];
            const uint32_t slot = localSlot_[index];
            const bool number = t.IsNum();
            if (t.kind == ValType::Kind::V128) Materialize();
            if (op == 0x20) {
                if (!inited_[index]) return Error("uninitialized local");
                if (number) {
                    Operand o;
                    o.kind = Operand::Kind::Local;
                    o.src = slot;
                    PushOperand(t, o);
                } else {
                    Push(t);
                }
            } else {
                if (!Pop(t)) return false;
                const Operand v = lastPopped_;
                if (!inited_[index]) {
                    inited_[index] = true;
                    initLog_.push_back(index);
                }
                if (number && !Dead()) {
                    SpillLocal(slot);
                    if (v.kind == Operand::Kind::Stack) {
                        static const bool noFold = std::getenv("QUANTA_WASM_NO_FOLD") != nullptr;
                        if (!noFold && lastEnd_ == Here() && lastDst_ == v.home) {
                            out_.code[lastDstAt_] = slot;  // (the instruction that made the value writes the local itself)
                        } else {
                            Emit(Op::kCopyN, slot);
                            Emit(v.home);
                        }
                    } else if (v.kind == Operand::Kind::Local) {
                        if (v.src != slot) {
                            Emit(Op::kCopyN, slot);
                            Emit(v.src);
                        }
                    } else {
                        Emit(Op::kConstSlot, slot);
                        Emit(static_cast<uint32_t>(v.value), static_cast<uint32_t>(v.value >> 32));
                    }
                    lastEnd_ = kNone;
                }
                if (op == 0x22) {
                    if (number) {
                        Operand o;
                        o.kind = Operand::Kind::Local;
                        o.src = slot;
                        PushOperand(t, o);
                    } else {
                        Push(t);
                    }
                }
            }
            if (!number && !Dead()) {
                const int kind = t.IsRef() ? 2 : 1;
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
            if (g.type.kind == ValType::Kind::V128) Materialize();
            if (op == 0x23) {
                if (constMode_ && (index >= globalLimit_ || g.mut)) return Error("constant expression required");
                if (!Dead() && g.type.IsNum()) {
                    const uint32_t dst = nBase_ + hN_;
                    Emit(Op::kGlobalGetS);
                    Emit(dst);
                    Emit(index);
                    lastEnd_ = kNone;
                } else if (!Dead()) {
                    Emit((g.type.IsRef() ? Op::kGlobalGetR : Op::kGlobalGetV), index);
                }
                Push(g.type);
            } else {
                if (!g.mut) return Error("global is immutable");
                if (!Pop(g.type)) return false;
                const Operand v = lastPopped_;
                if (!Dead() && g.type.IsNum()) {
                    const uint32_t slot = SlotOf(v);
                    Emit(Op::kGlobalSetS);
                    Emit(index);
                    Emit(slot);
                } else if (!Dead()) {
                    Emit((g.type.IsRef() ? Op::kGlobalSetR : Op::kGlobalSetV), index);
                }
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
