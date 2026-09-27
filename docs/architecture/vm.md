# VM

```
BytecodeCompiler -> BytecodeChunk -> Interpreter (VM::run)
```

The bytecode is register-based: an operation reads/writes registers directly. Fewer instructions, less memory traffic per operation.

## Threaded Dispatch

Quanta didn't start here. The very first version had no bytecode at all -- it walked the AST directly, evaluating each node as it went. Moving to a real VM meant compiling to bytecode first, and the obvious way to run that bytecode was the obvious way anyone runs bytecode: a `switch(opcode)` loop, one case per instruction, jumping back to the top of the switch after each one.

Switch's performance wasn't good enough. Every instruction returns to the same dispatch point, and a big switch over a hot enum tends to fight the branch predictor and the instruction cache in ways that add up fast across millions of dispatches. So we moved again -- this time to threaded dispatch.

Every opcode is its own C++ function now (`h_gen_LdaLookup`, `h_GetNamedFast`, ...), and one jumps straight into the next with `[[clang::musttail]] return kHandlers[...](...)` -- a real tail call, enforced by the compiler, not something we hope `-O3` finds on its own. Two things fall out of that:

- The C++ call stack never grows across an entire script's execution -- there's no "return to the switch" step to grow it in the first place.
- The compiler gets to optimize each handler on its own terms, instead of one giant switch body sharing a single set of register allocation decisions across every opcode.

## Inline Caches

`obj.x` asking "which shape is this, which slot is x in" from scratch on every read would be far too slow for a dynamic language. Every call site that reads or writes a name carries a `FeedbackSlot` -- it remembers the shape(s) it has already seen.

A `*Fast` handler (`h_GetNamedFast`, `h_LdaLookupFast`) checks this first. Only a miss falls through to the general `h_gen_*` path, which also re-learns the site for next time. A site that sees too many different shapes gives up and goes megamorphic -- no more caching for it.

## Call Paths

Not every function call needs the same amount of ceremony, and we decide how much at compile time, not at call time:

- **`fast_gate`** -- no closures created inside, so no `Environment` is even built. Pure register traffic.
- **`fast_env_gate` / `fast_no_closures`** -- an `Environment` is needed (something's captured), but nothing actually closes over it, so the call can share parts of the caller's `Context` instead of building its own.
- **`call_tree_walker`** -- the general path. Generators, async, class constructors, real closures -- anything the two above can't promise never happens.

Which path a function gets is baked into its `FunctionExecutable` once, at compile time (`fast_gate`/`fast_no_closures` flags) -- a call site just reads the flag and branches, it never re-decides.

`call_tree_walker`'s name is a leftover from when the engine really was one -- see Threaded Dispatch above. It doesn't walk the AST today: like every other path, it compiles to a `BytecodeChunk` and runs it through `VM::run` (`Function::call_tree_walker`, `Function.cpp`). "General path" is what the name actually means now.

See also: [internals/execution.md](../internals/execution.md), [internals/bytecode.md](../internals/bytecode.md).
