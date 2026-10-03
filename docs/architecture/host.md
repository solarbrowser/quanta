# Host

`Engine`/`Context` is what a host drives; `console.cpp` is just its first, and least privileged, consumer -- an embedder never has to go near it.

A program that links Quanta as a library does not use `Engine` directly. It includes `quanta/Embed.h`, which wraps `Engine` in a `Realm` (and an `Isolate` that owns the heap the realms share, and `Runtime` for the two together) and adds the one thing a host needs the engine to know about: a native object the collector manages (`DOMObject`). The engine carries no Web API; a host writes those on top. See [embedding](../embedding/README.md).

## Engine Config

`Engine::Config` is what you hand the engine before anything runs: `max_heap_size`, `initial_heap_size`, `max_stack_size`, `strict_mode`, `expose_test262_globals`, `host_drives_event_loop`.

`host_drives_event_loop` is what lets a host own its event loop: with it set, running a script drains the promise job queue and returns instead of sleeping until every timer has fired, and the host fires timers itself (`EventLoop::run_due_timers`). `Isolate::CreateRealm` sets it; the CLI does not.

`register_function`/`register_object` are how a host reaches back in -- injecting a native function or object into the global scope.

## Isolates and realms

An `Isolate` owns a thread's GC heap and is where its realms meet; an `Engine` is one realm of it, and `Engine(Isolate&, Config)` makes one. `$262.createRealm` and `ShadowRealm` add realms to the Isolate of the one that made them, so all of a thread's realms share cells. What belongs to a realm (the intrinsics: `Array.prototype`, the iterator, generator and collection prototypes, `%ThrowTypeError%`, the primitive wrappers, `%Promise%`, the protectors that guard fast paths) is in its `Realm`, found through the thread's current realm, which a `RealmScope` switches where execution enters one: the engine's entry points, each queued job and timer, a resumed generator or async function, and a call to a function of another realm.

A function belongs to the realm that parsed it (its executable records it), a native to the realm that made it (its closure context is that realm's global `Context`). A realm destroyed while others remain hands its global `Context`, its `Realm` and the contexts its closures escaped into to a surviving realm's survivor pools, where the collector frees each once nothing alive reaches it.

## Builtins

Builtins live one directory per global (`src/core/engine/builtins/{array,promise,regexp,...}/`), each exposing one `register_X_builtins(Context&, Object* function_prototype)` the engine calls in turn while setting up a `Context`.

Adding a global is adding a directory, not touching the ones already there. See [internals/builtins.md](../internals/builtins.md) for how the more involved ones (Promise, generators, Proxy, Map/Set) actually work.

## Native objects

`Object` has no vtable, so a host's own types cannot be `virtual` subclasses of it. `DOMObject` is a `CustomObjectBase` of kind `Host`: the collector's trace and sweep dispatch for it goes through a per-type table of function pointers (`DOMTypeOf<T>::info`) instead of one more `switch` case per class. `Heap::Allocate<T>` is what stamps the table onto a new cell. See [native-objects.md](../embedding/native-objects.md).

## console.cpp

`console.cpp` (the CLI/REPL) is deliberately just a caller of all of the above -- `-c`/`--module`/`--preload` are its own concerns, not the engine's.

See also: [reference/cli.md](../reference/cli.md).
