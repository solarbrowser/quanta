# Parser

_Companion to [architecture/parser.md](../architecture/parser.md) -- this is the mechanics a change here has to respect, not the reasoning behind them._

## The Backtracking Rule

A `try_parse_*`/`try_tape_*` function is a bet: try a narrower grammar, and if it doesn't fit, undo and let the real parser take over. `current_token_index_` is what gets saved and restored -- but restoring it does *not* undo everything a real `parse_*` call would have done along the way. `note_name`, `subtree_acc_` mutation, anything pushed onto the error list -- none of that rewinds with the token position.

So the rule is absolute: a `try_*` function calls no real `parse_*`/`add_error` function that has side effects like these. `try_tape_primary` checks a token's raw `TokenType` directly instead of calling `parse_identifier`, for exactly this reason.

## Where the Tape Attaches

`parse_assignment_maybe_tape`/`parse_expression_maybe_tape` are the actual hook points -- wrapping most expression positions (`if`/`while`/`for` conditions, declarator initializers, call arguments, array/object literal elements, ...).

Each one: save the token position, try `try_tape_assignment`/`try_tape_expression`, and on success wrap the result in a `TapedExpression` (`TapedExpression::make`); on failure, restore the position and call the real `parse_assignment_expression()`/`parse_expression()` -- no cost beyond the one attempt.

## What the Tape Can't Hold

An unsupported sub-expression (spread, optional chaining, a whole function/class literal, ...) doesn't fail the attempt -- it gets parsed for real and embedded (`TapedExpression::embedded()`), with the tape holding a `Node`-tagged entry that points at it by index.

The rest of the tape still compiles through the fast path; only that one entry falls back to `compile_expression` on the embedded tree.

## The Compile-Time Fallback, and Its One Hard Limit

Whether a name ends up register-resident or environment-slotted (`is_local`/`lexical_out_of_scope`) is a whole-function-body fact -- a closure declared on line 400 can capture a name the tape guessed about on line 2. So `compile_tape_expr` can fail *after* parsing already committed to a tape, and `BytecodeCompiler`'s `TAPED_EXPRESSION` case handles that by reparsing the tape's own source range from scratch (`ScriptUnit::parse_expression_from_source` -> `Parser::parse_expression_at`) and compiling the resulting tree instead -- always safe, since nothing tape-eligible needs any context (class body, generator, `super`) that a bare reparse doesn't already provide.

One case has no fallback at all: a tape that embeds a function or class literal. Reparsing would produce a *new* tree, but any closure already made from the embedded original would still point at it -- and the tree is dropped once compiled, which would leave that closure's body dangling. So `tape_compilable` failing on an embedding tape is a hard compile failure for that expression, not a fallback trigger; the tape's own coverage has to be right the first time whenever it embeds something.

## Lazy Bodies (`FunctionExecutable`)

`defer_body` only applies to a **leaf** body -- one with no nested function or class -- because `ensure_body()` rebuilds the tree from source on demand, and a rebuilt inner literal would be a different node instance than whatever cached executable it owns.

What's kept for a deferred body is a `Position` + an end offset into the owning `ScriptUnit`'s source, not a token index -- keeping token-stream position would mean keeping the whole token stream alive for the program's lifetime, tens of megabytes for a large script, against a few bytes of range.

`release_rebuilt_body()` drops the rebuilt tree again once its one consumer (compiling to bytecode) is done with it.
