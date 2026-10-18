/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_VM_JITLAYOUT_H
#define QUANTA_VM_JITLAYOUT_H

// Data the baseline JIT's generated code reads directly. Kept apart from
// Jit.inc (which needs the interpreter's Frame) so that it can live in
// namespace Quanta, where BytecodeChunk's `JitCode` forward declaration and
// the `friend struct Jit*Layout` declarations in Object and Environment
// already point.

#include "quanta/core/engine/Context.h"
#include "quanta/core/runtime/ArrayBuffer.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/TypedArray.h"
#include "quanta/core/vm/Bytecode.h"
#include <cstddef>
#include <memory>
#include <tuple>
#include <cstdint>
#include <exception>
#include <vector>

// Targets the baseline JIT can run on: x86-64 and AArch64, each on Linux, macOS and Windows,
// each with a way to get executable memory (JitPlatform.h). Anywhere else every function stays
// with the interpreter.
#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__) || defined(_WIN32))
#define QUANTA_JIT_PLATFORM_OK 1
#define QUANTA_JIT_ARCH_X64 1
#elif defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__) || defined(_WIN32))
#define QUANTA_JIT_PLATFORM_OK 1
#define QUANTA_JIT_ARCH_A64 1
#endif

// Which x86-64 calling convention generated code uses to reach the C++ helpers and
// to be entered. Windows uses the Microsoft one (arguments in rcx, rdx, r8, r9 with
// 32 bytes of shadow space, rdi and rsi preserved); everything else uses System V.
// QUANTA_JIT_FORCE_WIN64 selects the Microsoft convention on any x86-64 target by
// declaring every function the generated code touches with clang's ms_abi
// attribute, which is how that half of the code generator is exercised and tested
// without a Windows machine.
#if defined(QUANTA_JIT_ARCH_X64) && (defined(_WIN32) || defined(QUANTA_JIT_FORCE_WIN64))
#define QUANTA_JIT_WIN64 1
#endif
#if defined(QUANTA_JIT_FORCE_WIN64) && !defined(_WIN32)
#define QUANTA_JIT_ABI __attribute__((ms_abi))
#else
#define QUANTA_JIT_ABI
#endif

namespace Quanta {

// One inline cache owned by a JIT'd GetNamed/SetNamed site. The key is the
// receiver's whole tagged shape word (Shape* | ObjectType), so a hit also
// proves the receiver is an Ordinary object -- the only type whose own named
// properties are certain to live in shape slots.
struct JitIC {
    // A site owns a block of this many entries, so a site that sees a few shapes
    // (the common polymorphic case) still hits.
    static constexpr uint32_t kWays = 4;
    enum Kind : uint32_t {
        Empty = 0,
        Own = 1,          // the property is in the receiver's own shape slot
        ProtoSlot = 2,    // inherited: a data slot of `holder`
        ProtoValue = 3,   // inherited: `value` was copied out of the holder's descriptor map
    };
    // ~0 = empty. Not 0: a dictionary-mode Ordinary object has a null shape and
    // an Ordinary type, so its shape word IS 0.
    uint64_t shape_word = ~0ull;
    uint32_t slot = 0;
    uint32_t kind = Empty;
    uint32_t pad = 0;
    uint32_t signature = 0;        // first entry of a block: what the feedback looked like when it was last copied
    // Inherited reads (they mirror FeedbackBody::ProtoEntry, which the interpreter
    // trusts under exactly these conditions): the receiver's [[Prototype]], the
    // global prototype epoch the entry was learned at and, for a cached value,
    // the global descriptor epoch.
    uint64_t proto_word = 0;
    uint64_t proto_epoch = 0;
    uint64_t desc_epoch = 0;
    Object* holder = nullptr;      // GC cells: JitCode is traced through its chunk (jit_trace)
    Value value;
};

// The part of a JitCode the generated code itself reads, addressed from r14.
struct JitRt {
    int32_t sp_counter = 0;            // back-edges left until the next GC safepoint
    uint32_t table_len = 0;
    const uint32_t* table = nullptr;   // bytecode pc -> offset of its native code (0 = none)
    uint8_t* base = nullptr;           // start of the executable mapping
    JitIC* ics = nullptr;              // r15 while generated code runs: the sites' caches
    // The thread's shared stubs (Jit.inc, StubBuilder): the inline-cache logic
    // that every site would otherwise carry its own copy of. Generated code
    // calls them through this table.
    const void* stubs[8] = {};
    uint32_t last_deopt = 0;
    uint32_t last_reason = 0;
    int32_t stress = 0;                // QUANTA_OPT_DEOPT_STRESS: guards left before the next forced deoptimization
    uint64_t opt_runs = 0, opt_exits = 0, opt_cycles = 0, opt_iters = 0;
    uint32_t opt_pc = 0;           // optimizing tier: the frame state a deoptimization came from
    // Optimizing tier: where a deoptimization inside an inlined callee leaves the variables that have no
    // register bank of their own (the interpreter frames are built from them right after).
    uint64_t* inl_scratch = nullptr;
    double one = 1.0;                  // Inc/Dec add +-one from memory instead of building it in a register
    double minus_one = -1.0;
};
static_assert(sizeof(JitIC) == 64, "generated code scales an IC index by 64");

struct JitCode {
    JitRt rt;                          // must stay first: r14 points at it
    void* mem = nullptr;
    size_t mem_size = 0;
    size_t code_bytes = 0;
    // (Frame*, JitCode*, native address to start at) -> 0 finished (result in
    // Frame::acc), 1 rethrow `pending`, 2 continue in the interpreter at Frame::pc
    uint64_t (QUANTA_JIT_ABI *entry)(void*, JitCode*, const void*) = nullptr;
    // The bytecode as the interpreter runs it while stepping one instruction:
    // identical to the chunk's own, except that a stop byte is written over the
    // instruction the step must end at (see jit_step).
    std::vector<uint8_t> code_copy;
    std::vector<uint32_t> native_off;
    std::vector<JitIC> ics;
    std::vector<uint32_t> handler_pcs;
    std::exception_ptr pending;
    const void* owner_thread = nullptr;
    uint32_t code_size = 0;
    // Self-profile: how often generated code left its fast paths (a stub miss or a
    // step) against how often it was entered. A function that keeps missing is
    // faster interpreted, and is switched off (see jit::tally).
    uint32_t entries = 0;
    uint32_t slow = 0;
    bool disabled = false;
    uint32_t start_off = 0;        // native offset of the instruction at pc 0
    bool has_handlers = false;     // the chunk has try regions: a step must stop at their landing pads

    // Optimizing tier (Opt.inc). Separate from `entry`/`native_off` above: its code can be entered
    // only at the points listed in each unit, and a chunk that was never baseline-compiled still
    // gets a JitCode (entry == nullptr) because stepping needs code_copy and the offset table.
    // One unit per loop nest that got hot.
    struct OptUnit {
        void* mem = nullptr;
        size_t mem_size = 0;
        size_t code_bytes = 0;
        uint8_t* base = nullptr;
        uint64_t (QUANTA_JIT_ABI *entry)(void*, JitRt*, const void*) = nullptr;
        std::vector<std::pair<uint32_t, uint32_t>> entries;   // (bytecode pc, native offset)
        // What the generated code addresses (r14): the GC poll counter. Heap-allocated so that its
        // address survives the unit being moved between JitCodes when the baseline JIT replaces one.
        std::unique_ptr<JitRt> rt = std::make_unique<JitRt>();
        uint32_t deopts = 0;
        // Inlined callees (index 0 is the root and unused): what rebuilding their interpreter frames needs.
        struct InlFrame {
            const BytecodeChunk* chunk = nullptr;
            int32_t parent = -1;
            uint32_t var_base = 0, nregs = 0, call_pc = 0, call_len = 0;
        };
        // What an inlined `new` allocates (helper_new_object): the object takes its final shape at once.
        struct NewDesc { Shape* from = nullptr; Shape* to = nullptr; Object* proto = nullptr; uint32_t nslots = 0; };
        std::vector<std::unique_ptr<NewDesc>> descs;
        // A native function a unit calls directly (helper_call_native): what it may be given, and how many arguments.
        struct NativeDesc { Function* fn = nullptr; uint8_t argc = 0; uint8_t recv = 0; bool any_arg = false; uint8_t special = 0; };   // special: 1 charCodeAt, 2 charAt
        std::vector<std::unique_ptr<NativeDesc>> natives;
        // A property access the unit makes through the interpreter's generic path (K::Rt): the name, the site's feedback,
        // the function the access is made for, and the root's pc a throw is reported at.
        struct RtDesc { const std::string* name = nullptr; FeedbackSlot* slot = nullptr; Function* owner = nullptr; uint32_t pc = 0; bool is_length = false; };
        std::vector<std::unique_ptr<RtDesc>> rtdescs;
        std::vector<InlFrame> inl;
        struct InlState { int32_t func = 0; uint32_t pc = 0; std::vector<uint32_t> vars; };   // vars: global variable numbers
        std::vector<InlState> inl_states;       // by deopt state id; func 0 for a state of the root
        uint32_t root_vars = 0;                 // variables [0, root_vars) live in the root's register bank
        uint32_t inl_words = 0, inl_depth = 0;  // arena words and frames the deepest inlined chain needs
        std::unique_ptr<uint64_t[]> inl_scratch_mem;   // the buffer JitRt::inl_scratch points at
        bool inl_strict = false;                // the strictness the inlined callees were checked against
        uint64_t runs = 0, exits = 0;
        uint32_t first_pc = 0;
        std::vector<std::tuple<uint32_t, const char*, int32_t>> guards;   // deopt state id -> (pc, kind of the guard, phi variable or -1)
    };
    std::vector<OptUnit> opt_units;
    uint8_t opt_state = 0;         // 0 on, 2 given up on (too many deoptimizations)
    uint32_t opt_deopts = 0;
};

// Marks the cells a chunk's JIT caches keep alive (called from BytecodeChunk::trace).
void jit_trace(const JitCode* code, Visitor& v);

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

// Field offsets of Object the generated code uses.
struct JitObjectLayout {
    static size_t proto() { return offsetof(Object, proto_); }         // TaggedProto word
    static size_t shape() { return offsetof(Object, shape_); }         // TaggedShapePtr word
    static size_t butterfly() { return offsetof(Object, butterfly_); } // Value* to shape slot 0
    static size_t descriptor_type() { return offsetof(PropertyDescriptor, type_); }       // 0: a data descriptor
    static size_t descriptor_value() { return offsetof(PropertyDescriptor, value_); }
    static size_t function_closure_environment() { return offsetof(Function, closure_environment_); }
    static size_t function_executable() { return offsetof(Function, executable_); }   // FunctionExecutable* (the ref holds only it)
    static constexpr uint64_t kHasDescriptors = Object::kHasDescriptors;
    static constexpr uint64_t kProtoFlagMask = 0x7;                    // flag bits below kHasDescriptors
    static constexpr uint64_t kShapeTypeMask = 0x1F;                   // ObjectType lives in the shape word's low bits
    // ButterflyCore sits one Value below the butterfly pointer; its first
    // word is shape_capacity with three flag bits on top.
    static constexpr uint32_t kShapeCapacityMask = ~Object::kButterflyFlagBits;
    static constexpr uint32_t kDenseVerifiedBit = Object::kDenseVerifiedBit;
    static constexpr uint32_t kExtrasBit = Object::kButterflyExtrasBit;
    // element i lives at butterfly_[-kElementBase - i]; the extras (elements_length
    // first) sit at byte offset -kExtrasBack from butterfly_.
    static constexpr int64_t kElementBase = static_cast<int64_t>(Object::kFullHeaderWidths) + 1;
    static constexpr int64_t kExtrasBack = static_cast<int64_t>(sizeof(ButterflyCore) + sizeof(ButterflyExtras));
    static constexpr size_t kElementsLengthOff = offsetof(ButterflyExtras, elements_length);
    // The ObjectTypes the JIT tests for.
    static constexpr uint64_t kOrdinaryType = static_cast<uint64_t>(Object::ObjectType::Ordinary);
    static constexpr uint64_t kArrayType = static_cast<uint64_t>(Object::ObjectType::Array);
    // The address of this thread's epoch counters. JIT code only ever runs on
    // the thread that compiled it (checked at entry), so the address is stable.
    static const uint64_t* proto_epoch_addr() { return &Object::proto_epoch_; }
    static const uint64_t* descriptor_epoch_addr() { return &Object::descriptor_epoch_; }
};

// A typed array view and the buffer behind it, as generated code reads them.
struct JitTypedLayout {
    using TA = TypedArrayBase;
    static size_t buffer() { return offsetof(TA, buffer_); }                 // shared_ptr<ArrayBuffer>: the first word is the pointer
    static size_t byte_offset() { return offsetof(TA, byte_offset_); }
    static size_t length() { return offsetof(TA, length_); }
    static size_t array_type() { return offsetof(TA, array_type_); }         // a 32-bit enum
    static size_t length_tracking() { return offsetof(TA, is_length_tracking_); }
    static size_t buf_store() { return offsetof(ArrayBuffer, store_); }      // shared_ptr<BackingStore>: the first word is the pointer
    static size_t buf_detached() { return offsetof(ArrayBuffer, is_detached_); }
    static size_t buf_resizable() { return offsetof(ArrayBuffer, is_resizable_); }
    static size_t store_data() { return offsetof(ArrayBuffer::BackingStore, data); }
    static constexpr uint64_t kTypedArrayType = static_cast<uint64_t>(Object::ObjectType::TypedArray);
};

// Fields of a chunk's name-lookup cache entry (BytecodeChunk::LookupCacheEntry) the generated code reads.
struct JitLookupLayout {
    using E = BytecodeChunk::LookupCacheEntry;
    static constexpr size_t kSize = sizeof(E);
    static size_t env() { return offsetof(E, env); }
    static size_t slot() { return offsetof(E, slot); }
    static size_t obj_shape() { return offsetof(E, obj_shape); }
    static size_t descriptor_epoch() { return offsetof(E, descriptor_epoch); }
    static size_t obj_slot_index() { return offsetof(E, obj_slot_index); }
    static size_t shadow_epoch() { return offsetof(E, shadow_epoch); }
    static size_t dict_desc() { return offsetof(E, dict_desc); }
};

struct JitEnvLayout {
    using Entry = Environment::SlotMap::InlineEntry;
    static constexpr uint32_t kInlineSlots = static_cast<uint32_t>(Environment::SlotMap::kInlineCapacity);
    static size_t ctx_lexical_environment() { return offsetof(Context, lexical_environment_); }
    // Byte offsets, from the Environment, of inline slot `index`'s key, value and flags.
    static size_t entry(uint32_t index) {
        return offsetof(Environment, slots_) + offsetof(Environment::SlotMap, inline_entries) + index * sizeof(Entry);
    }
    static size_t key() { return offsetof(Entry, key); }
    static size_t value() { return offsetof(Entry, slot) + offsetof(Environment::BindingSlot, value); }
    static size_t initialized() { return offsetof(Entry, slot) + offsetof(Environment::BindingSlot, initialized); }
    static size_t mutable_flag() { return offsetof(Entry, slot) + offsetof(Environment::BindingSlot, mutable_flag); }
    static size_t binding_object() { return offsetof(Environment, binding_object_); }
    static size_t outer() { return offsetof(Environment, outer_environment_); }
    // Slots past the inline ones: a pointer to an exact-sized array that is never reallocated.
    static size_t ext() { return offsetof(Environment, slots_) + offsetof(Environment::SlotMap, ext); }
    static size_t ext_capacity() { return offsetof(Environment, slots_) + offsetof(Environment::SlotMap, ext_capacity); }
    static constexpr size_t kEntrySize = sizeof(Entry);
    static constexpr uint32_t kMaxSlots = static_cast<uint32_t>(Environment::SlotMap::kMaxReservedSlots);
    static const uint32_t* shadow_epoch_addr() { return &Environment::s_binding_shadow_epoch; }
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

}  // namespace Quanta

#endif
