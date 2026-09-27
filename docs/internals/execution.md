# Execution

`VM::run` isn't a loop in the C sense -- it's a chain. Every handler ends by tail-calling the next one (`DISPATCH()` -> `[[clang::musttail]] return kHandlers[f.code[pc]](f, pc, acc)`), so the one real constraint on writing a handler is that nothing can happen after that call -- musttail requires it to be the function's last act, and a handler that tried to do something after `DISPATCH()` either fails to compile or silently stops being a tail call.

## Frame

The state one call carries: `regs` (the register file, sitting on the C++ stack), `chunk`, `owner` (which `Function` is running this chunk -- decides whose feedback/lookup caches are in play), `env_saves` (for entering/leaving block scopes), and `ctx` -- a pointer, not a reference, because a frame-resident `Context` can be materialized to a new heap address mid-call, and every later read through `f.ctx` has to see that new address.

## CHECK_EXC

Every handler that can throw checks for an exception before trusting its result. On a hit, it searches the chunk's `handlers` table for the pc range it's in and jumps to whichever matching handler has the *narrowest* range -- necessary because nested try/catch blocks both cover the throwing pc, and only the inner one is the actual target.

No match means unwinding: return `Value()` and let the caller's own `CHECK_EXC()` take over.

## Call Paths, at the Instruction Level

Which path a function runs (`fast_gate`, `fast_env_gate`/`fast_no_closures`, `call_tree_walker` -- see [architecture/vm.md](../architecture/vm.md)) is decided once, at compile time, and stored as a flag on its `FunctionExecutable`. `fast_callable()` is the runtime side of that: read the flag, branch, nothing more.

The one path worth knowing the mechanics of is `fast_env_gate`/`fast_no_closures`, because it shares parts of the caller's `Context` (lexical env, `this_needs_super`, strict-mode) rather than building a fresh one. Anything reading or writing one of those shared fields has to go through the `Frame` accessors (`frame_lexical_env`, `frame_set_lexical_env`, ...) rather than touching `ctx` directly -- a direct write there is visible to whatever nested call happens to be reading the same `Context`, which is exactly the bug class this path has to keep proving it doesn't have.

## Arguments Elision

`elide_arguments_` compiles `arguments.length`/`arguments[i]` straight into `LdaArgLength`/`LdaArgAt`, reading `f.args` directly instead of materializing an arguments object.

`params_never_written` widens this further -- if no parameter is ever reassigned in the body, the elision holds even where it otherwise couldn't.

`arguments_elision_refused_` is the escape hatch: if something turns up that genuinely needs the real object (arguments handed to another function, for instance), the function gets recompiled with one.
