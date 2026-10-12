// -*- C++ -*-
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Baseline JIT. Included by Interpreter.cpp (inside namespace Quanta::VM, after
// the handler table) because it is built on the interpreter's own Frame and
// handlers rather than beside them.
//
// What it is: a template compiler. Every bytecode instruction becomes a fixed
// run of x86-64 and nothing is optimised across instructions. The register
// file stays in Frame::regs (memory), and only the accumulator lives in a
// machine register (r13), so there is nothing to reconcile with the
// interpreter when the two hand a frame back and forth: the Frame *is* the
// state, in both directions.
//
// The design rule that keeps it small and safe: the JIT never re-implements a
// semantic it is not sure about. An instruction it does not translate (most of
// them) is *stepped* -- run by the interpreter's own handler, on the very same
// Frame, for exactly that one instruction (see jit_step). An instruction it
// does translate handles only the case it can prove inline (two finite
// doubles, an Ordinary object whose shape matches the site's cache, ...), and
// anything else branches to the same step, so a surprise is the interpreter's
// answer, never a different one.
//
// Memory safety, in the order the generated code could break it:
//  * every register / constant / feedback / jump operand is range-checked
//    while compiling; a chunk that fails any check is refused outright;
//  * generated code only reads and writes Frame, the register bank, the
//    chunk's constant pool, its own JitCode, and objects through checks the
//    interpreter's inline-cache handlers make as well;
//  * C++ exceptions never unwind through generated code (it has no unwind
//    info): helpers catch everything and the trampoline rethrows after the
//    native frame is gone;
//  * the GC is conservative and non-moving, so values in generated code are
//    found on the C stack like any other -- and the accumulator is spilled to
//    the Frame before every call out;
//  * code pages are written once and then flipped to read+execute.

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
#define QUANTA_JIT_X64 1
#endif

#ifdef QUANTA_JIT_X64

namespace {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
constexpr size_t kOffRegs = offsetof(Frame, regs);
constexpr size_t kOffAcc = offsetof(Frame, acc);
constexpr size_t kOffPc = offsetof(Frame, pc);
constexpr size_t kOffThisValue = offsetof(Frame, this_value);
constexpr size_t kOffThisResolved = offsetof(Frame, this_resolved);
constexpr size_t kOffCtx = offsetof(Frame, ctx);
constexpr size_t kOffLookupCache = offsetof(Frame, lookup_cache_data);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}  // namespace

namespace jit {

using namespace x64;

// ---------------------------------------------------------------------------
// Value encoding. These are asserted against the real Value class once at
// start-up (jit_selfcheck); if the NaN-boxing ever changes, the JIT switches
// itself off rather than generate code for an encoding it does not know.
// ---------------------------------------------------------------------------
constexpr uint64_t kTagShift = 48;
constexpr uint16_t kTopUndefined = 0x7FF0, kTopNull = 0x7FF1, kTopFalse = 0x7FF2, kTopTrue = 0x7FF3,
                   kTopString = 0x7FF4, kTopSymbol = 0x7FF5, kTopBigInt = 0x7FF6, kTopObject = 0x7FF7,
                   kTopNaN = 0x7FF8, kTopFunction = 0x7FF9;
constexpr uint64_t kFalseBits = static_cast<uint64_t>(kTopFalse) << kTagShift;
constexpr uint64_t kTrueBits = static_cast<uint64_t>(kTopTrue) << kTagShift;
constexpr uint64_t kUndefinedBits = static_cast<uint64_t>(kTopUndefined) << kTagShift;
constexpr uint64_t kNullBits = static_cast<uint64_t>(kTopNull) << kTagShift;
constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFull;

inline uint64_t bits_of(const Value& v) { return std::bit_cast<uint64_t>(v); }
inline uint64_t bits_of_double(double d) { return std::bit_cast<uint64_t>(d); }

constexpr uint8_t kHaltOp = 0xFF;   // an opcode value no instruction uses; see h_JitHalt
constexpr uint32_t kNoOperand = 0xFFFFFFFFu;
constexpr int32_t kStepFinished = -1;
constexpr int32_t kStepRethrow = -2;
constexpr int32_t kStepBail = -3;
constexpr uint64_t kExitFinished = 0, kExitRethrow = 1, kExitBail = 2;

struct Config {
    bool enabled = false;
    bool log = false;
    bool stats = false;
    bool trace = false;
    bool debug = false;           // stats || trace: the one flag the hot helpers test
    int32_t safepoint_reload = 64;
    uint32_t mode = 1;             // 1: functions with loops only (default), 0: any hot function
    uint32_t threshold = 6;        // calls before a loop-free function is compiled
    const char* dump_dir = nullptr;
    size_t mapped_bytes = 0;
    size_t max_mapped_bytes = 192u << 20;
    uint32_t compiled = 0, refused = 0;
    // Debug aids: compile only the functions numbered [skip, skip+limit) in the order they get hot.
    uint32_t limit = 0xFFFFFFFFu, skip = 0, seen = 0;
    uint64_t steps[256] = {};
    uint64_t step_cycles[256] = {};   // inclusive of anything the instruction runs
    std::unordered_map<uint64_t, std::pair<const BytecodeChunk*, uint32_t>> sites;
    std::unordered_map<uint64_t, uint64_t> site_counts;
    uint64_t step_total = 0;
    // Layout of the one bitfield the generated code tests.
    bool this_needs_super_inline = false;
    size_t this_needs_super_off = 0;
    uint8_t this_needs_super_mask = 0;
};
Config g_cfg;
thread_local char g_thread_marker;

inline void note_site(const BytecodeChunk& chunk, uint32_t pc) {
    const uint64_t key = (reinterpret_cast<uint64_t>(&chunk) << 16) ^ (static_cast<uint64_t>(pc) * 0x9E3779B97F4A7C15ull);
    g_cfg.sites.emplace(key, std::make_pair(&chunk, pc));
    g_cfg.site_counts[key]++;
}

void print_stats() {
    if (!g_cfg.stats && !g_cfg.log) return;
    std::fprintf(stderr, "[jit] compiled=%u refused=%u steps=%llu\n", g_cfg.compiled, g_cfg.refused,
                 static_cast<unsigned long long>(g_cfg.step_total));
    if (g_cfg.stats) {
        // Top stepped opcodes: the list of what to translate next.
        for (int round = 0; round < 25; round++) {
            int best = -1;
            for (int i = 0; i < 256; i++) if (g_cfg.steps[i] && (best < 0 || g_cfg.steps[i] > g_cfg.steps[best])) best = i;
            if (best < 0) break;
            const char* nm = best < static_cast<int>(Op::kCount) ? op_name(static_cast<Op>(best)) : "?";
            std::fprintf(stderr, "[jit]   step op %3d %-24s count=%llu cycles/step=%llu\n", best, nm,
                         static_cast<unsigned long long>(g_cfg.steps[best]),
                         static_cast<unsigned long long>(g_cfg.step_cycles[best] / g_cfg.steps[best]));
            g_cfg.steps[best] = 0;
        }
        // The hottest individual sites.
        for (int round = 0; round < 25; round++) {
            uint64_t best_key = 0, best = 0;
            for (auto& kv : g_cfg.site_counts) if (kv.second > best) { best = kv.second; best_key = kv.first; }
            if (!best) break;
            auto [chunk, pc] = g_cfg.sites[best_key];
            const Op op = static_cast<Op>(chunk->code[pc]);
            std::string detail;
            const int n = op_operand_bytes(op);
            const char kind = op_operand_kind(op);
            if ((kind == 'g' || kind == 'G') && n >= 4) detail = chunk->name_at(read_u16(chunk->code.data(), pc + 2));
            else if (kind == 'n' && n >= 2) detail = chunk->name_at(read_u16(chunk->code.data(), pc + 1));
            else if (kind == 'N' && n >= 2) detail = chunk->name_at(read_u16(chunk->code.data(), pc + 1));
            std::fprintf(stderr, "[jit]   site %p pc=%u %-18s %-24s count=%llu\n", static_cast<const void*>(chunk), pc,
                         op_name(op), detail.c_str(), static_cast<unsigned long long>(best));
            g_cfg.site_counts[best_key] = 0;
        }
    }
}

bool selfcheck() {
    // Tags the generated code hard-codes.
    if (bits_of(Value()) != kUndefinedBits) return false;
    if (bits_of(Value::null()) != kNullBits) return false;
    if (bits_of(Value(false)) != kFalseBits || bits_of(Value(true)) != kTrueBits) return false;
    if ((bits_of(Value::nan()) >> kTagShift) != kTopNaN) return false;
    if (bits_of(Value(1.0)) != bits_of_double(1.0) || bits_of(Value(-0.0)) != bits_of_double(-0.0)) return false;
    Function* fake_fn = reinterpret_cast<Function*>(static_cast<uintptr_t>(0x1230));
    if (bits_of(Value(fake_fn)) != ((static_cast<uint64_t>(kTopFunction) << kTagShift) | 0x1230)) return false;
    if (!std::is_trivially_copyable_v<Value> || sizeof(Value) != 8) return false;
    if (static_cast<int>(Op::kCount) >= kHaltOp) return false;
    if (kHaltOp != 0xFF) return false;
    if (kHandlers[kHaltOp] != &h_JitHalt) return false;
    // The object tag is private to Value; check it through a real round trip
    // on a value the engine itself builds.
    {
        Object* probe = reinterpret_cast<Object*>(static_cast<uintptr_t>(0x4560));
        // Value(Object*) may inspect the object, so go through the tag the
        // public predicates expose instead: a value with the object tag must be
        // is_object() and nothing else.
        uint64_t b = (static_cast<uint64_t>(kTopObject) << kTagShift) | reinterpret_cast<uintptr_t>(probe);
        Value v = std::bit_cast<Value>(b);
        if (!v.is_object() || v.is_function() || v.is_string() || v.as_object() != probe) return false;
        uint64_t fb = (static_cast<uint64_t>(kTopFunction) << kTagShift) | 0x1230;
        if (!std::bit_cast<Value>(fb).is_function()) return false;
        uint64_t sb = (static_cast<uint64_t>(kTopString) << kTagShift) | 0x1230;
        if (!std::bit_cast<Value>(sb).is_string()) return false;
    }
    // Layout of Context's this_needs_super_ bitfield: set it on a zeroed
    // buffer and see which single bit moved.
    {
        alignas(Context) unsigned char buf[sizeof(Context)];
        std::memset(buf, 0, sizeof(buf));
        Context* c = reinterpret_cast<Context*>(buf);
        c->set_this_needs_super(true);
        size_t where = 0; int changed = 0; uint8_t mask = 0;
        for (size_t i = 0; i < sizeof(buf); i++) if (buf[i]) { changed++; where = i; mask = buf[i]; }
        if (changed == 1 && mask != 0 && (mask & (mask - 1)) == 0) {
            g_cfg.this_needs_super_inline = true;
            g_cfg.this_needs_super_off = where;
            g_cfg.this_needs_super_mask = mask;
        }
    }
    return true;
}

void init_once() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* e = std::getenv("QUANTA_JIT");
    g_cfg.enabled = !(e && (e[0] == '0'));
    if (const char* l = std::getenv("QUANTA_JIT_LOG")) g_cfg.log = l[0] && l[0] != '0';
    if (const char* s = std::getenv("QUANTA_JIT_TRACE")) g_cfg.trace = s[0] && s[0] != '0';
    if (const char* s = std::getenv("QUANTA_JIT_STATS")) g_cfg.stats = s[0] && s[0] != '0';
    if (const char* t = std::getenv("QUANTA_JIT_MODE")) g_cfg.mode = static_cast<uint32_t>(std::atoi(t));
    if (const char* t = std::getenv("QUANTA_JIT_THRESHOLD")) g_cfg.threshold = static_cast<uint32_t>(std::atoi(t));
    if (const char* d = std::getenv("QUANTA_JIT_DUMP")) g_cfg.dump_dir = d;
    if (const char* m = std::getenv("QUANTA_JIT_LIMIT")) g_cfg.limit = static_cast<uint32_t>(std::atoi(m));
    if (const char* m = std::getenv("QUANTA_JIT_SKIP")) g_cfg.skip = static_cast<uint32_t>(std::atoi(m));
    g_cfg.debug = g_cfg.stats || g_cfg.trace;
    // A collection at every safepoint is what QUANTA_GC_STRESS asks for, so the
    // back-edge countdown must not defer one.
    if (std::getenv("QUANTA_GC_STRESS")) g_cfg.safepoint_reload = 1;
    if (g_cfg.enabled && !selfcheck()) {
        g_cfg.enabled = false;
        if (g_cfg.log) std::fprintf(stderr, "[jit] disabled: self-check failed\n");
    }
}

// ---------------------------------------------------------------------------
// Runtime helpers called from generated code. All noexcept: nothing may unwind
// through a native frame.
// ---------------------------------------------------------------------------

// Learns the inline caches of a GetNamed/SetNamed site from what the
// interpreter has learned in the chunk's feedback slot, which is the only thing
// they trust: the own entries (data slots, as the interpreter's fast handlers
// try them) and, for a read, the inherited ones under the conditions
// h_GetNamedFast checks. `block` is the site's first entry in JitCode::ics.
// Cheap when nothing changed: the feedback's shape counts are the signature.
void learn_ic(Frame* f, JitCode* jc, uint32_t pc, uint32_t block) noexcept {
    if (block + JitIC::kWays > jc->ics.size()) return;
    const uint32_t fb_index = read_u16(jc->code_copy.data(), pc + 4);
    if (fb_index >= f->chunk.feedback.size()) return;
    const FeedbackBody* fb = f->chunk.feedback[fb_index].peek();
    if (!fb) return;
    JitIC* ics = &jc->ics[block];
    const uint32_t sig = 0x80000000u | fb->count | (static_cast<uint32_t>(fb->proto_count) << 8) |
                         (static_cast<uint32_t>(fb->mega) << 16) | (static_cast<uint32_t>(fb->proto_mega) << 17) |
                         (static_cast<uint32_t>(f->feedback_rooted) << 18);
    if (ics[0].signature == sig) return;
    const bool is_set = static_cast<Op>(f->chunk.code[pc]) == Op::SetNamed;
    JitIC fresh[JitIC::kWays];
    uint32_t n = 0;
    if (!fb->mega) {
        for (uint8_t k = 0; k < fb->count && n < JitIC::kWays; k++) {
            const FeedbackSlot::Entry& e = fb->entries[k];
            if (!e.shape || e.is_accessor) continue;
            fresh[n].shape_word = reinterpret_cast<uint64_t>(e.shape) | JitObjectLayout::kOrdinaryType;
            fresh[n].slot = e.slot_index;
            fresh[n].kind = JitIC::Own;
            n++;
        }
    }
    if (!is_set && f->feedback_rooted && fb->proto_count != 0 && !fb->proto_mega) {
        for (uint8_t k = 0; k < fb->proto_count && n < JitIC::kWays; k++) {
            const FeedbackSlot::ProtoEntry& pe = fb->proto_entries[k];
            if (!pe.receiver_shape || !pe.prototype || pe.absent || pe.is_getter || pe.is_setter ||
                pe.proto_epoch != Object::proto_epoch()) continue;
            JitIC& ic = fresh[n];
            ic.shape_word = reinterpret_cast<uint64_t>(pe.receiver_shape) | JitObjectLayout::kOrdinaryType;
            ic.proto_word = reinterpret_cast<uint64_t>(pe.prototype);
            ic.proto_epoch = pe.proto_epoch;
            if (pe.from_descriptor) {
                ic.kind = JitIC::ProtoValue;
                ic.value = pe.cached_value;
                ic.desc_epoch = pe.desc_epoch;
            } else if (pe.holder) {
                ic.kind = JitIC::ProtoSlot;
                ic.holder = pe.holder;
                ic.slot = pe.slot_index;
            } else {
                continue;
            }
            n++;
        }
    }
    for (uint32_t k = 0; k < JitIC::kWays; k++) ics[k] = fresh[k];   // unused entries stay Empty
    ics[0].signature = sig;
}

// Runs the one instruction at `pc` through the interpreter's own handler and
// returns to generated code at whichever instruction the handler stops before.
//
// How it stops: the interpreter's handlers tail-call straight into the next
// instruction, so the way to run exactly one is to make the next one a handler
// that returns. The JIT keeps a private copy of the bytecode (`code_copy`),
// identical to the chunk's, and for the duration of a step overwrites with
// kHaltOp the opcode byte of every place the instruction may continue at: the
// instruction after it, a branch target, and the chunk's exception handlers.
// h_JitHalt records where it stopped and returns. Frame::code points at the
// copy meanwhile; every other piece of state is the chunk's own.
//
// Stopping at *any* instruction boundary is a valid place to resume (the
// generated code has a label for each one), which is why it does not matter
// that a nested activation of the same chunk can run into a stop byte written
// by an outer step: it just stops there, early, and resumes natively.
//
// Returns the pc to continue at, or a kStep* code.
int32_t step_impl(Frame* f, uint32_t pc, uint32_t len, uint32_t target, uint32_t ic_index) noexcept {
    JitCode* jc = f->chunk.jit.get();
    const uint32_t size = jc->code_size;
    if (__builtin_expect(g_cfg.debug, 0)) {
        if (g_cfg.stats) { g_cfg.steps[f->chunk.code[pc]]++; g_cfg.step_total++; note_site(f->chunk, pc); }
        if (g_cfg.trace) std::fprintf(stderr, "[jit] step pc=%u op=%u\n", pc, f->chunk.code[pc]);
    }

    const uint8_t op = f->chunk.code[pc];
    const uint8_t* saved_code = f->code;
    f->jit_halted = false;
    Value result;
    bool threw = false;

    if (target == kNoOperand && !jc->has_handlers && len <= 15) {
        // Nothing can leave this instruction except by falling through (no branch,
        // and no handler table for an exception to be routed through), so the
        // interpreter can run it on a private copy of just its own bytes with the
        // stop byte behind them. Frame::code is offset so the handler's usual
        // `code[pc + n]` lands in the copy while `pc` stays the chunk's real one
        // (positions, stack traces and instr_pc all want that).
        uint8_t stub[16];
        const uint8_t* src = f->chunk.code.data() + pc;
        if (pc + 16 <= size) {                        // two fixed-size copies, not a call to memcpy
            uint64_t lo, hi;
            std::memcpy(&lo, src, 8);
            std::memcpy(&hi, src + 8, 8);
            std::memcpy(stub, &lo, 8);
            std::memcpy(stub + 8, &hi, 8);
        } else {
            std::memcpy(stub, src, len);
        }
        stub[len] = kHaltOp;
        f->code = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(stub) - pc);
        try {
            result = kHandlers[op](*f, pc, f->acc);
        } catch (...) {
            jc->pending = std::current_exception();
            threw = true;
        }
        f->code = saved_code;
    } else {
        uint8_t* code = jc->code_copy.data();
        struct Patch { uint32_t off; uint8_t orig; };
        constexpr uint32_t kMaxPatches = 12;
        Patch patches[kMaxPatches];
        uint32_t np = 0;
        auto patch = [&](uint32_t off) {
            if (off == pc || off >= size || np == kMaxPatches) return;   // [size] is a permanent stop byte
            for (uint32_t i = 0; i < np; i++) if (patches[i].off == off) return;
            patches[np++] = {off, code[off]};
            code[off] = kHaltOp;
        };
        patch(pc + len);
        if (target != kNoOperand) patch(target);
        for (uint32_t h : jc->handler_pcs) patch(h);
        // The instruction being stepped must read as itself even if an outer
        // activation of this same chunk (a recursive call, mid-step) has a stop
        // byte over it: handlers such as the fused ones read their own opcode back
        // from the code they run on.
        const uint8_t at_pc = code[pc];
        code[pc] = op;
        f->code = code;
        try {
            result = kHandlers[op](*f, pc, f->acc);
        } catch (...) {
            jc->pending = std::current_exception();
            threw = true;
        }
        f->code = saved_code;
        while (np > 0) { --np; code[patches[np].off] = patches[np].orig; }
        code[pc] = at_pc;
    }

    if (threw) return kStepRethrow;
    if (f->jit_halted) {
        f->jit_halted = false;
        const uint32_t npc = f->pc;
        if (npc >= size) { return kStepFinished; }   // fell off the end: cannot happen in well-formed bytecode
        if (ic_index != kNoOperand) learn_ic(f, jc, pc, ic_index);
        if (jc->native_off[npc] == 0) return kStepBail;   // not an instruction start: let the interpreter sort it out
        return static_cast<int32_t>(npc);
    }
    f->acc = result;
    return kStepFinished;
}

int32_t step(Frame* f, uint32_t pc, uint32_t len, uint32_t target, uint32_t ic_index) noexcept {
    if (__builtin_expect(g_cfg.stats, 0)) {
        const uint8_t op = f->chunk.code[pc];
        const uint64_t t0 = __builtin_ia32_rdtsc();
        const int32_t r = step_impl(f, pc, len, target, ic_index);
        g_cfg.step_cycles[op] += __builtin_ia32_rdtsc() - t0;
        return r;
    }
    return step_impl(f, pc, len, target, ic_index);
}

// The handler search CHECK_EXC does: the narrowest try region covering `pc`.
int32_t find_handler(const BytecodeChunk& chunk, uint32_t pc) noexcept {
    int32_t handler_pc = -1;
    uint32_t best_width = UINT32_MAX;
    if (chunk.handlers) for (const auto& h : *chunk.handlers) {
        if (pc >= h.start_pc && pc < h.end_pc) {
            const uint32_t width = h.end_pc - h.start_pc;
            if (width < best_width) { best_width = width; handler_pc = static_cast<int32_t>(h.handler_pc); }
        }
    }
    return handler_pc;
}

// What CHECK_EXC does after a call that may have left an exception pending, then
// the safepoint the call handlers end with. Returns the pc to continue at.
[[gnu::always_inline]] inline int32_t after_op(Frame* f, JitCode* jc, uint32_t pc, uint32_t next) noexcept {
    if (f->ctx->has_exception()) {
        const int32_t handler_pc = find_handler(f->chunk, pc);
        if (handler_pc < 0) { f->acc = Value(); return kStepFinished; }
        f->acc = f->ctx->get_exception();
        f->ctx->clear_exception();
        if (static_cast<uint32_t>(handler_pc) >= jc->code_size || jc->native_off[handler_pc] == 0) {
            f->pc = static_cast<uint32_t>(handler_pc);
            return kStepBail;
        }
        return handler_pc;
    }
    return static_cast<int32_t>(next);
}

[[gnu::always_inline]] inline int32_t after_call(Frame* f, JitCode* jc, uint32_t pc, uint32_t next) noexcept {
    const int32_t r = after_op(f, jc, pc, next);
    if (r == static_cast<int32_t>(next)) Collector::safepoint();
    return r;
}

// ---------------------------------------------------------------------------
// Fast helpers: the fast paths of the interpreter's handlers, for the cases
// the generated code does not inline. Each answers "handled" (1, with the
// result in Frame::acc where there is one) or "not mine" (0), and a 0 changes
// nothing, so the caller simply steps the instruction. They are plain
// functions of the same Frame the handlers use and never throw.
// ---------------------------------------------------------------------------

// GetNamed / GetNamedStar beyond the monomorphic own and inherited reads the
// generated code makes itself: the rest of h_GetNamedFast / h_GetNamedRest,
// minus the paths that call a getter (a call can throw and re-enter; the step
// does those).
int fast_get_named_impl(Frame* f, uint32_t pc, uint32_t ic_index) noexcept {
    if (__builtin_expect(g_cfg.debug && g_cfg.stats, 0)) {
        note_site(f->chunk, pc);
        if (g_cfg.trace && ic_index != kNoOperand && ic_index < f->chunk.jit->ics.size()) {
            const Value& rv = f->regs[f->chunk.code[pc + 1]];
            uint64_t shape_word = 0;
            if (rv.is_object()) shape_word = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const char*>(rv.as_object()) + JitObjectLayout::shape());
            const JitIC& ic = f->chunk.jit->ics[ic_index];
            static int budget = 40; static uint64_t calls = 0; calls++;
            if (calls > 3000000 && budget-- > 0) std::fprintf(stderr, "[jit] get_named miss pc=%u recv_obj=%d shape_word=%llx ic.kind=%u ic.shape=%llx\n", pc, rv.is_object(),
                (unsigned long long)shape_word, ic.kind, (unsigned long long)ic.shape_word);
        }
    }
    const uint8_t* code = f->chunk.code.data();
    const Value& receiver = f->regs[code[pc + 1]];
    const FeedbackBody& fb = f->chunk.feedback[read_u16(code, pc + 4)].read();
    if (fb.own_desc_receiver != nullptr && fb.own_desc_epoch == Object::descriptor_epoch() &&
        as_object_like(receiver) == fb.own_desc_receiver) {
        f->acc = fb.own_desc_value;
        return 1;
    }
    if (receiver.is_object()) {
        Object* obj = receiver.as_object();
        const auto type = obj->get_type();
        if (type == Object::ObjectType::Array) {
            if (fb.array_length && obj->has_only_dense_elements()) {
                f->acc = Value(static_cast<double>(obj->element_count()));
                return 1;
            }
            if (f->feedback_rooted && f->chunk.name_at(read_u16(code, pc + 2)) != "length" &&
                inherited_data_entry(fb, obj, f->acc)) {
                return 1;
            }
        } else if (type == Object::ObjectType::Map || type == Object::ObjectType::Set) {
            if (f->feedback_rooted && f->chunk.name_at(read_u16(code, pc + 2)) != "size" &&
                inherited_data_entry(fb, obj, f->acc)) {
                return 1;
            }
        } else if (shape_fast_path_ok(type)) {
            Shape* own_shape = obj->get_shape();
            for (uint8_t k = 0; k < fb.count; k++) {
                const FeedbackSlot::Entry& cand = fb.entries[k];
                if (cand.shape != own_shape) continue;
                if (obj->has_any_descriptor_override()) break;
                if (const Value* slot = obj->get_shape_slot_unchecked(cand.slot_index)) {
                    if (!cand.is_accessor) { f->acc = *slot; return 1; }
                }
                break;
            }
            if (f->feedback_rooted && !fb.proto_mega && fb.proto_count > 0 && !obj->has_any_descriptor_override()) {
                Shape* rs = obj->get_shape();
                Object* p0 = obj->get_prototype_raw();
                const uint64_t pep = Object::proto_epoch();
                for (uint8_t k = 0; k < fb.proto_count; k++) {
                    const FeedbackSlot::ProtoEntry& pe = fb.proto_entries[k];
                    if (pe.receiver_shape == rs && pe.prototype == p0 && pe.proto_epoch == pep) {
                        if (pe.absent) { f->acc = Value(); return 1; }
                        if (pe.is_getter) return 0;
                        if (pe.from_descriptor) {
                            if (pe.desc_epoch == Object::descriptor_epoch()) { f->acc = pe.cached_value; return 1; }
                        } else if (const Value* hs = pe.holder->get_shape_slot_unchecked(pe.slot_index)) {
                            f->acc = *hs;
                            return 1;
                        }
                        return 0;
                    }
                }
            }
        }
    } else if (receiver.is_function()) {
        Object* obj = receiver.as_object();
        if (f->feedback_rooted && !fb.proto_mega && fb.proto_count > 0 && !obj->has_any_descriptor_override() &&
            function_proto_read_cacheable(f->chunk.name_at(read_u16(code, pc + 2)))) {
            Shape* rs = obj->get_shape();
            Object* p0 = obj->get_prototype_raw();
            const uint64_t pep = Object::proto_epoch();
            for (uint8_t k = 0; k < fb.proto_count; k++) {
                const FeedbackSlot::ProtoEntry& pe = fb.proto_entries[k];
                if (pe.receiver_shape == rs && pe.prototype == p0 && pe.proto_epoch == pep) {
                    if (pe.absent) { f->acc = Value(); return 1; }
                    if (pe.is_getter) return 0;
                    if (pe.from_descriptor) {
                        if (pe.desc_epoch == Object::descriptor_epoch()) { f->acc = pe.cached_value; return 1; }
                    } else if (const Value* hs = pe.holder->get_shape_slot_unchecked(pe.slot_index)) {
                        f->acc = *hs;
                        return 1;
                    }
                    return 0;
                }
            }
        }
    } else {
        if (fb.prim_valid && !fb.prim_is_getter && fb.prim_desc_epoch == Object::descriptor_epoch()) {
            using PK = Context::PrimitiveKind;
            PK kind = receiver.is_string()  ? PK::String
                    : receiver.is_number()  ? PK::Number
                    : receiver.is_boolean() ? PK::Boolean
                    : receiver.is_bigint()  ? PK::BigInt
                    : receiver.is_symbol()  ? PK::Symbol
                                            : PK::Count;
            if (kind != PK::Count && Context::primitive_prototype(kind) == fb.prim_proto) {
                f->acc = fb.prim_value;
                return 1;
            }
        }
    }
    return 0;
}

int fast_get_named(Frame* f, uint32_t pc, uint32_t ic_index) noexcept {
    const int handled = fast_get_named_impl(f, pc, ic_index);
    // The interpreter has already learned this site (that is what let the helper
    // answer), so the generated code's own cache can copy it now rather than wait
    // for a step that will never come.
    if (handled && ic_index != kNoOperand) learn_ic(f, f->chunk.jit.get(), pc, ic_index);
    return handled;
}

// SetNamed beyond the monomorphic own-slot store: the other learned shapes and
// the new-property transition cache (h_SetNamedFast / h_SetNamedRest).
int fast_set_named_impl(Frame* f, uint32_t pc) noexcept {
    const uint8_t* code = f->chunk.code.data();
    const Value& receiver = f->regs[code[pc + 1]];
    const Value& acc = f->acc;
    if (!receiver.is_object()) return 0;
    Object* obj = receiver.as_object();
    if (!shape_fast_path_ok(obj->get_type())) return 0;
    const FeedbackBody& fb = f->chunk.feedback[read_u16(code, pc + 4)].read();
    Shape* own_shape = obj->get_shape();
    for (uint8_t k = 0; k < fb.count; k++) {
        const FeedbackSlot::Entry& e = fb.entries[k];
        if (e.shape != own_shape || e.is_accessor) continue;
        if (obj->has_any_descriptor_override()) break;
        if (Value* slot = obj->get_shape_slot_unchecked(e.slot_index)) {
            write_barrier_for(obj, acc);
            *slot = acc;
            return 1;
        }
        break;
    }
    if (!fb.transition_mega && own_shape && obj->is_extensible()) {
        Object* proto0 = obj->get_prototype_raw();
        const uint64_t epoch = Object::proto_epoch();
        for (uint8_t i = 0; i < fb.transition_count; i++) {
            const auto& te = fb.transitions[i];
            if (te.from_shape != own_shape || te.prototype != proto0 || te.proto_epoch != epoch) continue;
            const std::string& name = f->chunk.name_at(read_u16(code, pc + 2));
            if (obj->has_descriptor_override(name)) break;
            write_barrier_for(obj, acc);
            obj->add_shape_property_cached(name, acc, te.to_shape);
            return 1;
        }
    }
    return 0;
}

int fast_set_named(Frame* f, uint32_t pc, uint32_t ic_index) noexcept {
    const int handled = fast_set_named_impl(f, pc);
    if (handled && ic_index != kNoOperand) learn_ic(f, f->chunk.jit.get(), pc, ic_index);
    return handled;
}

// LdaLookup / LdaLookupStar: every cached form (h_LdaLookupFast).
int fast_lookup(Frame* f, uint32_t pc) noexcept {
    const auto& entry = f->lookup_cache_data[read_u16(f->chunk.code.data(), pc + 1)];
    if (entry.shadow_epoch != Environment::binding_shadow_epoch()) return 0;
    if (entry.dict_desc) {
        Object* bo = entry.env->get_binding_object();
        if (bo && !bo->get_shape() && entry.descriptor_epoch == Object::descriptor_epoch() &&
            entry.dict_desc->is_data_descriptor()) {
            f->acc = entry.dict_desc->get_value();
            return 1;
        }
    } else if (entry.obj_shape) {
        Object* bo = entry.env->get_binding_object();
        if (bo && bo->get_shape() == entry.obj_shape && entry.descriptor_epoch == Object::descriptor_epoch()) {
            if (const Value* s = bo->get_shape_slot_unchecked(entry.obj_slot_index)) {
                f->acc = *s;
                return 1;
            }
        }
    } else if (entry.slot) {
        f->acc = *entry.slot;
        return 1;
    }
    return 0;
}

// The environment-slot forms (h_LdaEnvSlotFast, h_StaEnvSlotFast and their
// hop-walking counterparts): kind 0 LdaEnvSlot, 1 StaEnvSlot, 2 LdaEnvSlotAt,
// 3 StaEnvSlotAt.
int fast_env_slot(Frame* f, uint32_t pc, uint32_t kind) noexcept {
    const uint8_t* code = f->chunk.code.data();
    Environment* env = frame_lexical_env(*f);
    uint32_t slot, name_at;
    if (kind >= 2) {
        for (uint8_t h = code[pc + 1]; h > 0 && env; h--) env = env->get_outer();
        slot = code[pc + 2]; name_at = pc + 3;
    } else {
        slot = code[pc + 1]; name_at = pc + 2;
    }
    if (!env) return 0;
    auto* e = env->inline_slot_interned(slot, f->chunk.names[read_u16(code, name_at)]);
    if (!e || !e->slot.initialized) return 0;
    if (kind & 1) {
        if (!e->slot.mutable_flag) return 0;
        Collector::write_barrier_env_for(env, f->acc);
        e->slot.value = f->acc;
    } else {
        f->acc = e->slot.value;
    }
    return 1;
}

// SetKeyed's element stores (h_SetKeyedFast / h_SetKeyedElement): an in-range or
// appending store to a dense array, and a typed-array number store.
int fast_set_keyed(Frame* f, uint32_t pc) noexcept {
    const uint8_t* code = f->chunk.code.data();
    const Value& acc = f->acc;
    uint32_t index;
    if (!array_index_key(f->regs[code[pc + 2]], index)) return 0;
    const Value& recv = f->regs[code[pc + 1]];
    Object* dense;
    if (dense_element_store_slot(recv, index, dense) && dense->store_dense_element(index, acc)) return 1;
    if (acc.is_number()) {
        Object* obj = as_object_like(recv);
        if (obj && obj->get_type() == Object::ObjectType::TypedArray &&
            static_cast<TypedArrayBase*>(obj)->write_plain_number(index, acc.as_number())) {
            return 1;
        }
    }
    return 0;
}

// The binary operators' general path (every NUMERIC_/BITWISE_BINARY_HANDLER's
// `_slow`, and TestInstanceOf): acc = binary_slow(regs[r], acc), then the
// exception routing. `op` is the BinaryExpression::Operator.
int32_t binary_op(Frame* f, uint32_t pc, uint32_t op) noexcept {
    JitCode* jc = f->chunk.jit.get();
    if (__builtin_expect(g_cfg.stats, 0)) { g_cfg.steps[f->chunk.code[pc]]++; g_cfg.step_total++; }
    f->instr_pc = pc;
    try {
        const uint8_t* code = f->chunk.code.data();
        f->acc = binary_slow(*f->ctx, static_cast<BinOp>(op), f->regs[code[pc + 1]], f->acc);
    } catch (...) {
        jc->pending = std::current_exception();
        return kStepRethrow;
    }
    return after_op(f, jc, pc, pc + 2);
}

// Op::Call (kind 0) and Op::CallResolved (kind 1): the bodies of h_gen_Call and
// h_gen_CallResolved. A call is by far the most frequent thing the generated
// code cannot do inline, and routing it through `step` would pay the stop-byte
// machinery for an instruction that never needs it.
int32_t call_op(Frame* f, uint32_t pc, uint32_t kind) noexcept {
    JitCode* jc = f->chunk.jit.get();
    if (__builtin_expect(g_cfg.stats, 0)) { g_cfg.steps[f->chunk.code[pc]]++; g_cfg.step_total++; }
    const uint8_t* code = f->chunk.code.data();
    Context& ctx = *f->ctx;
    f->instr_pc = pc;
    uint32_t next;
    try {
        const Value* regs = f->regs;
        Value receiver;
        uint32_t callee_reg, args_start, argc, name_idx;
        if (kind == 0 || kind == 2) {
            callee_reg = code[pc + 1]; args_start = code[pc + 2]; argc = code[pc + 3];
            name_idx = read_u16(code, pc + 4); next = pc + 6;
        } else {
            callee_reg = code[pc + 1]; receiver = regs[code[pc + 2]]; args_start = code[pc + 3]; argc = code[pc + 4];
            name_idx = read_u16(code, pc + 5); next = pc + 7;
        }
        const Value& callee = regs[callee_reg];
        std::span<const Value> call_args(regs + args_start, argc);
        Value acc;
        if (kind == 2) {
            // Op::Construct (h_gen_Construct).
            if (callee.is_function()) {
                Value old_new_target = ctx.get_new_target();
                ctx.set_new_target(callee);
                acc = callee.as_function()->construct(ctx, call_args);
                ctx.set_new_target(old_new_target);
            } else if (callee.is_object() && callee.as_object()->get_type() == Object::ObjectType::Proxy) {
                acc = static_cast<Proxy*>(callee.as_object())->construct_trap(call_args);
            } else {
                ctx.throw_type_error(f->chunk.name_at(name_idx) + " is not a constructor");
            }
        } else if (callee.is_function()) {
            Function* fn = callee.as_function();
            acc = fn->fast_callable() ? fn->call_fast_gate(ctx, call_args, receiver)
                                      : fn->call_register_args(ctx, call_args, receiver);
        } else if (callee.is_object() && callee.as_object()->get_type() == Object::ObjectType::Proxy) {
            std::vector<Value> trap_args(call_args.begin(), call_args.end());
            acc = static_cast<Proxy*>(callee.as_object())->apply_trap(trap_args, receiver);
        } else {
            ctx.throw_type_error(f->chunk.name_at(name_idx) + " is not a function");
        }
        f->acc = acc;
    } catch (...) {
        jc->pending = std::current_exception();
        return kStepRethrow;
    }
    return after_call(f, jc, pc, next);
}

void safepoint(Frame* f) noexcept {
    f->chunk.jit->rt.sp_counter = g_cfg.safepoint_reload;
    Collector::safepoint();
}

void write_barrier(Object* obj) noexcept { Collector::write_barrier(obj); }

// ---------------------------------------------------------------------------
// The compiler.
// ---------------------------------------------------------------------------

struct Insn {
    uint32_t pc;
    uint8_t op;
    uint8_t len;
};

// Where an instruction's i16 jump offset sits (byte index from the opcode), or
// -1 when it never branches.
int branch_offset_pos(Op op) {
    switch (op) {
    case Op::Jump: case Op::JumpIfTrue: case Op::JumpIfFalse:
    case Op::JumpIfNotNullish: case Op::JumpIfNullish: case Op::JumpIfNotUndefined:
    case Op::JumpIfNotNumber: case Op::JumpIfNotString:
        return 1;
    case Op::IteratorNextOrJump: return 3;
    case Op::TryCollectRestArray: return 4;
    default: return -1;
    }
}

// The BinaryExpression operator an arithmetic / comparison opcode stands for.
uint32_t binop_of(Op op) {
    using B = BinOp;
    switch (op) {
    case Op::Add: return static_cast<uint32_t>(B::ADD);
    case Op::Sub: return static_cast<uint32_t>(B::SUBTRACT);
    case Op::Mul: return static_cast<uint32_t>(B::MULTIPLY);
    case Op::Div: return static_cast<uint32_t>(B::DIVIDE);
    case Op::Mod: return static_cast<uint32_t>(B::MODULO);
    case Op::Exp: return static_cast<uint32_t>(B::EXPONENT);
    case Op::BitAnd: return static_cast<uint32_t>(B::BITWISE_AND);
    case Op::BitOr: return static_cast<uint32_t>(B::BITWISE_OR);
    case Op::BitXor: return static_cast<uint32_t>(B::BITWISE_XOR);
    case Op::Shl: return static_cast<uint32_t>(B::LEFT_SHIFT);
    case Op::Sar: return static_cast<uint32_t>(B::RIGHT_SHIFT);
    case Op::Shr: return static_cast<uint32_t>(B::UNSIGNED_RIGHT_SHIFT);
    case Op::TestEq: return static_cast<uint32_t>(B::EQUAL);
    case Op::TestNe: return static_cast<uint32_t>(B::NOT_EQUAL);
    case Op::TestStrictEq: return static_cast<uint32_t>(B::STRICT_EQUAL);
    case Op::TestStrictNe: return static_cast<uint32_t>(B::STRICT_NOT_EQUAL);
    case Op::TestLt: return static_cast<uint32_t>(B::LESS_THAN);
    case Op::TestGt: return static_cast<uint32_t>(B::GREATER_THAN);
    case Op::TestLe: return static_cast<uint32_t>(B::LESS_EQUAL);
    case Op::TestGe: return static_cast<uint32_t>(B::GREATER_EQUAL);
    case Op::TestInstanceOf: return static_cast<uint32_t>(B::INSTANCEOF);
    default: return 0xFFFFFFFFu;
    }
}

// Opcodes that suspend the frame (a generator's or async function's fiber
// switch) or hand control to machinery that expects to own the dispatch loop.
bool op_unsupported(Op op) {
    switch (op) {
    case Op::Yield: case Op::YieldStar: case Op::Await: case Op::SettleReturn:
    case Op::GetAsyncIterator: case Op::AsyncIteratorNextOrJump: case Op::AsyncIteratorClose:
    case Op::ReraiseGeneratorReturn:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Shared stubs. The inline-cache logic of a property access is a couple of
// dozen instructions; emitting it at every site made the generated code so
// large that a program with a few dozen hot functions no longer fit the L1
// instruction cache (the interpreter's handlers, shared by every function, do).
// So the logic is emitted once per thread, here, and a site is only the glue
// around a `call`.
//
// Convention: called with the generated code's pinned registers intact (rbx
// Frame, r12 register bank, r13 accumulator, r14 JitCode, r15 the site caches);
// may clobber rax, rcx, rdx, rsi, rdi, r8-r11 and the flags. CF = 0 on a hit,
// CF = 1 on a miss, in which case nothing visible has changed.
// ---------------------------------------------------------------------------
enum StubId { kStubGetNamed = 0, kStubSetNamed, kStubLookup, kStubGetKeyed, kStubSetKeyed, kStubCount };

struct StubSet {
    void* mem = nullptr;
    size_t size = 0;
    const void* entry[8] = {};
    bool tried = false;
};
thread_local StubSet t_stubs;

void write_barrier(Object* obj) noexcept;

bool alloc_exec(const std::vector<uint8_t>& bytes, void*& mem, size_t& size, uint8_t*& start);

// Loads the Object* of an object-tagged receiver in rax, or branches to `miss`.
void stub_unbox_object(Assembler& a, Label& miss) {
    a.mov(RCX, RAX);
    a.shr_imm(RCX, 48);
    a.cmp32_imm(RCX, kTopObject);
    a.jcc(CC_NE, miss);
    a.shl_imm(RAX, 16);
    a.shr_imm(RAX, 16);
}

// rdx = the first entry of the block whose shape word equals rcx, or `miss`.
void stub_find_way(Assembler& a, Label& miss) {
    static_assert(JitIC::kWays == 4, "unrolled for four ways");
    Label found;
    a.cmp_load(RCX, mem(RDX, offsetof(JitIC, shape_word)));
    a.jcc(CC_E, found);
    for (int i = 1; i < 3; i++) {
        a.add_imm(RDX, sizeof(JitIC));
        a.cmp_load(RCX, mem(RDX, offsetof(JitIC, shape_word)));
        a.jcc(CC_E, found);
    }
    a.add_imm(RDX, sizeof(JitIC));
    a.cmp_load(RCX, mem(RDX, offsetof(JitIC, shape_word)));
    a.jcc(CC_NE, miss);
    a.bind(found);
}

// rsi = the butterfly of the object in `obj`, and ecx = how many shape slots it has.
void stub_slots(Assembler& a, Reg obj, Label& miss) {
    a.mov_load(RSI, mem(obj, static_cast<int32_t>(JitObjectLayout::butterfly())));
    a.test(RSI, RSI);
    a.jcc(CC_E, miss);
    a.mov_load32(RCX, mem(RSI, -static_cast<int32_t>(sizeof(Value))));
    a.and32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kShapeCapacityMask));
}

bool build_stubs() {
    StubSet& t = t_stubs;
    if (t.mem) return true;
    if (t.tried) return false;
    t.tried = true;
    Assembler a;
    Label l_ret_hit, l_ret_miss;
    auto finish_labels = [&]() {
        a.bind(l_ret_hit);  a.clc(); a.ret();
        a.bind(l_ret_miss); a.stc(); a.ret();
    };
    size_t offs[8] = {};

    // ---- get_named: rax = receiver bits, rdx = &ic; r13 = value ----------
    {
        offs[kStubGetNamed] = a.size();
        Label miss, inherited, as_value;
        stub_unbox_object(a, miss);
        a.mov_load(RCX, mem(RAX, static_cast<int32_t>(JitObjectLayout::shape())));
        stub_find_way(a, miss);
        a.cmp32_mem_imm(mem(RDX, offsetof(JitIC, kind)), JitIC::Own);
        a.jcc(CC_NE, inherited);
        a.test8_mem_imm(mem(RAX, static_cast<int32_t>(JitObjectLayout::proto())), static_cast<uint8_t>(JitObjectLayout::kHasDescriptors));
        a.jcc(CC_NE, miss);
        stub_slots(a, RAX, miss);
        a.mov_load32(RDI, mem(RDX, offsetof(JitIC, slot)));
        a.cmp32(RDI, RCX);
        a.jcc(CC_AE, miss);
        a.mov_load(R13, mem_idx(RSI, RDI, 8));
        a.clc(); a.ret();
        // Inherited: the receiver's [[Prototype]] and the global prototype epoch
        // are the facts the interpreter's ProtoEntry is keyed on.
        a.bind(inherited);
        a.mov_load(RCX, mem(RAX, static_cast<int32_t>(JitObjectLayout::proto())));
        a.and_imm(RCX, -static_cast<int32_t>(JitObjectLayout::kProtoFlagMask + 1));   // bit 3 (has descriptors) must be 0
        a.cmp_load(RCX, mem(RDX, offsetof(JitIC, proto_word)));
        a.jcc(CC_NE, miss);
        a.mov_imm64(RCX, reinterpret_cast<uint64_t>(JitObjectLayout::proto_epoch_addr()));
        a.mov_load(RCX, mem(RCX));
        a.cmp_load(RCX, mem(RDX, offsetof(JitIC, proto_epoch)));
        a.jcc(CC_NE, miss);
        a.cmp32_mem_imm(mem(RDX, offsetof(JitIC, kind)), JitIC::ProtoValue);
        a.jcc(CC_E, as_value);
        a.mov_load(RAX, mem(RDX, offsetof(JitIC, holder)));       // a data slot of the holder
        stub_slots(a, RAX, miss);
        a.mov_load32(RDI, mem(RDX, offsetof(JitIC, slot)));
        a.cmp32(RDI, RCX);
        a.jcc(CC_AE, miss);
        a.mov_load(R13, mem_idx(RSI, RDI, 8));
        a.clc(); a.ret();
        a.bind(as_value);                                          // copied out of the holder's descriptor map
        a.mov_imm64(RCX, reinterpret_cast<uint64_t>(JitObjectLayout::descriptor_epoch_addr()));
        a.mov_load(RCX, mem(RCX));
        a.cmp_load(RCX, mem(RDX, offsetof(JitIC, desc_epoch)));
        a.jcc(CC_NE, miss);
        a.mov_load(R13, mem(RDX, offsetof(JitIC, value)));
        a.clc(); a.ret();
        a.bind(miss); a.stc(); a.ret();
    }

    // ---- set_named: rax = receiver bits, rdx = &ic, r13 = value ----------
    {
        offs[kStubSetNamed] = a.size();
        Label miss, no_barrier;
        stub_unbox_object(a, miss);
        a.mov_load(RCX, mem(RAX, static_cast<int32_t>(JitObjectLayout::shape())));
        stub_find_way(a, miss);
        a.cmp32_mem_imm(mem(RDX, offsetof(JitIC, kind)), JitIC::Own);
        a.jcc(CC_NE, miss);
        a.test8_mem_imm(mem(RAX, static_cast<int32_t>(JitObjectLayout::proto())), static_cast<uint8_t>(JitObjectLayout::kHasDescriptors));
        a.jcc(CC_NE, miss);
        stub_slots(a, RAX, miss);
        a.mov_load32(RDI, mem(RDX, offsetof(JitIC, slot)));
        a.cmp32(RDI, RCX);
        a.jcc(CC_AE, miss);
        // The remembered-set barrier the interpreter runs for any stored value
        // that can hold an edge (everything but numbers, booleans, undefined, null).
        a.mov(RCX, R13);
        a.shr_imm(RCX, 52);
        a.and32_imm(RCX, 0x7FF);
        a.cmp32_imm(RCX, 0x7FF);
        a.jcc(CC_NE, no_barrier);                 // a finite double
        a.mov(RCX, R13);
        a.shr_imm(RCX, 48);
        a.and32_imm(RCX, 0xF);                    // the tag
        a.cmp32_imm(RCX, 4);  a.jcc(CC_B, no_barrier);    // undefined, null, false, true
        a.cmp32_imm(RCX, 8);  a.jcc(CC_E, no_barrier);    // NaN
        a.cmp32_imm(RCX, 10); a.jcc(CC_E, no_barrier);    // -Infinity
        a.cmp32_imm(RCX, 11); a.jcc(CC_E, no_barrier);    // +Infinity
        // The call below is the only thing in a stub that can move the stack: the
        // caller's rsp is 16-aligned, the stub's return address makes it 8 off, and
        // five pushes put it back.
        a.push(RAX); a.push(RSI); a.push(RDI); a.push(RDX); a.push(RAX);
        a.mov(RDI, RAX);
        a.mov_store(mem(RBX, kOffAcc), R13);                      // keep the value visible to the collector
        a.call_abs(reinterpret_cast<const void*>(&write_barrier));
        a.pop(RAX); a.pop(RDX); a.pop(RDI); a.pop(RSI); a.pop(RAX);
        a.bind(no_barrier);
        a.mov_store(mem_idx(RSI, RDI, 8), R13);
        a.clc(); a.ret();
        a.bind(miss); a.stc(); a.ret();
    }

    // ---- lookup: rax = &LookupCacheEntry; r13 = value --------------------
    {
        using LCE = BytecodeChunk::LookupCacheEntry;
        offs[kStubLookup] = a.size();
        Label miss, global_object;
        a.mov_imm64(RCX, reinterpret_cast<uint64_t>(JitEnvLayout::shadow_epoch_addr()));
        a.mov_load32(RCX, mem(RCX));
        a.cmp32_load(RCX, mem(RAX, offsetof(LCE, shadow_epoch)));
        a.jcc(CC_NE, miss);
        a.cmp_mem_imm(mem(RAX, offsetof(LCE, dict_desc)), 0);
        a.jcc(CC_NE, miss);
        a.mov_load(RDX, mem(RAX, offsetof(LCE, obj_shape)));
        a.test(RDX, RDX);
        a.jcc(CC_NE, global_object);
        a.mov_load(RCX, mem(RAX, offsetof(LCE, slot)));
        a.test(RCX, RCX);
        a.jcc(CC_E, miss);
        a.mov_load(R13, mem(RCX));
        a.clc(); a.ret();
        a.bind(global_object);          // a data slot of the global object
        a.mov_load(RCX, mem(RAX, offsetof(LCE, env)));
        a.test(RCX, RCX);
        a.jcc(CC_E, miss);
        a.mov_load(RCX, mem(RCX, static_cast<int32_t>(JitEnvLayout::binding_object())));
        a.test(RCX, RCX);
        a.jcc(CC_E, miss);
        a.mov_load(RSI, mem(RCX, static_cast<int32_t>(JitObjectLayout::shape())));
        a.and_imm(RSI, -static_cast<int32_t>(JitObjectLayout::kShapeTypeMask + 1));   // Shape* without the type bits
        a.cmp(RSI, RDX);
        a.jcc(CC_NE, miss);
        a.mov_imm64(RSI, reinterpret_cast<uint64_t>(JitObjectLayout::descriptor_epoch_addr()));
        a.mov_load(RSI, mem(RSI));
        a.cmp_load(RSI, mem(RAX, offsetof(LCE, descriptor_epoch)));
        a.jcc(CC_NE, miss);
        stub_slots(a, RCX, miss);
        a.mov_load32(RDI, mem(RAX, offsetof(LCE, obj_slot_index)));
        a.cmp32(RDI, RCX);
        a.jcc(CC_AE, miss);
        a.mov_load(R13, mem_idx(RSI, RDI, 8));
        a.clc(); a.ret();
        a.bind(miss); a.stc(); a.ret();
    }

    // ---- get_keyed: rax = receiver bits, r13 = key; r13 = element --------
    {
        offs[kStubGetKeyed] = a.size();
        Label miss;
        stub_unbox_object(a, miss);
        a.mov_load(RCX, mem(RAX, static_cast<int32_t>(JitObjectLayout::shape())));
        a.and32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kShapeTypeMask));
        a.cmp32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kArrayType));
        a.jcc(CC_NE, miss);
        a.mov_load(RSI, mem(RAX, static_cast<int32_t>(JitObjectLayout::butterfly())));
        a.test(RSI, RSI);
        a.jcc(CC_E, miss);
        a.mov_load32(RCX, mem(RSI, -static_cast<int32_t>(sizeof(Value))));
        a.and32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kDenseVerifiedBit | JitObjectLayout::kExtrasBit));
        a.cmp32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kDenseVerifiedBit | JitObjectLayout::kExtrasBit));
        a.jcc(CC_NE, miss);
        // The key: a finite double that is exactly a non-negative int32.
        a.mov(RCX, R13);
        a.shr_imm(RCX, 52);
        a.and32_imm(RCX, 0x7FF);
        a.cmp32_imm(RCX, 0x7FF);
        a.jcc(CC_E, miss);
        a.movq_to_xmm(XMM0, R13);
        a.cvttsd2si32(RDI, XMM0);
        a.cvtsi2sd32(XMM1, RDI);
        a.ucomisd(XMM0, XMM1);
        a.jcc(CC_NE, miss);
        a.test32(RDI, RDI);
        a.jcc(CC_S, miss);
        a.cmp32_load(RDI, mem(RSI, -static_cast<int32_t>(JitObjectLayout::kExtrasBack) + static_cast<int32_t>(JitObjectLayout::kElementsLengthOff)));
        a.jcc(CC_AE, miss);
        a.mov32(RDI, RDI);
        a.neg64(RDI);
        a.mov_load(R13, mem_idx(RSI, RDI, 8, -static_cast<int32_t>(JitObjectLayout::kElementBase * sizeof(Value))));
        a.clc(); a.ret();
        a.bind(miss); a.stc(); a.ret();
    }

    // ---- set_keyed: rax = receiver bits, rcx = key bits, r13 = value -----
    {
        offs[kStubSetKeyed] = a.size();
        Label miss;
        a.mov(R8, RCX);                                  // the key (rcx is scratch below)
        a.mov(RCX, R13);                                 // the value: a number needs no write barrier
        a.shr_imm(RCX, 52);
        a.and32_imm(RCX, 0x7FF);
        a.cmp32_imm(RCX, 0x7FF);
        a.jcc(CC_E, miss);
        a.mov(RCX, R8);
        a.shr_imm(RCX, 52);
        a.and32_imm(RCX, 0x7FF);
        a.cmp32_imm(RCX, 0x7FF);
        a.jcc(CC_E, miss);
        a.movq_to_xmm(XMM0, R8);
        a.cvttsd2si32(RDI, XMM0);
        a.cvtsi2sd32(XMM1, RDI);
        a.ucomisd(XMM0, XMM1);
        a.jcc(CC_NE, miss);
        a.test32(RDI, RDI);
        a.jcc(CC_S, miss);
        stub_unbox_object(a, miss);
        a.mov_load(RCX, mem(RAX, static_cast<int32_t>(JitObjectLayout::shape())));
        a.and32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kShapeTypeMask));
        a.cmp32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kArrayType));
        a.jcc(CC_NE, miss);
        a.mov_load(RSI, mem(RAX, static_cast<int32_t>(JitObjectLayout::butterfly())));
        a.test(RSI, RSI);
        a.jcc(CC_E, miss);
        a.mov_load32(RCX, mem(RSI, -static_cast<int32_t>(sizeof(Value))));
        a.and32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kDenseVerifiedBit | JitObjectLayout::kExtrasBit));
        a.cmp32_imm(RCX, static_cast<int32_t>(JitObjectLayout::kDenseVerifiedBit | JitObjectLayout::kExtrasBit));
        a.jcc(CC_NE, miss);
        a.cmp32_load(RDI, mem(RSI, -static_cast<int32_t>(JitObjectLayout::kExtrasBack) + static_cast<int32_t>(JitObjectLayout::kElementsLengthOff)));
        a.jcc(CC_AE, miss);
        a.mov32(RDI, RDI);
        a.neg64(RDI);
        a.mov_store(mem_idx(RSI, RDI, 8, -static_cast<int32_t>(JitObjectLayout::kElementBase * sizeof(Value))), R13);
        a.clc(); a.ret();
        a.bind(miss); a.stc(); a.ret();
    }
    (void)finish_labels;

    for (const Label* l : {&l_ret_hit, &l_ret_miss}) if (!l->fixups.empty()) return false;
    uint8_t* start = nullptr;
    if (!alloc_exec(a.buf, t.mem, t.size, start)) return false;
    for (int i = 0; i < kStubCount; i++) t.entry[i] = start + offs[i];
    return true;
}

class Compiler {
public:
    explicit Compiler(const BytecodeChunk& c) : chunk_(c) {}

    std::unique_ptr<JitCode, JitCodeDeleter> compile(const char*& why);

private:
    const BytecodeChunk& chunk_;
    Assembler a_;
    std::vector<Insn> insns_;
    std::vector<int32_t> insn_at_;    // pc -> index into insns_, -1 if not an instruction start
    std::vector<uint8_t> is_target_;  // pc -> some branch / handler lands here
    std::deque<Label> labels_;        // stable addresses
    std::vector<Label*> lbl_;         // per instruction
    std::vector<std::function<void()>> cold_;
    JitCode* jc_ = nullptr;
    uint32_t next_ic_ = 0;
    Label step_other_, exit_finished_, exit_rethrow_, exit_bail_, exit_bail_pc_, epilogue_;
    uint32_t code_size_ = 0;
    const char* fail_ = nullptr;

    // Whether acc is known to hold a boolean at the start of the instruction
    // being emitted (the previous instruction left it, and nothing can jump
    // here).
    bool acc_is_bool_ = false;

    Label& new_label() { labels_.emplace_back(); return labels_.back(); }
    bool fail(const char* why) { if (!fail_) fail_ = why; return false; }

    bool reg_ok(uint32_t r) const { return r < chunk_.register_count; }
    Mem reg_mem(uint32_t r) const { return mem(R12, static_cast<int32_t>(r * sizeof(Value))); }
    uint16_t u16(uint32_t at) const { return read_u16(chunk_.code.data(), at); }
    int16_t i16(uint32_t at) const { return read_i16(chunk_.code.data(), at); }

    bool decode();
    bool emit_all();
    void emit_prologue();
    void emit_shared_tails();
    void emit_insn(size_t idx);

    // -- building blocks --
    void finite_check(Reg bits, Reg tmp, Label& slow);
    void spill_acc() { a_.mov_store(mem(RBX, kOffAcc), R13); }
    void reload_acc() { a_.mov_load(R13, mem(RBX, kOffAcc)); }
    void emit_step_call(const Insn& in, uint32_t target, uint32_t ic);   // leaves eax = result, acc reloaded
    void emit_step(const Insn& in, uint32_t target = kNoOperand, uint32_t ic = kNoOperand);
    void emit_step_cold(const Insn& in, Label& slow, uint32_t target = kNoOperand, uint32_t ic = kNoOperand);
    void branch(int cond, uint32_t from_pc, uint32_t target_pc);   // cond < 0: unconditional
    Label& label_for_pc(uint32_t pc) { return *lbl_[insn_at_[pc]]; }
    Label* next_label(size_t idx) { return idx + 1 < insns_.size() ? lbl_[idx + 1] : nullptr; }

    // -- instructions --
    void emit_arith(const Insn& in, size_t idx);
    void emit_bitwise(const Insn& in, size_t idx);
    void emit_compare(const Insn& in, size_t idx);
    void emit_incdec(const Insn& in, size_t idx, bool inc);
    void emit_truthiness(Label& when_true, Label& when_false, Label& slow);
    void emit_cond_jump(const Insn& in, size_t idx, bool jump_when_true, bool bool_known);
    void emit_get_named(const Insn& in, size_t idx, bool fused);
    void emit_set_named(const Insn& in, size_t idx);
    void emit_lookup(const Insn& in, size_t idx, bool fused);
    void emit_call(const Insn& in, uint32_t kind);
    // helper-backed slow paths (see "Fast helpers")
    void emit_fast_cold(const Insn& in, Label& slow, const void* fn, uint32_t kind, bool produces_acc, Label& resume, uint32_t ic = kNoOperand);
    void emit_fast_inline(const Insn& in, const void* fn, uint32_t kind, bool produces_acc, int fused_dst);
    void emit_binary_cold(const Insn& in, Label& slow, uint32_t binop);
    void emit_set_keyed(const Insn& in, size_t idx);
    void emit_get_keyed(const Insn& in, size_t idx);
    void emit_this(const Insn& in, size_t idx, bool fused);
};

bool Compiler::decode() {
    const uint8_t* code = chunk_.code.data();
    code_size_ = static_cast<uint32_t>(chunk_.code.size());
    insn_at_.assign(code_size_ + 1, -1);
    is_target_.assign(code_size_ + 1, 0);
    uint32_t pc = 0;
    while (pc < code_size_) {
        const uint8_t raw = code[pc];
        if (raw >= static_cast<uint8_t>(Op::kCount)) return fail("invalid opcode");
        const Op op = static_cast<Op>(raw);
        if (op_unsupported(op)) return fail("suspending opcode");
        const uint32_t len = 1 + static_cast<uint32_t>(op_operand_bytes(op));
        if (pc + len > code_size_) return fail("truncated instruction");
        insn_at_[pc] = static_cast<int32_t>(insns_.size());
        insns_.push_back({pc, raw, static_cast<uint8_t>(len)});
        pc += len;
    }
    if (pc != code_size_ || insns_.empty()) return fail("bad code size");
    // Branch targets must be instruction starts.
    for (const Insn& in : insns_) {
        const Op op = static_cast<Op>(in.op);
        const char kind = op_operand_kind(op);
        const int pos = branch_offset_pos(op);
        if (pos < 0) {
            if (kind == 'o' || kind == 'j' || kind == 'R') return fail("unknown branch form");
            continue;
        }
        const int64_t target = static_cast<int64_t>(in.pc) + in.len + i16(in.pc + pos);
        if (target < 0 || target >= code_size_ || insn_at_[target] < 0) return fail("bad branch target");
        is_target_[target] = 1;
    }
    if (chunk_.handlers) {
        for (const HandlerEntry& h : *chunk_.handlers) {
            if (h.genreturn_pc >= 0) return fail("generator return pad");
            if (h.handler_pc >= code_size_ || insn_at_[h.handler_pc] < 0) return fail("bad handler target");
            is_target_[h.handler_pc] = 1;
        }
    }
    return true;
}

void Compiler::emit_prologue() {
    // uint64_t entry(Frame* rdi, JitCode* rsi, const void* rdx)
    a_.push(RBX); a_.push(R12); a_.push(R13); a_.push(R14); a_.push(R15);
    a_.mov(RBX, RDI);
    a_.mov(R14, RSI);
    a_.mov_load(R12, mem(RBX, kOffRegs));
    a_.mov_load(R13, mem(RBX, kOffAcc));
    a_.mov_load(R15, mem(R14, static_cast<int32_t>(offsetof(JitRt, ics))));
    a_.jmp_reg(RDX);
}

void Compiler::emit_shared_tails() {
    // step_other_: eax = a pc or a kStep* code, acc already reloaded.
    a_.bind(step_other_);
    a_.test32(RAX, RAX);
    Label& generic = new_label();
    a_.jcc(CC_NS, generic);
    a_.cmp32_imm(RAX, kStepFinished);
    a_.jcc(CC_E, exit_finished_);
    a_.cmp32_imm(RAX, kStepRethrow);
    a_.jcc(CC_E, exit_rethrow_);
    a_.jmp(exit_bail_);
    a_.bind(generic);
    a_.mov32(RAX, RAX);          // the helper's int32 result leaves rax's upper half undefined
    // Dispatch on a pc through the chunk's pc -> native table, bounds-checked
    // here as well as by the helper that produced it.
    a_.cmp32_load(RAX, mem(R14, offsetof(JitRt, table_len)));
    a_.jcc(CC_AE, exit_bail_pc_);
    a_.mov_load(RCX, mem(R14, offsetof(JitRt, table)));
    a_.mov_load32(RDX, mem_idx(RCX, RAX, 4));
    a_.test32(RDX, RDX);
    a_.jcc(CC_E, exit_bail_pc_);
    a_.mov_load(RCX, mem(R14, offsetof(JitRt, base)));
    a_.add(RCX, RDX);
    a_.jmp_reg(RCX);

    a_.bind(exit_bail_pc_);
    a_.mov_store32(mem(RBX, kOffPc), RAX);
    a_.bind(exit_bail_);
    spill_acc();
    a_.mov_imm32(RAX, static_cast<uint32_t>(kExitBail));
    a_.jmp(epilogue_);

    a_.bind(exit_rethrow_);
    a_.mov_imm32(RAX, static_cast<uint32_t>(kExitRethrow));
    a_.jmp(epilogue_);

    a_.bind(exit_finished_);
    spill_acc();
    a_.mov_imm32(RAX, static_cast<uint32_t>(kExitFinished));
    a_.bind(epilogue_);
    a_.pop(R15); a_.pop(R14); a_.pop(R13); a_.pop(R12); a_.pop(RBX);
    a_.ret();
}

void Compiler::finite_check(Reg bits, Reg tmp, Label& slow) {
    // A finite double has an exponent field other than 0x7FF; every tagged
    // value (and NaN / infinity, which are tags too) has exactly 0x7FF.
    a_.mov(tmp, bits);
    a_.shr_imm(tmp, 52);
    a_.and32_imm(tmp, 0x7FF);
    a_.cmp32_imm(tmp, 0x7FF);
    a_.jcc(CC_E, slow);
}

void Compiler::emit_step_call(const Insn& in, uint32_t target, uint32_t ic) {
    spill_acc();
    a_.mov(RDI, RBX);
    a_.mov_imm32(RSI, in.pc);
    a_.mov_imm32(RDX, in.len);
    a_.mov_imm32(RCX, target);
    a_.mov_imm32(R8, ic);
    a_.call_abs(reinterpret_cast<const void*>(&step));
    reload_acc();
}

void Compiler::emit_step(const Insn& in, uint32_t target, uint32_t ic) {
    emit_step_call(in, target, ic);
    const uint32_t next = in.pc + in.len;
    if (next < code_size_) {
        a_.cmp32_imm(RAX, static_cast<int32_t>(next));
        a_.jcc(CC_NE, step_other_);
    } else {
        a_.jmp(step_other_);
    }
}

void Compiler::emit_step_cold(const Insn& in, Label& slow, uint32_t target, uint32_t ic) {
    const uint32_t next = in.pc + in.len;
    Label* resume = (next < code_size_) ? &label_for_pc(next) : nullptr;
    cold_.push_back([this, in, &slow, target, ic, resume, next]() {
        a_.bind(slow);
        emit_step_call(in, target, ic);
        if (resume) {
            a_.cmp32_imm(RAX, static_cast<int32_t>(next));
            a_.jcc(CC_NE, step_other_);
            a_.jmp(*resume);
        } else {
            a_.jmp(step_other_);
        }
    });
}

// A branch to `target_pc` from the instruction at `from_pc`. A backward branch
// pays a countdown and, every sp_counter-th time, the GC safepoint the
// interpreter runs on each back edge.
void Compiler::branch(int cond, uint32_t from_pc, uint32_t target_pc) {
    Label& dest = label_for_pc(target_pc);
    if (target_pc > from_pc) {
        if (cond < 0) a_.jmp(dest); else a_.jcc(static_cast<Cond>(cond), dest);
        return;
    }
    Label& back = new_label();
    if (cond < 0) a_.jmp(back); else a_.jcc(static_cast<Cond>(cond), back);
    cold_.push_back([this, &back, &dest]() {
        a_.bind(back);
        a_.sub32_mem_imm(mem(R14, offsetof(JitRt, sp_counter)), 1);
        a_.jcc(CC_G, dest);
        spill_acc();
        a_.mov(RDI, RBX);
        a_.call_abs(reinterpret_cast<const void*>(&safepoint));
        a_.jmp(dest);
    });
}

void Compiler::emit_arith(const Insn& in, size_t idx) {
    const uint32_t r = chunk_.code[in.pc + 1];
    Label& slow = new_label();
    a_.mov_load(RAX, reg_mem(r));
    finite_check(RAX, RCX, slow);
    finite_check(R13, RDX, slow);
    a_.movq_to_xmm(XMM0, RAX);
    a_.movq_to_xmm(XMM1, R13);
    switch (static_cast<Op>(in.op)) {
    case Op::Add: a_.addsd(XMM0, XMM1); break;
    case Op::Sub: a_.subsd(XMM0, XMM1); break;
    case Op::Mul: a_.mulsd(XMM0, XMM1); break;
    case Op::Div: a_.divsd(XMM0, XMM1); break;
    default: break;
    }
    a_.movq_from_xmm(RAX, XMM0);
    // An overflow to infinity (or 0/0) has to become the tagged form: the
    // interpreter's Value(double) does that, so leave it to the step.
    finite_check(RAX, RCX, slow);
    a_.mov(R13, RAX);
    emit_binary_cold(in, slow, binop_of(static_cast<Op>(in.op)));
    (void)idx;
}

void Compiler::emit_bitwise(const Insn& in, size_t idx) {
    const uint32_t r = chunk_.code[in.pc + 1];
    const Op op = static_cast<Op>(in.op);
    Label& slow = new_label();
    a_.mov_load(RAX, reg_mem(r));
    finite_check(RAX, RCX, slow);
    finite_check(R13, RDX, slow);
    a_.movq_to_xmm(XMM0, RAX);
    a_.movq_to_xmm(XMM1, R13);
    // cvttsd2si answers 0x80000000 for anything outside int32 (and for
    // INT_MIN itself); both go to the step, which does the full modular
    // ToInt32.
    a_.cvttsd2si32(RAX, XMM0);
    a_.cmp32_imm(RAX, INT32_MIN);
    a_.jcc(CC_E, slow);
    a_.cvttsd2si32(RCX, XMM1);
    a_.cmp32_imm(RCX, INT32_MIN);
    a_.jcc(CC_E, slow);
    switch (op) {
    case Op::BitAnd: a_.and32(RAX, RCX); break;
    case Op::BitOr: a_.or32(RAX, RCX); break;
    case Op::BitXor: a_.xor32(RAX, RCX); break;
    case Op::Shl: a_.shl32_cl(RAX); break;      // the hardware masks the count to 5 bits, as JS does
    case Op::Sar: a_.sar32_cl(RAX); break;
    case Op::Shr: a_.shr32_cl(RAX); break;
    default: break;
    }
    if (op == Op::Shr) a_.cvtsi2sd64(XMM0, RAX);   // a 32-bit op zero-extended into rax: unsigned
    else a_.cvtsi2sd32(XMM0, RAX);
    a_.movq_from_xmm(R13, XMM0);
    emit_binary_cold(in, slow, binop_of(op));
    (void)idx;
}

void Compiler::emit_compare(const Insn& in, size_t idx) {
    const uint32_t r = chunk_.code[in.pc + 1];
    const Op op = static_cast<Op>(in.op);
    const bool eq_family = op == Op::TestEq || op == Op::TestNe || op == Op::TestStrictEq || op == Op::TestStrictNe;
    const bool negate = op == Op::TestNe || op == Op::TestStrictNe;
    const bool strict = op == Op::TestStrictEq || op == Op::TestStrictNe;
    Label& slow = new_label();
    Label& done = new_label();
    Label& non_numeric = eq_family ? new_label() : slow;
    a_.mov_load(RAX, reg_mem(r));
    finite_check(RAX, RCX, non_numeric);
    finite_check(R13, RDX, non_numeric);
    a_.movq_to_xmm(XMM0, RAX);
    a_.movq_to_xmm(XMM1, R13);
    a_.ucomisd(XMM0, XMM1);      // regs[r] against acc; no NaN is possible past the checks
    Cond cc = CC_E;
    switch (op) {
    case Op::TestLt: cc = CC_B; break;
    case Op::TestLe: cc = CC_BE; break;
    case Op::TestGt: cc = CC_A; break;
    case Op::TestGe: cc = CC_AE; break;
    case Op::TestEq: case Op::TestStrictEq: cc = CC_E; break;
    case Op::TestNe: case Op::TestStrictNe: cc = CC_NE; break;
    default: break;
    }
    a_.setcc(cc, RAX);
    a_.movzx8(RAX, RAX);
    a_.shl_imm(RAX, kTagShift);
    a_.mov_imm64(RCX, kFalseBits);
    a_.add(RAX, RCX);            // false + (1 << 48) is true
    a_.mov(R13, RAX);
    a_.bind(done);
    emit_binary_cold(in, slow, binop_of(op));
    acc_is_bool_ = true;
    if (eq_family) {
        // Operands that are not both numbers: the cases decided by the tags alone.
        cold_.push_back([this, &non_numeric, &slow, &done, strict, negate]() {
            Label& res_true = new_label();
            Label& res_false = new_label();
            a_.bind(non_numeric);
            if (strict) {
                Label& same = new_label();
                a_.cmp(RAX, R13);
                a_.jcc(CC_E, same);
                // Different bits. Strings and BigInts can still be equal by content.
                a_.mov(RCX, RAX); a_.shr_imm(RCX, 48);
                a_.mov(RDX, R13); a_.shr_imm(RDX, 48);
                for (Reg t : {RCX, RDX}) {
                    a_.cmp32_imm(t, kTopString); a_.jcc(CC_E, slow);
                    a_.cmp32_imm(t, kTopBigInt); a_.jcc(CC_E, slow);
                }
                a_.jmp(res_false);
                a_.bind(same);
                // Identical bits: equal, except NaN, which equals nothing.
                a_.shr_imm(RAX, 48);
                a_.cmp32_imm(RAX, kTopNaN);
                a_.jcc(CC_E, res_false);
                a_.jmp(res_true);
            } else {
                // Loose: undefined and null are equal to each other and to nothing
                // else, an object included (apply_operator never converts it).
                Label& l_nullish = new_label();
                a_.mov(RCX, RAX); a_.shr_imm(RCX, 48);
                a_.mov(RDX, R13); a_.shr_imm(RDX, 48);
                a_.mov32(RSI, RCX); a_.sub32_imm(RSI, kTopUndefined);
                a_.mov32(RDI, RDX); a_.sub32_imm(RDI, kTopUndefined);
                a_.cmp32_imm(RSI, 2);
                a_.jcc(CC_B, l_nullish);
                a_.cmp32_imm(RDI, 2);
                a_.jcc(CC_AE, slow);                       // neither side is nullish
                a_.jmp(res_false);                         // only the right one is
                a_.bind(l_nullish);
                a_.cmp32_imm(RDI, 2);
                a_.jcc(CC_B, res_true);                    // both nullish
                a_.jmp(res_false);
            }
            a_.bind(res_true);
            a_.mov_imm64(R13, negate ? kFalseBits : kTrueBits);
            a_.jmp(done);
            a_.bind(res_false);
            a_.mov_imm64(R13, negate ? kTrueBits : kFalseBits);
            a_.jmp(done);
        });
    }
    (void)idx;
}

void Compiler::emit_incdec(const Insn& in, size_t idx, bool inc) {
    Label& slow = new_label();
    finite_check(R13, RCX, slow);
    a_.movq_to_xmm(XMM0, R13);
    a_.mov_imm64(RAX, bits_of_double(inc ? 1.0 : -1.0));
    a_.movq_to_xmm(XMM1, RAX);
    a_.addsd(XMM0, XMM1);
    a_.movq_from_xmm(RAX, XMM0);
    finite_check(RAX, RCX, slow);
    a_.mov(R13, RAX);
    emit_step_cold(in, slow);
    (void)idx;
}

// Decides acc's truthiness from its tag alone and jumps to `when_true` or
// `when_false`; a string, BigInt, symbol, NaN or infinity goes to `slow`.
// Booleans, undefined, null, objects, functions and finite numbers are all it
// takes to cover nearly every condition a program tests.
void Compiler::emit_truthiness(Label& when_true, Label& when_false, Label& slow) {
    a_.mov(RAX, R13);
    a_.shr_imm(RAX, 48);
    a_.cmp32_imm(RAX, kTopTrue);      a_.jcc(CC_E, when_true);
    a_.cmp32_imm(RAX, kTopFalse);     a_.jcc(CC_E, when_false);
    a_.cmp32_imm(RAX, kTopObject);    a_.jcc(CC_E, when_true);
    a_.cmp32_imm(RAX, kTopFunction);  a_.jcc(CC_E, when_true);
    a_.cmp32_imm(RAX, kTopUndefined); a_.jcc(CC_E, when_false);
    a_.cmp32_imm(RAX, kTopNull);      a_.jcc(CC_E, when_false);
    finite_check(R13, RCX, slow);
    // A finite number is falsy only when it is +0 or -0: shift the sign out.
    a_.mov(RAX, R13);
    a_.add(RAX, RAX);
    a_.test(RAX, RAX);
    a_.jcc(CC_NE, when_true);
    a_.jmp(when_false);
}

void Compiler::emit_cond_jump(const Insn& in, size_t idx, bool jump_when_true, bool bool_known) {
    const uint32_t target = in.pc + in.len + i16(in.pc + 1);
    (void)idx;
    if (bool_known) {
        // Straight after a comparison: a boolean for certain, and bit 48 is the
        // difference between true and false.
        a_.bt_imm(R13, 48);
        branch(jump_when_true ? CC_B : CC_AE, in.pc, target);   // CF = 1 <=> true
        acc_is_bool_ = true;   // the accumulator is untouched on both paths
        return;
    }
    Label& slow = new_label();
    Label& go = new_label();     // the jump is taken
    Label& done = new_label();   // falls through
    emit_truthiness(jump_when_true ? go : done, jump_when_true ? done : go, slow);
    a_.bind(go);
    branch(-1, in.pc, target);
    emit_step_cold(in, slow, target);
    a_.bind(done);
}

// The own-property and inherited reads a site's JitIC can serve (see
// learn_ic); anything else asks the helper and then the step.
void Compiler::emit_get_named(const Insn& in, size_t idx, bool fused) {
    const uint32_t r = chunk_.code[in.pc + 1];
    const uint32_t ic_index = next_ic_++ * JitIC::kWays;
    // A site whose feedback already says the cache cannot serve it (many shapes,
    // or only accessors) goes straight to the helper instead of paying for a
    // stub call that always misses.
    if (const FeedbackBody* fb = chunk_.feedback[u16(in.pc + 4)].peek()) {
        const bool own = !fb->mega && fb->entries[0].shape && !fb->entries[0].is_accessor;
        const bool inherited = fb->proto_count != 0 && !fb->proto_mega;
        if (!own && !inherited) {
            emit_fast_inline(in, reinterpret_cast<const void*>(&fast_get_named), kNoOperand, true,
                             fused ? static_cast<int>(chunk_.code[in.pc + 6]) : -1);
            (void)idx;
            return;
        }
    }
    Label& slow = new_label();
    Label& done = new_label();
    a_.mov_load(RAX, reg_mem(r));
    a_.lea(RDX, mem(R15, static_cast<int32_t>(ic_index * sizeof(JitIC))));
    a_.call_mem(mem(R14, static_cast<int32_t>(offsetof(JitRt, stubs) + kStubGetNamed * sizeof(void*))));
    a_.jcc(CC_B, slow);
    a_.bind(done);
    if (fused) a_.mov_store(reg_mem(chunk_.code[in.pc + 6]), R13);
    emit_fast_cold(in, slow, reinterpret_cast<const void*>(&fast_get_named), ic_index, true, done, ic_index);
    (void)idx;
}

// LdaLookup / LdaLookupStar: a name resolved through the scope chain, read from
// the instance's lookup cache the way h_LdaLookupFast does it -- a direct
// binding slot, or a data slot of the global object (a script's own `var`s and
// function declarations live there).
void Compiler::emit_lookup(const Insn& in, size_t idx, bool fused) {
    const uint32_t n = u16(in.pc + 1);
    constexpr int32_t entry_size = static_cast<int32_t>(sizeof(BytecodeChunk::LookupCacheEntry));
    Label& slow = new_label();
    Label& done = new_label();
    a_.mov_load(RAX, mem(RBX, kOffLookupCache));
    a_.test(RAX, RAX);
    a_.jcc(CC_E, slow);
    a_.add_imm(RAX, static_cast<int32_t>(n) * entry_size);
    a_.call_mem(mem(R14, static_cast<int32_t>(offsetof(JitRt, stubs) + kStubLookup * sizeof(void*))));
    a_.jcc(CC_B, slow);
    a_.bind(done);
    if (fused) a_.mov_store(reg_mem(chunk_.code[in.pc + 3]), R13);
    emit_fast_cold(in, slow, reinterpret_cast<const void*>(&fast_lookup), 0, true, done);
    (void)idx;
}

void Compiler::emit_set_named(const Insn& in, size_t idx) {
    const uint32_t r = chunk_.code[in.pc + 1];
    const uint32_t ic_index = next_ic_++ * JitIC::kWays;
    // A site that adds properties (a constructor's `this.x = ...`) has no own
    // entry to cache: its work is the transition, which the helper does.
    if (const FeedbackBody* fb = chunk_.feedback[u16(in.pc + 4)].peek()) {
        if (fb->mega || !fb->entries[0].shape || fb->entries[0].is_accessor) {
            emit_fast_inline(in, reinterpret_cast<const void*>(&fast_set_named), kNoOperand, false, -1);
            (void)idx;
            return;
        }
    }
    Label& slow = new_label();
    a_.mov_load(RAX, reg_mem(r));
    a_.lea(RDX, mem(R15, static_cast<int32_t>(ic_index * sizeof(JitIC))));
    a_.call_mem(mem(R14, static_cast<int32_t>(offsetof(JitRt, stubs) + kStubSetNamed * sizeof(void*))));
    a_.jcc(CC_B, slow);
    const uint32_t next = in.pc + in.len;
    if (next < code_size_) emit_fast_cold(in, slow, reinterpret_cast<const void*>(&fast_set_named), ic_index, false, label_for_pc(next), ic_index);
    else emit_step_cold(in, slow, kNoOperand, ic_index);
    (void)idx;
}

// acc = regs[r][acc] for a dense, hole-free Array and an in-range integer
// index: the one case h_GetKeyedFast reads without a call. Typed arrays,
// strings, sparse or holey arrays and every other key are the step's.
void Compiler::emit_get_keyed(const Insn& in, size_t idx) {
    const uint32_t r = chunk_.code[in.pc + 1];
    Label& slow = new_label();
    a_.mov_load(RAX, reg_mem(r));
    a_.call_mem(mem(R14, static_cast<int32_t>(offsetof(JitRt, stubs) + kStubGetKeyed * sizeof(void*))));
    a_.jcc(CC_B, slow);
    emit_step_cold(in, slow);
    (void)idx;
}

// SetKeyed: a number stored into an in-range element of a dense array is a
// store; appends, other values and typed arrays go through the helper.
void Compiler::emit_set_keyed(const Insn& in, size_t idx) {
    const uint32_t r_obj = chunk_.code[in.pc + 1];
    const uint32_t r_key = chunk_.code[in.pc + 2];
    Label& slow = new_label();
    Label& done = new_label();
    a_.mov_load(RAX, reg_mem(r_obj));
    a_.mov_load(RCX, reg_mem(r_key));
    a_.call_mem(mem(R14, static_cast<int32_t>(offsetof(JitRt, stubs) + kStubSetKeyed * sizeof(void*))));
    a_.jcc(CC_B, slow);
    a_.bind(done);
    emit_fast_cold(in, slow, reinterpret_cast<const void*>(&fast_set_keyed), 0, false, done);
    (void)idx;
}

void Compiler::emit_call(const Insn& in, uint32_t kind) {
    spill_acc();
    a_.mov(RDI, RBX);
    a_.mov_imm32(RSI, in.pc);
    a_.mov_imm32(RDX, kind);
    a_.call_abs(reinterpret_cast<const void*>(&call_op));
    reload_acc();
    const uint32_t next = in.pc + in.len;
    if (next < code_size_) {
        a_.cmp32_imm(RAX, static_cast<int32_t>(next));
        a_.jcc(CC_NE, step_other_);
    } else {
        a_.jmp(step_other_);
    }
}

// A slow path that asks a fast helper first and steps only if it declines.
// `resume` is where generated code continues after a success: for an
// instruction whose result is stored by code after that label this is that
// label; otherwise the next instruction's.
void Compiler::emit_fast_cold(const Insn& in, Label& slow, const void* fn, uint32_t kind, bool produces_acc, Label& resume, uint32_t ic) {
    Label& step_label = new_label();
    cold_.push_back([this, in, &slow, &step_label, fn, kind, produces_acc, &resume]() {
        a_.bind(slow);
        spill_acc();
        a_.mov(RDI, RBX);
        a_.mov_imm32(RSI, in.pc);
        a_.mov_imm32(RDX, kind);
        a_.call_abs(fn);
        a_.test32(RAX, RAX);
        a_.jcc(CC_E, step_label);
        if (produces_acc) reload_acc();
        a_.jmp(resume);
    });
    emit_step_cold(in, step_label, kNoOperand, ic);
}

// An instruction made entirely of a fast helper (no inline fast path), with the
// step as its fallback.
void Compiler::emit_fast_inline(const Insn& in, const void* fn, uint32_t kind, bool produces_acc, int fused_dst) {
    Label& step_label = new_label();
    spill_acc();
    a_.mov(RDI, RBX);
    a_.mov_imm32(RSI, in.pc);
    a_.mov_imm32(RDX, kind);
    a_.call_abs(fn);
    a_.test32(RAX, RAX);
    a_.jcc(CC_E, step_label);
    if (produces_acc) reload_acc();
    if (fused_dst >= 0) a_.mov_store(reg_mem(static_cast<uint32_t>(fused_dst)), R13);
    emit_step_cold(in, step_label);
}

// The general path of a binary operator: acc = binary_slow(regs[r], acc).
void Compiler::emit_binary_cold(const Insn& in, Label& slow, uint32_t binop) {
    const uint32_t next = in.pc + in.len;
    Label* resume = (next < code_size_) ? &label_for_pc(next) : nullptr;
    cold_.push_back([this, in, &slow, binop, resume, next]() {
        a_.bind(slow);
        spill_acc();
        a_.mov(RDI, RBX);
        a_.mov_imm32(RSI, in.pc);
        a_.mov_imm32(RDX, binop);
        a_.call_abs(reinterpret_cast<const void*>(&binary_op));
        reload_acc();
        if (resume) {
            a_.cmp32_imm(RAX, static_cast<int32_t>(next));
            a_.jcc(CC_NE, step_other_);
            a_.jmp(*resume);
        } else {
            a_.jmp(step_other_);
        }
    });
}

void Compiler::emit_this(const Insn& in, size_t idx, bool fused) {
    Label& slow = new_label();
    a_.cmp8_mem_imm(mem(RBX, kOffThisResolved), 0);
    a_.jcc(CC_E, slow);
    a_.mov_load(RAX, mem(RBX, kOffCtx));
    a_.test8_mem_imm(mem(RAX, static_cast<int32_t>(g_cfg.this_needs_super_off)), g_cfg.this_needs_super_mask);
    a_.jcc(CC_NE, slow);
    a_.mov_load(R13, mem(RBX, kOffThisValue));
    if (fused) a_.mov_store(reg_mem(chunk_.code[in.pc + 1]), R13);
    emit_step_cold(in, slow);
    (void)idx;
}

void Compiler::emit_insn(size_t idx) {
    const Insn& in = insns_[idx];
    const Op op = static_cast<Op>(in.op);
    const uint8_t* code = chunk_.code.data();
    const bool was_bool = acc_is_bool_;
    acc_is_bool_ = false;
    // A label something branches to may be entered with any accumulator.
    const bool bool_known = was_bool && !is_target_[in.pc];

    auto step_only = [&]() { emit_step(in); };
    switch (op) {
    case Op::LdaZero: a_.mov_imm64(R13, bits_of_double(0.0)); break;
    case Op::LdaUndefined: a_.mov_imm64(R13, kUndefinedBits); break;
    case Op::LdaNull: a_.mov_imm64(R13, kNullBits); break;
    case Op::LdaTrue: a_.mov_imm64(R13, kTrueBits); acc_is_bool_ = true; break;
    case Op::LdaFalse: a_.mov_imm64(R13, kFalseBits); acc_is_bool_ = true; break;
    case Op::LdaSmi: a_.mov_imm64(R13, bits_of_double(static_cast<double>(static_cast<int8_t>(code[in.pc + 1])))); break;
    case Op::LdaConst: {
        const uint32_t k = u16(in.pc + 1);
        if (k >= chunk_.constants.size()) { fail("constant index"); return; }
        const Value& c = chunk_.constants[k];
        if (c.is_finite_double()) a_.mov_imm64(R13, bits_of(c));
        else { a_.mov_imm64(RAX, reinterpret_cast<uint64_t>(&chunk_.constants[k])); a_.mov_load(R13, mem(RAX)); }
        break;
    }
    case Op::Ldar: if (!reg_ok(code[in.pc + 1])) { fail("register"); return; } a_.mov_load(R13, reg_mem(code[in.pc + 1])); break;
    case Op::Star: if (!reg_ok(code[in.pc + 1])) { fail("register"); return; } a_.mov_store(reg_mem(code[in.pc + 1]), R13); break;
    case Op::Mov:
        if (!reg_ok(code[in.pc + 1]) || !reg_ok(code[in.pc + 2])) { fail("register"); return; }
        a_.mov_load(RAX, reg_mem(code[in.pc + 2]));
        a_.mov_store(reg_mem(code[in.pc + 1]), RAX);
        break;
    case Op::LdarStar:
        if (!reg_ok(code[in.pc + 1]) || !reg_ok(code[in.pc + 2])) { fail("register"); return; }
        a_.mov_load(R13, reg_mem(code[in.pc + 1]));
        a_.mov_store(reg_mem(code[in.pc + 2]), R13);
        break;
    case Op::LdaSmiStar:
        if (!reg_ok(code[in.pc + 2])) { fail("register"); return; }
        a_.mov_imm64(R13, bits_of_double(static_cast<double>(static_cast<int8_t>(code[in.pc + 1]))));
        a_.mov_store(reg_mem(code[in.pc + 2]), R13);
        break;
    case Op::LdaZeroStar:
        if (!reg_ok(code[in.pc + 1])) { fail("register"); return; }
        a_.mov_imm64(R13, bits_of_double(0.0));
        a_.mov_store(reg_mem(code[in.pc + 1]), R13);
        break;
    case Op::LdaConstStar: {
        const uint32_t k = u16(in.pc + 1);
        const uint32_t dst = code[in.pc + 3];
        if (k >= chunk_.constants.size() || !reg_ok(dst)) { fail("operand"); return; }
        const Value& c = chunk_.constants[k];
        if (c.is_finite_double()) a_.mov_imm64(R13, bits_of(c));
        else { a_.mov_imm64(RAX, reinterpret_cast<uint64_t>(&chunk_.constants[k])); a_.mov_load(R13, mem(RAX)); }
        a_.mov_store(reg_mem(dst), R13);
        break;
    }
    case Op::LdaThis:
    case Op::LdaThisStar:
        if (op == Op::LdaThisStar && !reg_ok(code[in.pc + 1])) { fail("register"); return; }
        if (g_cfg.this_needs_super_inline) emit_this(in, idx, op == Op::LdaThisStar);
        else step_only();
        break;
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
        if (!reg_ok(code[in.pc + 1])) { fail("register"); return; }
        emit_arith(in, idx); break;
    case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: case Op::Sar:
        if (!reg_ok(code[in.pc + 1])) { fail("register"); return; }
        emit_bitwise(in, idx); break;
    case Op::TestLt: case Op::TestGt: case Op::TestLe: case Op::TestGe:
    case Op::TestEq: case Op::TestNe: case Op::TestStrictEq: case Op::TestStrictNe:
        if (!reg_ok(code[in.pc + 1])) { fail("register"); return; }
        emit_compare(in, idx); break;
    case Op::Inc: emit_incdec(in, idx, true); break;
    case Op::Dec: emit_incdec(in, idx, false); break;
    case Op::Neg: {
        Label& slow = new_label();
        finite_check(R13, RCX, slow);
        a_.mov_imm64(RAX, 0x8000000000000000ull);
        a_.xor_(R13, RAX);
        emit_step_cold(in, slow);
        break;
    }
    case Op::LogicalNot: {
        Label& slow = new_label();
        if (bool_known) {
            a_.mov_imm64(RAX, 1ull << 48);
            a_.xor_(R13, RAX);
            acc_is_bool_ = true;
            break;
        }
        Label& t = new_label();
        Label& fl = new_label();
        Label& done = new_label();
        emit_truthiness(t, fl, slow);
        a_.bind(t);
        a_.mov_imm64(R13, kFalseBits);
        a_.jmp(done);
        a_.bind(fl);
        a_.mov_imm64(R13, kTrueBits);
        a_.bind(done);
        emit_step_cold(in, slow);
        acc_is_bool_ = true;
        break;
    }
    case Op::ToNumeric:
    case Op::ToNumber: {
        // A number is already numeric: finite, NaN or an infinity (tags 8, 10, 11).
        Label& slow = new_label();
        Label& done = new_label();
        a_.mov(RAX, R13);
        a_.shr_imm(RAX, 52);
        a_.and32_imm(RAX, 0x7FF);
        a_.cmp32_imm(RAX, 0x7FF);
        a_.jcc(CC_NE, done);
        a_.mov(RAX, R13);
        a_.shr_imm(RAX, 48);
        a_.cmp32_imm(RAX, kTopNaN);  a_.jcc(CC_E, done);
        a_.cmp32_imm(RAX, 0x7FFA);   a_.jcc(CC_E, done);
        a_.cmp32_imm(RAX, 0x7FFB);   a_.jcc(CC_NE, slow);
        a_.bind(done);
        emit_step_cold(in, slow);
        break;
    }
    case Op::ToPropertyKey: {
        // A string or a number is left alone (see h_gen_ToPropertyKey).
        Label& slow = new_label();
        Label& done = new_label();
        a_.mov(RAX, R13);
        a_.shr_imm(RAX, 52);
        a_.and32_imm(RAX, 0x7FF);
        a_.cmp32_imm(RAX, 0x7FF);
        a_.jcc(CC_NE, done);
        a_.mov(RAX, R13);
        a_.shr_imm(RAX, 48);
        a_.cmp32_imm(RAX, kTopString); a_.jcc(CC_E, done);
        a_.cmp32_imm(RAX, kTopNaN);    a_.jcc(CC_E, done);
        a_.cmp32_imm(RAX, 0x7FFA);     a_.jcc(CC_E, done);
        a_.cmp32_imm(RAX, 0x7FFB);     a_.jcc(CC_NE, slow);
        a_.bind(done);
        emit_step_cold(in, slow);
        break;
    }
    case Op::CheckObjectCoercible: {
        Label& slow = new_label();
        a_.mov(RAX, R13);
        a_.shr_imm(RAX, 48);
        a_.sub32_imm(RAX, kTopUndefined);
        a_.cmp32_imm(RAX, 2);                 // undefined or null
        a_.jcc(CC_B, slow);
        emit_step_cold(in, slow);
        break;
    }
    case Op::Jump: {
        const uint32_t target = in.pc + in.len + i16(in.pc + 1);
        branch(-1, in.pc, target);
        break;
    }
    case Op::JumpIfTrue: emit_cond_jump(in, idx, true, bool_known); break;
    case Op::JumpIfFalse: emit_cond_jump(in, idx, false, bool_known); break;
    case Op::Return: a_.jmp(exit_finished_); break;
    case Op::GetNamed:
        if (!reg_ok(code[in.pc + 1]) || u16(in.pc + 4) >= chunk_.feedback.size()) { fail("operand"); return; }
        emit_get_named(in, idx, false); break;
    case Op::GetNamedStar:
        if (!reg_ok(code[in.pc + 1]) || !reg_ok(code[in.pc + 6]) || u16(in.pc + 4) >= chunk_.feedback.size()) { fail("operand"); return; }
        emit_get_named(in, idx, true); break;
    case Op::Call: {
        const uint32_t argc = code[in.pc + 3], start = code[in.pc + 2];
        if (!reg_ok(code[in.pc + 1]) || start > chunk_.register_count || start + argc > chunk_.register_count + (argc ? 0u : start)) { fail("call operands"); return; }
        if (argc && start + argc > chunk_.register_count) { fail("call operands"); return; }
        if (u16(in.pc + 4) >= chunk_.names.size()) { fail("call name"); return; }
        emit_call(in, 0); break;
    }
    case Op::CallResolved: {
        const uint32_t argc = code[in.pc + 4], start = code[in.pc + 3];
        if (!reg_ok(code[in.pc + 1]) || !reg_ok(code[in.pc + 2]) || start > chunk_.register_count) { fail("call operands"); return; }
        if (argc && start + argc > chunk_.register_count) { fail("call operands"); return; }
        if (u16(in.pc + 5) >= chunk_.names.size()) { fail("call name"); return; }
        emit_call(in, 1); break;
    }
    case Op::GetKeyed:
        if (!reg_ok(code[in.pc + 1])) { fail("register"); return; }
        emit_get_keyed(in, idx); break;
    case Op::SetKeyed:
        if (!reg_ok(code[in.pc + 1]) || !reg_ok(code[in.pc + 2])) { fail("register"); return; }
        emit_set_keyed(in, idx); break;
    case Op::Construct: {
        const uint32_t argc = code[in.pc + 3], start = code[in.pc + 2];
        if (!reg_ok(code[in.pc + 1]) || start > chunk_.register_count || (argc && start + argc > chunk_.register_count)) { fail("call operands"); return; }
        if (u16(in.pc + 4) >= chunk_.names.size()) { fail("call name"); return; }
        emit_call(in, 2); break;
    }
    case Op::TestInstanceOf: case Op::Exp: {
        if (!reg_ok(code[in.pc + 1])) { fail("register"); return; }
        Label& slow = new_label();
        a_.jmp(slow);
        emit_binary_cold(in, slow, binop_of(op));
        break;
    }
    case Op::LdaEnvSlot: case Op::LdaEnvSlotStar: {
        if (u16(in.pc + 2) >= chunk_.names.size() || (op == Op::LdaEnvSlotStar && !reg_ok(code[in.pc + 4]))) { fail("operand"); return; }
        emit_fast_inline(in, reinterpret_cast<const void*>(&fast_env_slot), 0, true, op == Op::LdaEnvSlotStar ? code[in.pc + 4] : -1);
        break;
    }
    case Op::StaEnvSlot:
        if (u16(in.pc + 2) >= chunk_.names.size()) { fail("operand"); return; }
        emit_fast_inline(in, reinterpret_cast<const void*>(&fast_env_slot), 1, false, -1);
        break;
    case Op::LdaEnvSlotAt:
        if (u16(in.pc + 3) >= chunk_.names.size()) { fail("operand"); return; }
        emit_fast_inline(in, reinterpret_cast<const void*>(&fast_env_slot), 2, true, -1);
        break;
    case Op::StaEnvSlotAt:
        if (u16(in.pc + 3) >= chunk_.names.size()) { fail("operand"); return; }
        emit_fast_inline(in, reinterpret_cast<const void*>(&fast_env_slot), 3, false, -1);
        break;
    case Op::LdaLookup: case Op::LdaLookupStar:
        if (u16(in.pc + 1) >= chunk_.names.size() || (op == Op::LdaLookupStar && !reg_ok(code[in.pc + 3]))) { fail("operand"); return; }
        emit_lookup(in, idx, op == Op::LdaLookupStar); break;
    case Op::SetNamed:
        if (!reg_ok(code[in.pc + 1]) || u16(in.pc + 4) >= chunk_.feedback.size()) { fail("operand"); return; }
        emit_set_named(in, idx); break;
    default: {
        // Branching ops the JIT does not translate still need their target
        // stopped at.
        const int pos = branch_offset_pos(op);
        uint32_t target = kNoOperand;
        if (pos >= 0) target = in.pc + in.len + i16(in.pc + pos);
        emit_step(in, target);
        break;
    }
    }
}


// Maps `bytes` as read+execute memory of their own. The code starts at a
// different cache-line offset in each mapping, not always at the page's start:
// functions that each began on a page boundary would all compete for the same
// handful of L1 instruction-cache sets, and the hot head of every one of them
// would evict the others'.
bool alloc_exec(const std::vector<uint8_t>& bytes, void*& mem, size_t& size, uint8_t*& start) {
    static uint32_t counter = 0;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t lines = page / 64;
    const size_t offset = (static_cast<size_t>(counter++) * 7 % lines) * 64;
    size = (offset + bytes.size() + page - 1) / page * page;
    if (g_cfg.mapped_bytes + size > g_cfg.max_mapped_bytes) return false;
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;
    std::memcpy(static_cast<uint8_t*>(p) + offset, bytes.data(), bytes.size());
    // Written once, then never writable again while it can run.
    if (mprotect(p, size, PROT_READ | PROT_EXEC) != 0) { munmap(p, size); return false; }
    mem = p;
    start = static_cast<uint8_t*>(p) + offset;
    g_cfg.mapped_bytes += size;
    return true;
}

std::unique_ptr<JitCode, JitCodeDeleter> Compiler::compile(const char*& why) {
    why = "";
    if (!decode()) { why = fail_; return nullptr; }
    if (!build_stubs()) { why = "stubs"; return nullptr; }
    std::unique_ptr<JitCode, JitCodeDeleter> jc(new JitCode());
    jc_ = jc.get();

    uint32_t ic_sites = 0;
    for (const Insn& in : insns_) {
        const Op op = static_cast<Op>(in.op);
        if (op == Op::GetNamed || op == Op::GetNamedStar || op == Op::SetNamed) ic_sites++;
    }
    jc->ics.assign(static_cast<size_t>(ic_sites) * JitIC::kWays, JitIC{});
    jc->code_copy.assign(chunk_.code.data(), chunk_.code.data() + code_size_);
    jc->code_copy.push_back(kHaltOp);   // permanent stop one past the end
    jc->native_off.assign(code_size_, 0);
    jc->code_size = code_size_;
    jc->owner_thread = &g_thread_marker;
    jc->has_handlers = chunk_.handlers && !chunk_.handlers->empty();
    if (chunk_.handlers) {
        for (const HandlerEntry& h : *chunk_.handlers) {
            bool seen = false;
            for (uint32_t p : jc->handler_pcs) seen |= (p == h.handler_pc);
            if (!seen && jc->handler_pcs.size() < 8) jc->handler_pcs.push_back(h.handler_pc);
        }
    }

    lbl_.resize(insns_.size());
    for (auto& l : lbl_) l = &new_label();

    emit_prologue();
    for (size_t i = 0; i < insns_.size(); i++) {
        a_.bind(*lbl_[i]);
        emit_insn(i);
        if (fail_) { why = fail_; return nullptr; }
    }
    // Falling off the end cannot happen in well-formed bytecode; if it does,
    // finish with whatever the accumulator holds rather than run into the
    // shared tails.
    a_.jmp(exit_finished_);
    emit_shared_tails();
    for (size_t i = 0; i < cold_.size(); i++) cold_[i]();   // cold stubs may queue more
    for (const Label& l : labels_) {
        if (!l.fixups.empty()) { why = "unbound label"; return nullptr; }
    }
    if (a_.size() > (8u << 20)) { why = "code too large"; return nullptr; }

    for (size_t i = 0; i < insns_.size(); i++) {
        const int32_t pos = lbl_[i]->pos;
        if (pos <= 0) { why = "unbound instruction"; return nullptr; }
        jc->native_off[insns_[i].pc] = static_cast<uint32_t>(pos);
    }
    jc->code_bytes = a_.size();
    uint8_t* start = nullptr;
    if (!alloc_exec(a_.buf, jc->mem, jc->mem_size, start)) { why = "out of executable memory"; return nullptr; }
    jc->rt.sp_counter = g_cfg.safepoint_reload;
    jc->rt.ics = jc->ics.data();
    for (int i = 0; i < kStubCount; i++) jc->rt.stubs[i] = t_stubs.entry[i];
    jc->rt.table_len = code_size_;
    jc->rt.table = jc->native_off.data();
    jc->rt.base = start;
    jc->start_off = jc->native_off[0];
    jc->entry = reinterpret_cast<uint64_t (*)(void*, JitCode*, const void*)>(start);

    if (g_cfg.dump_dir) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/jit_%p.bin", g_cfg.dump_dir, jc->mem);
        if (FILE* fp = std::fopen(path, "wb")) { std::fwrite(a_.buf.data(), 1, a_.buf.size(), fp); std::fclose(fp); }
    }
    return jc;
}

void free_code(JitCode* c) noexcept {
    if (!c) return;
    if (c->mem) {
        munmap(c->mem, c->mem_size);
        g_cfg.mapped_bytes -= c->mem_size;
    }
    delete c;
}

// Compiles `chunk` now. Leaves jit_state at 1 (compiled) or 2 (never).
void compile_chunk(const BytecodeChunk& chunk) noexcept {
    const char* why = "";
    std::unique_ptr<JitCode, JitCodeDeleter> compiled;
    const uint32_t ordinal = g_cfg.seen++;
    if (ordinal < g_cfg.skip || ordinal >= g_cfg.skip + g_cfg.limit) {
        chunk.jit_state = 2;
        return;
    }
    try {
        Compiler c(chunk);
        compiled = c.compile(why);
    } catch (...) {
        why = "compiler threw";
    }
    if (compiled) {
        chunk.jit = std::move(compiled);
        chunk.jit_state = 1;
        g_cfg.compiled++;
        if (g_cfg.log) std::fprintf(stderr, "[jit] #%u compiled %u bytes of bytecode -> %zu bytes of code (%zu mapped)\n", ordinal,
                                    static_cast<unsigned>(chunk.code.size()), chunk.jit->code_bytes, chunk.jit->mem_size);
    } else {
        chunk.jit_state = 2;
        g_cfg.refused++;
        if (g_cfg.log) std::fprintf(stderr, "[jit] refused (%s): %u bytes of bytecode\n", why,
                                    static_cast<unsigned>(chunk.code.size()));
    }
}

// What a chunk's first call tells the JIT: whether it has a loop (a backward
// branch), which is where a baseline JIT earns its keep.
bool chunk_has_loop(const BytecodeChunk& chunk) noexcept {
    const uint8_t* code = chunk.code.data();
    const uint32_t size = static_cast<uint32_t>(chunk.code.size());
    for (uint32_t pc = 0; pc < size;) {
        const uint8_t raw = code[pc];
        if (raw >= static_cast<uint8_t>(Op::kCount)) return false;
        const Op op = static_cast<Op>(raw);
        const int pos = branch_offset_pos(op);
        if (pos >= 0 && pc + pos + 1 < size && read_i16(code, pc + pos) < 0) return true;
        pc += 1 + static_cast<uint32_t>(op_operand_bytes(op));
    }
    return false;
}

// jit_state: 0 unseen, 1 compiled, 2 never, 3 has a loop and is counting calls,
// 4 loop-free and counting calls.
//
// A function with a loop is compiled on its second call, or sooner by osr_try
// when a loop in it runs hot. A loop-free function is left to the interpreter by
// default: it is the loops that dispatch overhead multiplies in, while a short
// method that is mostly property accesses and calls spends its time in the
// runtime either way (QUANTA_JIT_MODE=0 compiles those too, after
// QUANTA_JIT_THRESHOLD calls).
[[gnu::noinline]] void consider(const BytecodeChunk& chunk) noexcept {
    init_once();
    if (!g_cfg.enabled) { chunk.jit_state = 2; return; }
    if (chunk.jit_state == 0) {
        const uint32_t size = static_cast<uint32_t>(chunk.code.size());
        if (size > (1u << 20) || chunk.code.size() == 0) { chunk.jit_state = 2; return; }
        const bool has_loop = chunk_has_loop(chunk);
        if (!has_loop && g_cfg.mode == 1) { chunk.jit_state = 2; return; }
        chunk.jit_state = has_loop ? 3 : 4;
        chunk.jit_hotness = 0;
    }
    const uint32_t threshold = chunk.jit_state == 3 ? (g_cfg.threshold < 2 ? g_cfg.threshold : 2) : g_cfg.threshold;
    if (++chunk.jit_hotness < threshold) return;
    compile_chunk(chunk);
}

// The interpreter's offer at a hot back edge (see osr_try in Interpreter.cpp).
bool osr(Frame& f, uint32_t target, const Value& acc) noexcept;

// The slow half of run(): everything but "started at pc 0 and finished".
[[gnu::noinline]] Value run_resume(Frame& f, JitCode* jc, uint64_t status) {
    if (status == kExitRethrow) {
        std::exception_ptr ep = std::move(jc->pending);
        jc->pending = nullptr;
        if (ep) std::rethrow_exception(ep);
        return Value();
    }
    return run_dispatch(f);   // kExitBail: Frame::pc / Frame::acc say where
}

[[gnu::noinline]] Value run_from(Frame& f, JitCode* jc) {
    const uint32_t pc = f.pc;
    if (pc >= jc->code_size || jc->native_off[pc] == 0) return run_dispatch(f);
    const uint64_t status = jc->entry(&f, jc, jc->rt.base + jc->native_off[pc]);
    if (status == kExitFinished) return f.acc;
    return run_resume(f, jc, status);
}

// Runs the frame in generated code from f.pc until it finishes or has to hand
// the rest to the interpreter.
[[gnu::always_inline]] inline Value run(Frame& f, JitCode* jc) {
    if (__builtin_expect(f.pc == 0 && jc->owner_thread == &g_thread_marker, 1)) {
        const uint64_t status = jc->entry(&f, jc, jc->rt.base + jc->start_off);
        if (__builtin_expect(status == kExitFinished, 1)) return f.acc;
        return run_resume(f, jc, status);
    }
    if (jc->owner_thread != &g_thread_marker) return run_dispatch(f);
    return run_from(f, jc);
}

bool osr(Frame& f, uint32_t target, const Value& acc) noexcept {
    f.osr_budget = INT32_MAX;                       // one offer per activation unless it is taken up
    const BytecodeChunk& chunk = f.chunk;
    if (chunk.jit_state == 2) return false;
    if (chunk.jit_state != 1) {
        init_once();
        if (!g_cfg.enabled) { chunk.jit_state = 2; return false; }
        compile_chunk(chunk);
        if (chunk.jit_state != 1) return false;
    }
    JitCode* jc = chunk.jit.get();
    if (jc->owner_thread != &g_thread_marker || target >= jc->code_size || jc->native_off[target] == 0) return false;
    f.pc = target;
    f.acc = acc;
    f.acc = run(f, jc);
    return true;
}

}  // namespace jit


bool osr_try(Frame& f, uint32_t target_pc, const Value& acc) { return jit::osr(f, target_pc, acc); }

#endif  // QUANTA_JIT_X64
