# Debugging

## ASan

`make asan` catches memory-safety bugs -- stack-use-after-return is already disabled at compile time (`-fsanitize-address-use-after-return=never`), so it's just build and run.

## Isolating Something That's Actually Wrong

The fastest path is usually to drop a temporary, env-var-gated counter or print directly into the function under suspicion -- something like:

```cpp
if (std::getenv("QUANTA_DEBUG_X")) { ... }
```

right where the behavior is happening. Run the one script that reproduces it, read the numbers, then delete the instrumentation once you know what's going on.

A debugger (gdb or similar) works too -- that's the usual fallback when instrumentation alone doesn't pin it down.

## Touched Something at the Engine's Root

`Context.h`, `Interpreter.h`, or anything else that's widely included -- do a clean build (`make clean && make`) before trusting the result.

An incremental build can silently mix an old and a new struct layout across translation units, and that failure mode doesn't look like a build error, it looks like a bug in your change.

See also: [reference/configuration.md](../reference/configuration.md) for the `QUANTA_GC_*` env vars that help with GC-specific bugs.
