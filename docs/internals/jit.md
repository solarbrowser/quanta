# Baseline JIT

Quanta has a baseline JIT: a template compiler that turns the bytecode of a hot function into machine code. It runs on x86-64 (Linux, macOS, Windows) and AArch64 (Linux, macOS). On any other target, and in any process that cannot map executable memory, every function stays with the interpreter and nothing else changes.

Source: [`Jit.inc`](../../src/core/vm/Jit.inc) (the code generator and its runtime, included by `Interpreter.cpp`), [`JitLayout.h`](../../src/core/vm/JitLayout.h) (target detection and the layouts generated code depends on), [`JitPlatform.h`](../../src/core/vm/JitPlatform.h) / [`JitPlatform.cpp`](../../src/core/vm/JitPlatform.cpp) (executable memory), [`X64Asm.h`](../../src/core/vm/X64Asm.h) and [`A64Asm.h`](../../src/core/vm/A64Asm.h) (the two instruction encoders).

## What it does, and what it deliberately does not

A template JIT removes one cost: the dispatch between bytecode instructions. Nothing is optimised across instructions, there is no type specialisation beyond what an instruction can check inline, and no deoptimisation. The speed-up comes from loops of numeric work, property reads through inline caches and array accesses, where the interpreter's per-instruction dispatch is a large share of the time.

The rule that keeps it small and safe: **the JIT never re-implements a semantic it is not sure about.** The interpreter's ~190 opcodes carry years of specification corner cases (classes, private names, spread, destructuring, `eval`, `with`, disposal scopes), and most of them are not hot. A second implementation of each would be a second place for every conformance bug to live. So:

- **Translated** instructions handle only the case that can be proven inline: two finite doubles, an ordinary object whose shape matches a site cache, a dense array and an in-range index. Anything else leaves through a slow path.
- **Everything else is stepped.** The interpreter's own handler runs that one instruction on the same `Frame`, and control returns to generated code.
- **Fast helpers** (`fast_get_named`, `fast_set_named`, `fast_lookup`, `fast_env_slot`, `fast_set_keyed`, `call_op`, `binary_op`) are the interpreter's own fast paths as plain C++ functions, tried before falling back to a step.

## Machine state

The register file stays in `Frame::regs`; only the accumulator is held in a machine register. The `Frame` is the whole state, so the interpreter and generated code can hand a frame to each other at any instruction boundary. That is also what makes on-stack replacement and bailing out cheap.

Registers keep the same roles on both targets. The names are x86-64's, and `kPhys` in `A64Asm.h` says which AArch64 register each one is:

| role | x86-64 | AArch64 |
|---|---|---|
| `Frame*` | rbx | x19 |
| register bank | r12 | x20 |
| accumulator | r13 | x21 |
| `JitCode*` | r14 | x22 |
| this code's inline caches | r15 | x23 |
| finite-double limit (`finite_check`) | rbp | x24 |
| helper arguments 0-3 | rdi, rsi, rdx, rcx | x0-x3 |
| temporaries | rax, r8-r11 | x9-x13 |

Generated code uses only the low four SIMD registers, which are caller-saved in every ABI involved.

### How a step works

The interpreter's handlers tail-call straight into the next instruction, so the way to run exactly one is to make the next one a handler that returns. The JIT keeps a private copy of the bytecode (`code_copy`) and, for the duration of a step, overwrites with a stop byte (`h_JitHalt`) every place the instruction may continue: the next instruction, a branch target, the chunk's exception handlers. A step whose instruction cannot branch and has no handler table runs on a 16-byte stack copy of just its own bytes instead. Stopping at any instruction boundary is a valid place to resume, because generated code has a label for each one.

### Inline caches

Property reads and writes call shared per-thread **stubs** (4-way polymorphic own/prototype caches, global lookups, dense array reads and writes) instead of inlining the cache logic: inlining made the code large enough to thrash the L1 instruction cache. A stub answers hit or miss in the carry flag. On AArch64 the flags are returned just the same (`clc`/`stc` are `cmp xzr, xzr` / `cmn xzr, xzr`, and the caller branches on "carry clear").

## When code is compiled

- A function with a loop: on its second call, or sooner by **on-stack replacement**. The interpreter counts back edges (`Frame::osr_budget`), and `osr_try` compiles and enters at the loop head.
- A loop-free function stays interpreted by default. A short method that is mostly property accesses and calls spends its time in the runtime either way, and compiling it only adds instruction-cache pressure. `QUANTA_JIT_MODE=0` compiles hot loop-free functions too, after `QUANTA_JIT_THRESHOLD` calls.
- Chunks with suspension points (generators, async) are refused.
- A chunk that cannot pay for itself is refused, before any code is mapped:
  - more than `QUANTA_JIT_MAXBYTES` (16 KB) of bytecode: decoding and emitting need scratch memory proportional to its size before there is a verdict;
  - more than `QUANTA_JIT_MAXSTEPS` percent (10) of its instructions are bare steps: each one costs more than the dispatch it saved;
  - more than `QUANTA_JIT_MAXCALLS` percent (10) of its instructions are calls. A call from generated code is not cheaper than one from the interpreter (it builds the same `Frame`; measured at about 390 instructions against the interpreter's 300), so a function dominated by calls gains nothing and the interpreter's stackless calls serve it better.
- A compiled function that keeps leaving its fast paths (more than `QUANTA_JIT_MAXSLOW` slow exits per call, usually a megamorphic site) is no longer entered. The code stays allocated, since frames may be running it.

## Memory safety

- Every register, constant, name and jump operand is range-checked while compiling; a chunk failing any check is refused.
- Generated code touches only the `Frame`, the register bank, the chunk's constants, its own `JitCode`, and objects through the same checks the interpreter's inline-cache handlers make.
- C++ exceptions never unwind through generated code, which has no unwind information. Helpers catch everything, store it, and the trampoline rethrows after the native frame is gone.
- The collector is conservative and non-moving, so values in generated code are found on the stack; the accumulator is spilled to the `Frame` before every call out. Cells held by cache entries (prototypes, holders, cached values) are traced through `BytecodeChunk::trace`.
- Code pages are written once, then made read+execute; they are never writable and executable at once (see below for Apple Silicon). Code only runs on the thread that compiled it.
- The total mapped size is capped (192 MB).

## Targets

Everything operating-system dependent is in `JitPlatform.cpp` (declared in `JitPlatform.h`, which has nothing from the engine in it). It is a translation unit of its own because it is the only one that includes `<windows.h>`, whose macros (`IN`, `OUT`, `ERROR`, `DELETE`, ...) collide with identifiers in the engine's headers; everything architecture dependent is in the encoder (`X64Asm.h` / `A64Asm.h`) and a few `#ifdef QUANTA_JIT_ARCH_*` blocks in `Jit.inc` (the calling convention in `namespace abi`, the entry and exit sequences, the cycle counter).

| | executable memory | instruction cache |
|---|---|---|
| Linux, macOS x86-64 | `mmap`, then `mprotect` to read+execute | coherent with stores |
| Windows x86-64 | `VirtualAlloc`, then `VirtualProtect` | `FlushInstructionCache` |
| Linux AArch64 | `mmap`, then `mprotect` | `__builtin___clear_cache` after the write |
| macOS AArch64 (Apple Silicon) | `MAP_JIT` mapping; `pthread_jit_write_protect_np` turns this thread's write permission off after the copy | `sys_icache_invalidate` |

### x86-64

System V (Linux, macOS) passes arguments in rdi, rsi, rdx, rcx and preserves rbx, rbp, r12-r15. Microsoft x64 (Windows) passes them in rcx, rdx, r8, r9, wants 32 bytes of shadow space at `rsp` for the callee (reserved once, in the prologue), and also preserves rdi and rsi. Every helper takes at most four arguments, so none needs a stack argument.

### AArch64

AAPCS64: arguments in x0-x3, callee-saved x19-x28, `sp` 16-byte aligned at every call. The entry sequence saves fp, lr and x19-x24 in an 80-byte frame; generated code has no frame of its own, and a stub that calls out saves the link register around the call.

`A64Asm.h` has the same interface as `X64Asm.h`, so `Jit.inc` is one code generator rather than two. It differs from x86-64 where the hardware does:

- Condition codes keep the x86 meaning and numbering and are translated to A64's when a branch is emitted. An unsigned compare means the same on both, and so does a floating-point compare once NaN is excluded, which generated code always does first. Parity conditions have no counterpart and are not used.
- Two scratch registers, x16 (addresses, large immediates) and x17 (data), stand in for x86's memory and 32-bit immediate operands. No role uses them.
- A call's result arrives in x0 and every call copies it to the register the code generator reads it from.
- `cvttsd2si` answers `0x80000000` for anything outside int32 on x86 and the code generator tests for that; `fcvtzs` saturates, so the encoder produces the x86 value.
- Branches reach ±1 MB (`b.cond`) and ±128 MB (`b`). A chunk whose code needs more is refused ("branch out of range"), which the size limits make practically unreachable.

Windows on ARM64 is not supported: the Microsoft ARM64 ABI reserves x18 and has its own unwinding and icache rules, none of which have been looked at.

Apple Silicon: a process using the JIT needs to be allowed to create `MAP_JIT` memory. An ad-hoc signed local build is; a binary built with the hardened runtime needs the `com.apple.security.cs.allow-jit` entitlement, and without it every function simply stays interpreted. (The macOS half has not been run on a Mac yet; it is written against Apple's documented interface.)

## Environment

| variable | meaning |
|---|---|
| `QUANTA_JIT=0` | disable |
| `QUANTA_JIT_MODE=0` | also compile loop-free functions that become hot |
| `QUANTA_JIT_THRESHOLD=n` | calls before a loop-free function is compiled (default 6; loops: 2) |
| `QUANTA_JIT_MAXBYTES=n` | largest bytecode attempted (default 16384) |
| `QUANTA_JIT_MAXSTEPS=pct` | refuse a chunk with more plain steps than this share (default 10) |
| `QUANTA_JIT_MAXCALLS=pct` | refuse a chunk with more calls than this share (default 10; 100 = never) |
| `QUANTA_JIT_MAXSLOW=n` | slow exits per call after which compiled code stops being entered (default 10) |
| `QUANTA_JIT_LOG=1` | compile log and per-chunk shape |
| `QUANTA_JIT_STATS=1` | what is still stepped, per opcode and per site |
| `QUANTA_JIT_TRACE=1` | every step, as it happens |
| `QUANTA_JIT_LIMIT=n`, `QUANTA_JIT_SKIP=n` | compile only functions number [skip, skip+n): bisecting |
| `QUANTA_JIT_DUMP=dir` | write each function's machine code to `dir/jit_<address>.bin` |
| `QUANTA_GC_STRESS` | also makes generated code poll the GC at every back edge |

## Testing

Differential: every program must give the same result with and without the JIT. Forcing every function through it (`QUANTA_JIT_THRESHOLD=1 QUANTA_JIT_MODE=0`) is the strongest form, and the whole of `tests/comparison` should match the interpreter's output in that mode, as should test262's `language` and `built-ins` suites.

The x86-64 build tests the System V half. Building with `-DQUANTA_JIT_FORCE_WIN64=1` makes the code generator emit the Microsoft convention on any x86-64 target and declares every helper and the entry point with clang's `ms_abi` attribute, so generated code and the C++ it calls really do speak Win64 to each other without a Windows machine. What it cannot cover is the operating-system layer.

AArch64 can be tested from an x86-64 machine with no root access: cross-compile with clang against an arm64 sysroot (Debian's libc6, libc6-dev, libstdc++-dev and libgcc packages unpacked into a directory), build with `--target=aarch64-linux-gnu`, and run under `qemu-aarch64` with `QEMU_LD_PREFIX` pointing at the sysroot. `qemu-user` executes the generated code, so the encoder, the entry and exit sequences and the stubs are all exercised for real. `A64Asm.h`'s encodings can additionally be checked one by one by disassembling the emitted bytes with `llvm-mc --disassemble --triple=aarch64`.

## Known limits

- **Calls.** A call from generated code costs more than one from the interpreter (see `QUANTA_JIT_MAXCALLS`). The cost is building the callee's `Frame`, not the number of C++ layers around it: running the callee as a stackless interpreter frame from generated code was tried and was slower still. A real fix is a smaller frame, which would also speed up the interpreter.
- Windows on ARM64, and 32-bit targets, are unsupported.
