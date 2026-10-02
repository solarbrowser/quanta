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
| `Runtime::Create()` | New runtime; its heap becomes the thread's active one. Null on failure. |
| `Runtime::GetContext()` | The realm's global context -- what `DefineClass` and `DefineGlobal` take. |
| `Runtime::Evaluate(source, filename)` | Runs a script and drains the job queue once. Returns `{ok, exception, error}`. There is no completion value: the engine does not produce one for scripts, so a script that wants to hand something back sets a global or calls a function the host defined. |
| `Runtime::CollectGarbage()` | Full collection now. Ordinary ones happen on their own. |
| `Runtime::PerformMicrotaskCheckpoint()`, `RunDueTimers()`, `NextTimerDelayMs()` | The host's turns of the event loop. See [event-loop.md](event-loop.md). |

## Exposing a class

| | |
|---|---|
| `DefineClass(ctx, name, ctor, length, parentProto = nullptr)` | Creates the interface object and its prototype, linked both ways; returns `ClassRef {constructor, prototype}`. The prototype inherits `parentProto` (or `Object.prototype`) and carries `@@toStringTag` = `name`. Both objects live as long as the realm. Not visible to script until `DefineGlobal`. |
| `DefineMethod(proto, name, fn, length)` | Operation on the prototype. Web IDL attributes: writable, enumerable, configurable. |
| `DefineStaticMethod(ctor, name, fn, length)` | Same, on the interface object. |
| `DefineAccessor(proto, name, getter, setter)` | Enumerable, configurable accessor. A null `setter` makes it read-only. |
| `DefineToStringTag(proto, tag)` | Overrides `@@toStringTag` (iterator classes use e.g. `"Foo Iterator"`). |
| `DefineGlobal(ctx, name, ctor)` | Binds the interface object as a global (writable, configurable, not enumerable). |
| `PrototypeFromNewTarget(ctx, newTarget)` | `newTarget.prototype` if it is an object, else null. Use it so subclasses construct their own instances. |

Symbol-keyed members are not part of the surface. For an iterable class, install `[Symbol.iterator]` from script after defining the method (see `tests/embed/embed_test.cpp` for the pattern).

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

Strings are UTF-8 inside the engine; lone surrogates are stored as 3-byte sequences, which is why `ToUsvUtf8` exists.

## Arrays and properties

| | |
|---|---|
| `NewArray(ctx)`, `ArrayPush(ctx, array, element)` | |
| `Get(ctx, object, name)` | Full `[[Get]]`: accessors, proxies, inherited properties. Non-object is a TypeError. |
| `Set(ctx, object, name, value)` | Full `[[Set]]`. |
| `OwnKeys(ctx, object)` | Own enumerable string keys, in property order (symbols excluded). |

## Calling script

| | |
|---|---|
| `Call(ctx, callable, thisValue, args)` | Calls a function. Check `HasException(ctx)` afterwards. |

## Iterators

| | |
|---|---|
| `GetIteratorPrototype(ctx)` | `%IteratorPrototype%`. Pass it as `parentProto` to `DefineClass` for an iterator class; it then gets `[Symbol.iterator]() { return this }`. |
| `MakeIterResult(ctx, value, done)` | The `{value, done}` object `next()` returns. |

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
