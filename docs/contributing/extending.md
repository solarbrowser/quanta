# Extending the Engine

The three most common additions, and the concrete places each one touches.

## A New Builtin

1. New folder: `src/core/engine/builtins/<name>/`, header alongside the others in `include/quanta/core/engine/builtins/<Name>Builtin.h` (see [architecture/host.md](../architecture/host.md) -- headers all live in one shared folder even though sources get their own).
2. Define `void register_<name>_builtins(Context& ctx, Object* function_prototype)` there.
3. Call it from `Context::initialize_built_ins()` (`src/core/engine/Context.cpp`), alongside every other builtin's registration call.

For anything more involved than a handful of methods (Promise's microtask handling, a generator's suspend/resume), read [internals/builtins.md](../internals/builtins.md) first -- the mechanism it documents is very likely already the right one to reuse, not a new one to invent.

## A New Token / Keyword

1. Add the `TokenType` in `include/quanta/lexer/Token.h`.
2. A reserved word: `src/lexer/Lexer.cpp` buckets keywords by exact string length (`kKeywords3`, `kKeywords4`, `kKeywords5`, ...) rather than hashing them -- add the entry to the bucket matching the new keyword's length. A punctuator instead (a new operator symbol) goes through the lexer's own character-by-character scan, not this table.

## A New Bytecode Opcode

1. Add the `Op` to the enum in `include/quanta/core/vm/Bytecode.h`.
2. Add its entry to the `op_info` table in `src/core/vm/Bytecode.cpp` (`{name, operand_bytes, operand_kind}`) -- **at the same position** as the enum. A `static_assert` right after the table checks the two stay in sync; get the position wrong and the build tells you.
3. Write the handler (`h_<Name>` or `h_gen_<Name>` -- see [internals/execution.md](../internals/execution.md) for the musttail/`DISPATCH()` shape every handler has to have) in `src/core/vm/Interpreter.cpp`, then register it: `t[static_cast<uint8_t>(Op::<Name>)] = &h_<Name>;` in the handler-table builder.
4. Emit it from wherever `BytecodeCompiler.cpp` (`src/core/vm/BytecodeCompiler.cpp`) compiles the construct it's for.

Whichever of these you touch, [contributing/testing.md](./testing.md) and [contributing/code-style.md](./code-style.md) still apply in full.
