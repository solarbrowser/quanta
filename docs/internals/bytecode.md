# Bytecode

`u8 opcode + operands` -- `r` a register, `k` a constant-pool index, `o` a jump offset relative to the pc after the instruction.

A `BytecodeChunk` holds the code itself alongside `constants`, `names` (interned string pointers), `feedback` (`FeedbackSlot[]`), and `lookup_cache_data` (`LookupCacheEntry[]`).

## Fused Opcodes

Two operations that always happen back to back get one handler instead of two dispatches -- `FUSED_EPILOGUE`/`FUSED_TAIL` are what a handler uses to fall straight into the next instruction's effect without a real `DISPATCH()` round trip.

Not every opcode has a fused form; the compiler only emits one where the pattern is common enough to be worth the second handler.

## Wide Variants

`LdaLookupWide`/`StaLookupWide` and a few others exist because a 16-bit operand field runs out once a chunk has more than 65536 names or constants -- rare, but a real chunk (a big bundled script) can hit it.

Only `LdaLookup`/`StaLookup` got a wide form; see `add_name`'s own comment for why those two specifically.

## FeedbackSlot

The inline cache state for one call site. Two different shapes depending on what the site does:

- **`GetNamed`/`SetNamed`** -- the property name is a compile-time constant here, so caching keys on shape alone. `Entry[]` holds what shapes this site has seen (monomorphic: one, polymorphic: a few); too many distinct shapes and the site goes megamorphic, giving up on caching. `ProtoEntry[]` is the same idea one level up -- a read that resolved through the prototype chain, keyed on receiver shape + prototype + `Object::proto_epoch()`, with `from_descriptor` distinguishing "the value lives in a shape slot" from "the value is a copy sitting right in this entry" (the latter for a non-enumerable builtin method, which has no shape slot to point at).
- **`GetKeyed`/`SetKeyed`** (`KeyedFeedback`) -- the key comes from a register and can differ on every execution of the same instruction, so an entry validates the key too, not just the shape. No proto-entry equivalent -- an inherited read through a keyed site always takes the slow path.

## LookupCacheEntry

For a name resolved through scope (a global, or an outer function's variable) rather than through a receiver. One entry can be one of three things:

1. A direct `Value*` (`slot`) -- an environment's own binding slot, whose address is stable for the chunk's lifetime.
2. A global's shape-slot (`obj_shape` + `obj_slot_index`) -- the object can grow and move its slot storage, so the *index* is cached, not the address, and re-resolved through the object on every hit.
3. A dictionary-mode global's descriptor (`dict_desc`, a `PropertyDescriptor*`) -- once an object passes `Shape::kMaxSlots` and loses its shape entirely, there's no slot to index into, but a descriptor's address in the map is stable, so that gets cached directly instead.

All three are guarded by an epoch: `descriptor_epoch` catches a property being deleted, redefined, or turned into an accessor; `shadow_epoch` catches a nearer binding appearing later (an Annex B block-scoped function is the way that happens after code has already run). A mismatch on either just falls through to re-resolving the name from scratch, same as a cold cache.

See also: [architecture/vm.md](../architecture/vm.md), [internals/execution.md](./execution.md).
