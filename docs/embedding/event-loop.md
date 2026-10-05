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

## Taking the timers over

A browser has an event loop of its own: task sources, background tabs whose timers are throttled, `requestAnimationFrame` ordering. `SetTimerProvider` hands the built-in `setTimeout`, `setInterval` and `setImmediate` to it:

```cpp
isolate->SetTimerProvider({
    /* schedule */ [&](Embed::Task task) { loop.Post(task.DelayMs(), task); },
    /* cancel   */ [&](Embed::Realm*, int64_t id) { loop.Remove(id); }});   // optional
```

From then on the engine's own loop knows nothing of those timers: `RunDueTimers` does not fire them. Each request arrives as a `Task`, a cheap handle that keeps the callback and its arguments alive:

| | |
|---|---|
| `Run()` | Runs the callback in its realm, then performs a microtask checkpoint when no script is running. An exception goes to the uncaught exception handler (origin `"timer"`). A no-op once the task is cancelled or its realm is gone. A one-shot task is spent afterwards; a repeating one stays armed, and the host schedules its next run. |
| `Cancel()`, `IsCancelled()` | `clearTimeout` and `clearInterval` cancel the task and call the provider's `cancel`. A host cancels a task it will not run. A realm that is destroyed cancels all of its tasks. |
| `GetRealm()`, `Id()`, `Source()` | The realm the task belongs to, what script was handed back by `setTimeout`, and its task source (`"setTimeout"`, `"setInterval"`, or the label given to `EnqueueTask`). |
| `DelayMs()`, `IsRepeating()` | What script asked for. The engine does no clamping: that is the host's, with the nesting level. |
| `NestingLevel()` | The HTML "timer nesting level" for the next run: a timer made inside a timer task starts one deeper, and a repeating task gains one with each run. At 5 or more the host clamps the delay to 4 ms. |

`Realm::EnqueueTask(source, fn, delayMs)` queues host code as a task of a realm the same way, for a host that makes its own timers and wants them to run in the realm with the right nesting and error reporting. With no provider it goes through the built-in loop (`RunDueTimers`), and the task handle is empty then.

Drop the `Task` handles before the Isolate goes.


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
