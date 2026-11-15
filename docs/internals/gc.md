# Garbage Collector

Cells live in 16KB `HeapBlock`s, carved from 1MB chunks (`BlockAllocator`). Non-moving -- a cell's address is fixed for its whole life. See [architecture/memory.md](../architecture/memory.md) for why that trade-off was made at all.

## Minor and Major

A minor collection (`run_minor_collection`) only walks unmarked (young) cells. Old-to-young edges are what a write barrier (`Collector::write_barrier`) records into a remembered set as they're created -- the minor starts from that set instead of rescanning the old generation to find them.

A major (`run_major_slice`) is the only one that re-derives the whole reachability graph from scratch (`clear_all_marks()` first); it's split into time-bounded slices (`next_slice_budget()`) so one collection doesn't become one long pause. The mutator is stopped while a slice runs, and marking is not concurrent with it; what runs alongside is the helper threads described below.

There's no separate nursery region behind this generational split. A mark bit surviving from the previous cycle *is* the "old" flag -- sticky, not copied. `Collector::collect_minor`'s own comment calls this out directly.

## Marking on Helper Threads

While a slice (or a minor's or a major's final drain) has enough to trace, helper threads mark with the collecting thread (`ParallelMarkPool`; two helpers where the machine has the cores, `QUANTA_GC_THREADS` changes it). Whether to start them is guessed from what the last cycle of the same kind marked, since the gray list is a stack and its length says little about how much is left. What a helper cannot trace itself (functions, maps, host objects) and the contexts and environments it meets are handed back to the collecting thread, which is why a minor whose cost is mostly context work gains nothing from them.

## Survivor Contexts

A `Context` is not a cell: no mark bit, and no write barrier on most of its state. One whose call has ended but whose environment escaped (a closure may still name it) is handed to the engine's survivor pool. Nothing in such a context changes any more -- what its environments hold has the barrier of environments (`write_barrier_env`) -- so a minor roots only the survivors added since a collection last traced the pool, and the ones kept only by a pending job (no real edge has reached them since the last major). The rest were traced before and their referents are already marked. `Engine::survivor_traced()` is the boundary; both kinds of collection prune the pool afterwards, and a context that no real edge has reached in the current major epoch, and no pending job holds, is deleted. `QUANTA_GC_ROOT_ALL_SURVIVORS=1` roots every survivor in every minor, as it once did, for telling whether a bug lies in skipping them.

Whether a context has already been queued in the current collection is a stamp on the context (`gc_seen_cycle_`), as it is on an environment, not a set of addresses.

## Roots of a Major

The first slice of a major scans the stacks, the contexts and the thread-wide roots, and the executables that are pending (new, or whose feedback changed). The executables that are settled are traced once per major epoch too, but not in that slice: the walk is set up when the cycle opens and done by the following slices (`FunctionExecutable::gc_settled_walk_step`, half of a slice's budget each), and the cycle cannot complete before it has been done. A cursor into the settled list is moved on by `unlink_live` when its executable leaves the list.

## Sweep

A sweep first collects the dead cells of the blocks it has reason to look at (the dirty blocks, for a minor). Blocks of pure cells are freed by their bitmaps without reading a cell. For the rest, the destructors run first, with the bitmaps still stable, and then each block frees what is allocated and not marked in one pass over its words (`HeapBlock::free_dead_bits`); large cells are freed one by one. Under `QUANTA_GC_POISON=1` every dead cell is retired individually instead.

## Profiling

`QUANTA_GC_PROFILE=1` prints a line per phase for every minor and every slice of a major, what the marking helpers did, the cost of a sweep and what it freed, the phases of a major's roots and of `finish_major_cycle`, and, after each major, a profile of the heap's blocks by kind and size class (live cells, free slots, headers). `QUANTA_GC_LOG=1` adds a summary line per collection; `QUANTA_GC_VERIFY=1` checks every marked cell's edges after marking; `QUANTA_GC_STRESS=1` (full) and `=2` (minor) collect at every safepoint; `QUANTA_GC_SLICE_STRESS=1` gives every slice no budget, to surface interleaving bugs.

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
