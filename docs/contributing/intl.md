# Intl (ECMA-402)

`Intl`, `String.prototype.localeCompare`, `toLocaleUpperCase`/`toLocaleLowerCase` and the `toLocale*String` methods are built on [ICU4X](https://github.com/unicode-org/icu4x). Intl is **optional**: a normal build has no `Intl` global at all, and the executable does not carry any locale data.

An Intl build needs two things: the code, built with `ICU4X=1`, and a data file, `quanta-icu.postcard`, that you generate once. The data file is not in the repository (it is about 30 MB and ignored by git), so every checkout has to make its own.

## 1. Requirements

* Everything from [building](building.md)
* `cargo` and a stable Rust toolchain (https://rustup.rs), for the Rust crate in `third_party/quanta_icu` and for ICU4X's data generator
* Network access the first time you generate data (the generator downloads CLDR and the time zone database)

## 2. Build the engine

```
 make ICU4X=1
```

or, with the scripts:

```
 QUANTA_ICU4X=1 ./build.sh
 set QUANTA_ICU4X=1 & build-windows.bat
```

The Windows script has not been tried yet. Changing `ICU4X` between 0 and 1 needs a `make clean` first, because the object files differ.

## 3. Generate the data

```
 make icu-data ICU4X=1
```

This writes `build/quanta-icu.postcard`. It takes about a quarter of an hour. The generator sometimes stops making progress; if the log has not moved for several minutes, interrupt it and run it again.

`make clean` keeps the file, `make clean-all` deletes it.

## 4. Where the program finds the data

In this order:

1. the file named by the `QUANTA_ICU_DATA` environment variable
2. next to the executable
3. one directory above the executable (this is where `build/quanta-icu.postcard` is found from `build/bin/quanta`)
4. `../share/quanta/` relative to the executable

When none exists there is no `Intl` global. To ship an Intl build, put the file next to the executable.

## 5. Checking it works

```
 echo 'console.log(new Intl.NumberFormat("de", {style: "currency", currency: "EUR"}).format(1234.5))' > intl-check.js
 build/bin/quanta intl-check.js
```

prints `1.234,50 €`. The test262 directory `test/intl402` is the test suite for this; run it with an Intl build.

## 6. How it is put together

| Part | Where |
|---|---|
| ICU4X itself (a release tag) | `third_party/icu4x`, a submodule |
| Rust crate: ICU4X's C API without data, plus a small shim for the formatters the C API lacks (percent, currency, compact, units, relative time, display names, date and time) | `third_party/quanta_icu` |
| The services (`Locale`, `NumberFormat`, `DateTimeFormat`, ...) | `src/core/engine/builtins/intl/` |
| How `Number`, `Date`, `String`, `Array` call into Intl | `IntlHooks` in `include/quanta/core/engine/builtins/IntlBuiltin.h` |
| Generated tables checked in next to the code | `src/core/engine/builtins/intl/*.inc`, made by the scripts in `tools/icu/` |

Run the scripts in `tools/icu/` again when `third_party/icu4x` moves to a new release: `gen_locale_list.sh`, `gen_locale_data.py`, `gen_locale_info.py`.

## 7. What is approximated

ICU4X has no skeleton matcher, and its data does not name every unit, so:

* `DateTimeFormat` maps the requested fields onto the closest field set ICU4X has. `formatRange` does not merge the shared parts of two dates, and `dateStyle: "short"` keeps four-digit years.
* Flexible day periods ("in the morning") are written for English only. ICU4X does not expose the CLDR day period rules for the other languages.
* Units whose names the data lacks (bits and bytes, temperature, angle, speeds) use English patterns.
* Time zone offsets come from the host's time zone database (Linux and macOS). On Windows only UTC, fixed offsets and the local zone work.
* The NaN symbol is "NaN" in every locale.
