# CLI

```
Usage: quanta [options] [file]

Options:
  -c <code>      Execute the given code and exit
  --module       Force-load the file as an ES module
  --test262      Expose the $262 test-harness API (createRealm, evalScript, gc, ...)
  --positions    Put the line and column of every call in an error's stack trace (a little slower)
  --preload <f>  Run <f> as a script in the same realm first (repeatable)
  -v, --version  Print the engine version and exit
  -h, --help     Show this help message and exit

With no file and no -c, starts the interactive REPL.
```

## `--test262`

Turns on `$262` (`Engine::Config::expose_test262_globals`, off by default) -- `createRealm`, `evalScript`, `gc`, `detachArrayBuffer`. This is a test-harness interface, not something an embedder should ever expose to arbitrary script, which is why it's a flag and not always-on. See [internals/builtins.md](../internals/builtins.md).

## `--positions`

Every call records where in the source it is, so an error's stack trace names the line and column of each call in it (`at inner (app.js:3:3)`) instead of where each function is declared. Off by default: it costs about one to four percent on call-heavy code. The scripts are compiled with their position tables either way (a few bytes a statement, about a tenth the size of the bytecode), which is why it can be switched on for code already loaded -- an embedder does that with `Isolate::SetSourcePositionTracking`. Frames of a generator's or an async function's body are not placed.

## `--preload`

Runs before `-c`, before a given file, and before the REPL -- always first, and always in the same realm as whatever runs after it.

That's deliberate: a module's own imports are already instantiated before its first statement runs, so injecting a global from inside the module body itself would be too late for anything that module imports.

## REPL Commands

Only available with no file and no `-c`:

| Command | Does |
|---|---|
| `.help` | lists these commands |
| `.tokens <expr>` | tokenizes `<expr>` and prints the token stream |
| `.ast <expr>` | parses `<expr>` and prints its AST |
| `.clear` | clears the screen |
| `.quit` / `.exit` | exits |

See also: [architecture/host.md](../architecture/host.md).
