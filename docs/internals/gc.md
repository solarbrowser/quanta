# Garbage Collector

Cells live in 16KB `HeapBlock`s, carved from 1MB chunks (`BlockAllocator`). Non-moving -- a cell's address is fixed for its whole life. See [architecture/memory.md](../architecture/memory.md) for why that trade-off was made at all.

## Minor and Major

A minor collection (`run_minor_collection`) only walks unmarked (young) cells. Old-to-young edges are what a write barrier (`Collector::write_barrier`) records into a remembered set as they're created -- the minor starts from that set instead of rescanning the old generation to find them.

A major (`run_major_slice`) is the only one that re-derives the whole reachability graph from scratch (`clear_all_marks()` first); it's split into time-bounded slices (`next_slice_budget()`) so one collection doesn't become one long pause, though it's still single-threaded -- no concurrent marking.

There's no separate nursery region behind this generational split. A mark bit surviving from the previous cycle *is* the "old" flag -- sticky, not copied. `Collector::collect_minor`'s own comment calls this out directly.

## Growth Trigger and Backoff

A major that reclaims little (`swept * kMajorYieldDivisor < marked`) doubles `major_interval` (8 up to a cap of 64) -- the next major waits for proportionally more allocation before it's asked for again. The heap's growth trigger (`growth_needed`) scales with that same interval.

`note_major_yield` is what decides "little" -- and it only counts a major as unproductive once the live set has stopped growing (`still_growing`: more than 25% growth since the last major disqualifies it). A program still *building* its data (a tree, say) also reclaims nothing on its early majors, but for a different reason than an unproductive one -- there's simply no garbage yet -- and `still_growing` is what keeps that case from climbing `major_interval` (and the growth trigger with it) the same way a genuinely unproductive major does.

## The Structural Limit

An object promoted to the old generation can only be proven dead by a major -- a minor's entire premise is *not* rescanning the old generation, so it has no way to notice one of its members died. A workload that builds a long-lived structure and only later tears large parts of it down (a splay tree's rebalancing, for instance) gets no benefit from minors on that garbage; it waits for a major every time.

This falls directly out of the generational split above, not out of anything else in Quanta's design -- worth knowing before reaching for a minor-side fix to a problem that only a major can actually solve.

## Decommit

After every major, `Heap::decommit_idle_memory` -> `BlockAllocator::decommit_idle_chunks` hands a chunk's physical pages back to the OS (`madvise(MADV_DONTNEED)`) once all 64 of its blocks are free. The virtual range stays reserved -- a stale hit just faults in a fresh zero page.

## Fragmentation

Non-moving means no compaction either -- nothing ever consolidates survivors into fewer chunks. A single long-lived object sitting alone in an otherwise-empty 1MB chunk holds that whole chunk hostage, since decommit needs all 64 blocks free at once.

A heap that briefly peaked and then dropped back down doesn't necessarily give back what it peaked at -- it depends on whether the survivors happened to land packed together or scattered one-per-chunk. This is the real cost non-moving buys freedom from pointer-fixup with, and it isn't a gap in the collector to close -- a moving/compacting collector is the only way to consolidate survivors, and that's a different architecture, not a fix within this one.
