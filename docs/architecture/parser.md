# Parser

```
Source -> Lexer -> Parser -> AST -> BytecodeCompiler
```

Our parser can't fully generate bytecode itself. Only hoisting (`prescan_declarations`) and sibling registers (`collect_lexical_regions`/`compute_sibling_safe_names`) -- for safety -- depends on it. But that doesn't mean every expression needs to be held as a tree node. See [Expression Tape](#expression-tape).

## Lazy Parsing

Quanta parses every function body fully up front -- so syntax errors are still caught eagerly, never deferred to a first call that may never come.

What IS lazy is what happens after: a body that contains no nested function/class (a leaf) has its tree dropped (`FunctionExecutable::defer_body`) once analysis is done, keeping only its position in the source (`body_start_` + `body_end_offset_`). When it's actually called, we re-parse just that range (`ensure_body` -> `ScriptUnit::parse_body_from_source`), compile it, and drop the tree again (`release_rebuilt_body`) -- based on our own finding that nearly nobody asks for it again after compiling.

Not "never parse it again": if someone does ask, we just do the same thing again. It's a memory trade-off -- holding a body that isn't running costs more than re-parsing it the rare time it's needed.

## Expression Tape

We hold expressions as a straight, post-order tape (`ExprTape`/`TapeTag`) rather than a tree. 17 expression types create in this form by the parser directly (`parse_assignment_maybe_tape` -> `try_tape_assignment`, ...); not supported rare expressions embed as a real AST node inside the tape (`TapedExpression::embedded()`), compiled through the ordinary tree path when they're hit.

Consequently, statements are still a tree (not worth doing for now) but expressions are cheaper than ever in quanta's history because of the tape (`compile_tape_expr`). It saved up to 60 megabytes of memory (in rss) in huge scripts.

See also: [internals/parser.md](../internals/parser.md).
