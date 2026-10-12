# Baseline JIT (experimental)

x86-64 only (Linux, and macOS on Intel); anywhere else every chunk stays with the interpreter. Source: `src/core/vm/Jit.inc` (included by `Interpreter.cpp`, because it is built on the interpreter's `Frame` and handlers), `JitLayout.h`, `X64Asm.h`.

## Shape of it

A template compiler: each bytecode instruction becomes a fixed run of machine code, nothing is optimised across instructions. The register file stays in `Frame::regs`; only the accumulator lives in a machine register (`r13`). The `Frame` is the whole state, so interpreter and generated code can hand a frame back and forth at any instruction boundary.

The rule that keeps it small and safe: the JIT never re-implements a semantic it is not sure about.

- **Translated** instructions handle only the case provable inline (two finite doubles, an Ordinary object whose shape matches a site cache, a dense Array in range, ...). Anything else goes to the slow path.
- **Everything else is stepped**: the interpreter's own handler runs that one instruction on the same `Frame`, then control returns to generated code. A stop byte (`h_JitHalt`) is placed where the instruction continues (the next one, a branch target, exception landing pads), so the handler chain ends there. Frames that cannot be left any other way than falling through run on a private copy of the instruction's bytes.
- **Fast helpers** (`fast_get_named`, `fast_set_named`, `fast_lookup`, `fast_env_slot`, `fast_set_keyed`, `call_op`, `binary_op`) are the interpreter's own fast paths as plain C++ functions, used before falling back to a step.

Property accesses call shared per-thread **stubs** (4-way polymorphic own/prototype ICs, global lookups, dense array reads/writes) instead of inlining the cache logic: inlining made the code large enough to thrash the L1 instruction cache.

## When code is compiled

- A function containing a loop: on its second call, or earlier by **on-stack replacement** — the interpreter counts back edges (`Frame::osr_budget`) and `osr_try` compiles and enters at the loop head.
- Loop-free functions stay interpreted by default (`QUANTA_JIT_MODE=0` compiles them after `QUANTA_JIT_THRESHOLD` calls). Measured: loops are where dispatch cost multiplies; short call/property-bound methods gain nothing.
- Chunks with suspension points (generators, async) are refused.

## Memory safety

- Every register / constant / name / jump operand is range-checked when compiling; a chunk failing any check is refused.
- Generated code touches only `Frame`, the register bank, the chunk's constants, its own `JitCode`, and objects through the same checks the interpreter's inline-cache handlers make.
- C++ exceptions never unwind through generated code: helpers catch everything, store it, and the trampoline rethrows once the native frame is gone.
- The GC is conservative and non-moving, so values in generated code are found on the stack; the accumulator is spilled to the `Frame` before every call out. IC entries holding cells (prototypes, holders, cached values) are traced through `BytecodeChunk::trace`.
- Code pages are written once, then flipped to read+execute. Code only runs on the thread that compiled it.

## Environment

| variable | meaning |
|---|---|
| `QUANTA_JIT=0` | disable |
| `QUANTA_JIT_MODE=0` | also compile loop-free hot functions |
| `QUANTA_JIT_THRESHOLD=n` | calls before compiling (default 6; loops: 2) |
| `QUANTA_JIT_LOG=1` / `QUANTA_JIT_STATS=1` | compile log / what is still stepped, per opcode and site |
| `QUANTA_JIT_LIMIT=n`, `QUANTA_JIT_SKIP=n` | compile only functions number [skip, skip+limit): bisecting |
| `QUANTA_GC_STRESS` | also makes the generated code poll the GC at every back edge |

Verified by running the whole test262 `language` and `built-ins` suites with every function forced through the JIT (`QUANTA_JIT_THRESHOLD=1 QUANTA_JIT_MODE=0`, also with `QUANTA_GC_STRESS=2` on a subset): results identical to the interpreter's.
