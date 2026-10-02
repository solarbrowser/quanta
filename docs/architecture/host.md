# Host

`Engine`/`Context` is what a host drives; `console.cpp` is just its first, and least privileged, consumer -- an embedder never has to go near it.

A program that links Quanta as a library does not use `Engine` directly. It includes `quanta/Embed.h`, which wraps `Engine` in a `Runtime` and adds the one thing a host needs the engine to know about: a native object the collector manages (`DOMObject`). The engine carries no Web API; a host writes those on top. See [embedding](../embedding/README.md).

## Engine Config

`Engine::Config` is what you hand the engine before anything runs: `max_heap_size`, `initial_heap_size`, `max_stack_size`, `strict_mode`, `expose_test262_globals`, `host_drives_event_loop`.

`host_drives_event_loop` is what lets a host own its event loop: with it set, running a script drains the promise job queue and returns instead of sleeping until every timer has fired, and the host fires timers itself (`EventLoop::run_due_timers`). `Runtime::Create` sets it; the CLI does not.

`register_function`/`register_object` are how a host reaches back in -- injecting a native function or object into the global scope.

## Builtins

Builtins live one directory per global (`src/core/engine/builtins/{array,promise,regexp,...}/`), each exposing one `register_X_builtins(Context&, Object* function_prototype)` the engine calls in turn while setting up a `Context`.

Adding a global is adding a directory, not touching the ones already there. See [internals/builtins.md](../internals/builtins.md) for how the more involved ones (Promise, generators, Proxy, Map/Set) actually work.

## Native objects

`Object` has no vtable, so a host's own types cannot be `virtual` subclasses of it. `DOMObject` is a `CustomObjectBase` of kind `Host`: the collector's trace and sweep dispatch for it goes through a per-type table of function pointers (`DOMTypeOf<T>::info`) instead of one more `switch` case per class. `Heap::Allocate<T>` is what stamps the table onto a new cell. See [native-objects.md](../embedding/native-objects.md).

## console.cpp

`console.cpp` (the CLI/REPL) is deliberately just a caller of all of the above -- `-c`/`--module`/`--preload` are its own concerns, not the engine's.

See also: [reference/cli.md](../reference/cli.md).
