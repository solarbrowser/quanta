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

## Why `Visit` and the destructor are not virtual

Quanta's `Object` has no vtable. The collector, the write barrier and the conservative probe all treat a cell's base address and its `Object*` as the same word, and a vptr in front of the `Object` would pull them apart. The engine's own subclasses (`Function`, the iterators, ...) dispatch on a kind tag instead.

`DOMObject` does the same for an open-ended set of types: each type gets a small constant table of function pointers (its `Visit` and its destructor), reachable from a field. In your code the only difference from a virtual hierarchy is that you do not write `virtual` or `override`. The table comes from the type passed to `Heap::Allocate`, so the most derived `Visit` and destructor run even when the object is only held through a base pointer.

## Naming

The engine has types called `Shape`, `Iterator`, `Promise` and others. A host that writes `using namespace Quanta;` will collide with its own types of those names. Keep host types in their own namespace and name engine types explicitly.
