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
// Threading: an Isolate and everything allocated in it belong to the thread that
// created it, and none may move to another. One Isolate is live on a thread at a
// time; destroy it before making the next, and any number may follow one another.
// Realms are what a browser has several of at once -- see Isolate and Realm below.
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
#include <functional>
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

// What Evaluate reports. A script's completion value is deliberately not part of
// it: the engine does not produce one for scripts (only eval has an observable
// result), so a script that wants to hand something back sets a global, or calls a
// function the embedder defined.
// One line of a stack trace. The position is the call the frame is making when
// Isolate::SetSourcePositionTracking is on, and where its function is declared when it is not
// (line and column are 0 when unknown).
struct StackFrame {
    std::string function;
    std::string filename;
    uint32_t line = 0;
    uint32_t column = 0;
};

// What can be said about a thrown value without running any script of its own: the name, message
// and cause an Error carries, and its stack as frames. Anything other than an Error object that was
// thrown (a string, a number) has is_error false and only `message`, its ToString.
struct ErrorInfo {
    bool is_error = false;
    std::string name;
    std::string message;
    std::string stack;                  // the engine's text, "Name: message\n    at f (file:1:2)..."
    std::vector<StackFrame> frames;
    std::string filename;               // where it was thrown: the top frame, or a syntax error's own place
    uint32_t line = 0;
    uint32_t column = 0;
    bool has_cause = false;
    Value cause;
};

struct EvaluateResult {
    bool ok = false;
    Value exception;
    std::string error;
    // Where it failed, and how it got there. A syntax error has its own position and no frames; an
    // exception thrown by a script has the position and frames of its stack.
    std::string filename;
    uint32_t line = 0;
    uint32_t column = 0;
    std::vector<StackFrame> stack;
};

class Isolate;
class Realm;

// An exception that nobody caught: from a timer, a job, a FinalizationRegistry cleanup. `exception`
// is good for the duration of the call; keep it in a Persistent to keep it longer.
struct UncaughtException {
    Realm* realm;              // null when the realm was not made through this Isolate
    Value exception;
    ErrorInfo info;
    const char* origin;        // "timer", "queueMicrotask", "FinalizationRegistry cleanup"
};
using UncaughtExceptionHandler = std::function<void(const UncaughtException&)>;

// What the host learns about a promise's rejection, for `unhandledrejection` and
// `rejectionhandled`: it is Unhandled the moment a promise is rejected with no handler, and Handled
// if one is attached to it afterwards. The promise and reason are good for the duration of the call.
enum class RejectionEvent { Unhandled, Handled };
using PromiseRejectionHandler =
    std::function<void(Realm* realm, const Value& promise, const Value& reason, RejectionEvent event)>;

// What a host answers a module fetch with. `type` is what the import asked for in its `with { type }`:
// empty for a script module, "json", "css", and so on. A script module's answer is Script; a typed
// module's is Json (the engine parses it), Text, Bytes or Value, whose default export is the result.
struct ModuleSource {
    enum class Kind { Script, Json, Text, Bytes, Value, Failure };
    Kind kind = Kind::Failure;
    std::string text;                  // Script, Json, Text
    std::vector<uint8_t> bytes;        // Bytes: an immutable Uint8Array over them is the default export
    Value value;                // Value: made by the host (a CSSStyleSheet, say), exported as it is
    std::string error;                 // Failure: the message of the TypeError the import rejects with
    Value errorValue;           // Failure: or exactly what to reject with, when the host has one

    static ModuleSource Script(std::string source) { ModuleSource s; s.kind = Kind::Script; s.text = std::move(source); return s; }
    static ModuleSource Json(std::string source) { ModuleSource s; s.kind = Kind::Json; s.text = std::move(source); return s; }
    static ModuleSource Text(std::string text) { ModuleSource s; s.kind = Kind::Text; s.text = std::move(text); return s; }
    static ModuleSource Bytes(std::vector<uint8_t> bytes) { ModuleSource s; s.kind = Kind::Bytes; s.bytes = std::move(bytes); return s; }
    static ModuleSource Default(Value value) { ModuleSource s; s.kind = Kind::Value; s.value = value; return s; }
    static ModuleSource Failure(std::string message) { ModuleSource s; s.kind = Kind::Failure; s.error = std::move(message); return s; }
};

// How modules reach the outside world: where the HTML and ECMAScript module machinery calls the host.
// The engine keeps the module map (a URL is loaded once per realm, a failure is remembered too) and walks
// the graph; the host names, fetches and decorates.
struct ModuleHooks {
    // An absolute URL for `specifier` as written in the module at `referrer` (empty for a request that
    // has none), or false with the message the import fails with. Import maps, bare specifiers, data: and
    // blob: are the host's.
    std::function<bool(Realm* realm, const std::string& specifier, const std::string& referrer,
                       std::string& resolved, std::string& error)> resolve;
    // The content of `url`. `done` can be called at once or later, from the thread the realm lives on, and
    // the graph walk goes on inside it; a host that fetches over the network calls it when the bytes are
    // there, and then runs PerformMicrotaskCheckpoint.
    std::function<void(Realm* realm, const std::string& url, const std::string& type,
                       std::function<void(ModuleSource)> done)> fetch;
    // import.meta of the module at `url` is `meta`, a new object without a prototype: set `url` and
    // `resolve` on it, whatever the host adds.
    std::function<void(Realm* realm, const Value& meta, const std::string& url)> initImportMeta;
    // `import()` evaluated in the module or script at `referrer`: nothing to return lets it go on through
    // resolve and fetch; a message fails the import with a TypeError of that message (a CSP that forbids it).
    std::function<std::optional<std::string>(Realm* realm, const std::string& specifier,
                                             const std::string& referrer, const std::string& type)> dynamicImport;
};

// ---- Structured clone ------------------------------------------------------------------
//
// StructuredSerializeWithTransfer and StructuredDeserializeWithTransfer, for structuredClone, postMessage,
// MessageChannel, history.state and workers. A SerializedData holds no reference to any heap: it can be sent
// to another Isolate, on another thread, and deserialized there. A SharedArrayBuffer in it is the same
// memory on both sides; a transferred ArrayBuffer is moved (the source is detached) and can be received once.
//
// The engine clones primitives, BigInt, wrapper objects, Date, RegExp, ArrayBuffer (also resizable), views,
// Map, Set, Error, Array and plain objects (their own enumerable properties, cycles and identity kept), and
// raises a DataCloneError (a DOMException if the realm has one, else an Error named so) for the rest: functions,
// symbols, Proxies, WeakMaps, promises, ... A host object is cloned only if the hooks say how.
using SerializedData = Quanta::SerializedData;
using SerializeOptions = Quanta::SerializeOptions;
using HostObjectData = Quanta::HostObjectData;
using SerializationMode = Quanta::SerializationHost::Mode;

struct SerializationHooks {
    // The state of a host object, as bytes and the JS values it holds (an ImageData's pixel array); false, with
    // the message for the DataCloneError, if it is not serializable. With mode.transferring the object is in the
    // transfer list: take its state out and leave it unusable (a MessagePort), and give no values.
    std::function<bool(Realm* realm, const Value& object, const SerializationMode& mode, HostObjectData& out,
                       std::string& error)> serialize;
    // Whether `object` may be in a transfer list.
    std::function<bool(Realm* realm, const Value& object)> isTransferable;
    // The object `data` stands for, in `realm`: undefined with an exception pending if it cannot be made.
    // `data.values` are already cloned into the realm. `transferred` for an object that was in a transfer list.
    std::function<Value(Realm* realm, const HostObjectData& data, bool transferred)> deserialize;
};

// False with a DataCloneError (or another exception: a getter that threw) pending.
bool Serialize(Context& ctx, const Value& value, const SerializeOptions& options, SerializedData& out);
// Into the realm of `ctx`. Undefined with an exception pending on failure.
Value Deserialize(Context& ctx, SerializedData& data);
// Both at once, in one realm: what structuredClone does.
Value StructuredClone(Context& ctx, const Value& value, const SerializeOptions& options = {});

// ---- Tasks and timers ------------------------------------------------------------------
//
// A task is a piece of script (a timer's callback) or host code that a realm asks to have run later. The
// engine's own loop runs them by itself under RunDueTimers; a host that has an event loop of its own (task
// sources, throttled background tabs, requestAnimationFrame ordering) takes them over with SetTimerProvider and
// fires each one from its own queue, when it sees fit.
//
// A Task is a handle, cheap to copy; the callback and arguments behind it stay alive for as long as it can
// still run. Drop the handles before the Isolate goes. A realm that is destroyed cancels every task it has.
class Task {
public:
    Task() = default;
    explicit operator bool() const { return impl_ != nullptr; }

    // Runs it in its realm, then performs a microtask checkpoint when no script is running (HTML "clean up
    // after running script"). An exception it throws goes to the uncaught exception handler. A no-op for a
    // cancelled task or a destroyed realm. A one-shot task is spent afterwards; a repeating one stays armed
    // until cancelled, and the host schedules its next run.
    void Run();
    // Drops it and what it holds. clearTimeout does this; a host does it to a task it will not run.
    void Cancel();
    bool IsCancelled() const;

    Realm* GetRealm() const;
    // What script was given back by setTimeout; clearTimeout names it. 0 for EnqueueTask.
    int64_t Id() const;
    // "setTimeout", "setInterval", or the label EnqueueTask was given: the task source.
    const std::string& Source() const;
    bool IsRepeating() const;
    // The delay script asked for, in milliseconds, and the HTML "timer nesting level" for the next run: with
    // 5 or more, the host clamps the delay to 4 ms. It grows by one with each run of a repeating task.
    double DelayMs() const;
    int NestingLevel() const;

    struct Impl;   // opaque

private:
    friend class Realm;
    friend class Isolate;
    friend class EmbedTimerHost;
    explicit Task(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
};

struct TimerProvider {
    // setTimeout, setInterval, setImmediate and EnqueueTask asked for a task. The host keeps it and calls
    // Run() when it is time (and again, for a repeating one, after the delay).
    std::function<void(Task task)> schedule;
    // clearTimeout or clearInterval named `id`, whose task is cancelled already: the host takes it out of
    // its queue. Optional.
    std::function<void(Realm* realm, int64_t id)> cancel;
};

// ---- Named properties objects ---------------------------------------------------------------
//
// `window.foo` for an element with id="foo": the global answers for names the document defines, by way of an
// object in its prototype chain (Window.prototype -> the named properties object -> EventTarget.prototype),
// which is what WebIDL says a global with a named getter has. The host makes the object, wires it into the
// chain, and keeps the names it answers for current.
struct NamedPropertiesHooks {
    // [[GetOwnProperty]] for a name: the value, or false when the name is not one the global answers for.
    std::function<bool(Realm* realm, const std::string& name, Value& out)> get;
    // The supported property names, in tree order for the document.
    std::function<std::vector<std::string>(Realm* realm)> names;
    // [LegacyUnenumerableNamedProperties]: the properties are not enumerable.
    bool unenumerable = true;
};

// A named properties object for `global`, whose [[Prototype]] is `parentPrototype` (EventTarget.prototype, or
// null) and cannot be changed. Put it between the global's interface prototype and that parent:
//   SetPrototypeOf(ctx, windowPrototype, named). It holds the hooks for as long as the object lives: they must
// not capture cells (a Value or a Persistent), and are not called once the realm is destroyed.
// A name is visible when `get` knows it and neither `global` nor anything in its chain above the named properties
// object has a property of that name; those properties are writable and configurable, so assigning to one makes
// an ordinary own property of the global; defining or deleting one, preventing extensions or changing the
// prototype fails.
Value NewNamedPropertiesObject(Context& ctx, const Value& global, const Value& parentPrototype, NamedPropertiesHooks hooks);

// ---- Compiling strings ------------------------------------------------------------------
//
// eval, the Function constructors and string timer handlers compile text at run time, which a
// Content-Security-Policy may forbid (`unsafe-eval`) and Trusted Types may require to come from a policy. The
// hooks are the ECMAScript host hooks for that: HostGetCodeForEval, and HostEnsureCanCompileStrings together
// with the string replacement the Trusted Types default policy makes. With no hooks everything may be compiled.
using CompileKind = Quanta::CompileKind;

struct CodeGenerationHooks {
    // eval(x) where x is an object: the code of it (a TrustedScript's), or nothing to have eval hand x back as it
    // does any non-string.
    std::function<std::optional<std::string>(Realm* realm, const Value& object)> codeForEval;
    // The strings about to be compiled, for the host to replace in place: `originals` are the values the caller
    // passed (the parameters and then the body for a Function; the one argument for eval) and `parts` their
    // strings. Returning a message refuses with a TypeError. Optional.
    std::function<std::optional<std::string>(Realm* realm, CompileKind kind, const std::vector<Value>& originals,
                                             std::vector<std::string>& parts)> transform;
    // The final strings: a message refuses with an EvalError (what a CSP without 'unsafe-eval' gives).
    std::function<std::optional<std::string>(Realm* realm, CompileKind kind, const std::vector<std::string>& parts)> ensureCanCompile;
};

// One global environment: a document's, or a frame's. It has its own intrinsics
// (its own Array.prototype, its own %ThrowTypeError%, ...), so an array made in one
// is not `instanceof Array` in another, as between frames.
//
// Realms of one Isolate share its heap. A value made in one can be handed to another
// (through Get, Set, Call, DefineGlobal) and stays alive for as long as anything
// reaches it, from either. Destroying a Realm drops its timers and its queued jobs
// at once; what it made stays usable for as long as something else still holds it,
// and is freed after that. A Realm must be destroyed from outside script: not from a
// native function it called, nor one called from it.
class Realm {
public:
    ~Realm();

    Realm(const Realm&) = delete;
    Realm& operator=(const Realm&) = delete;

    // The realm's global context: what DefineGlobal and DefineClass want. A class is
    // defined in a realm, and so once for each realm that is to have it: ClassRef's
    // pointers belong to the realm they were made in.
    Context& GetContext();

    EvaluateResult Evaluate(std::string_view source, const std::string& filename = "<embed>");

    // Loads and runs `source` as the module at `url`, with the imports it makes fetched through the
    // Isolate's module hooks. The promise it returns is fulfilled with the module's namespace once it and
    // everything it imports have run (waiting for any top-level await), and rejected with the error that
    // stopped it: a failed fetch, a SyntaxError, an exception, each of them placed in the module it is
    // from (InspectError tells which, and the line). Undefined, with an exception pending, if it could
    // not start. A URL that has been loaded in this realm is not loaded again.
    Value EvaluateModule(std::string_view source, const std::string& url);
    // The same for a module the hooks fetch: `import(specifier)` from the module at `referrerUrl`.
    Value ImportModule(const std::string& specifier, const std::string& referrerUrl = "", const std::string& type = "");

    // For a host that compiles strings itself (its own setTimeout, an `eval`-like API): runs the realm's
    // code-generation hooks on `code`, which they may replace. False with an exception pending when they refuse it.
    // `original` is the value the script passed, for Trusted Types to look at.
    bool PrepareCodeString(CompileKind kind, const Value& original, std::string& code);

    // Queues `fn` as a task of this realm after `delayMs`, labelled `source`, through the timer provider if
    // there is one and otherwise the built-in loop (RunDueTimers). Returns the task, which can be cancelled.
    Task EnqueueTask(const std::string& source, std::function<void()> fn, double delayMs = 0);

    // Runs host code inside this realm: it is the realm that is running, and its context the
    // one engine code that wants a running context finds. What a host that calls into script
    // (or makes values) from outside any native function needs, so that what it makes belongs
    // to this realm and not to whichever was entered last.
    void Run(const std::function<void()>& body);

    // The Realm a context belongs to; null for a realm that was not made through an Isolate
    // (one script made with $262.createRealm, say) or that has been destroyed.
    static Realm* FromContext(Context& ctx);

private:
    friend class Task;
    friend class Isolate;
    Realm(Isolate& isolate, std::unique_ptr<Engine> engine);

    Isolate* isolate_;
    std::unique_ptr<Engine> engine_;
};

// A heap and an event loop, and the realms that run on them. A browser makes one per
// thread and a Realm in it for each document.
class Isolate {
public:
    // Makes the new Isolate's heap the thread's active one. Null while another
    // Isolate is live on this thread, or if the engine could not start.
    static std::unique_ptr<Isolate> Create();
    // Destroys the realms still alive in it, then frees the heap.
    ~Isolate();

    Isolate(const Isolate&) = delete;
    Isolate& operator=(const Isolate&) = delete;

    struct RealmOptions {
        // Quanta's own console: log, error and warn on stdout and stderr. A host that implements console
        // itself turns it off, so that there is no global of Quanta's to overwrite or to see through.
        bool installConsole = true;
    };
    std::unique_ptr<Realm> CreateRealm(const RealmOptions& options);
    std::unique_ptr<Realm> CreateRealm() { return CreateRealm(RealmOptions{}); }

    // Where the engine reports what no script was left to catch. Without a handler an exception is
    // printed to stderr, as the CLI does; with one, the host decides (window.onerror, a console). A
    // script that fails in Evaluate is not reported through this: its result says so.
    void SetUncaughtExceptionHandler(UncaughtExceptionHandler handler);
    // How this Isolate's realms load modules. Without hooks a realm reads them from the file system, as
    // the CLI does. Takes effect for the realms that exist and the ones made after.
    void SetModuleHooks(ModuleHooks hooks);
    // Who runs timers. Without a provider the built-in setTimeout and setInterval are serviced by RunDueTimers.
    void SetTimerProvider(TimerProvider provider);
    // What may be compiled from a string (eval, Function, string timers). Without hooks, anything.
    void SetCodeGenerationHooks(CodeGenerationHooks hooks);
    // How host objects are cloned. Without hooks they are a DataCloneError.
    void SetSerializationHooks(SerializationHooks hooks);
    // With a handler set the engine no longer prints unhandled rejections either.
    void SetPromiseRejectionHandler(PromiseRejectionHandler handler);

    // True when no script is running on this thread: the spec's "JavaScript execution context stack is
    // empty", which is when a host performs a microtask checkpoint after running a callback. False
    // inside a native function, a script, a module or anything either one called.
    bool JsStackEmpty() const;

    // Whether calls record where in the source they are, so that an error's stack trace has the line and
    // column of every call in it (otherwise a frame is placed where its function is declared). Off by
    // default: it costs a few percent on call-heavy code, which a host turns on while its developer tools
    // are open and off after. Scripts are always compiled with the positions, so it takes effect for code
    // already loaded; only calls made after it is switched on are placed. Frames of a generator's or an
    // async function's body are not placed.
    void SetSourcePositionTracking(bool on);

    // Full collection, now. Ordinary collections happen on their own at the
    // interpreter's safepoints.
    void CollectGarbage();

    // The event loop is the host's, and one per Isolate: its jobs and timers belong
    // to whichever realm queued them and run in it. Evaluate runs a script and
    // drains the job queue (promise reactions) once, and never waits for a timer.
    // Everything below is how the host takes its turns:
    //
    // Runs every queued promise job, including ones those jobs queue, then reports
    // rejections nobody handled. Call it after calling into script from the host
    // (Call, resolving a promise), when JsStackEmpty() says no script is running. It may be called from
    // inside a native function: it then runs the jobs that are queued, on top of the script that called it. A
    // job that asks for a checkpoint, or the native a job called, finds the one already under way and
    // returns at once.
    void PerformMicrotaskCheckpoint();
    // Fires the built-in setTimeout/setInterval timers that are due, without
    // waiting for any that are not, and runs the job queue after each. A host
    // that defines its own timers has no use for these two.
    bool RunDueTimers();
    // Milliseconds until the next built-in timer is due (0 if one already is);
    // nothing when there are none.
    std::optional<int64_t> NextTimerDelayMs();

private:
    friend class Realm;
    Isolate() = default;

    std::unique_ptr<Quanta::Isolate> isolate_;
    std::vector<Realm*> realms_;
    UncaughtExceptionHandler uncaught_;
    PromiseRejectionHandler rejection_;
    std::shared_ptr<const ModuleHooks> module_hooks_;
    std::shared_ptr<const SerializationHooks> serialization_hooks_;
    std::shared_ptr<const TimerProvider> timer_provider_;
    std::shared_ptr<const CodeGenerationHooks> code_generation_hooks_;
    void install_code_generation_host(Realm& realm);
    void install_timer_host(Realm& realm);
    void install_module_host(Realm& realm);
    void install_serialization_host(Realm& realm);
};

// An Isolate with one Realm in it, for a host that has no use for more than one.
class Runtime {
public:
    using Result = EvaluateResult;

    // Makes the new runtime's heap the thread's active one.
    static std::unique_ptr<Runtime> Create();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // The realm's global context: what DefineGlobal and DefineClass want.
    Context& GetContext();

    Result Evaluate(std::string_view source, const std::string& filename = "<embed>");

    void CollectGarbage();
    void PerformMicrotaskCheckpoint();
    bool RunDueTimers();
    std::optional<int64_t> NextTimerDelayMs();

private:
    Runtime() = default;
    std::unique_ptr<Isolate> isolate_;
    std::unique_ptr<Realm> realm_;
};

// ---- Per-realm data -------------------------------------------------------

// A host that keeps something per realm (the prototypes of the classes it defined, say)
// cannot keep it in a static: a second realm would take the place of the first. These
// hold one pointer per key per realm instead, `key` being any address the host owns
// (typically a static's). SetRealmData is for setup, with the realm's own context from
// Realm::GetContext(), next to DefineClass. GetRealmData is for natives: it answers for
// the realm the native is running in, which is the one that defined it even when script
// of another realm called it. Called from host code outside any native it answers for the
// realm last entered, so go through Realm::Evaluate or Call first, or keep the realm in
// hand and ask Set's context. The pointer is the host's: the realm neither owns nor
// traces it, so a cell it names has to be kept alive some other way (a ClassRef is).
void  SetRealmData(Context& ctx, const void* key, void* value);
void* GetRealmData(Context& ctx, const void* key);   // null: not set in that realm

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

// A global function (`atob`, `queueMicrotask`, `fetch`): made in the realm of `ctx` and
// bound as a global there (writable, configurable, not enumerable). `fn` gets undefined
// as newTarget, and calling it with `new` is a TypeError as for any built-in function.
void DefineGlobalFunction(Context& ctx, const char* name, NativeFn fn, int length);

// The prototype a constructor should give its new object: newTarget.prototype,
// so `class X extends Base {}` constructs X's instances. Null when newTarget is
// undefined or has no object-valued prototype; the caller then uses the class's
// own prototype (ClassRef::prototype). An exception may be pending on a null
// return.
Object* PrototypeFromNewTarget(Context& ctx, const Value& newTarget);
// The same, but when newTarget.prototype is not an object the answer is the default of
// newTarget's own realm (GetPrototypeFromConstructor's fallback, which is what makes a class
// from one realm extend a constructor of another correctly): the pointer that realm kept
// under `fallbackKey` with SetRealmData, which a host sets to the class's prototype.
Object* PrototypeFromNewTarget(Context& ctx, const Value& newTarget, const void* fallbackKey);
// GetFunctionRealm: the Realm a function belongs to (through a Proxy's target or a bound
// function's), or null if it is none of an Isolate's (see Realm::FromContext). A revoked
// Proxy is a TypeError, pending.
Realm* GetFunctionRealm(Context& ctx, const Value& function);

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

// ---- Values, conversions and objects ---------------------------------------

inline bool IsString(const Value& v) { return v.is_string(); }
inline bool IsNumber(const Value& v) { return v.is_number(); }
inline bool IsBoolean(const Value& v) { return v.is_boolean(); }
inline bool IsSymbol(const Value& v) { return v.is_symbol(); }
inline bool IsBigInt(const Value& v) { return v.is_bigint(); }
inline bool IsNullish(const Value& v) { return v.is_nullish(); }
inline Value FromNumber(double d) { return Value(d); }
inline Value FromInt32(int32_t n) { return Value(static_cast<double>(n)); }
inline double AsNumber(const Value& v) { return v.as_number(); }
inline bool AsBoolean(const Value& v) { return v.as_boolean(); }

// The abstract operations, with their spec semantics: each may run script, and an exception it
// raises is pending afterwards (the return value is then meaningless).
bool ToBoolean(const Value& v);
double ToNumber(Context& ctx, const Value& v);
// ToString, as a string value.
Value ToString(Context& ctx, const Value& v);
// ToPropertyKey: a symbol stays itself, anything else becomes a string. What GetOwnProperty,
// Get and the rest take as a key.
Value ToPropertyKey(Context& ctx, const Value& v);
bool SameValue(const Value& a, const Value& b);
// `v instanceof ctor`, including ctor[Symbol.hasInstance].
bool InstanceOf(Context& ctx, const Value& v, const Value& ctor);
// Array.isArray: sees through Proxies, and throws for a revoked one.
bool IsArray(Context& ctx, const Value& v);
// IsConstructor: a function that can be `new`ed, a Proxy of one included.
bool IsConstructor(const Value& v);

// Objects, in the realm of `ctx`.
Value NewObject(Context& ctx);
Value NewArray(Context& ctx, Args elements);
Value NewString(Context& ctx, std::string_view utf8);
Value NewString(Context& ctx, std::u16string_view utf16);

// [[Construct]]: `new ctor(...args)`, with `newTarget` for Reflect.construct's third argument
// (ctor itself when undefined). A Proxy's construct trap fires. Undefined with an exception
// pending when ctor is not a constructor.
Value Construct(Context& ctx, const Value& ctor, Args args = {}, const Value& newTarget = Value());

// A native function made from a closure. Where NativeFn is a bare pointer, this carries state:
//     NewFunction(ctx, "resolve", 1, [handle](Context& c, Value, Args a, Value) { ... });
// The closure is destroyed with the function, once the collector sweeps it. A cell it captures
// is not seen by the collector: hold it in a Persistent captured by value (which then lives as
// long as the function does), and do not capture the function itself in such a way, or the two
// keep each other alive. `newTarget` is undefined unless the function is called with `new`
// (it is not a constructor: that is a TypeError before the closure runs).
using NativeClosure = std::function<Value(Context& ctx, Value thisValue, Args args, Value newTarget)>;
Value NewFunction(Context& ctx, std::string_view name, int length, NativeClosure closure);

// A property descriptor, in the form of Object.defineProperty's argument: a field is present
// when its has_ flag says so. The Values are not seen by the collector from here: keep a
// Descriptor on the native stack, not in a member.
struct Descriptor {
    Value value, get, set;
    bool has_value = false, has_get = false, has_set = false;
    bool has_writable = false, writable = false;
    bool has_enumerable = false, enumerable = false;
    bool has_configurable = false, configurable = false;
};

// The object internal methods, each as the spec's, so that a Proxy's traps fire and a legacy
// platform object answers for itself. `key` is a string or symbol Value (see ToPropertyKey);
// the string_view forms are for a name known in advance. `object` must be an object.
Value GetIndex(Context& ctx, const Value& object, uint32_t index);
bool HasProperty(Context& ctx, const Value& object, const Value& key);
bool HasProperty(Context& ctx, const Value& object, std::string_view name);
bool DeleteProperty(Context& ctx, const Value& object, const Value& key);
bool DeleteProperty(Context& ctx, const Value& object, std::string_view name);
// [[GetOwnProperty]]: the descriptor, or nothing when there is no such own property.
std::optional<Descriptor> GetOwnProperty(Context& ctx, const Value& object, const Value& key);
std::optional<Descriptor> GetOwnProperty(Context& ctx, const Value& object, std::string_view name);
// [[DefineOwnProperty]]: false when it was refused (no exception is raised for that).
bool DefineProperty(Context& ctx, const Value& object, const Value& key, const Descriptor& descriptor);
bool DefineProperty(Context& ctx, const Value& object, std::string_view name, const Descriptor& descriptor);
// null (Embed::Null()) for no prototype.
Value GetPrototypeOf(Context& ctx, const Value& object);
bool SetPrototypeOf(Context& ctx, const Value& object, const Value& prototype);
bool IsExtensible(Context& ctx, const Value& object);
bool PreventExtensions(Context& ctx, const Value& object);

// ---- Keeping values alive ---------------------------------------------------

// A cell the host holds in a C++ member is invisible to the collector, which finds a
// value only on the native stack or through a native object's Visit(). A Persistent
// is the other way: while it holds a value, the value and everything it reaches stay
// alive, whichever realm made it. What a host needs when it starts something that
// finishes later (a fetch, a timer of its own) and must not lose the promise or the
// objects it will settle it with.
//
// Reset it, or let it go out of scope, to let the value be collected. It belongs to
// the thread that made it, and must be gone before the Isolate its value lives in.
class Persistent {
public:
    Persistent();
    // `ctx` is a context of the realm the value belongs to.
    Persistent(Context& ctx, const Value& value);
    ~Persistent();

    Persistent(Persistent&&) noexcept;
    Persistent& operator=(Persistent&&) noexcept;
    Persistent(const Persistent&) = delete;
    Persistent& operator=(const Persistent&) = delete;

    // The held value; undefined once Reset (or if it never held one).
    Value Get() const;
    bool IsEmpty() const { return !slot_; }
    void Reset();

private:
    struct Slot;
    std::unique_ptr<Slot> slot_;
};

// A list of values the collector can see, for what a call hands back that is more than
// one cell (a std::vector<Value> is not seen). Moves, does not copy.
class ValueList {
public:
    ValueList();
    ~ValueList();
    ValueList(ValueList&&) noexcept;
    ValueList& operator=(ValueList&&) noexcept;

    size_t size() const;
    const Value& operator[](size_t index) const;
    const Value* begin() const;
    const Value* end() const;

    void Append(const Value& value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- Byte buffers ---------------------------------------------------------

// An ArrayBuffer over memory the host owns, without a copy. `release(data, user)` is called when
// the buffer is gone (swept, or detached and unreferenced), from the thread that owns the Isolate
// and while the heap is being swept: it must free the memory and do nothing else with the engine.
// The size is reported to the collector as memory held outside its heap.
Value NewArrayBuffer(Context& ctx, void* data, size_t size, void (*release)(void* data, void* user), void* user);
// A zero-filled one of `size` bytes.
Value NewArrayBuffer(Context& ctx, size_t size);
// A SharedArrayBuffer, which any number of Isolates on different threads may hold the same store of.
Value NewSharedArrayBuffer(Context& ctx, size_t size);
// A Uint8Array viewing `buffer` from `offset`, `length` elements long (a view the buffer's
// realm does not matter to: it is made in the realm of `ctx`).
Value NewUint8Array(Context& ctx, const Value& buffer, size_t offset, size_t length);
bool IsDetached(const Value& bufferOrView);
bool IsSharedArrayBuffer(const Value& v);
// DetachArrayBuffer: true on success; a SharedArrayBuffer cannot be, and the views it had see
// a length of zero.
bool DetachArrayBuffer(Context& ctx, const Value& buffer);
// ArrayBuffer.prototype.transfer(): a new buffer holding the bytes, the old one detached.
Value TransferArrayBuffer(Context& ctx, const Value& buffer);
// BytesOf, writable. The same lifetime rule: good until script next runs.
std::optional<std::span<uint8_t>> MutableBytesOf(const Value& value);

// A new Uint8Array of the realm of `ctx`, over a fresh ArrayBuffer holding a copy of
// `bytes`. An exception may be pending on a undefined return.
Value NewUint8Array(Context& ctx, std::span<const uint8_t> bytes);

// The bytes an ArrayBuffer (or SharedArrayBuffer) holds, or the window of its buffer a
// typed array or DataView views. Nothing if `value` is none of those, or the buffer is
// detached or the view is out of bounds. The span points into the buffer itself, not a
// copy: it is good until script next runs (which may detach, resize or write the
// buffer) and for as long as the value is kept alive.
std::optional<std::span<const uint8_t>> BytesOf(const Value& value);

// A DOMString is a sequence of 16-bit code units and may hold a lone surrogate; a USVString
// may not. The engine stores strings as WTF-8 (UTF-8, with a lone surrogate as the 3-byte
// sequence its code point would have), so these four keep a lone surrogate where the
// functions above would turn it into U+FFFD. Use them for character data that must come back
// out of script exactly as it went in.
//
// `wtf8` is accepted as the engine writes it: well-formed UTF-8, plus surrogates as 3-byte
// sequences. A high surrogate directly followed by a low one is the pair it was; anything
// malformed otherwise becomes U+FFFD.
Value FromWtf8(Context& ctx, std::string_view wtf8);
// From UTF-16 code units, unpaired ones included.
Value FromUtf16(Context& ctx, std::u16string_view units);
// ToString, as the engine holds it (WTF-8, lone surrogates kept). Empty with an exception
// pending if ToString throws.
std::string ToWtf8(Context& ctx, const Value& v);
// ToString as UTF-16 code units, lone surrogates kept.
std::u16string ToUtf16(Context& ctx, const Value& v);

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

// The operations a WebIDL record conversion makes, each as one observable step so a
// Proxy's traps fire in the order the spec gives them:
//   keys = OwnPropertyKeys(ctx, obj)            [[OwnPropertyKeys]]: strings and symbols
//   for each key: if GetOwnEnumerable(ctx, obj, key) then Get(ctx, obj, key)
// A key is a string or a symbol Value. An exception may be pending after any of them.
ValueList OwnPropertyKeys(Context& ctx, const Value& object);
// [[GetOwnProperty]]: true when the property is an own, enumerable one.
bool GetOwnEnumerable(Context& ctx, const Value& object, const Value& key);
// [[Get]] with a string or symbol key.
Value Get(Context& ctx, const Value& object, const Value& key);

// ---- Calling back into script ---------------------------------------------

Value Call(Context& ctx, const Value& callable, const Value& thisValue, Args args = {});

// ---- Iterators ------------------------------------------------------------

// %IteratorPrototype%: the parent to pass to DefineClass for an iterator class,
// which then gets [Symbol.iterator]() { return this } from it. The class defines
// next() with DefineMethod and builds each result with MakeIterResult.
Object* GetIteratorPrototype(Context& ctx);
// GetMethod(object, @@iterator): the function, or undefined when the object has
// none (null and undefined count as none). What tells a record from a sequence
// in a Web IDL union: `new URLSearchParams({a: "b"})` has no iterator method,
// `new URLSearchParams([["a", "b"]])` does. A property that is present but not
// callable is a TypeError, as is a null or undefined `object`; a primitive is
// looked up through its wrapper, so a string has one. An exception may be pending
// on return.
Value GetIteratorMethod(Context& ctx, const Value& object);
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

// ---- Inspecting values ---------------------------------------------------------
//
// What a console or an inspector needs to describe a value: what kind of thing it is, what is inside it
// that script reaches only through its own methods (a promise's result, a Map's entries, a Proxy's
// target), and its properties as descriptors, none of this running a getter or a trap.

enum class ObjectKind {
    Plain, Array, Arguments, Function, Error, Date, RegExp, Promise, Proxy,
    Map, Set, WeakMap, WeakSet, WeakRef, FinalizationRegistry,
    ArrayBuffer, SharedArrayBuffer, TypedArray, DataView,
    BoxedBoolean, BoxedNumber, BoxedString, BoxedSymbol, BoxedBigInt,
    Generator, AsyncGenerator, Iterator, ModuleNamespace, Host, Other
};

enum class PromiseState { Pending, Fulfilled, Rejected };

struct FunctionInfo {
    std::string name;
    uint32_t length = 0;
    bool is_class = false;
    bool is_arrow = false;
    bool is_async = false;
    bool is_generator = false;
    bool is_native = false;
    bool is_bound = false;
    bool is_constructor = false;
    std::string source;        // what Function.prototype.toString answers
};

// The Values in these are not seen by the collector from where they sit: keep an ObjectInfo on the
// native stack, not in a member.
struct ObjectInfo {
    bool is_object = false;    // false for a primitive, which has none of the rest
    ObjectKind kind = ObjectKind::Other;
    // The name of the nearest `constructor` on the prototype chain, as a console prints it ("Map", "Foo").
    std::string class_name;
    // Stays the same for the life of the object and differs between objects: for spotting a cycle.
    uint64_t id = 0;
    Value prototype;                      // [[Prototype]] (null for none); the rest of the chain by GetPrototypeOf
    bool extensible = true;

    std::optional<FunctionInfo> function;

    PromiseState promise_state = PromiseState::Pending;
    Value promise_result;                 // the value or the reason, once settled

    Value proxy_target, proxy_handler;    // both null once revoked
    bool proxy_revoked = false;

    Value primitive_value;                // a boxed primitive's, and nothing else's
    double date_value = 0;                // a Date's time value (NaN for an invalid one)
    std::string regexp_source, regexp_flags;

    size_t size = 0;                      // Array length, Map/Set size, typed array length, ArrayBuffer byteLength
    size_t byte_length = 0;               // typed array, DataView
    std::string element_type;             // a typed array's: "Uint8Array", "Float64Array", ...
    bool detached = false;                // an ArrayBuffer, or a view of one
};
ObjectInfo Inspect(Context& ctx, const Value& value);

// One own property as it is stored: a data property has a value, an accessor its getter and setter as
// they are (never called).
struct PropertyInfo {
    Value key;                            // a string or a symbol
    bool has_value = false;
    Value value;
    Value getter, setter;
    bool has_getter = false, has_setter = false;
    bool writable = false, enumerable = false, configurable = false;
};

// The own properties of an object, strings and then symbols, in property order, with the Values rooted
// for as long as the list lives. A Proxy has none to list without its traps: Inspect gives its target.
class PropertyList {
public:
    size_t size() const { return items_.size(); }
    const PropertyInfo& operator[](size_t i) const { return items_[i]; }
    const PropertyInfo* begin() const { return items_.data(); }
    const PropertyInfo* end() const { return items_.data() + items_.size(); }
    void Add(PropertyInfo info);

private:
    std::vector<PropertyInfo> items_;
    ValueList roots_;
};
PropertyList InspectProperties(Context& ctx, const Value& object);

// A Map's entries as key, value, key, value, ...; a Set's values. A WeakMap's and WeakSet's are not
// there to list, by design, and neither are what a WeakRef holds.
ValueList MapEntries(const Value& map);
ValueList SetValues(const Value& set);

// ---- Inspecting errors --------------------------------------------------------

// Name, message, cause and stack of a thrown value, without running its own toString or any getter
// that throws (one that does is skipped). Works on whatever script threw.
ErrorInfo InspectError(Context& ctx, const Value& thrown);

// ---- Memory ---------------------------------------------------------------

// Tells the collector about memory a host object owns outside the cell heap (a
// decoded image, a response body), so a program that makes many small cells
// holding large buffers collects often enough. Call it once, with the size, when
// the memory is acquired; there is no matching release, because the count is
// reset by each major collection.
void ReportExternalAllocation(size_t bytes);

// ---- Errors ---------------------------------------------------------------

void ThrowTypeError(Context& ctx, const std::string& message);
void ThrowRangeError(Context& ctx, const std::string& message);
void ThrowSyntaxError(Context& ctx, const std::string& message);
void ThrowReferenceError(Context& ctx, const std::string& message);
// An error object of the realm of `ctx`: `kind` is the name of the global error class
// ("Error", "TypeError", "RangeError", ...), so a host that has defined its own (DOMException)
// can make one the same way. Null-valued with an exception pending if there is no such class.
Value NewError(Context& ctx, std::string_view kind, std::string_view message);
// Throws that class, or any value at all.
void ThrowError(Context& ctx, std::string_view kind, std::string_view message);
void Throw(Context& ctx, const Value& exception);

}

#endif
