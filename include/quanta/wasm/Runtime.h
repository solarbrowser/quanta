/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_WASM_RUNTIME_H
#define QUANTA_WASM_RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "quanta/core/runtime/DOMObject.h"
#include "quanta/wasm/Module.h"
#include "quanta/wasm/TypeRegistry.h"

namespace Quanta::Wasm {

struct InstanceObject;

// What stops a function that runs: a trap of its own, or an exception (a JavaScript one, pending in the context, or one thrown with a tag).
enum class TrapKind : uint8_t {
    Unreachable, DivideByZero, IntegerOverflow, InvalidConversion, MemoryOutOfBounds, TableOutOfBounds, UninitializedElement, IndirectCallTypeMismatch,
    NullReference, NullFunction, CastFailure, ArrayOutOfBounds, StackOverflow, UnalignedAtomic, OutOfMemory, DataSegmentOutOfBounds, ElementSegmentOutOfBounds, kCount
};
const char* TrapMessage(TrapKind kind);

// The memory of a module: a reserved range of address space of which the first pages are in use, so that it never moves.
struct MemoryObject : DOMObject {
    MemType type;
    uint8_t* data = nullptr;
    uint64_t pages = 0;
    uint64_t maxPages = 0;    // what can be had at the most (the declared maximum, or what the implementation allows)
    uint64_t reserved = 0;    // the pages of address space reserved
    TracedValue buffer;       // the ArrayBuffer script sees, if it has been asked for

    MemoryObject() = default;
    ~MemoryObject();
    bool Init(const MemType& type);
    // The number of pages before, or -1 if it cannot be grown.
    int64_t Grow(uint64_t delta);
    uint64_t Bytes() const { return pages << 16; }
};

struct TableObject : DOMObject {
    TableType type;  // (canonical)
    TracedList elems;
    uint64_t maxSize = 0;

    bool Init(const TableType& type, const Value& init);
    int64_t Grow(uint64_t delta, const Value& init);
    uint64_t Size() const { return elems.size(); }
};

struct GlobalObject : DOMObject {
    GlobalType type;  // (canonical)
    uint64_t lo = 0, hi = 0;
    TracedValue ref;
};

struct TagObject : DOMObject {
    uint32_t sig = 0;  // the canonical id of its function type
};

// A function that can be called: one of a module, or a JavaScript one that was imported. As a reference it is what funcref values are.
struct FuncObject : DOMObject {
    uint32_t sig = 0;
    InstanceObject* instance = nullptr;
    uint32_t index = 0;
    const CompiledFunction* code = nullptr;
    TracedValue instanceValue;
    TracedValue host;     // the JavaScript function of an imported one
    WeakHandle<Object> wrapper;  // the function script sees, while there is one (it keeps this alive, not the other way)
};

// A thrown exception: the tag it was thrown with and the values it carries (numbers in slots, references as Values).
struct ExceptionObject : DOMObject {
    TracedValue tag;  // the TagObject
    std::vector<uint64_t> numbers;
    TracedList refs;
    TracedValue stack;
};

// The exception for a tag (its values to be filled in), made in the realm of `ctx`; and what the interpreter needs of the rest of the host.
ExceptionObject* NewException(Context& ctx, TagObject* tag);
// An exception that JavaScript threw, as one of WebAssembly (thrown with the JS tag), or the exception itself if it is one already.
ExceptionObject* WrapException(Context& ctx, const Value& thrown);
// Throws it in the context: as the value it carries if it was thrown with the JS tag, as the object if not.
void ThrowException(Context& ctx, ExceptionObject* exception);

struct ModuleObject : DOMObject {
    std::shared_ptr<Module> module;
};

struct InstanceObject : DOMObject {
    std::shared_ptr<Module> module;
    std::vector<FuncObject*> funcs;
    std::vector<TableObject*> tables;
    std::vector<MemoryObject*> memories;
    std::vector<GlobalObject*> globals;
    std::vector<TagObject*> tags;
    std::vector<std::vector<Value>> elems;  // the items of the element segments (emptied when dropped)
    std::vector<bool> dataDropped;
    TracedValue exports;

    void Visit(Visitor& v) {
        for (FuncObject* f : funcs) v.Mark(static_cast<Object*>(f));
        for (TableObject* t : tables) v.Mark(static_cast<Object*>(t));
        for (MemoryObject* m : memories) v.Mark(static_cast<Object*>(m));
        for (GlobalObject* g : globals) v.Mark(static_cast<Object*>(g));
        for (TagObject* t : tags) v.Mark(static_cast<Object*>(t));
        for (const std::vector<Value>& e : elems) for (const Value& x : e) v.Mark(x);
    }
};

// The buffer script was given of a memory is detached when the memory has grown (defined by the JavaScript API).
void NotifyMemoryGrown(Context& ctx, MemoryObject* memory);

// ---- Running ----

class Machine {
public:
    enum class Status { Ok, Trap, Exception };

    static Machine& Current();

    // Calls `function` with the arguments in the slots of the stacks (see Code.h): `n` and `r` are its parameters on each. On Ok, `n` and
    // `r` are overwritten with its results.
    Status Invoke(Context& ctx, FuncObject* function, uint64_t* n, Value* r);

    // Runs a constant expression of the module (see Module::constExprs): its result is in `n` and `r`.
    Status EvaluateConst(Context& ctx, InstanceObject* instance, const CompiledFunction& code, uint64_t* n, Value* r);

    // Calls a function of the host (JavaScript) with the arguments as slots; they are overwritten with its results. Defined by the JavaScript API.
    Status CallHost(Context& ctx, FuncObject* function, uint64_t* n, Value* r);

    TrapKind lastTrap = TrapKind::Unreachable;
};

}  // namespace Quanta::Wasm

#endif
