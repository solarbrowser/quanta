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
| `Isolate::CreateRealm(RealmOptions)` | As above, with options: `installConsole = false` leaves out Quanta's own console (log/error/warn on stdout and stderr), so the host's is the only one. |
| `Isolate::SetUncaughtExceptionHandler(fn)` | Where the engine reports an exception no script was left to catch (a timer, a `queueMicrotask` callback, a `FinalizationRegistry` cleanup): `fn(UncaughtException{realm, exception, info, origin})`. Without one it is printed to stderr. A script that fails in `Evaluate` is not reported this way: its result says so. |
| `Isolate::SetPromiseRejectionHandler(fn)` | `fn(realm, promise, reason, RejectionEvent)`: `Unhandled` the moment a promise is rejected with no handler, `Handled` if one is attached afterwards (HostPromiseRejectionTracker: the host decides when to fire `unhandledrejection`, and cancels it on `Handled`). With a handler set the engine no longer prints unhandled rejections. |
| `Isolate::SetSourcePositionTracking(on)` | Whether calls record where in the source they are, so that an error's stack trace (and `Evaluate`'s `stack`, `line` and `column`) has the line and column of each call. Off by default: about one to four percent on call-heavy code, so a host turns it on while its developer tools are open. Scripts always carry their position tables, so it works for code already loaded; only calls made after it is switched on are placed. A generator's or async function's body frames are not. |
| `Isolate::JsStackEmpty()` | True when no script is running on the thread -- the spec's "JavaScript execution context stack is empty", which is when a host performs a microtask checkpoint after running a callback. |
| `Isolate::CollectGarbage()` | Full collection now. Ordinary ones happen on their own. |
| `Isolate::PerformMicrotaskCheckpoint()`, `RunDueTimers()`, `NextTimerDelayMs()` | The host's turns of the event loop, one per Isolate. See [event-loop.md](event-loop.md). |
| `~Isolate()` | Destroys the realms still alive in it, then frees the heap. |
| `Realm::GetContext()` | The realm's global context -- what `DefineClass` and `DefineGlobal` take. |
| `Realm::Run(fn)` | Runs host code inside the realm: it is the realm that is running, and its context the one engine code that wants a running context finds. For a host that calls into script, or makes values, from outside any native function, so that what it makes belongs to this realm. |
| `Realm::FromContext(ctx)` | The `Realm` a context belongs to; null for one made some other way (`$262.createRealm`) or destroyed. |
| `Realm::Evaluate(source, filename)` | Runs a script in the realm and drains the job queue once. Returns `{ok, exception, error, filename, line, column, stack}`: a syntax error is placed in the source (and `exception` is the `SyntaxError`), an exception thrown by script has the position and frames of its stack. With `Isolate::SetSourcePositionTracking(true)` a frame is placed at the call it is making; without, at where its function is declared. There is no completion value: the engine does not produce one for scripts, so a script that wants to hand something back sets a global or calls a function the host defined. |
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

## Modules

A realm runs ES modules through `Isolate::SetModuleHooks`; without hooks it reads them from the file system, as the CLI does. The engine owns the module map and walks the graph (static imports, `export ... from`, cycles, top-level await, import attributes); the hooks only name, fetch and decorate. A URL is loaded once per realm, and a failed fetch is remembered too.

| | |
|---|---|
| `ModuleHooks::resolve(realm, specifier, referrer, resolved, error)` | An absolute URL for `specifier` as written in the module at `referrer` (empty when the request has none), or false with the message the import fails with. Import maps, bare specifiers, `data:` and `blob:` are the host's. |
| `ModuleHooks::fetch(realm, url, type, done)` | The content of `url`, as a `ModuleSource`: `Script` for an ES module, and for an import with `with { type }` one of `Json` (the engine parses it), `Text`, `Bytes` (an immutable `Uint8Array`) or `Value` (a value the host made, exported as the default as it is: a `CSSStyleSheet`). `Failure(message)` rejects the importers with a `TypeError`. `done` may be called at once or later, from the realm's thread; the graph walk goes on inside it. The host decides which types exist: the engine does not check `type` while hooks are set. |
| `ModuleHooks::initImportMeta(realm, meta, url)` | `meta` is a new object without a prototype. Set `url`, `resolve`, whatever the host adds. Without it `import.meta.url` is the module's URL. |
| `ModuleHooks::dynamicImport(realm, specifier, referrer, type)` | Called for each `import()`. Nothing lets it go on through `resolve` and `fetch`; a message rejects it with a `TypeError` of that message (a CSP that forbids it). |
| `Realm::EvaluateModule(source, url)` | Runs `source` as the module at `url` and returns a promise: fulfilled with the namespace once it and everything it imports have run, rejected with what stopped it (a failed fetch, a `SyntaxError`, a thrown value). |
| `Realm::ImportModule(specifier, referrerUrl, type)` | The same for a module the hooks fetch. |

`InspectError` on a rejection places it in the module it came from: a `SyntaxError` has the file and the line of the parse error. An exception a module body throws has the file and line of the throw when `SetSourcePositionTracking(true)`, and otherwise only the message, as everywhere else. After fetching over the network call `PerformMicrotaskCheckpoint` as for any other promise job.

A realm destroyed while modules are fetching drops them: answering `done` afterwards does nothing. The modules a destroyed realm loaded are kept for the life of the process (a closure or a namespace object another realm holds reaches them).

## Structured clone

`structuredClone(value, { transfer })` is a global of every realm (a host that wants its own overwrites it). The same algorithm is there for `postMessage`, `MessageChannel`, `history.state` and workers, as StructuredSerializeWithTransfer and StructuredDeserializeWithTransfer.

| | |
|---|---|
| `Serialize(ctx, value, options, out)` | Into a `SerializedData`. `options.transfer` is the transfer list, `options.for_storage` is for `history.state` and IndexedDB (a `SharedArrayBuffer` or a host object that says so is then refused). False with the exception pending: a `DataCloneError`, or what a getter threw. |
| `Deserialize(ctx, data)` | Into the realm of `ctx`, whichever realm or Isolate made it: the objects have that realm's prototypes. Undefined with an exception pending on failure. |
| `StructuredClone(ctx, value, options)` | Both, in one realm. |
| `Isolate::SetSerializationHooks(hooks)` | How a host object (a `DOMObject`) is cloned; without hooks it is a `DataCloneError`. |

A `SerializedData` holds no reference to any heap: it can be moved to another thread and deserialized by another Isolate. A `SharedArrayBuffer` in it is the same memory on both sides. A transferred `ArrayBuffer` is moved out of its source, which is detached, and can be received once.

What is cloned: primitives (a lone surrogate, `-0` and `BigInt` included), `Boolean`/`Number`/`String`/`BigInt` objects, `Date`, `RegExp` (without `lastIndex`), `ArrayBuffer` (resizable too), `SharedArrayBuffer`, typed arrays and `DataView` (length-tracking ones stay so), `Map`, `Set`, `Error` and its native subclasses (name, message, `cause`, stack), `Array` (holes and extra properties kept) and plain objects, the own enumerable string-keyed properties only. A class instance arrives as a plain object. Identity and cycles are kept, and a getter that deletes a later property is honoured. Anything else (a function, a symbol, a Proxy, a `WeakMap`, a promise, a module namespace) is a `DataCloneError`: a `DOMException` of that name if the realm has a global `DOMException`, otherwise an `Error` whose `name` is `DataCloneError`. Nesting deeper than a couple of thousand levels is a `RangeError`.

The hooks, per host object:

| | |
|---|---|
| `serialize(realm, object, mode, out, error)` | Fill `out.tag`, `out.bytes` and `out.values` (JS values it holds, cloned along with it, with identity kept: they must be reachable from `object`). False, with `error`, if it is not serializable. With `mode.transferring` the object is in the transfer list: move its state out and leave it unusable, with no values. |
| `isTransferable(realm, object)` | Whether it may be in a transfer list. |
| `deserialize(realm, data, transferred)` | The object, in `realm`; `data.values` are already cloned there. A value of `data.values` cannot refer back to the object being made. |

## Compiling strings

`eval`, the `Function` constructors (`Function`, `GeneratorFunction`, `AsyncFunction`, `AsyncGeneratorFunction`) and string timer handlers compile text at run time. A Content-Security-Policy without `unsafe-eval` forbids that, and Trusted Types requires the text to come from a policy. `Isolate::SetCodeGenerationHooks` is where the host answers; with no hooks everything compiles.

| | |
|---|---|
| `codeForEval(realm, object)` | HostGetCodeForEval: `eval(x)` with an object `x`. The code of it (a `TrustedScript`'s) to have `eval` run that, or nothing to have `eval` hand `x` back as it does any non-string. |
| `transform(realm, kind, originals, parts)` | The place for the Trusted Types default policy. `originals` are the values the script passed (a `Function`'s parameters and then its body; `eval`'s one argument), `parts` their strings, which the host may replace in place. A returned message refuses with a `TypeError`. Optional. |
| `ensureCanCompile(realm, kind, parts)` | HostEnsureCanCompileStrings, on the final strings. A returned message refuses with an `EvalError`, which is what a CSP without `unsafe-eval` gives. |
| `Realm::PrepareCodeString(kind, original, code)` | The same two steps for a host that compiles strings itself (its own `setTimeout`, an inline handler it treats like one). False with the exception pending when refused; `code` may have been replaced. |

`kind` is `DirectEval`, `IndirectEval`, `Function`, `GeneratorFunction`, `AsyncFunction`, `AsyncGeneratorFunction` or `Timer`. The `Function` constructors convert each argument to a string once, before the hooks see them, as `CreateDynamicFunction` does. A string handler passed to the built-in `setTimeout` or `setInterval` goes through the hooks with kind `Timer` and then runs as a classic script when the timer fires; without hooks it is ignored, as before.

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

## Inspecting values

| | |
|---|---|
| `Inspect(ctx, value)` | An `ObjectInfo`: the kind (`Plain`, `Array`, `Function`, `Error`, `Date`, `RegExp`, `Promise`, `Proxy`, `Map`, `Set`, the weak ones, `ArrayBuffer`, `SharedArrayBuffer`, `TypedArray`, `DataView`, the boxed primitives, generators and iterators, `ModuleNamespace`, `Host`), the class name a console prints (the nearest `constructor` by name), an `id` that is the same for the life of an object and differs between objects (what finds a cycle), `[[Prototype]]`, extensibility, and what is inside it that script reaches only through methods: a Promise's state and result, a Proxy's target and handler (or that it is revoked), a boxed primitive's value, a Date's time value, a RegExp's source and flags, sizes, a typed array's element type and lengths, whether a buffer is detached, and for a function its name, length, class/arrow/async/generator/native/bound/constructor flags and source text. None of it runs a getter or a trap. |
| `InspectProperties(ctx, object)` | The own properties as `PropertyInfo` descriptors (strings, then symbols, in order): a data property has its value, an accessor its getter and setter as they are, never called. The values stay alive with the list. A Proxy has none to list without its traps: use its target. |
| `MapEntries(map)`, `SetValues(set)` | A Map's entries as key, value, key, value, ...; a Set's values. A WeakMap's, a WeakSet's and a WeakRef's contents are not there to list, by design. |
| `InspectError(ctx, thrown)` | An `ErrorInfo`: name, message, cause, the engine's stack text and its frames (function, file, line, column). Anything thrown that is not an Error (a string, a number) has `is_error` false and only `message`. |

## Errors

| | |
|---|---|
| `ThrowTypeError(ctx, message)` | Sets the pending exception. Return afterwards. |
| `ThrowRangeError`, `ThrowSyntaxError`, `ThrowReferenceError` | Likewise. |
| `NewError(ctx, kind, message)` | An error object of `ctx`'s realm. `kind` names the global error class (`"RangeError"`, or a `DOMException` the host defined with `DefineGlobal`). |
| `ThrowError(ctx, kind, message)`, `Throw(ctx, value)` | Throw such an error, or any value at all. |
| `HasException(ctx)` | True while one is pending. |
