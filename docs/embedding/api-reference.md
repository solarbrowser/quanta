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
| `Realm::Run(fn)` | Runs host code inside the realm: it is the realm that is running, and its context the one engine code that wants a running context finds. For a host that calls into script, or makes values, from outside any native function, so that what it makes belongs to this realm. |
| `Realm::FromContext(ctx)` | The `Realm` a context belongs to; null for one made some other way (`$262.createRealm`) or destroyed. |
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
| `PrototypeFromNewTarget(ctx, newTarget, key)` | The same, but without a `prototype` object the answer is the default of newTarget's *own* realm: the pointer that realm kept under `key` with `SetRealmData` (set it to the class's prototype). This is what makes a class of one realm extend a constructor of another correctly. |
| `GetFunctionRealm(ctx, fn)` | The `Realm` a function belongs to, through a Proxy's target or a bound function's. A revoked Proxy is a pending `TypeError`. |

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

## Values, conversions and objects

| | |
|---|---|
| `IsString`, `IsNumber`, `IsBoolean`, `IsSymbol`, `IsBigInt`, `IsNullish`, `FromNumber`, `FromInt32`, `AsNumber`, `AsBoolean` | Inspecting and making plain values without the engine's headers. |
| `ToBoolean(v)`, `ToNumber(ctx, v)`, `ToString(ctx, v)`, `ToPropertyKey(ctx, v)` | The abstract operations. They can run script: an exception they raise is pending afterwards. `ToPropertyKey` leaves a symbol a symbol. |
| `SameValue(a, b)`, `InstanceOf(ctx, v, ctor)`, `IsArray(ctx, v)`, `IsConstructor(v)` | `instanceof` honours `Symbol.hasInstance`; `IsArray` and `IsConstructor` see through Proxies. |
| `NewObject(ctx)`, `NewArray(ctx, elements)`, `NewString(ctx, utf8 or utf16)` | Values of `ctx`'s realm. |
| `Construct(ctx, ctor, args, newTarget = undefined)` | `[[Construct]]`: `new ctor(...args)`, with `newTarget` for `Reflect.construct`'s third argument. A Proxy's construct trap fires. |
| `NewFunction(ctx, name, length, closure)` | A native function made from a `std::function`, so it carries state. It is destroyed with the function. A cell it captures is invisible to the collector: capture a `Persistent` by value, and do not capture the function itself. Not a constructor. |
| `GetIndex`, `HasProperty`, `DeleteProperty`, `GetOwnProperty`, `DefineProperty` | The object internal methods, each as the spec's, so a Proxy's traps fire in the spec's order and a legacy platform object answers for itself. Keys are string or symbol `Value`s; `string_view` forms exist for a known name. `GetOwnProperty` returns a `Descriptor` (`has_*` flags say which fields are present); `DefineProperty` returns false when it was refused. |
| `GetPrototypeOf`, `SetPrototypeOf`, `IsExtensible`, `PreventExtensions` | Likewise. |

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
| `NewArrayBuffer(ctx, data, size, release, user)` | An `ArrayBuffer` over memory the host owns, with no copy. `release(data, user)` is called when the buffer is gone, while the heap is being swept: free the memory and nothing else. The size is reported to the collector. |
| `NewArrayBuffer(ctx, size)`, `NewSharedArrayBuffer(ctx, size)` | Zero-filled ones. A `SharedArrayBuffer`'s store can be held by Isolates on several threads; `Atomics.wait` and `notify` are the engine's own. |
| `NewUint8Array(ctx, buffer, offset, length)` | A view onto an existing buffer. |
| `IsDetached(v)`, `IsSharedArrayBuffer(v)`, `DetachArrayBuffer(ctx, buffer)`, `TransferArrayBuffer(ctx, buffer)` | Detaching fails for a `SharedArrayBuffer`; transfer is `ArrayBuffer.prototype.transfer()`. |
| `MutableBytesOf(value)` | `BytesOf`, writable. |
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
| `ThrowRangeError`, `ThrowSyntaxError`, `ThrowReferenceError` | Likewise. |
| `NewError(ctx, kind, message)` | An error object of `ctx`'s realm. `kind` names the global error class (`"RangeError"`, or a `DOMException` the host defined with `DefineGlobal`). |
| `ThrowError(ctx, kind, message)`, `Throw(ctx, value)` | Throw such an error, or any value at all. |
| `HasException(ctx)` | True while one is pending. |
