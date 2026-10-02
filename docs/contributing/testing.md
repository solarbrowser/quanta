# Testing

After a change, run it against test262 ([quanta-test262-runner](https://github.com/ataturkcu/quanta-test262-runner)). A regression means fix it before anything else -- that's not optional.

## No Free Passes

If a change has no measurable gain and doesn't clean the code up either, there's no real reason to open it as a PR -- it's just noise for someone to review with nothing to show for it.

And if you do open it anyway, the same standard applies on review: something that doesn't demonstrably help gets rejected regardless of how correct it is, so it saves everyone the round trip to check first.

## Claiming a Speedup

If a change claims to be faster, that claim needs a benchmark behind it, not just a feeling. And it has to be measured on a **release build** (`make release`/`make`) -- a debug build carries assertions and other instrumentation that cost real time, and timing that build tells you nothing about the change itself, only about how much the debug scaffolding around it costs.

Compare before/after on the same build type, ideally more than once -- a single run can be noise as much as it can be signal. Measure with `time` and `perf` to see RSS, instruction count, and wall-clock differences between the old build and the new one.

## Embedding Tests

Changes to the embedding surface (`quanta/Embed.h`, `DOMObject`, the collector's handling of host objects) are checked by `tests/embed/embed_test.cpp`, linked against the library the way an embedder would:

```
./build.sh embed-test    # or: make embed-test
```

It checks that a host object's destructor runs when swept, that `Visit` keeps what it reports alive, that the write barrier covers an old object gaining a young reference, `Cast`, a class exposed to script (brand checks, accessors, subclassing, iteration, strings), promises settled from the host, and the host-driven timers.

Run it under the collector's stress modes as well -- a missed edge or a missing barrier shows up there and often nowhere else:

```
QUANTA_GC_STRESS=1 build/bin/embed-test
QUANTA_GC_STRESS=2 build/bin/embed-test
QUANTA_GC_VERIFY=1 build/bin/embed-test
```
