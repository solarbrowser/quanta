# Host

`Engine`/`Context` is the embedding surface; `console.cpp` is just its first, and least privileged, consumer -- an embedder never has to go near it.

## Engine Config

`Engine::Config` is what you hand the engine before anything runs: `max_heap_size`, `initial_heap_size`, `max_stack_size`, `strict_mode`.

`register_function`/`register_object` are how a host reaches back in -- injecting a native function or object into the global scope.

## Builtins

Builtins live one directory per global (`src/core/engine/builtins/{array,promise,regexp,...}/`), each exposing one `register_X_builtins(Context&, Object* function_prototype)` the engine calls in turn while setting up a `Context`.

Adding a global is adding a directory, not touching the ones already there. See [internals/builtins.md](../internals/builtins.md) for how the more involved ones (Promise, generators, Proxy, Map/Set) actually work.

## console.cpp

`console.cpp` (the CLI/REPL) is deliberately just a caller of all of the above -- `-c`/`--module`/`--preload` are its own concerns, not the engine's.

See also: [reference/cli.md](../reference/cli.md).
