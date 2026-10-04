# Native objects

`DOMObject` is the base of every native object a host hands to script. It is an ordinary JavaScript object (it has a prototype, properties, can be subclassed from script) that also owns C++ state, and whose lifetime the garbage collector manages.

```cpp
struct Holder : DOMObject {
    std::string label;        // fine: the destructor runs when the cell is swept
    Counted* child = nullptr; // a cell reference: Visit() must report it

    void Visit(Visitor& v) { v.Mark(child); }
};

Holder* h = Heap::Allocate<Holder>();
```

## What the collector guarantees

- **The cell never moves.** A `T*` stays valid for as long as the object is alive.
- **The destructor runs when the cell is swept.** `std::string`, `std::vector` and any other owning member are released then.
- **Allocation never collects.** `Heap::Allocate` only requests a collection; it happens at the interpreter's next safepoint. So a constructor can allocate other cells without any of them being swept from under it, and the pointer `Allocate` returns is safe wherever it is held.
- **Stack and registers are roots.** The scan is conservative, so a `T*` in a local variable or a register keeps its object alive.

## What you must do

### Report every cell your members reference

`Visit()` is called whenever the collector traces the object. Mark every cell a C++ member points at -- an `Object*`, a `String*`, a `Value` -- and nothing else will keep that cell alive:

```cpp
void Visit(Visitor& v) {
    v.Mark(child);          // Object*, String*, Symbol*, BigInt* or Value
    for (const Value& item : items) v.Mark(item);
}
```

The base class reports nothing; your `Visit` hides it (it is not `virtual`, see below). If you derive from another host type, call its `Visit` too.

### Tell the collector when you store a new edge

The collector is generational. A young object is traced in full by the next minor collection, but an old one is not -- unless it has been marked as written to. After an object has been through a collection, storing a cell reference into a member `Visit()` reports needs:

```cpp
holder->child = Heap::Allocate<Counted>();
holder->NoteWrite();                 // re-trace this object
holder->NoteWrite(Value(newChild));  // or: shade only the new target
```

Without it the next minor sweeps the target while the object still points at it. A constructor never needs it (the object is young). The value form is cheaper when an object reports many edges; the plain form is the safe default.

### Keep a cell you hold from C++ reachable

The collector sees the stack, registers, and objects it traces. It does not see static or global C++ variables, heap memory that is not a `Visit()`ed member, or a lambda's captures. A cell held only there is swept.

- `DefineClass` already roots the constructor and prototype it returns, so `ClassRef`'s pointers are safe to keep in a global.
- A script-visible home works for anything else: store the value on a global object property, or on a `DOMObject` member that is `Mark`ed.
- A native closure that captures a cell pointer in a lambda needs a traced mirror of it (a property on the function object) -- the engine does this itself for promise resolving functions.

## Allocation and casting

Create instances only with `Heap::Allocate<T>(args...)`. It constructs `T` in a heap cell and records its type; an object made any other way has no type, so `Cast` rejects it and `Visit` is never called.

`T` must derive from `DOMObject`, be no larger than 4096 bytes and need at most 16-byte alignment (checked at compile time).

`DOMObject::Cast<T>(value)` returns `T*`, or `nullptr` when the value is not a host object of that type. Use it as the brand check at the top of every method:

```cpp
Counter* c = DOMObject::Cast<Counter>(thisValue);
if (!c) { ThrowTypeError(ctx, "Illegal invocation"); return Undefined(); }
```

A type can declare an ancestor to make `Cast<Base>` succeed on its instances:

```cpp
struct Node : DOMObject {};
struct Element : Node { using Parent = Node; };
// Cast<Node>(elementValue) works; Cast<Element>(nodeValue) is nullptr.
```

`Cast<DOMObject>` accepts any host object.

## Constructing from script

A constructor receives `newTarget` (undefined when called without `new`). Give the new object `PrototypeFromNewTarget(ctx, newTarget)` so `class Sub extends YourClass {}` produces `Sub` instances, and fall back to your class's own prototype when it returns null:

```cpp
Object* proto = PrototypeFromNewTarget(ctx, newTarget);
if (HasException(ctx)) return Undefined();
obj->initialize_prototype(proto ? proto : g_my_proto);
```

## Indexed and named properties

A Web IDL legacy platform object (`NodeList`, `HTMLCollection`, `NamedNodeMap`) answers `list[0]` and `collection["id"]` itself. A `DOMObject` type opts in by declaring static member functions; the engine finds them by name when the type is allocated and gives the object the property semantics of Web IDL's legacy platform object internal methods (`[[GetOwnProperty]]`, `[[Set]]`, `[[DefineOwnProperty]]`, `[[Delete]]`, `[[OwnPropertyKeys]]`):

```cpp
struct NodeList : DOMObject {
    std::vector<Node*> nodes;

    static bool IndexedGetter(Context& ctx, NodeList& self, uint32_t index, Value& out);  // false: no such index
    static uint32_t IndexedLength(Context&, NodeList& self);          // supported indices are 0 .. length-1
    // Optional, each switching a behaviour on:
    //   IndexedSetter, IndexedDeleter                              (absent: read-only, not deletable)
    //   NamedGetter, NamedSetter, NamedDeleter, NamedKeys          (the supported names)
    //   static constexpr bool LegacyOverrideBuiltIns = true;
    //   static constexpr bool LegacyUnenumerableNamedProperties = true;
};
```

What follows is the spec's, so a type only says what exists:

- An index is read through `IndexedGetter`; its property is enumerable and configurable, and writable only if there is an `IndexedSetter`. An index with no setter cannot be assigned or defined (a strict write throws), and one with no deleter is not deletable.
- A name is visible only if `NamedGetter` has it and nothing nearer shadows it: an own property, or a property of a prototype (unless `LegacyOverrideBuiltIns`). That is why `list.forEach` copied from `Array.prototype` beats a child named `forEach`.
- `Object.keys`, `for...in`, `Object.getOwnPropertyNames` and `Reflect.ownKeys` list the indices ascending, then the visible names (not enumerable with `LegacyUnenumerableNamedProperties`), then ordinary own properties.
- `length`, `[Symbol.iterator]`, `forEach` and the rest are ordinary members: define `length` as an accessor with `DefineAccessor` and install `Array.prototype`'s `[Symbol.iterator]`, `forEach`, `keys`, `values`, `entries` on the prototype from script, as the spec has them, and `for...of` and `forEach` work on it.

A hook that raises an exception does so through the context's flag, as in any native. The hooks run on the running script's context, or on the realm's own when a host reads a property from outside script.

## Finalization

A host object is destroyed when the collector sweeps it. Two things about that are guaranteed, and one is not:

- **Not guaranteed: order.** The destructors of one collection's dead objects run in an unspecified order, and so do those of the objects that die when an `Isolate` is destroyed (its realms go first, then the heap is swept once and everything in it is dead). A destructor may free what its object owns -- `std::string`, `std::vector`, a file handle -- and must do nothing else: not read another cell (it may be destroyed already), not call into script, not create cells.
- **Guaranteed: a first pass.** A type that declares `void Finalize()` is told once that it is dead, before **any** destructor of that collection runs. Every cell of the collection, dead or alive, is still intact then, so `Finalize` is where an object takes itself out of the host's registries (a document's list of ranges, an observer table) and where it may read the objects it was linked to. It must not run script, create cells or store the object anywhere (it is dead). At `Isolate` teardown every host object with a `Finalize` gets it before the first destructor.
- **Guaranteed: weak handles.** `WeakHandle<T>(ptr)` names a cell without keeping it alive: `Get()` returns it while it lives and null once the collector found it dead, a pass that happens before `Finalize` and before any destructor. It works for host objects and for script objects (`WeakHandle<Object>`). Copies share the answer. For the host's own bookkeeping that must not extend a lifetime.

## Members that hold cells

A C++ member that holds a cell is invisible to the collector, and a store into an old object needs `NoteWrite`. Two member types do both by themselves, so `Visit` does not have to list them and no store can forget the barrier:

```cpp
struct Observer : DOMObject {
    TracedValue callback;      // one value
    TracedList targets;        // a list of values: push_back, set, erase, clear, indexing, iteration
};
```

They attach themselves to the object being constructed, so they must be members of a `DOMObject` type (a `Traced` made anywhere else aborts at once rather than quietly not tracing). They have no copy or move.

## Why `Visit` and the destructor are not virtual

Quanta's `Object` has no vtable. The collector, the write barrier and the conservative probe all treat a cell's base address and its `Object*` as the same word, and a vptr in front of the `Object` would pull them apart. The engine's own subclasses (`Function`, the iterators, ...) dispatch on a kind tag instead.

`DOMObject` does the same for an open-ended set of types: each type gets a small constant table of function pointers (its `Visit` and its destructor), reachable from a field. In your code the only difference from a virtual hierarchy is that you do not write `virtual` or `override`. The table comes from the type passed to `Heap::Allocate`, so the most derived `Visit` and destructor run even when the object is only held through a base pointer.

## Naming

The engine has types called `Shape`, `Iterator`, `Promise` and others. A host that writes `using namespace Quanta;` will collide with its own types of those names. Keep host types in their own namespace and name engine types explicitly.
