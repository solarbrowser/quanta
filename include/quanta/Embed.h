/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef QUANTA_EMBED_H
#define QUANTA_EMBED_H

// The one header an embedder includes. It carries no Web API of its own: the
// only concept it adds to the engine is a native object the collector manages
// (DOMObject), plus the small set of operations needed to expose a class built
// on it to script.
//
// Building against it: `./build.sh lib` produces build/lib/libquanta.a, which
// already contains PCRE2, utf8proc and mimalloc. A program needs only
//     clang++ -std=c++20 -I<quanta>/include app.cpp libquanta.a -pthread
// No -D flags: nothing in the headers depends on one. To also have the whole
// process allocate through mimalloc, name build/lib/quanta_mimalloc_override.o on
// the link line; leave it out and the host keeps its own allocator.
//
// Threading: a Runtime and everything allocated in it belong to the thread that
// created it. Any number of Runtimes may live on one thread; none may move to
// another.
//
// Exceptions: a native function does not throw or return an error value. It
// reports failure by calling one of the Throw* functions and returning (the
// returned Value is ignored while an exception is pending). The same flag is
// how the engine reports failure back: after Get, Set, Call, ToUsvUtf8 or
// anything else that can run script, check HasException(ctx) and return.

#include "quanta/core/engine/Context.h"
#include "quanta/core/engine/Engine.h"
#include "quanta/core/gc/Heap.h"
#include "quanta/core/gc/Visitor.h"
#include "quanta/core/runtime/DOMObject.h"
#include "quanta/core/runtime/Object.h"
#include "quanta/core/runtime/Value.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Quanta::Embed {

using Args = std::span<const Value>;

// `newTarget` is undefined unless the function was invoked with `new` (or
// through a subclass's super()), which is how a constructor tells `URL()` from
// `new URL()`. Methods always receive undefined.
using NativeFn = Value (*)(Context& ctx, Value thisValue, Args args, Value newTarget);

// ---- Lifecycle ------------------------------------------------------------

class Runtime {
public:
    // A script's completion value is deliberately not part of this: the engine
    // does not produce one for scripts (only eval has an observable result), so
    // a script that wants to hand something back sets a global, or calls a
    // function the embedder defined.
    struct Result {
        bool ok = false;
        Value exception;
        std::string error;
    };

    // Makes the new runtime's heap the thread's active one.
    static std::unique_ptr<Runtime> Create();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // The realm's global context: what DefineGlobal and DefineClass want.
    Context& GetContext();

    Result Evaluate(std::string_view source, const std::string& filename = "<embed>");

    // Full collection, now. Ordinary collections happen on their own at the
    // interpreter's safepoints.
    void CollectGarbage();

    // The event loop is the host's. Evaluate runs a script and drains the job
    // queue (promise reactions) once, and never waits for a timer. Everything
    // below is how the host takes its turns:
    //
    // Runs every queued promise job, including ones those jobs queue, then
    // reports rejections nobody handled. Call it after calling into script from
    // the host (Call, resolving a promise) -- not from inside a native function,
    // where the script that called it is still on the stack.
    void PerformMicrotaskCheckpoint();
    // Fires the built-in setTimeout/setInterval timers that are due, without
    // waiting for any that are not, and runs the job queue after each. A host
    // that defines its own timers has no use for these two.
    bool RunDueTimers();
    // Milliseconds until the next built-in timer is due (0 if one already is);
    // nothing when there are none.
    std::optional<int64_t> NextTimerDelayMs();

private:
    Runtime() = default;
    std::unique_ptr<Engine> engine_;
};

// ---- Exposing a class to script -------------------------------------------

struct ClassRef {
    Object* constructor;
    Object* prototype;
};

// Creates the interface object and its prototype, linked both ways. The
// prototype inherits parentProto, or Object.prototype when null; it carries
// @@toStringTag set to `name`. Both live as long as the realm, so the pointers
// in ClassRef can be kept in ordinary C++ variables. Not yet visible to
// script: see DefineGlobal.
ClassRef DefineClass(Context& ctx, const char* name, NativeFn constructor, int length,
                     Object* parentProto = nullptr);

// Operations and attributes follow Web IDL: writable, enumerable, configurable.
void DefineMethod(Object* proto, const char* name, NativeFn fn, int length);
void DefineStaticMethod(Object* constructor, const char* name, NativeFn fn, int length);
// A null `setter` makes the attribute read-only.
void DefineAccessor(Object* proto, const char* name, NativeFn getter, NativeFn setter);
void DefineToStringTag(Object* proto, const char* tag);

// Binds the interface object as a global (writable, configurable, not
// enumerable). `ctx` is the realm's global context.
void DefineGlobal(Context& ctx, const char* name, Object* constructor);

// The prototype a constructor should give its new object: newTarget.prototype,
// so `class X extends Base {}` constructs X's instances. Null when newTarget is
// undefined or has no object-valued prototype; the caller then uses the class's
// own prototype (ClassRef::prototype). An exception may be pending on a null
// return.
Object* PrototypeFromNewTarget(Context& ctx, const Value& newTarget);

// ---- Values ---------------------------------------------------------------

inline Value Undefined() { return Value(); }
inline Value Null() { return Value::null(); }
inline Value FromBool(bool b) { return Value(b); }
inline Value FromUint32(uint32_t n) { return Value(n); }
inline Value FromObject(Object* o) { return Value(o); }

inline bool IsUndefined(const Value& v) { return v.is_undefined(); }
inline bool IsNull(const Value& v) { return v.is_null(); }
inline bool IsObject(const Value& v) { return v.is_object_like(); }
inline bool IsCallable(const Value& v) { return v.is_function(); }

inline bool HasException(const Context& ctx) { return ctx.has_exception(); }

// WebIDL ToUint32 (ToNumber, then modulo 2^32). Returns 0 with an exception
// pending if the conversion throws.
uint32_t ToUint32(Context& ctx, const Value& v);

// WebIDL USVString: ToString, with every lone surrogate replaced by U+FFFD.
// The engine stores strings as UTF-8 (lone surrogates as 3-byte sequences), so
// the result is well-formed UTF-8. Empty with an exception pending if ToString
// throws.
std::string ToUsvUtf8(Context& ctx, const Value& v);

// A string value from UTF-8. Malformed input is replaced with U+FFFD rather
// than stored, so the engine never holds invalid UTF-8.
Value FromUtf8(Context& ctx, std::string_view utf8);

// ---- Arrays and properties ------------------------------------------------

Value NewArray(Context& ctx);
void ArrayPush(Context& ctx, const Value& array, const Value& element);

// [[Get]] / [[Set]] with the full semantics (accessors, proxies, inherited
// properties). `object` must be an object or function; anything else is a
// TypeError.
Value Get(Context& ctx, const Value& object, std::string_view name);
bool Set(Context& ctx, const Value& object, std::string_view name, const Value& value);
// Own enumerable string-keyed properties, in property order.
std::vector<std::string> OwnKeys(Context& ctx, const Value& object);

// ---- Calling back into script ---------------------------------------------

Value Call(Context& ctx, const Value& callable, const Value& thisValue, Args args = {});

// ---- Iterators ------------------------------------------------------------

// %IteratorPrototype%: the parent to pass to DefineClass for an iterator class,
// which then gets [Symbol.iterator]() { return this } from it. The class defines
// next() with DefineMethod and builds each result with MakeIterResult.
Object* GetIteratorPrototype(Context& ctx);
Value MakeIterResult(Context& ctx, const Value& value, bool done);

// ---- Promises -------------------------------------------------------------

// A pending promise and the functions that settle it, as `new Promise` hands
// them to its executor: resolve adopts a thenable's state, reject takes any
// reason. Settle with Call(ctx, cap.resolve, Undefined(), Args(&value, 1)), then
// run the host's PerformMicrotaskCheckpoint. The three values are cells like
// any other: a host object that keeps them in C++ members must Mark them in
// Visit.
struct PromiseCapability {
    Value promise;
    Value resolve;
    Value reject;
};
PromiseCapability NewPromiseCapability(Context& ctx);

// ---- Memory ---------------------------------------------------------------

// Tells the collector about memory a host object owns outside the cell heap (a
// decoded image, a response body), so a program that makes many small cells
// holding large buffers collects often enough. Call it once, with the size, when
// the memory is acquired; there is no matching release, because the count is
// reset by each major collection.
void ReportExternalAllocation(size_t bytes);

// ---- Errors ---------------------------------------------------------------

void ThrowTypeError(Context& ctx, const std::string& message);

}

#endif
