# Event loop and promises

Quanta's own CLI keeps running after a script until every timer has fired, sleeping as long as that takes. A host with an event loop of its own cannot be blocked like that, so a realm made with `Isolate::CreateRealm()` (or a `Runtime`) is configured the other way (`Engine::Config::host_drives_event_loop`, see [reference/configuration.md](../reference/configuration.md)):

- `Evaluate` runs the script, then drains the promise job queue (every reaction, and every job those queue) once, and returns. It never waits for a timer.
- Everything after that is a turn the host takes itself.

The event loop is one per `Isolate`, shared by its realms; each job and timer runs in the realm that queued it. Every call below is the same on a `Runtime`.

## Microtasks

```cpp
isolate->PerformMicrotaskCheckpoint();
```

runs every queued promise job and then reports rejections nobody handled. Call it after calling into script from the host -- after `Call`, after settling a promise -- the way a browser runs a microtask checkpoint once the script stack is empty. Do not call it from inside a native function: the script that called you is still on the stack.

## Timers

The engine's built-in `setTimeout` / `setInterval` are serviced by the host:

```cpp
isolate->RunDueTimers();          // fires the timers whose time has come; never waits
isolate->NextTimerDelayMs();      // ms until the next one (0 if due); nullopt if none
```

A job queued by a timer callback runs right after that callback. A host that implements its own timers (as a Web API would) has no use for these two and can ignore them.

## Promises

`NewPromiseCapability(ctx)` returns a pending promise and its two settling functions, exactly as `new Promise` hands them to its executor:

```cpp
PromiseCapability cap = NewPromiseCapability(ctx);   // {promise, resolve, reject}

// later, when the work is done:
Value result = FromUint32(42);
Call(ctx, cap.resolve, Undefined(), Args(&result, 1));
isolate->PerformMicrotaskCheckpoint();
```

`resolve` adopts a thenable's state rather than wrapping it; `reject` takes any reason. Return `cap.promise` to script.

The three values are cells like any other. If a host object keeps them in C++ members until the work finishes, it must `Mark` them in `Visit` (see [native-objects.md](native-objects.md)). Keeping just `resolve` and `reject` is enough to keep the promise alive.

## External memory

A small cell can own a large buffer the collector never allocated (a decoded image, a response body). Tell it:

```cpp
ReportExternalAllocation(bytes);
```

once, with the size, when the memory is acquired. There is no matching release: the count restarts at each major collection. It makes a program that churns through such cells collect often enough.
