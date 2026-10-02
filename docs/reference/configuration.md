# Configuration

## Build Flags

- `make` / `make release` -- `-O3 -march=native -mtune=native`, `-DNDEBUG` (assertions off), ThinLTO + AVX2.
- `make debug` -- debug symbols and assertions on, no optimization.
- `make asan` -- ASan+UBSan, for memory-safety verification. See [contributing/debugging.md](../contributing/debugging.md).
- `make lib` / `./build.sh lib` -- the embedding library, `build/lib/libquanta.a`; see [embedding](../embedding/README.md). `embed-test` builds it and runs the embedding tests.
- `QUANTA_ARCH_FLAGS` (`./build.sh` only) -- replaces `-march=native -mtune=native` with a fixed CPU target, for binaries you distribute. See [contributing/building.md](../contributing/building.md).
- `make validate` -- release-speed, but with `-DQUANTA_VALIDATE_BYTECODE` on, which enables the `QUANTA_SUBTREE_CHECK`/`QUANTA_SCOPE_CHECK` consistency checks below (they don't exist in an ordinary build at all).

## Engine Config

Set on `Engine::Config` before the engine initializes:

| Field | Effect |
|---|---|
| `strict_mode` | run scripts as strict |
| `max_heap_size`, `initial_heap_size`, `max_stack_size` | sizing hints |
| `expose_test262_globals` | exposes `$262` (the CLI's `--test262`); never for untrusted script |
| `host_drives_event_loop` | running a script drains promise jobs and returns instead of sleeping until every timer has fired; the host fires timers itself. Set by `Runtime::Create`. See [embedding/event-loop.md](../embedding/event-loop.md) |

## Environment Variables

All `QUANTA_*`, read via `getenv` at runtime:

| Variable | Effect |
|---|---|
| `QUANTA_GC_STRESS` | `1` = full major GC at every safepoint; `2` = minor at every safepoint, full major every 64th (soaks the write barrier) |
| `QUANTA_GC_POISON` | fills freed cells with a recognizable pattern, for catching use-after-free |
| `QUANTA_GC_LOG` | prints one line per minor/major collection (`[gc] minor marked=... swept=...`) |
| `QUANTA_GC_PROFILE` | prints timing/yield per major slice (`[gc-prof] major slice=...`) |
| `QUANTA_GC_VERIFY` | runs extra verification passes during collection |
| `QUANTA_GC_MARK_ONLY` | skips sweep, runs mark only -- isolates mark's own cost |
| `QUANTA_GC_NO_BARRIER` | disables the write barrier (test-only, unsafe) |
| `QUANTA_GC_SLICE_STRESS` | forces incremental major's slice budget to its limit |
| `QUANTA_GC_ENV_HUNT` | a debug mode around environment roots |
| `QUANTA_HEAP_STATS` | prints a live-cell-by-kind summary on exit (`[heap N] chunks=... live_cells=...`) |
| `QUANTA_VM_DISASM` | prints the disassembly of compiled bytecode |
| `QUANTA_KEEP_BODIES` | keeps AST bodies that would otherwise be released after parsing -- for comparing a compile with and without that release |
| `QUANTA_SUBTREE_CHECK` | (`make validate` only) cross-checks a cached `SubtreeFlags` bit against walking the tree for real |
| `QUANTA_SCOPE_CHECK` | (`make validate` only) cross-checks parse-time scope analysis against walking the tree for real |
| `QUANTA_EXE_COUNT` | prints a running counter every 500 `FunctionExecutable` installs/shares |

See also: [internals/gc.md](../internals/gc.md), [internals/parser.md](../internals/parser.md).
