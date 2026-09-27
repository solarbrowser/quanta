# Builtins

For how a builtin gets registered into a `Context` at all, see [architecture/host.md](../architecture/host.md). This is about what's inside the more involved ones once they are.

## Generators, Async Functions, Async Generators: One Mechanism, Not Three

All three (`Generator`, `AsyncFunction`, `AsyncGenerator`) suspend and resume through the same thing: a real, stackful coroutine (`FiberState`, backed by `minicoro`), not a compile-time state-machine transform or a re-entrant tree-walk. `yield`/`await` calls `quanta_fiber_yield`, which records the fiber's stack pointer and switches back to the caller's own stack; resuming switches back and picks up exactly where the C++ call stack left off, locals and all.

The fiber's stack is its own allocation (`FiberStackPool`, not part of the owning `Generator`/`AsyncFunction` GC cell -- keeping it out is what stops those cells from ballooning into a much bigger size class), and the GC only scans the *live* part of it: from the recorded suspend point down to the stack's own high end, not the whole thing, since everything below where a fiber stopped is dead by construction.

## Promise

A per-`Context` microtask queue (`queue_microtask`/`drain_microtasks`), not a separate scheduler. `then`/`catch`/`finally` register a `ThenRecord` (handler pair + child promise); settling a promise (`fulfill`/`reject`) walks its own `then_records_` and queues each child's reaction as a microtask.

`take_settled_value()` is the one path every `await` and every promise-adopting-another-promise goes through: reading a rejection's reason this way marks `[[PromiseIsHandled]]`, on the reasoning that the reason is already on its way to being rethrown or handed to another promise, so it counts as handled the same way `.then()` would.

## Proxy / Reflect

`Proxy` holds `target_`/`handler_` and one trap method per spec internal method (`get_trap`, `set_trap`, `has_trap`, `delete_trap`, `own_keys_trap`, `get_prototype_of_trap`, ...) -- a fairly direct, one-to-one mapping to the spec's own [[Get]]/[[Set]]/[[HasProperty]]/... table, rather than funneling through `Object`'s ordinary property path.

## Map / Set

Both keep a vector for insertion order plus an `unordered_map<Value, uint32_t, SameValueZeroHash, SameValueZeroEqual>` (`index_`) from key to that vector's position -- not `==`/`===` but the spec's own SameValueZero (the algorithm that makes `NaN` equal to itself as a Map key, unlike everywhere else in the language).

## What's Not Supported

`tail-call-optimization`, `Temporal`, and `immutable-arraybuffer` are not supported yet.
