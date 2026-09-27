# Memory

Quanta's GC traces: it periodically walks the live object graph from its roots and reclaims whatever it doesn't reach, rather than reclaiming an object the instant its last reference drops.

## Non-Moving

A cell's address never changes across a collection. This follows directly from Solar Browser's own architectural decisions.

No pointer-fixup pass, no compaction either -- fragmentation is a real cost we just don't pay back automatically. See [internals/memory.md](../internals/memory.md).

## Generational

No separate young-object region exists. A mark bit sticking around from the last cycle IS the "old" flag (`Collector::collect_minor` calls this out directly: sticky mark-bit).

A minor collection only walks what's unmarked; anything old-to-young is caught through a write barrier's remembered set instead of a rescan.

See [internals/gc.md](../internals/gc.md) for the actual mechanics -- and the one real structural limit this buys: an object that dies only after being promoted old is invisible to every minor collection that runs after it. Only a major can ever find it.
