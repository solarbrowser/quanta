# Getting Started

Build first ([contributing/building.md](./building.md)), then just run something:

```
build/bin/quanta script.js
```

or drop into the REPL with no arguments -- `.help` lists its own commands (`.tokens`, `.ast`, `.quit`).

[architecture/overview.md](../architecture/overview.md) is the map if you're not sure where a change belongs; `internals/` is where you go once you're about to actually touch something.

## One More Thing

Everything else under `contributing/` -- code style, debugging, testing -- isn't a menu, it's one set of expectations. Follow all of it, not just the parts that are convenient for the change you're making.
