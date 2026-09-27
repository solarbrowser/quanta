# Memory

## BlockAllocator

Cuts 1MB chunks into 16KB `HeapBlock` regions, and is the process-wide registry of which addresses belong to any heap's block space at all -- `owns_address(p)` is what the conservative stack scanner asks before treating some arbitrary stack word as a possible GC pointer.

## Size Classes

Each `HeapBlock` is dedicated to one size class -- every cell in it is the same size, so the free-list is a plain linked list, no per-allocation bookkeeping. Fragmentation happens between blocks, never inside one.

The classes themselves are static, listed in full in `Heap::kSizeClasses`.

An allocation request always rounds *up* to the next class in this list -- there's no 8-byte class, so an 8-byte request costs a 16-byte cell, same as any request between 1 and 16 bytes. Past `kMaxTier1Size` (4096 bytes), a request stops going through size classes at all and becomes a `LargeCell` instead.

Every cell size, whichever class it lands on, is itself a multiple of 16 (`HeapBlock::kCellAlign`) -- this is what a cell's own address being 16-byte aligned buys `Object`'s spare low bits (see `internals/representation.md`).

Free space never merges across sizes, or even across types at the same size: `Heap::allocate` looks up its block list as `active_block_[kind][cls]` -- both the `CellKind` (`Object`, `String`, `Function`, ...) and the size class index. A cell an `Object` just freed can only ever be handed to another `Object` request of the exact same size class; it is never reachable by a `String` allocation, even one that would fit in the same number of bytes. Nothing needs to split or coalesce cells this way, but it also means there's no scavenging a freed cell of one kind/size for a request of another. Combined with `decommit_idle_chunks` handing a whole chunk back once every block in it is free, this is the extent of what keeps fragmentation down -- it isn't eliminated (see `internals/gc.md`), but it's kept low.

## Large Cells

Anything too big for a block (`LargeCell`) lives outside the block system entirely, in its own chain. `Heap::mark_exact` checks a single flag (`g_any_large_cell`) before deciding whether to even consider a range-probe for one -- on a program with no large cells at all, that check costs nothing.

## SmallMapPool

A fixed-size pool for the small, short-lived maps that come and go constantly (`HybridDescriptorMap`'s overflow map, among others) -- allocation and release are pool operations, not a trip through the general allocator.

## mimalloc

Backs everything that isn't a GC cell but still needs heap memory -- property storage, string internals, array elements.

Deliberately left out of the ASan build's link: mimalloc's own allocator override and ASan's interception both want the same symbols, and linking both in split some allocations onto the side ASan doesn't track, which then reads as a heap-buffer-overflow on memory ASan never saw allocated -- a real, previously-chased false positive, not a hypothetical one.

See also: [architecture/memory.md](../architecture/memory.md), [contributing/debugging.md](../contributing/debugging.md).
