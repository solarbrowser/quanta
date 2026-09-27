# Runtime

```
Value -> Object -> Shape
```

Every JS value is a `Value` -- one 8-byte word, no exceptions. Not a tagged union in the C sense; it's NaN-boxing. A real number sits there as an actual IEEE 754 double.

Anything else (`TAG_STRING`, `TAG_OBJECT`, `TAG_FUNCTION`, ...) reuses one of the double's unused NaN bit patterns as a tag, payload/pointer packed into the low 48 bits. One word, one register, one comparison to check the type.

## Shape

An object doesn't carry its own property table by default. Objects that add the same keys in the same order share one `Shape` (a transition tree) -- the layout lives once, only the slot values are per-object. `obj.x` becomes "shape says x is at slot 3," cacheable by an inline cache site.

That sharing has a ceiling: `Shape::kMaxSlots` is 128. Past it, an object gives up its shape and moves into dictionary mode (`migrate_to_dictionary_mode`, `HybridDescriptorMap`) -- every property now lives in a map instead of a flat slot table. Slower, but it has to happen somewhere; nothing scales a shared layout to an unbounded property count for free.

## Function is an Object

`class Function : public Object` -- a function is a callable object, same as the spec says.

`.prototype`/`.name`/`.length`/`.arguments`/`.caller` are the five names `Function::get_property` intercepts before falling through to the ordinary property path. `.prototype` in particular is lazy (`ensure_prototype`, cached once built), which is why `instanceof` should read it through `constructor_prototype()` and not a raw `get_property("prototype")` call.

## String

Not a flat buffer. A `String` is a cons/slice/tail union -- a slice is a window into its parent (offset + length), no bytes of its own.

Internal encoding is WTF-8 (UTF-8, plus a 3-byte form for lone surrogates); converted to UTF-16 only where something needs it (PCRE2 matching, `wtf8_to_utf16`/`utf16_to_wtf8`).

See also: [internals/representation.md](../internals/representation.md).
