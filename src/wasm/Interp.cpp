/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The machine that runs the code Compile makes. Two stacks (see Code.h): the numbers in 64-bit slots, the references as Values the collector
// reads. A call makes the arguments on the top of the caller's stacks the first slots of the callee's frame, so that nothing is copied.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <limits>

#include "quanta/core/engine/Context.h"
#include "quanta/core/gc/Collector.h"
#include "quanta/wasm/Runtime.h"
#include "quanta/wasm/Simd.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Quanta::Wasm {

const char* TrapMessage(TrapKind kind) {
    switch (kind) {
        case TrapKind::Unreachable: return "unreachable";
        case TrapKind::DivideByZero: return "divide by zero";
        case TrapKind::IntegerOverflow: return "integer overflow";
        case TrapKind::InvalidConversion: return "float unrepresentable in integer range";
        case TrapKind::MemoryOutOfBounds: return "memory access out of bounds";
        case TrapKind::TableOutOfBounds: return "table index is out of bounds";
        case TrapKind::UninitializedElement: return "null function or function signature mismatch";
        case TrapKind::IndirectCallTypeMismatch: return "null function or function signature mismatch";
        case TrapKind::NullReference: return "dereferencing a null pointer";
        case TrapKind::NullFunction: return "dereferencing a null pointer";
        case TrapKind::CastFailure: return "illegal cast";
        case TrapKind::ArrayOutOfBounds: return "array element access out of bounds";
        case TrapKind::StackOverflow: return "call stack exhausted";
        case TrapKind::UnalignedAtomic: return "operation does not support unaligned accesses";
        case TrapKind::OutOfMemory: return "out of memory";
        case TrapKind::DataSegmentOutOfBounds: return "data segment out of bounds";
        case TrapKind::ElementSegmentOutOfBounds: return "element segment out of bounds";
        default: return "trap";
    }
}

namespace {

constexpr size_t kNumSlots = size_t(8) << 20;      // 64 MB of number slots, reserved and used as they are touched
constexpr size_t kRefSlots = size_t(512) << 10;
constexpr size_t kMaxFrames = 25000;
constexpr size_t kStackSlack = 64;

struct Frame {
    const CompiledFunction* code;
    InstanceObject* instance;
    const uint32_t* pc;
    uint64_t* fp;
    Value* fr;
};

inline float AsF32(uint64_t v) { float f; const uint32_t b = static_cast<uint32_t>(v); std::memcpy(&f, &b, 4); return f; }
inline double AsF64(uint64_t v) { double d; std::memcpy(&d, &v, 8); return d; }
inline uint64_t FromF32(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline uint64_t FromF64(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

template <class F>
F Min(F a, F b) {
    if (a != a || b != b) return a + b;  // (a NaN stays one, quieted)
    if (a == b) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}
template <class F>
F Max(F a, F b) {
    if (a != a || b != b) return a + b;
    if (a == b) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
template <class F>
F Nearest(F f) { return std::nearbyint(f); }

template <class I, class F>
I Saturate(F f, F lo, F hi, I imin, I imax) {
    if (f != f) return 0;
    if (f <= lo) return imin;
    if (f >= hi) return imax;
    return static_cast<I>(f);
}


constexpr size_t kMaxArrayBytes = size_t(1) << 30;

}  // namespace

bool RefMatches(const Value& v, HeapType heap, bool nullable) {
    if (v.is_null()) return nullable;
    const bool number = v.is_number();
    StructObject* s = number ? nullptr : DOMObject::Cast<StructObject>(v);
    ArrayObject* a = (number || s) ? nullptr : DOMObject::Cast<ArrayObject>(v);
    FuncObject* f = (number || s || a) ? nullptr : DOMObject::Cast<FuncObject>(v);
    switch (heap) {
        case Heap::Any: return number || s || a || DOMObject::Cast<HostRefObject>(v) != nullptr;
        case Heap::Eq: return number || s || a;
        case Heap::I31: return number;
        case Heap::Struct: return s != nullptr;
        case Heap::Array: return a != nullptr;
        case Heap::Func: return f != nullptr;
        case Heap::Extern: return true;
        case Heap::Exn: return DOMObject::Cast<ExceptionObject>(v) != nullptr;
        case Heap::None: case Heap::NoFunc: case Heap::NoExtern: case Heap::NoExn: return false;
        default: break;
    }
    if (s) return TypeRegistry::HeapSubtype(static_cast<HeapType>(s->type), heap);
    if (a) return TypeRegistry::HeapSubtype(static_cast<HeapType>(a->type), heap);
    if (f) return TypeRegistry::HeapSubtype(static_cast<HeapType>(f->sig), heap);
    return false;
}

namespace {

inline Value I31Value(uint32_t bits) { return Value(static_cast<int32_t>(bits << 1) >> 1); }

// The value of a packed or whole element of the bytes of an array, as a slot.
inline uint64_t LoadElement(const uint8_t* p, uint32_t bytes, bool sign) {
    switch (bytes) {
        case 1: return sign ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(*p))) : *p;
        case 2: { uint16_t v; std::memcpy(&v, p, 2); return sign ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))) : v; }
        case 4: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        default: { uint64_t v; std::memcpy(&v, p, 8); return v; }
    }
}
inline void StoreElement(uint8_t* p, uint32_t bytes, uint64_t v) { std::memcpy(p, &v, bytes > 8 ? 8 : bytes); }

}  // namespace

// ---- The stacks ----

struct MachineState {
    uint64_t* numbers = nullptr;
    std::vector<Value> refs;
    std::vector<Frame> frames;
    size_t nsp = 0, rsp = 0;  // where the activations that are waiting (a host function called, which called back) have left off

    MachineState() {
        const size_t bytes = kNumSlots * sizeof(uint64_t);
#if defined(_WIN32)
        numbers = static_cast<uint64_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        numbers = p == MAP_FAILED ? nullptr : static_cast<uint64_t*>(p);
#endif
        refs.reserve(kRefSlots);
        frames.reserve(1024);
        Collector::push_value_vector(&refs);
    }
};

Machine& Machine::Current() {
    static thread_local Machine machine;
    return machine;
}

static MachineState& State() {
    static thread_local MachineState* state = new MachineState();
    return *state;
}

// The machine runs `code` of `instance` with its arguments already on the stacks at (n, r). Results are left there.
static Machine::Status Execute(Machine& machine, Context& ctx, const CompiledFunction* entryCode, InstanceObject* entryInstance, uint64_t* n0, Value* r0);

Machine::Status Machine::Invoke(Context& ctx, FuncObject* function, uint64_t* n, Value* r) {
    MachineState& s = State();
    const CanonType& sig = TypeRegistry::Get(function->sig);
    (void)sig;
    if (!function->code) return CallHost(ctx, function, n, r);
    if (!s.numbers) {
        lastTrap = TrapKind::OutOfMemory;
        return Status::Trap;
    }
    const CompiledFunction* code = function->code;
    // The arguments go on the stacks above whatever is waiting.
    uint64_t* base = s.numbers + s.nsp;
    if (s.nsp + code->paramsN + code->localsN + code->maxN + kStackSlack >= kNumSlots || s.rsp + code->paramsR + code->localsR + code->maxR + kStackSlack >= kRefSlots || s.frames.size() >= kMaxFrames) {
        lastTrap = TrapKind::StackOverflow;
        return Status::Trap;
    }
    if (s.refs.size() < s.rsp + code->paramsR + code->localsR + code->maxR + kStackSlack) s.refs.resize(s.rsp + code->paramsR + code->localsR + code->maxR + kStackSlack, Value::null());
    Value* rbase = s.refs.data() + s.rsp;
    for (uint32_t i = 0; i < code->paramsN; ++i) base[i] = n[i];
    for (uint32_t i = 0; i < code->paramsR; ++i) rbase[i] = r[i];
    const Status status = Execute(*this, ctx, code, function->instance, base, rbase);
    if (status == Status::Ok) {
        for (uint32_t i = 0; i < code->resultsN; ++i) n[i] = base[i];
        for (uint32_t i = 0; i < code->resultsR; ++i) r[i] = rbase[i];
    }
    return status;
}

Machine::Status Machine::EvaluateConst(Context& ctx, InstanceObject* instance, const CompiledFunction& code, uint64_t* n, Value* r) {
    MachineState& s = State();
    if (s.refs.size() < s.rsp + code.maxR + kStackSlack) s.refs.resize(s.rsp + code.maxR + kStackSlack, Value::null());
    uint64_t* base = s.numbers + s.nsp;
    Value* rbase = s.refs.data() + s.rsp;
    const Status status = Execute(*this, ctx, &code, instance, base, rbase);
    if (status == Status::Ok) {
        for (uint32_t i = 0; i < code.resultsN; ++i) n[i] = base[i];
        for (uint32_t i = 0; i < code.resultsR; ++i) r[i] = rbase[i];
    }
    return status;
}

// ---- The machine ----

#define TRAP(kind) do { machine.lastTrap = (kind); status = Machine::Status::Trap; goto stop; } while (0)

static Machine::Status Execute(Machine& machine, Context& ctx, const CompiledFunction* entryCode, InstanceObject* entryInstance, uint64_t* n0, Value* r0) {
    MachineState& s = State();
    Machine::Status status = Machine::Status::Ok;
    const size_t entryDepth = s.frames.size();
    const CompiledFunction* code = entryCode;
    InstanceObject* inst = entryInstance;
    const uint32_t* pc = code->code.data();
    uint64_t* fp = n0;
    Value* fr = r0;
    uint64_t* sp = fp + code->paramsN;
    Value* rsp = fr + code->paramsR;
    uint64_t* const nEnd = s.numbers + kNumSlots - kStackSlack;
    // (The locals of the entry function.)
    for (uint32_t i = 0; i < code->localsN; ++i) sp[i] = 0;
    sp += code->localsN;
    for (uint32_t i = 0; i < code->localsR; ++i) rsp[i] = Value::null();
    rsp += code->localsR;
    (void)nEnd;

    const auto memoryAt = [&](uint32_t index) -> MemoryObject* { return inst->memories[index]; };

// The address of an access of `size` bytes: the base from the stack, the offset of the instruction.
#define EFFECTIVE(mem, base, size, out) EFFECTIVE_AT(mem, base, size, out, pc[1], pc[2])
#define EFFECTIVE_AT(mem, base, size, out, offLo, offHi)                                                 \
    do {                                                                                                   \
        const uint64_t off = (static_cast<uint64_t>(offLo) | (static_cast<uint64_t>(offHi) << 32));      \
        uint64_t a = (base);                                                                               \
        if (!(mem)->type.limits.is64) a = static_cast<uint32_t>(a);                                       \
        const uint64_t ea = a + off;                                                                       \
        if (ea < a || ea > (mem)->Bytes() || (size) > (mem)->Bytes() - ea) TRAP(TrapKind::MemoryOutOfBounds); \
        (out) = (mem)->data + ea;                                                                          \
    } while (0)

#ifdef QUANTA_WASM_TRACING
    static const bool trace = std::getenv("QUANTA_WASM_TRACE") != nullptr;
#endif
    Value exception;  // what is being thrown, while it is looked for a handler
run:
    for (;;) {
        const uint32_t op = *pc++;
#ifdef QUANTA_WASM_TRACING
        if (trace) std::fprintf(stderr, "  [%zu] op %x  sp[-1]=%llx sp[-2]=%llx depth=%zu\n", static_cast<size_t>(pc - 1 - code->code.data()), op, sp > fp ? static_cast<unsigned long long>(sp[-1]) : 0ull, sp > fp + 1 ? static_cast<unsigned long long>(sp[-2]) : 0ull, s.frames.size());
#endif
        switch (op) {
            case Op::kUnreachable: TRAP(TrapKind::Unreachable);
            case Op::kJmp: pc = code->code.data() + pc[0]; break;
            case Op::kJz: {
                const uint32_t cond = static_cast<uint32_t>(*--sp);
                if (cond == 0) pc = code->code.data() + pc[0];
                else ++pc;
                break;
            }
            case Op::kBr: case Op::kBrIf: case Op::kBrOnNull: case Op::kBrOnNonNull: {
                bool take = true;
                if (op == Op::kBrIf) take = static_cast<uint32_t>(*--sp) != 0;
                else if (op == Op::kBrOnNull) {
                    if (rsp[-1].is_null()) --rsp;  // (the null goes, the branch is taken)
                    else take = false;
                } else if (op == Op::kBrOnNonNull) {
                    if (rsp[-1].is_null()) { --rsp; take = false; }
                }
                if (!take) { pc += 5; break; }
                const uint32_t target = pc[0], keepN = pc[1], keepR = pc[2], dropN = pc[3], dropR = pc[4];
                if (dropN) { std::memmove(sp - keepN - dropN, sp - keepN, keepN * sizeof(uint64_t)); sp -= dropN; }
                if (dropR) { std::memmove(rsp - keepR - dropR, rsp - keepR, keepR * sizeof(Value)); rsp -= dropR; }
                pc = code->code.data() + target;
                break;
            }
            case Op::kBrTable: {
                const uint32_t count = pc[0];
                uint32_t index = static_cast<uint32_t>(*--sp);
                if (index > count) index = count;
                const uint32_t* entry = pc + 1 + index * 5;
                const uint32_t target = entry[0], keepN = entry[1], keepR = entry[2], dropN = entry[3], dropR = entry[4];
                if (dropN) { std::memmove(sp - keepN - dropN, sp - keepN, keepN * sizeof(uint64_t)); sp -= dropN; }
                if (dropR) { std::memmove(rsp - keepR - dropR, rsp - keepR, keepR * sizeof(Value)); rsp -= dropR; }
                pc = code->code.data() + target;
                break;
            }
            case Op::kReturn: {
                const uint32_t resN = pc[0], resR = pc[1];
                if (sp - resN != fp) std::memmove(fp, sp - resN, resN * sizeof(uint64_t));
                if (rsp - resR != fr) std::memmove(fr, rsp - resR, resR * sizeof(Value));
                if (s.frames.size() == entryDepth) goto stop;
                const Frame f = s.frames.back();
                s.frames.pop_back();
                sp = fp + resN;
                rsp = fr + resR;
                code = f.code;
                inst = f.instance;
                pc = f.pc;
                fp = f.fp;
                fr = f.fr;
                break;
            }
            case Op::kCall: case Op::kCallIndirect: case Op::kCallRef: case Op::kReturnCall: case Op::kReturnCallIndirect: case Op::kReturnCallRef:
            case Op::kCallR: case Op::kCallIndirectR: case Op::kCallRefR: case Op::kReturnCallR: {
                FuncObject* callee = nullptr;
                const uint32_t* next = pc;
                uint64_t* argBase = nullptr;  // (the arguments of a call in register form: where in the frame they are)
                if (op == Op::kCallR || op == Op::kReturnCallR) {
                    callee = inst->funcs[pc[0]];
                    argBase = op == Op::kCallR ? fp + pc[1] : fp;
                    next = pc + 2;
                } else if (op == Op::kCallIndirectR) {
                    TableObject* table = inst->tables[pc[1]];
                    uint64_t index = fp[pc[3]];
                    if (!table->type.limits.is64) index = static_cast<uint32_t>(index);
                    if (index >= table->Size()) TRAP(TrapKind::TableOutOfBounds);
                    const Value& element = table->elems[static_cast<size_t>(index)];
                    if (element.is_null()) TRAP(TrapKind::UninitializedElement);
                    callee = DOMObject::Cast<FuncObject>(element);
                    if (!callee) TRAP(TrapKind::UninitializedElement);
                    if (callee->sig != pc[0] && !TypeRegistry::HeapSubtype(static_cast<HeapType>(callee->sig), static_cast<HeapType>(pc[0]))) TRAP(TrapKind::IndirectCallTypeMismatch);
                    argBase = fp + pc[2];
                    next = pc + 4;
                } else if (op == Op::kCallRefR) {
                    const Value element = *--rsp;
                    if (element.is_null()) TRAP(TrapKind::NullFunction);
                    callee = DOMObject::Cast<FuncObject>(element);
                    if (!callee) TRAP(TrapKind::NullFunction);
                    argBase = fp + pc[1];
                    next = pc + 2;
                } else if (op == Op::kCall || op == Op::kReturnCall) {
                    callee = inst->funcs[pc[0]];
                    next = pc + 1;
                } else if (op == Op::kCallIndirect || op == Op::kReturnCallIndirect) {
                    TableObject* table = inst->tables[pc[1]];
                    uint64_t index = *--sp;
                    if (!table->type.limits.is64) index = static_cast<uint32_t>(index);
                    if (index >= table->Size()) TRAP(TrapKind::TableOutOfBounds);
                    const Value& element = table->elems[static_cast<size_t>(index)];
                    if (element.is_null()) TRAP(TrapKind::UninitializedElement);
                    callee = DOMObject::Cast<FuncObject>(element);
                    if (!callee) TRAP(TrapKind::UninitializedElement);
                    if (callee->sig != pc[0] && !TypeRegistry::HeapSubtype(static_cast<HeapType>(callee->sig), static_cast<HeapType>(pc[0]))) TRAP(TrapKind::IndirectCallTypeMismatch);
                    next = pc + 2;
                } else {
                    const Value element = *--rsp;
                    if (element.is_null()) TRAP(TrapKind::NullFunction);
                    callee = DOMObject::Cast<FuncObject>(element);
                    if (!callee) TRAP(TrapKind::NullFunction);
                    next = pc + 1;
                }
                const bool tail = op == Op::kReturnCall || op == Op::kReturnCallIndirect || op == Op::kReturnCallRef || op == Op::kReturnCallR;
                if (!callee->code) {
                    // A function of the host: its arguments are taken off, its results put on.
                    const CanonType& sig = TypeRegistry::Get(callee->sig);
                    uint32_t pn = 0, pr = 0, rn = 0, rr = 0;
                    for (const ValType& t : sig.func.params) { if (t.IsRef()) ++pr; else pn += t.kind == ValType::Kind::V128 ? 2 : 1; }
                    for (const ValType& t : sig.func.results) { if (t.IsRef()) ++rr; else rn += t.kind == ValType::Kind::V128 ? 2 : 1; }
                    uint64_t smallN[16];
                    Value smallR[16];
                    std::vector<uint64_t> bigN;
                    std::vector<Value> bigR;
                    uint64_t* args = smallN;
                    Value* refs = smallR;
                    if (std::max(pn, rn) > 16) { bigN.resize(std::max(pn, rn)); args = bigN.data(); }
                    if (std::max(pr, rr) > 16) { bigR.resize(std::max(pr, rr)); refs = bigR.data(); }
                    for (uint32_t i = 0; i < pn; ++i) args[i] = argBase ? argBase[i] : sp[-static_cast<ptrdiff_t>(pn) + i];
                    for (uint32_t i = 0; i < pr; ++i) refs[i] = rsp[-static_cast<ptrdiff_t>(pr) + i];
                    // (What the host does in turn starts above this.)
                    s.nsp = static_cast<size_t>(sp - s.numbers);
                    s.rsp = static_cast<size_t>(rsp - s.refs.data());
                    const Machine::Status result = machine.CallHost(ctx, callee, args, refs);
                    if (result == Machine::Status::Exception) {
                        // (What the host threw is an exception of the module's world now: the handlers of the function look at it.)
                        const Value thrown = ctx.get_exception();
                        ctx.clear_exception();
                        exception = Value(static_cast<Object*>(WrapException(ctx, thrown)));
                        goto raise;
                    }
                    if (result != Machine::Status::Ok) { status = result; goto stop; }
                    if (argBase) {
                        for (uint32_t i = 0; i < rn; ++i) argBase[i] = args[i];
                    } else {
                        sp -= pn;
                        for (uint32_t i = 0; i < rn; ++i) sp[i] = args[i];
                        sp += rn;
                    }
                    rsp -= pr;
                    for (uint32_t i = 0; i < rr; ++i) rsp[i] = refs[i];
                    rsp += rr;
                    if (tail) {
                        // (A tail call to the host is a call, and then a return.)
                        const uint32_t resN = code->resultsN, resR = code->resultsR;
                        if (!argBase && sp - resN != fp) std::memmove(fp, sp - resN, resN * sizeof(uint64_t));
                        if (rsp - resR != fr) std::memmove(fr, rsp - resR, resR * sizeof(Value));
                        if (s.frames.size() == entryDepth) goto stop;
                        const Frame f = s.frames.back();
                        s.frames.pop_back();
                        sp = fp + resN;
                        rsp = fr + resR;
                        code = f.code; inst = f.instance; pc = f.pc; fp = f.fp; fr = f.fr;
                    } else {
                        pc = next;
                    }
                    break;
                }
                const CompiledFunction* c = callee->code;
                uint64_t* newFp = argBase ? argBase : sp - c->paramsN;
                Value* newFr = rsp - c->paramsR;
                if (newFp + c->paramsN + c->localsN + c->maxN + kStackSlack >= nEnd || static_cast<size_t>(newFr - s.refs.data()) + c->paramsR + c->localsR + c->maxR + kStackSlack >= kRefSlots ||
                    s.frames.size() >= kMaxFrames) TRAP(TrapKind::StackOverflow);
                {
                    const size_t needR = static_cast<size_t>(newFr - s.refs.data()) + c->paramsR + c->localsR + c->maxR + kStackSlack;
                    if (s.refs.size() < needR) s.refs.resize(needR, Value::null());
                }
                if (tail) {
                    // The frame is replaced: the arguments move to its base.
                    if (newFp != fp) std::memmove(fp, newFp, c->paramsN * sizeof(uint64_t));
                    if (newFr != fr) std::memmove(fr, newFr, c->paramsR * sizeof(Value));
                    newFp = fp;
                    newFr = fr;
                } else {
                    s.frames.push_back(Frame{code, inst, next, fp, fr});
                }
                code = c;
                inst = callee->instance;
                pc = c->code.data();
                fp = newFp;
                fr = newFr;
                sp = fp + c->paramsN;
                rsp = fr + c->paramsR;
                for (uint32_t i = 0; i < c->localsN; ++i) sp[i] = 0;
                sp += c->localsN;
                for (uint32_t i = 0; i < c->localsR; ++i) rsp[i] = Value::null();
                rsp += c->localsR;
                break;
            }
            case Op::kRet: {
                const uint32_t resR = pc[1];
                if (rsp - resR != fr) std::memmove(fr, rsp - resR, resR * sizeof(Value));
                if (s.frames.size() == entryDepth) goto stop;
                const Frame f = s.frames.back();
                s.frames.pop_back();
                rsp = fr + resR;
                code = f.code;
                inst = f.instance;
                pc = f.pc;
                fp = f.fp;
                fr = f.fr;
                break;
            }
            case Op::kRet1: {
                fp[0] = fp[pc[0]];
                const uint32_t resR = pc[1];
                if (rsp - resR != fr) std::memmove(fr, rsp - resR, resR * sizeof(Value));
                if (s.frames.size() == entryDepth) goto stop;
                const Frame f = s.frames.back();
                s.frames.pop_back();
                rsp = fr + resR;
                code = f.code;
                inst = f.instance;
                pc = f.pc;
                fp = f.fp;
                fr = f.fr;
                break;
            }
            case Op::kBrTableS: {
                const uint32_t count = pc[1];
                uint32_t index = static_cast<uint32_t>(fp[pc[0]]);
                if (index > count) index = count;
                pc = code->code.data() + pc[2 + index];
                break;
            }
            case Op::kJzSlot:
                if (static_cast<uint32_t>(fp[pc[0]]) == 0) pc = code->code.data() + pc[1];
                else pc += 2;
                break;
            case Op::kJnzSlot:
                if (static_cast<uint32_t>(fp[pc[0]]) != 0) pc = code->code.data() + pc[1];
                else pc += 2;
                break;
            case Op::kSelectSlot: fp[pc[0]] = static_cast<uint32_t>(fp[pc[3]]) ? fp[pc[1]] : fp[pc[2]]; pc += 4; break;
            case Op::kSelectRSlot: {
                const uint32_t c = static_cast<uint32_t>(fp[pc[0]]);
                --rsp;
                if (!c) rsp[-1] = rsp[0];
                pc += 1;
                break;
            }
            case Op::kGlobalGetS: fp[pc[0]] = inst->globals[pc[1]]->lo; pc += 2; break;
            case Op::kGlobalSetS: inst->globals[pc[0]]->lo = fp[pc[1]]; pc += 2; break;
            case Op::kThrow: {
                TagObject* tag = inst->tags[pc[0]];
                const CanonType& sig = TypeRegistry::Get(tag->sig);
                uint32_t pn = 0, pr = 0;
                for (const ValType& t : sig.func.params) { if (t.IsRef()) ++pr; else pn += t.kind == ValType::Kind::V128 ? 2 : 1; }
                ExceptionObject* e = NewException(ctx, tag);
                e->numbers.assign(sp - pn, sp);
                for (uint32_t i = 0; i < pr; ++i) e->refs.push_back(rsp[-static_cast<ptrdiff_t>(pr) + i]);
                sp -= pn;
                rsp -= pr;
                exception = Value(static_cast<Object*>(e));
                goto raise;
            }
            case Op::kThrowRef: {
                exception = *--rsp;
                if (exception.is_null()) TRAP(TrapKind::NullReference);
                goto raise;
            }
            case Op::kDropN: --sp; break;
            case Op::kDropV: sp -= 2; break;
            case Op::kDropR: --rsp; break;
            case Op::kSelectN: {
                const uint32_t c = static_cast<uint32_t>(sp[-1]);
                sp -= 2;
                if (!c) sp[-1] = sp[0];
                break;
            }
            case Op::kSelectV: {
                const uint32_t c = static_cast<uint32_t>(sp[-1]);
                sp -= 3;
                if (!c) { sp[-2] = sp[0]; sp[-1] = sp[1]; }
                break;
            }
            case Op::kSelectR: {
                const uint32_t c = static_cast<uint32_t>(*--sp);
                --rsp;
                if (!c) rsp[-1] = rsp[0];
                break;
            }
            case Op::kLocalGetN: *sp++ = fp[*pc++]; break;
            case Op::kLocalGetV: { const uint32_t i = *pc++; sp[0] = fp[i]; sp[1] = fp[i + 1]; sp += 2; break; }
            case Op::kLocalGetR: *rsp++ = fr[*pc++]; break;
            case Op::kLocalSetN: fp[*pc++] = *--sp; break;
            case Op::kLocalSetV: { const uint32_t i = *pc++; sp -= 2; fp[i] = sp[0]; fp[i + 1] = sp[1]; break; }
            case Op::kLocalSetR: fr[*pc++] = *--rsp; break;
            case Op::kLocalTeeN: fp[*pc++] = sp[-1]; break;
            case Op::kLocalTeeV: { const uint32_t i = *pc++; fp[i] = sp[-2]; fp[i + 1] = sp[-1]; break; }
            case Op::kLocalTeeR: fr[*pc++] = rsp[-1]; break;
            case Op::kGlobalGetN: { GlobalObject* g = inst->globals[*pc++]; *sp++ = g->lo; break; }
            case Op::kGlobalGetV: { GlobalObject* g = inst->globals[*pc++]; sp[0] = g->lo; sp[1] = g->hi; sp += 2; break; }
            case Op::kGlobalGetR: { GlobalObject* g = inst->globals[*pc++]; *rsp++ = g->ref.Get(); break; }
            case Op::kGlobalSetN: { GlobalObject* g = inst->globals[*pc++]; g->lo = *--sp; break; }
            case Op::kGlobalSetV: { GlobalObject* g = inst->globals[*pc++]; sp -= 2; g->lo = sp[0]; g->hi = sp[1]; break; }
            case Op::kGlobalSetR: { GlobalObject* g = inst->globals[*pc++]; g->ref.Set(*--rsp); break; }
            case Op::kConstN: *sp++ = *pc++; break;
            case Op::kConstW: *sp++ = static_cast<uint64_t>(pc[0]) | (static_cast<uint64_t>(pc[1]) << 32); pc += 2; break;
            case Op::kConstV:
                sp[0] = static_cast<uint64_t>(pc[0]) | (static_cast<uint64_t>(pc[1]) << 32);
                sp[1] = static_cast<uint64_t>(pc[2]) | (static_cast<uint64_t>(pc[3]) << 32);
                sp += 2;
                pc += 4;
                break;
            case Op::kRefNull: *rsp++ = Value::null(); break;
            case Op::kRefIsNull: { const bool isNull = (--rsp)->is_null(); *sp++ = isNull ? 1 : 0; break; }
            case Op::kRefFunc: *rsp++ = Value(static_cast<Object*>(inst->funcs[*pc++])); break;
            case Op::kRefEq: {
                rsp -= 2;
                const Value& a = rsp[0];
                const Value& b = rsp[1];
                *sp++ = a.strict_equals(b) ? 1 : 0;
                break;
            }
            case Op::kRefAsNonNull:
                if (rsp[-1].is_null()) TRAP(TrapKind::NullReference);
                break;
            case Op::kTableGet: {
                TableObject* table = inst->tables[*pc++];
                uint64_t index = sp[-1];
                if (!table->type.limits.is64) index = static_cast<uint32_t>(index);
                if (index >= table->Size()) TRAP(TrapKind::TableOutOfBounds);
                --sp;
                *rsp++ = table->elems[static_cast<size_t>(index)];
                break;
            }
            case Op::kTableSet: {
                TableObject* table = inst->tables[*pc++];
                const Value v = *--rsp;
                uint64_t index = *--sp;
                if (!table->type.limits.is64) index = static_cast<uint32_t>(index);
                if (index >= table->Size()) TRAP(TrapKind::TableOutOfBounds);
                table->elems.set(static_cast<size_t>(index), v);
                break;
            }
            case Op::kMemorySize: { MemoryObject* m = memoryAt(*pc++); *sp++ = m->pages; break; }
            case Op::kMemoryGrow: {
                MemoryObject* m = memoryAt(*pc++);
                uint64_t delta = sp[-1];
                if (!m->type.limits.is64) delta = static_cast<uint32_t>(delta);
                const int64_t before = m->Grow(delta);
                if (before >= 0) NotifyMemoryGrown(ctx, m);
                sp[-1] = before < 0 ? (m->type.limits.is64 ? ~uint64_t(0) : 0xFFFFFFFFull) : static_cast<uint64_t>(before);
                break;
            }

            // ---- Loads and stores ----
#define LOAD(opcode, ctype, conv)                                                    \
    case opcode: {                                                                    \
        MemoryObject* m = memoryAt(pc[2]);                                            \
        uint8_t* p;                                                                   \
        EFFECTIVE_AT(m, fp[pc[1]], sizeof(ctype), p, pc[3], pc[4]);                   \
        ctype v;                                                                      \
        std::memcpy(&v, p, sizeof(ctype));                                            \
        fp[pc[0]] = conv;                                                             \
        pc += 5;                                                                      \
        break;                                                                        \
    }
            LOAD(0x28, uint32_t, static_cast<uint64_t>(v))
            LOAD(0x29, uint64_t, v)
            LOAD(0x2A, uint32_t, static_cast<uint64_t>(v))
            LOAD(0x2B, uint64_t, v)
            LOAD(0x2C, int8_t, static_cast<uint32_t>(static_cast<int32_t>(v)))
            LOAD(0x2D, uint8_t, static_cast<uint64_t>(v))
            LOAD(0x2E, int16_t, static_cast<uint32_t>(static_cast<int32_t>(v)))
            LOAD(0x2F, uint16_t, static_cast<uint64_t>(v))
            LOAD(0x30, int8_t, static_cast<uint64_t>(static_cast<int64_t>(v)))
            LOAD(0x31, uint8_t, static_cast<uint64_t>(v))
            LOAD(0x32, int16_t, static_cast<uint64_t>(static_cast<int64_t>(v)))
            LOAD(0x33, uint16_t, static_cast<uint64_t>(v))
            LOAD(0x34, int32_t, static_cast<uint64_t>(static_cast<int64_t>(v)))
            LOAD(0x35, uint32_t, static_cast<uint64_t>(v))
#define STORE(opcode, ctype)                                                          \
    case opcode: {                                                                    \
        MemoryObject* m = memoryAt(pc[2]);                                            \
        uint8_t* p;                                                                   \
        EFFECTIVE_AT(m, fp[pc[1]], sizeof(ctype), p, pc[3], pc[4]);                   \
        const ctype v = static_cast<ctype>(fp[pc[0]]);                                \
        std::memcpy(p, &v, sizeof(ctype));                                            \
        pc += 5;                                                                      \
        break;                                                                        \
    }
            STORE(0x36, uint32_t)
            STORE(0x37, uint64_t)
            STORE(0x38, uint32_t)
            STORE(0x39, uint64_t)
            STORE(0x3A, uint8_t)
            STORE(0x3B, uint16_t)
            STORE(0x3C, uint8_t)
            STORE(0x3D, uint16_t)
            STORE(0x3E, uint32_t)

            // ---- Register form: the slots are those of the frame (see Code.h) ----
            case Op::kCopyN: fp[pc[0]] = fp[pc[1]]; pc += 2; break;
            case Op::kConstSlot: fp[pc[0]] = static_cast<uint64_t>(pc[1]) | (static_cast<uint64_t>(pc[2]) << 32); pc += 3; break;
            case Op::kSyncSp: sp = fp + code->paramsN + code->localsN + pc[0]; ++pc; break;

            // ---- Numbers ----
#define U32(x) static_cast<uint32_t>(x)
#define S32(x) static_cast<int32_t>(static_cast<uint32_t>(x))
#define S64(x) static_cast<int64_t>(x)
#define BIN32(opcode, expr) case opcode: { const uint32_t b = U32(fp[pc[2]]); const uint32_t a = U32(fp[pc[1]]); fp[pc[0]] = static_cast<uint64_t>(static_cast<uint32_t>(expr)); pc += 3; break; } \
    case opcode + Op::kImm: { const uint32_t b = pc[2]; const uint32_t a = U32(fp[pc[1]]); fp[pc[0]] = static_cast<uint64_t>(static_cast<uint32_t>(expr)); pc += 3; break; }
#define BIN64(opcode, expr) case opcode: { const uint64_t b = fp[pc[2]]; const uint64_t a = fp[pc[1]]; fp[pc[0]] = static_cast<uint64_t>(expr); pc += 3; break; } \
    case opcode + Op::kImm: { const uint64_t b = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(pc[2]))); const uint64_t a = fp[pc[1]]; fp[pc[0]] = static_cast<uint64_t>(expr); pc += 3; break; }
#define CMP32(opcode, expr) case opcode: { const uint32_t b = U32(fp[pc[2]]); const uint32_t a = U32(fp[pc[1]]); fp[pc[0]] = (expr) ? 1 : 0; pc += 3; break; } \
    case opcode + Op::kImm: { const uint32_t b = pc[2]; const uint32_t a = U32(fp[pc[1]]); fp[pc[0]] = (expr) ? 1 : 0; pc += 3; break; } \
    case opcode + Op::kJumpIf: { const uint32_t b = U32(fp[pc[1]]); const uint32_t a = U32(fp[pc[0]]); if (expr) pc = code->code.data() + pc[2]; else pc += 3; break; } \
    case opcode + Op::kJumpIfImm: { const uint32_t b = pc[1]; const uint32_t a = U32(fp[pc[0]]); if (expr) pc = code->code.data() + pc[2]; else pc += 3; break; }
#define CMP64(opcode, expr) case opcode: { const uint64_t b = fp[pc[2]]; const uint64_t a = fp[pc[1]]; fp[pc[0]] = (expr) ? 1 : 0; pc += 3; break; } \
    case opcode + Op::kImm: { const uint64_t b = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(pc[2]))); const uint64_t a = fp[pc[1]]; fp[pc[0]] = (expr) ? 1 : 0; pc += 3; break; } \
    case opcode + Op::kJumpIf: { const uint64_t b = fp[pc[1]]; const uint64_t a = fp[pc[0]]; if (expr) pc = code->code.data() + pc[2]; else pc += 3; break; } \
    case opcode + Op::kJumpIfImm: { const uint64_t b = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(pc[1]))); const uint64_t a = fp[pc[0]]; if (expr) pc = code->code.data() + pc[2]; else pc += 3; break; }
#define CMPF32(opcode, expr) case opcode: { const float b = AsF32(fp[pc[2]]); const float a = AsF32(fp[pc[1]]); fp[pc[0]] = (expr) ? 1 : 0; pc += 3; break; }
#define CMPF64(opcode, expr) case opcode: { const double b = AsF64(fp[pc[2]]); const double a = AsF64(fp[pc[1]]); fp[pc[0]] = (expr) ? 1 : 0; pc += 3; break; }
#define BINF32(opcode, expr) case opcode: { const float b = AsF32(fp[pc[2]]); const float a = AsF32(fp[pc[1]]); fp[pc[0]] = FromF32(expr); pc += 3; break; }
#define BINF64(opcode, expr) case opcode: { const double b = AsF64(fp[pc[2]]); const double a = AsF64(fp[pc[1]]); fp[pc[0]] = FromF64(expr); pc += 3; break; }
#define UNF32(opcode, expr) case opcode: { const float a = AsF32(fp[pc[1]]); fp[pc[0]] = FromF32(expr); pc += 2; break; }
#define UNF64(opcode, expr) case opcode: { const double a = AsF64(fp[pc[1]]); fp[pc[0]] = FromF64(expr); pc += 2; break; }

            case 0x45: fp[pc[0]] = U32(fp[pc[1]]) == 0 ? 1 : 0; pc += 2; break;
            CMP32(0x46, a == b)
            CMP32(0x47, a != b)
            CMP32(0x48, S32(a) < S32(b))
            CMP32(0x49, a < b)
            CMP32(0x4A, S32(a) > S32(b))
            CMP32(0x4B, a > b)
            CMP32(0x4C, S32(a) <= S32(b))
            CMP32(0x4D, a <= b)
            CMP32(0x4E, S32(a) >= S32(b))
            CMP32(0x4F, a >= b)
            case 0x50: fp[pc[0]] = fp[pc[1]] == 0 ? 1 : 0; pc += 2; break;
            CMP64(0x51, a == b)
            CMP64(0x52, a != b)
            CMP64(0x53, S64(a) < S64(b))
            CMP64(0x54, a < b)
            CMP64(0x55, S64(a) > S64(b))
            CMP64(0x56, a > b)
            CMP64(0x57, S64(a) <= S64(b))
            CMP64(0x58, a <= b)
            CMP64(0x59, S64(a) >= S64(b))
            CMP64(0x5A, a >= b)
            CMPF32(0x5B, a == b)
            CMPF32(0x5C, a != b)
            CMPF32(0x5D, a < b)
            CMPF32(0x5E, a > b)
            CMPF32(0x5F, a <= b)
            CMPF32(0x60, a >= b)
            CMPF64(0x61, a == b)
            CMPF64(0x62, a != b)
            CMPF64(0x63, a < b)
            CMPF64(0x64, a > b)
            CMPF64(0x65, a <= b)
            CMPF64(0x66, a >= b)
            case 0x67: { const uint32_t a = U32(fp[pc[1]]); fp[pc[0]] = a ? __builtin_clz(a) : 32; pc += 2; break; }
            case 0x68: { const uint32_t a = U32(fp[pc[1]]); fp[pc[0]] = a ? __builtin_ctz(a) : 32; pc += 2; break; }
            case 0x69: fp[pc[0]] = __builtin_popcount(U32(fp[pc[1]])); pc += 2; break;
            BIN32(0x6A, a + b)
            BIN32(0x6B, a - b)
            BIN32(0x6C, a * b)
            case 0x6D: {  // i32.div_s
                const int32_t b = S32(fp[pc[2]]), a = S32(fp[pc[1]]);
                if (b == 0) TRAP(TrapKind::DivideByZero);
                if (a == std::numeric_limits<int32_t>::min() && b == -1) TRAP(TrapKind::IntegerOverflow);
                
                fp[pc[0]] = static_cast<uint32_t>(a / b);
                pc += 3; break;
            }
            case 0x6E: {  // i32.div_u
                const uint32_t b = U32(fp[pc[2]]), a = U32(fp[pc[1]]);
                if (b == 0) TRAP(TrapKind::DivideByZero);
                
                fp[pc[0]] = a / b;
                pc += 3; break;
            }
            case 0x6F: {  // i32.rem_s
                const int32_t b = S32(fp[pc[2]]), a = S32(fp[pc[1]]);
                if (b == 0) TRAP(TrapKind::DivideByZero);
                
                fp[pc[0]] = b == -1 ? 0 : static_cast<uint32_t>(a % b);
                pc += 3; break;
            }
            case 0x70: {  // i32.rem_u
                const uint32_t b = U32(fp[pc[2]]), a = U32(fp[pc[1]]);
                if (b == 0) TRAP(TrapKind::DivideByZero);
                
                fp[pc[0]] = a % b;
                pc += 3; break;
            }
            BIN32(0x71, a & b)
            BIN32(0x72, a | b)
            BIN32(0x73, a ^ b)
            BIN32(0x74, a << (b & 31))
            BIN32(0x75, static_cast<uint32_t>(S32(a) >> (b & 31)))
            BIN32(0x76, a >> (b & 31))
            BIN32(0x77, (a << (b & 31)) | (a >> ((32 - (b & 31)) & 31)))
            BIN32(0x78, (a >> (b & 31)) | (a << ((32 - (b & 31)) & 31)))
            case 0x79: { const uint64_t a = fp[pc[1]]; fp[pc[0]] = a ? __builtin_clzll(a) : 64; pc += 2; break; }
            case 0x7A: { const uint64_t a = fp[pc[1]]; fp[pc[0]] = a ? __builtin_ctzll(a) : 64; pc += 2; break; }
            case 0x7B: fp[pc[0]] = __builtin_popcountll(fp[pc[1]]); pc += 2; break;
            BIN64(0x7C, a + b)
            BIN64(0x7D, a - b)
            BIN64(0x7E, a * b)
            case 0x7F: {  // i64.div_s
                const int64_t b = S64(fp[pc[2]]), a = S64(fp[pc[1]]);
                if (b == 0) TRAP(TrapKind::DivideByZero);
                if (a == std::numeric_limits<int64_t>::min() && b == -1) TRAP(TrapKind::IntegerOverflow);
                
                fp[pc[0]] = static_cast<uint64_t>(a / b);
                pc += 3; break;
            }
            case 0x80: {  // i64.div_u
                const uint64_t b = fp[pc[2]], a = fp[pc[1]];
                if (b == 0) TRAP(TrapKind::DivideByZero);
                
                fp[pc[0]] = a / b;
                pc += 3; break;
            }
            case 0x81: {  // i64.rem_s
                const int64_t b = S64(fp[pc[2]]), a = S64(fp[pc[1]]);
                if (b == 0) TRAP(TrapKind::DivideByZero);
                
                fp[pc[0]] = b == -1 ? 0 : static_cast<uint64_t>(a % b);
                pc += 3; break;
            }
            case 0x82: {  // i64.rem_u
                const uint64_t b = fp[pc[2]], a = fp[pc[1]];
                if (b == 0) TRAP(TrapKind::DivideByZero);
                
                fp[pc[0]] = a % b;
                pc += 3; break;
            }
            BIN64(0x83, a & b)
            BIN64(0x84, a | b)
            BIN64(0x85, a ^ b)
            BIN64(0x86, a << (b & 63))
            BIN64(0x87, static_cast<uint64_t>(S64(a) >> (b & 63)))
            BIN64(0x88, a >> (b & 63))
            BIN64(0x89, (a << (b & 63)) | (a >> ((64 - (b & 63)) & 63)))
            BIN64(0x8A, (a >> (b & 63)) | (a << ((64 - (b & 63)) & 63)))
            case 0x8B: fp[pc[0]] = (fp[pc[1]] & 0xFFFFFFFFull) & ~0x80000000ull; pc += 2; break;  // f32.abs
            case 0x8C: fp[pc[0]] = (fp[pc[1]] & 0xFFFFFFFFull) ^ 0x80000000ull; pc += 2; break;   // f32.neg
            UNF32(0x8D, std::ceil(a))
            UNF32(0x8E, std::floor(a))
            UNF32(0x8F, std::trunc(a))
            UNF32(0x90, Nearest(a))
            UNF32(0x91, std::sqrt(a))
            BINF32(0x92, a + b)
            BINF32(0x93, a - b)
            BINF32(0x94, a * b)
            BINF32(0x95, a / b)
            BINF32(0x96, Min(a, b))
            BINF32(0x97, Max(a, b))
            case 0x98: { const uint32_t b = U32(fp[pc[2]]), a = U32(fp[pc[1]]);  fp[pc[0]] = (a & 0x7FFFFFFFu) | (b & 0x80000000u); pc += 3; break; }
            case 0x99: fp[pc[0]] = fp[pc[1]] & ~0x8000000000000000ull; pc += 2; break;  // f64.abs
            case 0x9A: fp[pc[0]] = fp[pc[1]] ^ 0x8000000000000000ull; pc += 2; break;   // f64.neg
            UNF64(0x9B, std::ceil(a))
            UNF64(0x9C, std::floor(a))
            UNF64(0x9D, std::trunc(a))
            UNF64(0x9E, Nearest(a))
            UNF64(0x9F, std::sqrt(a))
            BINF64(0xA0, a + b)
            BINF64(0xA1, a - b)
            BINF64(0xA2, a * b)
            BINF64(0xA3, a / b)
            BINF64(0xA4, Min(a, b))
            BINF64(0xA5, Max(a, b))
            case 0xA6: { const uint64_t b = fp[pc[2]], a = fp[pc[1]];  fp[pc[0]] = (a & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull); pc += 3; break; }
            case 0xA7: fp[pc[0]] = U32(fp[pc[1]]); pc += 2; break;  // i32.wrap_i64
            case 0xA8: {  // i32.trunc_f32_s
                const float f = AsF32(fp[pc[1]]);
                if (f != f) TRAP(TrapKind::InvalidConversion);
                if (!(f >= -2147483648.0f && f < 2147483648.0f)) TRAP(TrapKind::IntegerOverflow);
                fp[pc[0]] = static_cast<uint32_t>(static_cast<int32_t>(f));
                pc += 2; break;
            }
            case 0xA9: {  // i32.trunc_f32_u
                const float f = AsF32(fp[pc[1]]);
                if (f != f) TRAP(TrapKind::InvalidConversion);
                if (!(f > -1.0f && f < 4294967296.0f)) TRAP(TrapKind::IntegerOverflow);
                fp[pc[0]] = static_cast<uint32_t>(f);
                pc += 2; break;
            }
            case 0xAA: {  // i32.trunc_f64_s
                const double f = AsF64(fp[pc[1]]);
                if (f != f) TRAP(TrapKind::InvalidConversion);
                if (!(f > -2147483649.0 && f < 2147483648.0)) TRAP(TrapKind::IntegerOverflow);
                fp[pc[0]] = static_cast<uint32_t>(static_cast<int32_t>(f));
                pc += 2; break;
            }
            case 0xAB: {  // i32.trunc_f64_u
                const double f = AsF64(fp[pc[1]]);
                if (f != f) TRAP(TrapKind::InvalidConversion);
                if (!(f > -1.0 && f < 4294967296.0)) TRAP(TrapKind::IntegerOverflow);
                fp[pc[0]] = static_cast<uint32_t>(f);
                pc += 2; break;
            }
            case 0xAC: fp[pc[0]] = static_cast<uint64_t>(static_cast<int64_t>(S32(fp[pc[1]]))); pc += 2; break;  // i64.extend_i32_s
            case 0xAD: fp[pc[0]] = U32(fp[pc[1]]); pc += 2; break;                                                // i64.extend_i32_u
            case 0xAE: {  // i64.trunc_f32_s
                const float f = AsF32(fp[pc[1]]);
                if (f != f) TRAP(TrapKind::InvalidConversion);
                if (!(f >= -9223372036854775808.0f && f < 9223372036854775808.0f)) TRAP(TrapKind::IntegerOverflow);
                fp[pc[0]] = static_cast<uint64_t>(static_cast<int64_t>(f));
                pc += 2; break;
            }
            case 0xAF: {  // i64.trunc_f32_u
                const float f = AsF32(fp[pc[1]]);
                if (f != f) TRAP(TrapKind::InvalidConversion);
                if (!(f > -1.0f && f < 18446744073709551616.0f)) TRAP(TrapKind::IntegerOverflow);
                fp[pc[0]] = static_cast<uint64_t>(f);
                pc += 2; break;
            }
            case 0xB0: {  // i64.trunc_f64_s
                const double f = AsF64(fp[pc[1]]);
                if (f != f) TRAP(TrapKind::InvalidConversion);
                if (!(f >= -9223372036854775808.0 && f < 9223372036854775808.0)) TRAP(TrapKind::IntegerOverflow);
                fp[pc[0]] = static_cast<uint64_t>(static_cast<int64_t>(f));
                pc += 2; break;
            }
            case 0xB1: {  // i64.trunc_f64_u
                const double f = AsF64(fp[pc[1]]);
                if (f != f) TRAP(TrapKind::InvalidConversion);
                if (!(f > -1.0 && f < 18446744073709551616.0)) TRAP(TrapKind::IntegerOverflow);
                fp[pc[0]] = static_cast<uint64_t>(f);
                pc += 2; break;
            }
            case 0xB2: fp[pc[0]] = FromF32(static_cast<float>(S32(fp[pc[1]]))); pc += 2; break;
            case 0xB3: fp[pc[0]] = FromF32(static_cast<float>(U32(fp[pc[1]]))); pc += 2; break;
            case 0xB4: fp[pc[0]] = FromF32(static_cast<float>(S64(fp[pc[1]]))); pc += 2; break;
            case 0xB5: fp[pc[0]] = FromF32(static_cast<float>(fp[pc[1]])); pc += 2; break;
            case 0xB6: fp[pc[0]] = FromF32(static_cast<float>(AsF64(fp[pc[1]]))); pc += 2; break;
            case 0xB7: fp[pc[0]] = FromF64(static_cast<double>(S32(fp[pc[1]]))); pc += 2; break;
            case 0xB8: fp[pc[0]] = FromF64(static_cast<double>(U32(fp[pc[1]]))); pc += 2; break;
            case 0xB9: fp[pc[0]] = FromF64(static_cast<double>(S64(fp[pc[1]]))); pc += 2; break;
            case 0xBA: fp[pc[0]] = FromF64(static_cast<double>(fp[pc[1]])); pc += 2; break;
            case 0xBB: fp[pc[0]] = FromF64(static_cast<double>(AsF32(fp[pc[1]]))); pc += 2; break;
            case 0xBC: fp[pc[0]] = U32(fp[pc[1]]); pc += 2; break;  // (reinterpret: the bits are where they were)
            case 0xBD: case 0xBE: case 0xBF: fp[pc[0]] = fp[pc[1]]; pc += 2; break;
            case 0xC0: fp[pc[0]] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(fp[pc[1]]))); pc += 2; break;
            case 0xC1: fp[pc[0]] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(fp[pc[1]]))); pc += 2; break;
            case 0xC2: fp[pc[0]] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(fp[pc[1]]))); pc += 2; break;
            case 0xC3: fp[pc[0]] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(fp[pc[1]]))); pc += 2; break;
            case 0xC4: fp[pc[0]] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(fp[pc[1]]))); pc += 2; break;


            // ---- 0xFC: saturating conversions and bulk operations ----
            case Op::kFcBase + 0: fp[pc[0]] = static_cast<uint32_t>(Saturate<int32_t, float>(AsF32(fp[pc[1]]), -2147483648.0f, 2147483648.0f, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max())); pc += 2; break;
            case Op::kFcBase + 1: fp[pc[0]] = Saturate<uint32_t, float>(AsF32(fp[pc[1]]), 0.0f, 4294967296.0f, 0u, 0xFFFFFFFFu); pc += 2; break;
            case Op::kFcBase + 2: fp[pc[0]] = static_cast<uint32_t>(Saturate<int32_t, double>(AsF64(fp[pc[1]]), -2147483648.0, 2147483648.0, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max())); pc += 2; break;
            case Op::kFcBase + 3: fp[pc[0]] = Saturate<uint32_t, double>(AsF64(fp[pc[1]]), 0.0, 4294967296.0, 0u, 0xFFFFFFFFu); pc += 2; break;
            case Op::kFcBase + 4: fp[pc[0]] = static_cast<uint64_t>(Saturate<int64_t, float>(AsF32(fp[pc[1]]), -9223372036854775808.0f, 9223372036854775808.0f, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max())); pc += 2; break;
            case Op::kFcBase + 5: fp[pc[0]] = Saturate<uint64_t, float>(AsF32(fp[pc[1]]), 0.0f, 18446744073709551616.0f, 0ull, ~0ull); pc += 2; break;
            case Op::kFcBase + 6: fp[pc[0]] = static_cast<uint64_t>(Saturate<int64_t, double>(AsF64(fp[pc[1]]), -9223372036854775808.0, 9223372036854775808.0, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max())); pc += 2; break;
            case Op::kFcBase + 7: fp[pc[0]] = Saturate<uint64_t, double>(AsF64(fp[pc[1]]), 0.0, 18446744073709551616.0, 0ull, ~0ull); pc += 2; break;
            case Op::kFcBase + 8: {  // memory.init data memory
                const uint32_t dataIndex = pc[0];
                MemoryObject* m = memoryAt(pc[1]);
                uint64_t length = U32(sp[-1]), source = U32(sp[-2]), dest = sp[-3];
                if (!m->type.limits.is64) dest = U32(dest);
                sp -= 3;
                const DataSegment& d = inst->module->datas[dataIndex];
                const uint64_t size = inst->dataDropped[dataIndex] ? 0 : d.bytes.size;
                if (source > size || length > size - source || dest > m->Bytes() || length > m->Bytes() - dest) TRAP(TrapKind::MemoryOutOfBounds);
                if (length) std::memcpy(m->data + dest, inst->module->bytes.data() + d.bytes.offset + source, length);
                pc += 2;
                break;
            }
            case Op::kFcBase + 9: inst->dataDropped[*pc++] = true; break;
            case Op::kFcBase + 10: {  // memory.copy dst src
                MemoryObject* dm = memoryAt(pc[0]);
                MemoryObject* sm = memoryAt(pc[1]);
                uint64_t length = sp[-1], source = sp[-2], dest = sp[-3];
                if (!(dm->type.limits.is64 && sm->type.limits.is64)) length = U32(length);
                if (!sm->type.limits.is64) source = U32(source);
                if (!dm->type.limits.is64) dest = U32(dest);
                sp -= 3;
                if (source > sm->Bytes() || length > sm->Bytes() - source || dest > dm->Bytes() || length > dm->Bytes() - dest) TRAP(TrapKind::MemoryOutOfBounds);
                if (length) std::memmove(dm->data + dest, sm->data + source, length);
                pc += 2;
                break;
            }
            case Op::kFcBase + 11: {  // memory.fill
                MemoryObject* m = memoryAt(*pc++);
                uint64_t length = sp[-1], dest = sp[-3];
                const uint8_t value = static_cast<uint8_t>(sp[-2]);
                if (!m->type.limits.is64) { length = U32(length); dest = U32(dest); }
                sp -= 3;
                if (dest > m->Bytes() || length > m->Bytes() - dest) TRAP(TrapKind::MemoryOutOfBounds);
                if (length) std::memset(m->data + dest, value, length);
                break;
            }
            case Op::kFcBase + 12: {  // table.init elem table
                const uint32_t elemIndex = pc[0];
                TableObject* t = inst->tables[pc[1]];
                uint64_t length = U32(sp[-1]), source = U32(sp[-2]), dest = sp[-3];
                if (!t->type.limits.is64) dest = U32(dest);
                sp -= 3;
                const std::vector<Value>& items = inst->elems[elemIndex];
                if (source > items.size() || length > items.size() - source || dest > t->Size() || length > t->Size() - dest) TRAP(TrapKind::TableOutOfBounds);
                for (uint64_t i = 0; i < length; ++i) t->elems.set(static_cast<size_t>(dest + i), items[static_cast<size_t>(source + i)]);
                pc += 2;
                break;
            }
            case Op::kFcBase + 13: { std::vector<Value>().swap(inst->elems[*pc++]); break; }
            case Op::kFcBase + 14: {  // table.copy dst src
                TableObject* dt = inst->tables[pc[0]];
                TableObject* st = inst->tables[pc[1]];
                uint64_t length = sp[-1], source = sp[-2], dest = sp[-3];
                if (!(dt->type.limits.is64 && st->type.limits.is64)) length = U32(length);
                if (!st->type.limits.is64) source = U32(source);
                if (!dt->type.limits.is64) dest = U32(dest);
                sp -= 3;
                if (source > st->Size() || length > st->Size() - source || dest > dt->Size() || length > dt->Size() - dest) TRAP(TrapKind::TableOutOfBounds);
                if (dt == st && dest > source) {
                    for (uint64_t i = length; i-- > 0;) dt->elems.set(static_cast<size_t>(dest + i), st->elems[static_cast<size_t>(source + i)]);
                } else {
                    for (uint64_t i = 0; i < length; ++i) dt->elems.set(static_cast<size_t>(dest + i), st->elems[static_cast<size_t>(source + i)]);
                }
                pc += 2;
                break;
            }
            case Op::kFcBase + 15: {  // table.grow
                TableObject* t = inst->tables[*pc++];
                uint64_t delta = sp[-1];
                if (!t->type.limits.is64) delta = U32(delta);
                const Value init = *--rsp;
                const int64_t before = t->Grow(delta, init);
                sp[-1] = before < 0 ? (t->type.limits.is64 ? ~uint64_t(0) : 0xFFFFFFFFull) : static_cast<uint64_t>(before);
                break;
            }
            case Op::kFcBase + 16: { TableObject* t = inst->tables[*pc++]; *sp++ = t->Size(); break; }
            case Op::kFcBase + 17: {  // table.fill
                TableObject* t = inst->tables[*pc++];
                uint64_t length = sp[-1], dest = sp[-2];
                const Value v = *--rsp;
                if (!t->type.limits.is64) { length = U32(length); dest = U32(dest); }
                sp -= 2;
                if (dest > t->Size() || length > t->Size() - dest) TRAP(TrapKind::TableOutOfBounds);
                for (uint64_t i = 0; i < length; ++i) t->elems.set(static_cast<size_t>(dest + i), v);
                break;
            }
            // ---- 0xFB: the instructions of the GC proposal ----
            case Op::kFbBase + 0x00: case Op::kFbBase + 0x01: {  // struct.new, struct.new_default
                const CanonType& t = TypeRegistry::Get(pc[0]);
                StructObject* o = ::Quanta::Heap::Allocate<StructObject>();
                o->initialize_prototype(nullptr);
                o->prevent_extensions();
                o->type = pc[0];
                if (op == Op::kFbBase + 0x00) {
                    o->numbers.assign(sp - t.numSlots, sp);
                    sp -= t.numSlots;
                    for (uint32_t i = 0; i < t.numRefs; ++i) o->refs.push_back(rsp[-static_cast<ptrdiff_t>(t.numRefs) + i]);
                    rsp -= t.numRefs;
                } else {
                    o->numbers.assign(t.numSlots, 0);
                    for (uint32_t i = 0; i < t.numRefs; ++i) o->refs.push_back(Value::null());
                }
                *rsp++ = Value(static_cast<Object*>(o));
                pc += 1;
                break;
            }
            case Op::kFbBase + 0x02: case Op::kFbBase + 0x03: case Op::kFbBase + 0x04: {  // struct.get, _s, _u
                const Value ref = *--rsp;
                if (ref.is_null()) TRAP(TrapKind::NullReference);
                StructObject* o = DOMObject::Cast<StructObject>(ref);
                const uint32_t slot = pc[0], flags = pc[1];
                if (flags & 1) {
                    *rsp++ = o->refs[slot];
                } else if (flags & 16) {
                    sp[0] = o->numbers[slot];
                    sp[1] = o->numbers[slot + 1];
                    sp += 2;
                } else {
                    uint64_t v = o->numbers[slot];
                    if (flags & 2) v = (flags & 8) ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v))) : (v & 0xFF);
                    else if (flags & 4) v = (flags & 8) ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))) : (v & 0xFFFF);
                    *sp++ = v;
                }
                pc += 2;
                break;
            }
            case Op::kFbBase + 0x05: {  // struct.set
                const uint32_t slot = pc[0], flags = pc[1];
                if (flags & 1) {
                    const Value v = *--rsp;
                    const Value ref = *--rsp;
                    if (ref.is_null()) TRAP(TrapKind::NullReference);
                    DOMObject::Cast<StructObject>(ref)->refs.set(slot, v);
                } else if (flags & 16) {
                    sp -= 2;
                    const uint64_t lo = sp[0], hi = sp[1];
                    const Value ref = *--rsp;
                    if (ref.is_null()) TRAP(TrapKind::NullReference);
                    StructObject* o = DOMObject::Cast<StructObject>(ref);
                    o->numbers[slot] = lo;
                    o->numbers[slot + 1] = hi;
                } else {
                    uint64_t v = *--sp;
                    if (flags & 2) v &= 0xFF;
                    else if (flags & 4) v &= 0xFFFF;
                    const Value ref = *--rsp;
                    if (ref.is_null()) TRAP(TrapKind::NullReference);
                    DOMObject::Cast<StructObject>(ref)->numbers[slot] = v;
                }
                pc += 2;
                break;
            }
            case Op::kFbBase + 0x06: case Op::kFbBase + 0x07: case Op::kFbBase + 0x08: case Op::kFbBase + 0x09: case Op::kFbBase + 0x0A: {
                // array.new, _default, _fixed, _data, _elem
                const CanonType& t = TypeRegistry::Get(pc[0]);
                const uint32_t eb = t.elemBytes;
                uint64_t length = 0;
                if (op == Op::kFbBase + 0x08) length = pc[1];
                else if (op == Op::kFbBase + 0x06 || op == Op::kFbBase + 0x07) length = static_cast<uint32_t>(*--sp);
                else length = static_cast<uint32_t>(sp[-1]);
                if (length * (t.elemIsRef ? 8 : eb) > kMaxArrayBytes) TRAP(TrapKind::OutOfMemory);
                ArrayObject* o = ::Quanta::Heap::Allocate<ArrayObject>();
                o->initialize_prototype(nullptr);
                o->prevent_extensions();
                o->type = pc[0];
                o->length = static_cast<uint32_t>(length);
                if (!t.elemIsRef) o->bytes.assign(static_cast<size_t>(length) * eb, 0);
                if (op == Op::kFbBase + 0x06) {
                    if (t.elemIsRef) {
                        const Value v = *--rsp;
                        for (uint64_t i = 0; i < length; ++i) o->refs.push_back(v);
                    } else if (eb == 16) {
                        sp -= 2;
                        for (uint64_t i = 0; i < length; ++i) { std::memcpy(&o->bytes[i * 16], &sp[0], 8); std::memcpy(&o->bytes[i * 16 + 8], &sp[1], 8); }
                    } else {
                        const uint64_t v = *--sp;
                        for (uint64_t i = 0; i < length; ++i) StoreElement(&o->bytes[static_cast<size_t>(i) * eb], eb, v);
                    }
                } else if (op == Op::kFbBase + 0x07) {
                    if (t.elemIsRef) for (uint64_t i = 0; i < length; ++i) o->refs.push_back(Value::null());
                } else if (op == Op::kFbBase + 0x08) {
                    if (t.elemIsRef) {
                        rsp -= length;
                        for (uint64_t i = 0; i < length; ++i) o->refs.push_back(rsp[i]);
                    } else if (eb == 16) {
                        sp -= length * 2;
                        for (uint64_t i = 0; i < length; ++i) { std::memcpy(&o->bytes[i * 16], &sp[i * 2], 8); std::memcpy(&o->bytes[i * 16 + 8], &sp[i * 2 + 1], 8); }
                    } else {
                        sp -= length;
                        for (uint64_t i = 0; i < length; ++i) StoreElement(&o->bytes[static_cast<size_t>(i) * eb], eb, sp[i]);
                    }
                } else {
                    // from a segment: [offset, length] on the stack
                    const uint64_t offset = static_cast<uint32_t>(sp[-2]);
                    sp -= 2;
                    if (op == Op::kFbBase + 0x09) {
                        const DataSegment& d = inst->module->datas[pc[1]];
                        const uint64_t size = inst->dataDropped[pc[1]] ? 0 : d.bytes.size;
                        if (offset > size || length * eb > size - offset) TRAP(TrapKind::MemoryOutOfBounds);
                        if (length) std::memcpy(o->bytes.data(), inst->module->bytes.data() + d.bytes.offset + offset, static_cast<size_t>(length) * eb);
                    } else {
                        const std::vector<Value>& items = inst->elems[pc[1]];
                        if (offset > items.size() || length > items.size() - offset) TRAP(TrapKind::TableOutOfBounds);
                        for (uint64_t i = 0; i < length; ++i) o->refs.push_back(items[static_cast<size_t>(offset + i)]);
                    }
                }
                *rsp++ = Value(static_cast<Object*>(o));
                pc += (op == Op::kFbBase + 0x06 || op == Op::kFbBase + 0x07) ? 1 : 2;
                break;
            }
            case Op::kFbBase + 0x0B: case Op::kFbBase + 0x0C: case Op::kFbBase + 0x0D: {  // array.get, _s, _u
                const uint32_t info = pc[0];
                const uint64_t index = static_cast<uint32_t>(*--sp);
                const Value ref = *--rsp;
                if (ref.is_null()) TRAP(TrapKind::NullReference);
                ArrayObject* o = DOMObject::Cast<ArrayObject>(ref);
                if (index >= o->length) TRAP(TrapKind::ArrayOutOfBounds);
                if (info & 0x100) {
                    *rsp++ = o->refs[static_cast<size_t>(index)];
                } else if ((info & 0xFF) == 16) {
                    sp[0] = LoadElement(&o->bytes[index * 16], 8, false);
                    sp[1] = LoadElement(&o->bytes[index * 16 + 8], 8, false);
                    sp += 2;
                } else {
                    *sp++ = LoadElement(&o->bytes[static_cast<size_t>(index) * (info & 0xFF)], info & 0xFF, (info & 0x200) != 0);
                }
                pc += 1;
                break;
            }
            case Op::kFbBase + 0x0E: {  // array.set
                const uint32_t info = pc[0];
                if (info & 0x100) {
                    const Value v = *--rsp;
                    const uint64_t index = static_cast<uint32_t>(*--sp);
                    const Value ref = *--rsp;
                    if (ref.is_null()) TRAP(TrapKind::NullReference);
                    ArrayObject* o = DOMObject::Cast<ArrayObject>(ref);
                    if (index >= o->length) TRAP(TrapKind::ArrayOutOfBounds);
                    o->refs.set(static_cast<size_t>(index), v);
                } else if ((info & 0xFF) == 16) {
                    sp -= 2;
                    const uint64_t lo = sp[0], hi = sp[1];
                    const uint64_t index = static_cast<uint32_t>(*--sp);
                    const Value ref = *--rsp;
                    if (ref.is_null()) TRAP(TrapKind::NullReference);
                    ArrayObject* o = DOMObject::Cast<ArrayObject>(ref);
                    if (index >= o->length) TRAP(TrapKind::ArrayOutOfBounds);
                    std::memcpy(&o->bytes[index * 16], &lo, 8);
                    std::memcpy(&o->bytes[index * 16 + 8], &hi, 8);
                } else {
                    const uint64_t v = *--sp;
                    const uint64_t index = static_cast<uint32_t>(*--sp);
                    const Value ref = *--rsp;
                    if (ref.is_null()) TRAP(TrapKind::NullReference);
                    ArrayObject* o = DOMObject::Cast<ArrayObject>(ref);
                    if (index >= o->length) TRAP(TrapKind::ArrayOutOfBounds);
                    StoreElement(&o->bytes[static_cast<size_t>(index) * (info & 0xFF)], info & 0xFF, v);
                }
                pc += 1;
                break;
            }
            case Op::kFbBase + 0x0F: {  // array.len
                const Value ref = *--rsp;
                if (ref.is_null()) TRAP(TrapKind::NullReference);
                *sp++ = DOMObject::Cast<ArrayObject>(ref)->length;
                break;
            }
            case Op::kFbBase + 0x10: {  // array.fill
                const uint32_t info = pc[0];
                const uint64_t count = static_cast<uint32_t>(*--sp);
                Value fillRef;
                uint64_t fillLo = 0, fillHi = 0;
                if (info & 0x100) fillRef = *--rsp;
                else if ((info & 0xFF) == 16) { sp -= 2; fillLo = sp[0]; fillHi = sp[1]; }
                else fillLo = *--sp;
                const uint64_t offset = static_cast<uint32_t>(*--sp);
                const Value ref = *--rsp;
                if (ref.is_null()) TRAP(TrapKind::NullReference);
                ArrayObject* o = DOMObject::Cast<ArrayObject>(ref);
                if (offset > o->length || count > o->length - offset) TRAP(TrapKind::ArrayOutOfBounds);
                for (uint64_t i = 0; i < count; ++i) {
                    if (info & 0x100) o->refs.set(static_cast<size_t>(offset + i), fillRef);
                    else if ((info & 0xFF) == 16) { std::memcpy(&o->bytes[(offset + i) * 16], &fillLo, 8); std::memcpy(&o->bytes[(offset + i) * 16 + 8], &fillHi, 8); }
                    else StoreElement(&o->bytes[static_cast<size_t>(offset + i) * (info & 0xFF)], info & 0xFF, fillLo);
                }
                pc += 1;
                break;
            }
            case Op::kFbBase + 0x11: {  // array.copy
                const uint32_t dinfo = pc[0];
                const uint64_t count = static_cast<uint32_t>(*--sp);
                const uint64_t soff = static_cast<uint32_t>(*--sp);
                const Value sref = *--rsp;
                const uint64_t doff = static_cast<uint32_t>(*--sp);
                const Value dref = *--rsp;
                if (sref.is_null() || dref.is_null()) TRAP(TrapKind::NullReference);
                ArrayObject* d = DOMObject::Cast<ArrayObject>(dref);
                ArrayObject* sa = DOMObject::Cast<ArrayObject>(sref);
                if (doff > d->length || count > d->length - doff || soff > sa->length || count > sa->length - soff) TRAP(TrapKind::ArrayOutOfBounds);
                if (dinfo & 0x100) {
                    if (d == sa && doff > soff) { for (uint64_t i = count; i-- > 0;) d->refs.set(static_cast<size_t>(doff + i), sa->refs[static_cast<size_t>(soff + i)]); }
                    else { for (uint64_t i = 0; i < count; ++i) d->refs.set(static_cast<size_t>(doff + i), sa->refs[static_cast<size_t>(soff + i)]); }
                } else {
                    const uint32_t eb = dinfo & 0xFF;
                    if (count) std::memmove(&d->bytes[static_cast<size_t>(doff) * eb], &sa->bytes[static_cast<size_t>(soff) * eb], static_cast<size_t>(count) * eb);
                }
                pc += 2;
                break;
            }
            case Op::kFbBase + 0x12: case Op::kFbBase + 0x13: {  // array.init_data, array.init_elem
                const uint32_t info = pc[0], segment = pc[1];
                const uint64_t count = static_cast<uint32_t>(*--sp);
                const uint64_t soff = static_cast<uint32_t>(*--sp);
                const uint64_t doff = static_cast<uint32_t>(*--sp);
                const Value ref = *--rsp;
                if (ref.is_null()) TRAP(TrapKind::NullReference);
                ArrayObject* o = DOMObject::Cast<ArrayObject>(ref);
                if (doff > o->length || count > o->length - doff) TRAP(TrapKind::ArrayOutOfBounds);
                if (op == Op::kFbBase + 0x12) {
                    const uint32_t eb = info & 0xFF;
                    const DataSegment& d = inst->module->datas[segment];
                    const uint64_t size = inst->dataDropped[segment] ? 0 : d.bytes.size;
                    if (soff > size || count * eb > size - soff) TRAP(TrapKind::MemoryOutOfBounds);
                    if (count) std::memcpy(&o->bytes[static_cast<size_t>(doff) * eb], inst->module->bytes.data() + d.bytes.offset + soff, static_cast<size_t>(count) * eb);
                } else {
                    const std::vector<Value>& items = inst->elems[segment];
                    if (soff > items.size() || count > items.size() - soff) TRAP(TrapKind::TableOutOfBounds);
                    for (uint64_t i = 0; i < count; ++i) o->refs.set(static_cast<size_t>(doff + i), items[static_cast<size_t>(soff + i)]);
                }
                pc += 2;
                break;
            }
            case Op::kFbBase + 0x14: case Op::kFbBase + 0x15: {  // ref.test
                const Value v = *--rsp;
                *sp++ = RefMatches(v, static_cast<HeapType>(static_cast<int32_t>(pc[0])), pc[1] != 0) ? 1 : 0;
                pc += 2;
                break;
            }
            case Op::kFbBase + 0x16: case Op::kFbBase + 0x17: {  // ref.cast
                if (!RefMatches(rsp[-1], static_cast<HeapType>(static_cast<int32_t>(pc[0])), pc[1] != 0)) TRAP(TrapKind::CastFailure);
                pc += 2;
                break;
            }
            case Op::kFbBase + 0x18: case Op::kFbBase + 0x19: {  // br_on_cast, br_on_cast_fail
                const bool matches = RefMatches(rsp[-1], static_cast<HeapType>(static_cast<int32_t>(pc[5])), pc[6] != 0);
                const bool take = op == Op::kFbBase + 0x18 ? matches : !matches;
                if (!take) { pc += 7; break; }
                const uint32_t target = pc[0], keepN = pc[1], keepR = pc[2], dropN = pc[3], dropR = pc[4];
                if (dropN) { std::memmove(sp - keepN - dropN, sp - keepN, keepN * sizeof(uint64_t)); sp -= dropN; }
                if (dropR) { std::memmove(rsp - keepR - dropR, rsp - keepR, keepR * sizeof(Value)); rsp -= dropR; }
                pc = code->code.data() + target;
                break;
            }
            case Op::kFbBase + 0x1A: {  // any.convert_extern
                const Value v = rsp[-1];
                if (v.is_null()) break;
                if (v.is_number()) {
                    const double d = v.as_number();
                    if (d == std::trunc(d) && d >= -1073741824.0 && d <= 1073741823.0 && !(d == 0 && std::signbit(d))) break;  // (it is an i31 as it is)
                }
                if (DOMObject::Cast<StructObject>(v) || DOMObject::Cast<ArrayObject>(v)) break;
                HostRefObject* h = ::Quanta::Heap::Allocate<HostRefObject>();
                h->initialize_prototype(nullptr);
                h->prevent_extensions();
                h->value.Set(v);
                rsp[-1] = Value(static_cast<Object*>(h));
                break;
            }
            case Op::kFbBase + 0x1B: {  // extern.convert_any
                const Value v = rsp[-1];
                if (HostRefObject* h = v.is_null() ? nullptr : DOMObject::Cast<HostRefObject>(v)) rsp[-1] = h->value.Get();
                break;
            }
            case Op::kFbBase + 0x1C: rsp[0] = I31Value(static_cast<uint32_t>(*--sp)); ++rsp; break;  // ref.i31
            case Op::kFbBase + 0x1D: case Op::kFbBase + 0x1E: {  // i31.get_s, i31.get_u
                const Value v = *--rsp;
                if (v.is_null()) TRAP(TrapKind::NullReference);
                const int32_t i = static_cast<int32_t>(v.as_number());
                *sp++ = op == Op::kFbBase + 0x1D ? static_cast<uint32_t>(i) : (static_cast<uint32_t>(i) & 0x7FFFFFFFu);
                break;
            }
            // ---- 0xFD: the vector instructions that touch memory ----
            case Op::kFdBase + 0x00: {  // v128.load
                MemoryObject* m = memoryAt(pc[0]);
                uint8_t* p;
                EFFECTIVE(m, sp[-1], 16, p);
                std::memcpy(&sp[-1], p, 8);
                std::memcpy(&sp[0], p + 8, 8);
                ++sp;
                pc += 3;
                break;
            }
            case Op::kFdBase + 0x01: case Op::kFdBase + 0x02: case Op::kFdBase + 0x03: case Op::kFdBase + 0x04: case Op::kFdBase + 0x05: case Op::kFdBase + 0x06: {
                MemoryObject* m = memoryAt(pc[0]);
                uint8_t* p;
                EFFECTIVE(m, sp[-1], 8, p);
                uint8_t src[8], out[16];
                std::memcpy(src, p, 8);
                const bool sign = !(op & 1) ? false : false;
                (void)sign;
                const uint32_t kind = (op - Op::kFdBase - 0x01) / 2;  // 0: 8x8, 1: 16x4, 2: 32x2
                const bool isSigned = ((op - Op::kFdBase) & 1) == 1;
                if (kind == 0) {
                    for (int i = 0; i < 8; ++i) { const int16_t v = isSigned ? static_cast<int16_t>(static_cast<int8_t>(src[i])) : static_cast<int16_t>(src[i]); std::memcpy(out + i * 2, &v, 2); }
                } else if (kind == 1) {
                    for (int i = 0; i < 4; ++i) { int16_t h; std::memcpy(&h, src + i * 2, 2); const int32_t v = isSigned ? static_cast<int32_t>(h) : static_cast<int32_t>(static_cast<uint16_t>(h)); std::memcpy(out + i * 4, &v, 4); }
                } else {
                    for (int i = 0; i < 2; ++i) { int32_t w; std::memcpy(&w, src + i * 4, 4); const int64_t v = isSigned ? static_cast<int64_t>(w) : static_cast<int64_t>(static_cast<uint32_t>(w)); std::memcpy(out + i * 8, &v, 8); }
                }
                std::memcpy(&sp[-1], out, 8);
                std::memcpy(&sp[0], out + 8, 8);
                ++sp;
                pc += 3;
                break;
            }
            case Op::kFdBase + 0x07: case Op::kFdBase + 0x08: case Op::kFdBase + 0x09: case Op::kFdBase + 0x0A: {  // load splat
                MemoryObject* m = memoryAt(pc[0]);
                const uint32_t size = 1u << (op - Op::kFdBase - 0x07);
                uint8_t* p;
                EFFECTIVE(m, sp[-1], size, p);
                uint8_t out[16];
                for (uint32_t i = 0; i < 16; i += size) std::memcpy(out + i, p, size);
                std::memcpy(&sp[-1], out, 8);
                std::memcpy(&sp[0], out + 8, 8);
                ++sp;
                pc += 3;
                break;
            }
            case Op::kFdBase + 0x5C: case Op::kFdBase + 0x5D: {  // load32_zero, load64_zero
                MemoryObject* m = memoryAt(pc[0]);
                const uint32_t size = op == Op::kFdBase + 0x5C ? 4 : 8;
                uint8_t* p;
                EFFECTIVE(m, sp[-1], size, p);
                uint64_t lo = 0;
                std::memcpy(&lo, p, size);
                sp[-1] = lo;
                sp[0] = 0;
                ++sp;
                pc += 3;
                break;
            }
            case Op::kFdBase + 0x0B: {  // v128.store
                MemoryObject* m = memoryAt(pc[0]);
                sp -= 2;
                const uint64_t lo = sp[0], hi = sp[1];
                uint8_t* p;
                EFFECTIVE(m, sp[-1], 16, p);
                std::memcpy(p, &lo, 8);
                std::memcpy(p + 8, &hi, 8);
                --sp;
                pc += 3;
                break;
            }
            case Op::kFdBase + 0x54: case Op::kFdBase + 0x55: case Op::kFdBase + 0x56: case Op::kFdBase + 0x57: {  // load lane
                MemoryObject* m = memoryAt(pc[0]);
                const uint32_t size = 1u << (op - Op::kFdBase - 0x54);
                const uint32_t lane = pc[3];
                sp -= 2;
                uint8_t vector[16];
                std::memcpy(vector, &sp[0], 8);
                std::memcpy(vector + 8, &sp[1], 8);
                uint8_t* p;
                EFFECTIVE(m, sp[-1], size, p);
                std::memcpy(vector + lane * size, p, size);
                std::memcpy(&sp[-1], vector, 8);
                std::memcpy(&sp[0], vector + 8, 8);
                ++sp;
                pc += 4;
                break;
            }
            case Op::kFdBase + 0x58: case Op::kFdBase + 0x59: case Op::kFdBase + 0x5A: case Op::kFdBase + 0x5B: {  // store lane
                MemoryObject* m = memoryAt(pc[0]);
                const uint32_t size = 1u << (op - Op::kFdBase - 0x58);
                const uint32_t lane = pc[3];
                sp -= 2;
                uint8_t vector[16];
                std::memcpy(vector, &sp[0], 8);
                std::memcpy(vector + 8, &sp[1], 8);
                uint8_t* p;
                EFFECTIVE(m, sp[-1], size, p);
                std::memcpy(p, vector + lane * size, size);
                --sp;
                pc += 4;
                break;
            }
            default:
                // The loads and stores with a constant in them (see Code.h) are not cases of the switch: more cases make the compiler lay the whole of it out
                // worse for every instruction. They are told by their numbers here.
                if ((op >= Op::kAbs + 0x28 && op <= Op::kAbs + 0x3E) || (op >= Op::kImm + 0x36 && op <= Op::kImm + 0x3E) || (op >= Op::kImmAbs + 0x28 && op <= Op::kImmAbs + 0x3E)) {
                    const bool absolute = op >= Op::kImmAbs + 0x28 || (op >= Op::kAbs + 0x28 && op <= Op::kAbs + 0x3E);
                    const bool immediate = op >= Op::kImmAbs + 0x28 || (op >= Op::kImm + 0x36 && op <= Op::kImm + 0x3E);
                    const uint32_t base = op >= Op::kImmAbs + 0x28 ? op - Op::kImmAbs : op >= Op::kAbs + 0x28 && op <= Op::kAbs + 0x3E ? op - Op::kAbs : op - Op::kImm;
                    static const uint8_t kSize[] = {4, 8, 4, 8, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4, 4, 8, 4, 8, 1, 2, 1, 2, 4};  // 0x28 to 0x3E
                    const uint32_t size = kSize[base - 0x28];
                    MemoryObject* m = memoryAt(pc[2]);
                    uint8_t* p;
                    EFFECTIVE_AT(m, absolute ? uint64_t(0) : fp[pc[1]], size, p, pc[3], pc[4]);
                    if (base < 0x36) {
                        uint64_t v = 0;
                        switch (base) {
                            case 0x28: case 0x2A: { uint32_t x; std::memcpy(&x, p, 4); v = x; break; }
                            case 0x29: case 0x2B: std::memcpy(&v, p, 8); break;
                            case 0x2C: v = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(*p))); break;
                            case 0x2D: v = *p; break;
                            case 0x2E: { int16_t x; std::memcpy(&x, p, 2); v = static_cast<uint32_t>(static_cast<int32_t>(x)); break; }
                            case 0x2F: { uint16_t x; std::memcpy(&x, p, 2); v = x; break; }
                            case 0x30: v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(*p))); break;
                            case 0x31: v = *p; break;
                            case 0x32: { int16_t x; std::memcpy(&x, p, 2); v = static_cast<uint64_t>(static_cast<int64_t>(x)); break; }
                            case 0x33: { uint16_t x; std::memcpy(&x, p, 2); v = x; break; }
                            case 0x34: { int32_t x; std::memcpy(&x, p, 4); v = static_cast<uint64_t>(static_cast<int64_t>(x)); break; }
                            default: { uint32_t x; std::memcpy(&x, p, 4); v = x; break; }  // 0x35
                        }
                        fp[pc[0]] = v;
                    } else {
                        const uint64_t v = immediate ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(pc[0]))) : fp[pc[0]];
                        std::memcpy(p, &v, size);
                    }
                    pc += 5;
                    break;
                }
                if (op >= Op::kFdBase && op < Op::kFdBase + 0x120) {
                    const uint32_t sub = op - Op::kFdBase;
                    sp = SimdExec(sub, sp, pc);
                    pc += SimdImmediateWords(sub);
                    break;
                }
                std::fprintf(stderr, "wasm: internal opcode %u is not implemented\n", op);
                TRAP(TrapKind::Unreachable);
        }
    }
raise:
    {
        // The exception is looked for a handler in the function it was thrown in, and then in the ones that called it.
        ExceptionObject* e = DOMObject::Cast<ExceptionObject>(exception);
        for (;;) {
            const uint32_t position = static_cast<uint32_t>(pc - code->code.data());
            for (size_t i = code->tries.size(); i-- > 0;) {
                const TryEntry& entry = code->tries[i];
                if (!(entry.start < position && position <= entry.end)) continue;
                for (const CatchClause& clause : entry.catches) {
                    if (clause.kind <= 1 && DOMObject::Cast<TagObject>(e->tag.Get()) != inst->tags[clause.tag]) continue;
                    uint64_t* opN = fp + code->paramsN + code->localsN;
                    Value* opR = fr + code->paramsR + code->localsR;
                    sp = opN + entry.baseN;
                    rsp = opR + entry.baseR;
                    uint32_t keepN = 0, keepR = 0;
                    if (clause.kind <= 1) {
                        for (uint64_t v : e->numbers) *sp++ = v;
                        for (size_t k = 0; k < e->refs.size(); ++k) *rsp++ = e->refs[k];
                        keepN = static_cast<uint32_t>(e->numbers.size());
                        keepR = static_cast<uint32_t>(e->refs.size());
                    }
                    if (clause.kind & 1) { *rsp++ = exception; ++keepR; }
                    if (clause.dropN) { std::memmove(sp - keepN - clause.dropN, sp - keepN, keepN * sizeof(uint64_t)); sp -= clause.dropN; }
                    if (clause.dropR) { std::memmove(rsp - keepR - clause.dropR, rsp - keepR, keepR * sizeof(Value)); rsp -= clause.dropR; }
                    pc = code->code.data() + clause.target;
                    goto run;
                }
            }
            if (s.frames.size() == entryDepth) break;
            const Frame f = s.frames.back();
            s.frames.pop_back();
            code = f.code;
            inst = f.instance;
            pc = f.pc;
            fp = f.fp;
            fr = f.fr;
        }
        // Nothing in the function takes it: it is the caller's, in whatever it is that called.
        ThrowException(ctx, e);
        status = Machine::Status::Exception;
    }
stop:
    // Whatever frames the function pushed and did not get to pop are gone.
    s.frames.resize(entryDepth);
    return status;
}

}  // namespace Quanta::Wasm
