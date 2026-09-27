# Overview

Quanta is an ECMAScript engine written in C++20. It uses PCRE2 as a regexp engine and mimalloc for allocation.

```
Lexer -> Parser -> AST -> BytecodeCompiler -> VM
```

All layers have a serious amount of optimization.

## Folder Structure

Each file in `src/` has a corresponding header file with the same name under `include/quanta/`, except in `src/core/engine/builtins/` -- every built-in has a folder for itself there, while in `include/quanta/core/engine/builtins/` every header is in the same folder.

## Further Reading

- [architecture/parser.md](./parser.md), [architecture/vm.md](./vm.md), [architecture/runtime.md](./runtime.md), [architecture/memory.md](./memory.md), [architecture/host.md](./host.md)
- `internals/` for the mechanics behind each of these
- `reference/` for the CLI and env var reference
- `contributing/` before opening a change