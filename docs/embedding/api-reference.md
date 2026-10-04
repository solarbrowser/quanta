# API reference

Everything lives in `Quanta::Embed` unless noted, declared in `quanta/Embed.h`.

```cpp
using Args = std::span<const Value>;
using NativeFn = Value (*)(Context& ctx, Value thisValue, Args args, Value newTarget);
```

`newTarget` is undefined unless the function was invoked with `new` (or through a subclass's `super()`). Methods always receive undefined.

## Lifecycle

| | |
|---|---|
| `Isolate::Create()` | New Isolate (a heap and an event loop); its heap becomes the thread's active one. Null while another is live on the thread. |
| `Isolate::CreateRealm()` | A new global environment in the Isolate, returned as `unique_ptr<Realm>`. Null on failure. |
| `Isolate::CollectGarbage()` | Full collection now. Ordinary ones happen on their own. |
| `Isolate::PerformMicrotaskCheckpoint()`, `RunDueTimers()`, `NextTimerDelayMs()` | The host's turns of the event loop, one per Isolate. See [event-loop.md](event-loop.md). |
| `~Isolate()` | Destroys the realms still alive in it, then frees the heap. |
| `Realm::GetContext()` | The realm's global context -- what `DefineClass` and `DefineGlobal` take. |
| `Realm::Evaluate(source, filename)` | Runs a script in the realm and drains the job queue once. Returns `{ok, exception, error}`. There is no completion value: the engine does not produce one for scripts, so a script that wants to hand something back sets a global or calls a function the host defined. |
| `~Realm()` | Destroys the realm: its timers and queued jobs go at once, and what it made is freed once nothing else can reach it. |
| `Runtime::Create()` | An Isolate with one Realm in it. `GetContext`, `Evaluate`, `CollectGarbage`, `PerformMicrotaskCheckpoint`, `RunDueTimers` and `NextTimerDelayMs` are those of the Isolate or the Realm. |

## Per-realm data

| | |
|---|---|
| `SetRealmData(ctx, key, value)` | Keeps one `void*` per `key` in the realm `ctx` belongs to (use the realm's own `GetContext()`, at setup next to `DefineClass`). `key` is any address you own. A host that keeps a class's prototypes in a static would have the second realm replace the first; keep them here instead. |
| `GetRealmData(ctx, key)` | For natives: the value of the realm the native is running in, which is the one that defined it even when script of another realm called it. Null if never set there. From host code outside any native it answers for the realm last entered, so go through `Realm::Evaluate` or `Call` first. The realm neither owns nor traces the pointer. |

## Exposing a class

| | |
|---|---|
| `DefineClass(ctx, name, ctor, length, parentProto = nullptr)` | Creates the interface object and its prototype, linked both ways; returns `ClassRef {constructor, prototype}`. The prototype inherits `parentProto` (or `Object.prototype`) and carries `@@toStringTag` = `name`. Both objects live as long as the realm. Not visible to script until `DefineGlobal`. |
| `DefineMethod(proto, name, fn, length)` | Operation on the prototype. Web IDL attributes: writable, enumerable, configurable. |
| `DefineStaticMethod(ctor, name, fn, length)` | Same, on the interface object. |
| `DefineAccessor(proto, name, getter, setter)` | Enumerable, configurable accessor. A null `setter` makes it read-only. |
| `DefineToStringTag(proto, tag)` | Overrides `@@toStringTag` (iterator classes use e.g. `"Foo Iterator"`). |
| `DefineGlobal(ctx, name, ctor)` | Binds the interface object as a global (writable, configurable, not enumerable). |
| `DefineGlobalFunction(ctx, name, fn, length)` | A global function of `ctx`'s realm (writable, configurable, not enumerable), for `atob`, `queueMicrotask`, `fetch` and the like. Not a constructor. |
| `PrototypeFromNewTarget(ctx, newTarget)` | `newTarget.prototype` if it is an object, else null. Use it so subclasses construct their own instances. |

Defining symbol-keyed members is not part of the surface. For an iterable class, install `[Symbol.iterator]` from script after defining the method (see `tests/embed/embed_test.cpp` for the pattern). Reading one is: see `GetIteratorMethod`.

## Native objects

| | |
|---|---|
| `Heap::Allocate<T>(args...)` | `Quanta::Heap`. Constructs a `T : DOMObject` in a heap cell. |
| `DOMObject::Cast<T>(value)` | `T*`, or null if `value` is not a host object of that type (or one declaring `T` as `Parent`). |
| `DOMObject::Visit(Visitor&)` | Hidden by your type's own `Visit`. `v.Mark(...)` takes `Object*`, `String*`, `Symbol*`, `BigInt*` or `Value`. |
| `DOMObject::NoteWrite()` / `NoteWrite(value)` | Write barrier for a new cell reference stored in a `Visit`ed member. |

See [native-objects.md](native-objects.md).

## Values and strings

| | |
|---|---|
| `Undefined()`, `Null()`, `FromBool(b)`, `FromUint32(n)`, `FromObject(o)` | Constructors. |
| `IsUndefined(v)`, `IsNull(v)`, `IsObject(v)`, `IsCallable(v)` | `IsObject` is true for functions too, as in JavaScript. |
| `ToUint32(ctx, v)` | Web IDL `unsigned long` conversion (ToNumber, then modulo 2^32). 0 with an exception pending if it throws. |
| `ToUsvUtf8(ctx, v)` | Web IDL `USVString`: ToString, with every lone surrogate replaced by U+FFFD. Empty with an exception pending if ToString throws. |
| `FromUtf8(ctx, utf8)` | A string value. Malformed UTF-8 is replaced with U+FFFD, so the engine never holds invalid UTF-8. |

| `FromWtf8(ctx, wtf8)` | A string value from WTF-8: a lone surrogate (3-byte sequence) is kept, and a high one directly followed by a low one is the pair it was. Other malformed input becomes U+FFFD. |
| `FromUtf16(ctx, units)` | A string value from UTF-16 code units, unpaired surrogates kept. |
| `ToWtf8(ctx, v)` | ToString as the engine holds it, lone surrogates kept. |
| `ToUtf16(ctx, v)` | ToString as UTF-16 code units, lone surrogates kept. |

Strings are UTF-8 inside the engine; lone surrogates are stored as 3-byte sequences. A Web IDL `DOMString` may carry one and a `USVString` may not: use the first four where character data has to come back out exactly as it went in, `ToUsvUtf8` and `FromUtf8` where the spec says to replace.

## Arrays and properties

| | |
|---|---|
| `NewArray(ctx)`, `ArrayPush(ctx, array, element)` | |
| `Get(ctx, object, name)` | Full `[[Get]]`: accessors, proxies, inherited properties. Non-object is a TypeError. |
| `Set(ctx, object, name, value)` | Full `[[Set]]`. |
| `OwnKeys(ctx, object)` | Own enumerable string keys, in property order (symbols excluded). Does not run a Proxy's traps; for a WebIDL record conversion use the three below. |
| `OwnPropertyKeys(ctx, object)` | `[[OwnPropertyKeys]]`: every own key, strings and symbols, as a `ValueList`. Fires a Proxy's `ownKeys` trap. |
| `GetOwnEnumerable(ctx, object, key)` | `[[GetOwnProperty]]`: true when `key` (a string or symbol `Value`) is an own, enumerable property. Fires `getOwnPropertyDescriptor`. |
| `Get(ctx, object, key)` | `[[Get]]` with a string or symbol `Value` key. Fires `get`. |

A record conversion is `keys = OwnPropertyKeys(...)`, then for each key `GetOwnEnumerable` and, if true, `Get`: the traps fire in the order the spec gives them. An exception from any of them is pending afterwards, as everywhere.

## Keeping values alive

| | |
|---|---|
| `Persistent(ctx, value)` | Holds `value` and everything it reaches against collection, whichever realm made it: a `fetch` in flight keeps its promise and result objects here. Move-only. |
| `Persistent::Get()` | The value; undefined once reset. |
| `Persistent::Reset()`, `IsEmpty()` | Let it go / ask whether it holds one. Destroying a `Persistent` resets it. |
| `ValueList` | A list of values the collector sees, for calls that return more than one cell (`OwnPropertyKeys`). `size()`, `operator[]`, range-for, `Append`. |

A C++ member holding a cell is invisible to the collector; these are what to use when a `DOMObject`'s `Visit()` is not the owner. A `Persistent` belongs to the thread that made it and must be gone before the `Isolate` its value lives in.

## Byte buffers

| | |
|---|---|
| `NewUint8Array(ctx, bytes)` | A `Uint8Array` of `ctx`'s realm over a fresh `ArrayBuffer` holding a copy of `bytes`. |
| `BytesOf(value)` | The bytes of an `ArrayBuffer` or `SharedArrayBuffer`, or the window of its buffer a typed array or `DataView` views; nothing for anything else, a detached buffer or an out-of-bounds view. The span points into the buffer, not a copy: good until script next runs (which can detach, resize or write it) and while the value is kept alive. |

## Calling script

| | |
|---|---|
| `Call(ctx, callable, thisValue, args)` | Calls a function. Check `HasException(ctx)` afterwards. |

## Iterators

| | |
|---|---|
| `GetIteratorPrototype(ctx)` | `%IteratorPrototype%`. Pass it as `parentProto` to `DefineClass` for an iterator class; it then gets `[Symbol.iterator]() { return this }`. |
| `MakeIterResult(ctx, value, done)` | The `{value, done}` object `next()` returns. |
| `GetIteratorMethod(ctx, object)` | `object[Symbol.iterator]`, or undefined when there is none (null and undefined values of the property count as none). A present but non-callable method, or a null/undefined `object`, is a TypeError. This is how a Web IDL union tells a sequence from a record: `new URLSearchParams([["a","b"]])` has the method, `new URLSearchParams({a: "b"})` does not. |

## Promises and memory

| | |
|---|---|
| `NewPromiseCapability(ctx)` | `{promise, resolve, reject}`. |
| `ReportExternalAllocation(bytes)` | Memory owned outside the heap. See [event-loop.md](event-loop.md). |

## Errors

| | |
|---|---|
| `ThrowTypeError(ctx, message)` | Sets the pending exception. Return afterwards. |
| `HasException(ctx)` | True while one is pending. |
