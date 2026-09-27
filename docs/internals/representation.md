# Representation

## Value

`QUIET_NAN | TAG_*` in the top bits, payload/pointer in the low 48.

| Tag | Payload | Note |
|---|---|---|
| `TAG_UNDEFINED` (0) | -- | the default -- a plain `Value()` |
| `TAG_NULL` | -- | |
| `TAG_FALSE` | -- | |
| `TAG_TRUE` | -- | |
| `TAG_STRING` | pointer | |
| `TAG_SYMBOL` | pointer | |
| `TAG_BIGINT` | pointer | |
| `TAG_OBJECT` | pointer | |
| `TAG_FUNCTION` | pointer | |
| `TAG_NAN` | -- | a real `NaN` value, distinct from the tag scheme's own use of the NaN space |
| `TAG_NEG_INF` | -- | |
| `TAG_POS_INF` | -- | |
| `TAG_VM_TDZ` | -- | never reaches user code -- an internal sentinel meaning "this binding is in its temporal dead zone," not a value a script can observe or produce |

Anything not in this table is a real `double`, stored as-is -- the tags only exist because IEEE 754 leaves that much of NaN's bit pattern unused. `sizeof(Value)` is 8 bytes, always.

## Concrete Sizes

`*` marks a size that's been rounded up to its heap size class (see [internals/memory.md](./memory.md)) -- the raw `sizeof` is smaller.

| Type | Size | What it is |
|---|---|---|
| `Value` | 8 bytes | one NaN-boxed word |
| `Object` | 32 bytes\* | every object pays this before any property storage -- most builtins (`Date`, `Map`, `Set`, ...) never cost more than this, just a different `ObjectType` tag; see the breakdown below |
| `Function` | 80 bytes | the one builtin type with a real C++ layout of its own, on top of `Object`; see the breakdown below |
| `RegExp` | 128 bytes\* | the compiled-pattern side (PCRE2 code pointer, flags, named groups, backtrack engine) -- not the same object as the JS-visible `Object` with `ObjectType::RegExp`, which just holds a pointer to one of these |
| `BigInt` | 32 bytes | |
| `Symbol` | 80 bytes | |
| `Environment` | 216 bytes | a scope's bindings -- see [architecture/vm.md](../architecture/vm.md) for how a call decides whether it needs one at all |
| `Context` | 208 bytes | one JS realm's engine-side state; not a GC cell (lives on the C++ stack per call, occasionally pinned as a survivor -- see [internals/gc.md](./gc.md)) |
| `Shape` | 128 bytes | **not GC-managed at all** -- thread-local, effectively immortal for the thread's life, shared across every object with the same layout (see [architecture/runtime.md](../architecture/runtime.md)) |
| `PropertyDescriptor` | 40 bytes | one dictionary-mode property's value + attributes |
| `HybridDescriptorMap` | 216 bytes | the inline-4 array + summary bits + the (empty until spilled) overflow map header -- see below |
| `ButterflyCore` | 8 bytes | one `Value`-width -- the always-present part of an object's out-of-line storage |
| `ButterflyExtras` | 16 bytes | two `Value`-widths -- the rest, allocated only when an object actually needs it (elements, a dictionary map, ...) |
| `String` | 48 bytes | |
| `BytecodeChunk` | 128 bytes | the fixed struct only -- `code`/`constants`/`names`/`feedback`/`lookup_cache_data` are separately owned, dynamically-sized storage this struct points to, not counted here |

### `Object`'s 3 Words

Each is a tagged pointer doing two jobs at once:

| Word | Field | Holds | What's packed into its spare bits |
|---|---|---|---|
| 1 | `proto_` (`TaggedProto`) | `[[Prototype]]` pointer | `is_extensible` (objects are 16-byte aligned, so the low 4 bits are always free) |
| 2 | `shape_` (`TaggedShapePtr`) | `Shape*` | `ObjectType` (5 bits -- `Shape` is `alignas(32)`, so the low 5 bits are always free) |
| 3 | `butterfly_` | `Value*` | nothing -- `nullptr` until the object needs elements, shape slots, or `RareExtras` |

`ObjectType` living inside `shape_` (not its own field) is why `sizeof(Object)` is 24, not 32 -- no dedicated byte anywhere holds "this is a Date"; it's five bits borrowed from a pointer that has to exist anyway. The 32 bytes it actually occupies on the heap (the table above) is a separate, later rounding -- see [internals/memory.md](./memory.md).

Type-specific state that can't fit a shape slot (a `Date`'s `[[DateValue]]`, a class's private-field brand) goes in `InternalSlots` instead -- a `(key, Value)` pair store, 2 entries inline before spilling to a map, that doesn't exist at all until something actually asks for a slot.

### `Function`'s Own 56 Bytes

On top of the `Object` base:

| Field | Size | Holds |
|---|---|---|
| `executable_` | 8 | `ExecutableRef<const FunctionExecutable>` -- shared, decl-site-scoped data (AST/bytecode/caches) |
| `closure_context_` | 8 | the `Context*` captured at creation |
| `closure_environment_` | 8 | the lexical `Environment*` captured at creation |
| `prototype_` | 8 | lazily-built `.prototype`, mutable for that reason |
| `arrow_this_` | 8 | an arrow's captured `this` (fits padding the class already had) |
| ~13 packed flag bits | 2 | `function_kind_`, `is_native_`, `is_arrow_`, `is_strict_`, `prototype_pending_`, and the rest -- one 16-bit word instead of 12+ separate bytes |
| `instance_data_` | 8 | one `void*`, bundling whatever rare per-instance state actually applies (`NativeFunctionData`, `InstanceFeedback`, `ClassSlots`, ...) -- null until any of them is ever needed |

That's 48 bytes of pointers/`Value` + 2 bytes of packed flags, padded up to 56 -- 24 + 56 = 80, exactly the 80-byte size class.

## Shape Transitions and Dictionary Mode

`Shape::kMaxSlots` is 128. Past it, `migrate_to_dictionary_mode()` copies every shape-slot property into `HybridDescriptorMap` and sets `shape_` to `nullptr` -- that null is the tell every other function checks (`cacheable_dictionary_data`, for instance) to know an object has left shape-slot storage for good.

## HybridDescriptorMap

`kInlineCapacity = 4` -- a small, flat `std::array` first; spills into an `unordered_map` (`OverflowMap`, its own pool allocator) past that.

A 64-bit `key_bits_` summary answers most "is this key here" misses without a real lookup at all -- derived from a key's length plus its first and last byte, it can only ever be wrong in the safe direction (claiming a key that isn't there costs a lookup that then fails; it can never deny one that is).

## Epoch-Based Invalidation

`Object::descriptor_epoch()` is a single global counter, bumped whenever a descriptor is deleted, redefined, or turned into an accessor. A cache entry keeps its own copy from when it was made; a mismatch on read means the entry is stale and falls back to the slow path.

The one subtlety: the epoch moves on a write **only if some cache has actually copied that value** (`PropertyDescriptor::value_cached()`). Bumping on every write regardless would retire every global-read cache on every `var` assignment anywhere in the program -- the whole point of the flag is to make that costly case the rare one instead of the constant one.

## Strings

A cons/slice/tail union, not a flat buffer. A slice never copies -- it's an offset + length into its parent, so a regex match (or anything else) run over a slice runs over the parent's own bytes.

Internal encoding is WTF-8 (UTF-8 plus a 3-byte form for lone surrogates); converted to UTF-16 only where something specifically needs code units (PCRE2's 16-bit mode), and that conversion's ASCII-run fast path is vectorized (`wtf8_to_utf16`/`utf16_to_wtf8`, 16 bytes at a time under SSE2).
