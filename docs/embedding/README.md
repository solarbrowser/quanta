# Embedding Quanta

Quanta can be linked into another program as a library. The embedding surface is deliberately small and generic: it adds one concept to the engine -- a native object the garbage collector manages -- plus the few operations needed to expose a class built on it to script. It contains no Web API of any kind (no URL, DOM, events, fetch); a host writes those itself, on top of this. That is how Solar uses it.

One header is the whole interface:

```cpp
#include "quanta/Embed.h"
```

## Building and linking

```
./build.sh lib        # or: make lib
```

produces `build/lib/libquanta.a`, which already contains PCRE2, utf8proc and mimalloc. A program needs only:

```
clang++ -std=c++20 -I<quanta>/include app.cpp <quanta>/build/lib/libquanta.a -pthread
```

No `-D` flags: nothing in the headers depends on one.

The archive deliberately leaves out `MiMalloc.o`, the object that replaces `operator new` for the whole process. Doing that to a host program is the host's decision. It is built next to the archive as `build/lib/quanta_mimalloc_override.o`; name it on the link line to opt in, leave it out and the host keeps its own allocator. (It has to be a plain object on the link line: an archive member nothing references is dropped by the linker, so putting it in the archive would silently do nothing.)

`./build.sh embed-test` (or `make embed-test`) builds the library and runs `tests/embed/embed_test.cpp` against it, the way an embedder would use it. See [testing](../contributing/testing.md) and [building](../contributing/building.md).

## A first program

```cpp
#include "quanta/Embed.h"

#include <string>

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace app {

struct Greeter : DOMObject {
    std::string name;
};

Object* g_greeter_proto = nullptr;

Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
    if (qe::IsUndefined(newTarget)) {
        qe::ThrowTypeError(ctx, "Greeter must be called with new");
        return qe::Undefined();
    }
    std::string name = args.empty() ? "world" : qe::ToUsvUtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();

    Object* proto = qe::PrototypeFromNewTarget(ctx, newTarget);
    if (qe::HasException(ctx)) return qe::Undefined();

    Greeter* greeter = Heap::Allocate<Greeter>();
    greeter->name = std::move(name);
    greeter->initialize_prototype(proto ? proto : g_greeter_proto);
    return qe::FromObject(greeter);
}

Value Greet(Context& ctx, Value thisValue, qe::Args, Value) {
    Greeter* greeter = DOMObject::Cast<Greeter>(thisValue);
    if (!greeter) {
        qe::ThrowTypeError(ctx, "Illegal invocation");
        return qe::Undefined();
    }
    return qe::FromUtf8(ctx, "Hello, " + greeter->name + "!");
}

}

int main() {
    auto runtime = qe::Runtime::Create();
    Context& ctx = runtime->GetContext();

    qe::ClassRef greeter = qe::DefineClass(ctx, "Greeter", app::Construct, 1);
    app::g_greeter_proto = greeter.prototype;
    qe::DefineMethod(greeter.prototype, "greet", app::Greet, 0);
    qe::DefineGlobal(ctx, "Greeter", greeter.constructor);

    qe::Runtime::Result result = runtime->Evaluate("console.log(new Greeter('Ada').greet());");
    return result.ok ? 0 : 1;
}
```

Prints `Hello, Ada!`. `class Admin extends Greeter {}` works too: `PrototypeFromNewTarget` gives the subclass's prototype.

## The model in four rules

- **One thread per Runtime, one Runtime at a time.** A `Runtime` and everything allocated in it belong to the thread that created it, and none may move to another. The engine keeps a few per-realm intrinsics (the iterator, generator and collection prototypes, ...) in thread-wide state, so a second Runtime made while the first exists would take them over: destroy a Runtime before making the next. Any number may follow one another; destroying one runs a collection that frees everything it built, and nothing of it may be used afterwards.
- **Errors are a flag, not a return value.** A native function reports failure by calling `ThrowTypeError(ctx, ...)` and returning (the returned value is ignored while an exception is pending). The same flag is how the engine reports failure back: after `Get`, `Set`, `Call`, `ToUsvUtf8`, `ToUint32` or anything else that can run script, check `HasException(ctx)` and return.
- **The event loop is the host's.** `Evaluate` runs a script and drains the promise job queue once; it never sleeps waiting for a timer. See [event-loop.md](event-loop.md).
- **C++ members do not keep cells alive.** Only a native object's `Visit()` does. See [native-objects.md](native-objects.md); it is the part to read before writing a class.

## Further reading

- [native-objects.md](native-objects.md) -- `DOMObject`, `Visit`, `NoteWrite`, `Cast`, what the collector guarantees and what it needs from you.
- [event-loop.md](event-loop.md) -- microtask checkpoints, timers, promises.
- [api-reference.md](api-reference.md) -- every function in `Embed.h`.
