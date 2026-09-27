# CLI

```
Usage: quanta [options] [file]

Options:
  -c <code>      Execute the given code and exit
  --module       Force-load the file as an ES module
  --preload <f>  Run <f> as a script in the same realm first (repeatable)
  -v, --version  Print the engine version and exit
  -h, --help     Show this help message and exit

With no file and no -c, starts the interactive REPL.
```

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
