# Building Quanta

This document explains how to build Quanta from source, configure the build, run different build modes and troubleshoot common build issues.

If you don't care about build configurations just jump to [building](#4-building).

## 1. Prerequisites

Before building Quanta, make sure your system provides the following:

* Running computer
* clang++
* `Make`, `CMake` or provided build scripts
* Git

See [building](#4-building) for setting it up on your environment.

## 2. Clone the Source

You can either install zip from GitHub or clone it:

```
 git clone https://github.com/solarbrowser/quanta
 cd quanta
```

Quanta uses submodules, build scripts handle them but if you get any errors:

```
 git submodule update --init --recursive
```

run the command above.

## 3. Configuring the Build && Build Types

### Configuring the Build

We don't recommend messing with build flags, it can break edge cases so if you are testing the spec coverage build with current flags.

All platforms share same flags if available.

### Build Types

#### Debug

This build is intended for development and debugging, use command below:

```
 make debug
```

It enables debug symbols and assertions not intended for best performance.

#### Release

This build is intended for normal use and best performance.

`make`, `make release`, `./build.sh`, `./build-windows.bat` all builds in release.

#### Asan

This build is intended for debugging crashes, not intended for normal use.

```
 make asan
```

#### Library

This build is intended for embedding Quanta in another program: the engine without the command-line front end, as a static library.

```
 ./build.sh lib
 # or
 make lib
```

It produces `build/lib/libquanta.a` (PCRE2, utf8proc and mimalloc included) and `build/lib/quanta_mimalloc_override.o`, the opt-in object that makes the whole process allocate through mimalloc; it is kept out of the archive on purpose. See [embedding](../embedding/README.md) for how to link it.

To build the library and run the embedding tests against it:

```
 ./build.sh embed-test
 # or
 make embed-test
```

### Targeting a CPU

By default `./build.sh` compiles for the machine it runs on (`-march=native -mtune=native`), which is right for building for yourself and wrong for a binary you hand to someone else: it can contain instructions their CPU does not have and die with "illegal instruction". Set `QUANTA_ARCH_FLAGS` to build for a fixed target instead:

```
 QUANTA_ARCH_FLAGS='-march=x86-64 -msse4.2 -mavx -mavx2 -mtune=haswell' ./build.sh   # Linux x86-64
 QUANTA_ARCH_FLAGS='-mcpu=apple-m1' ./build.sh                                        # Apple Silicon
```

Those are the values the release builds in CI use. `make` does not read this variable.

## 4. Building

If you have running computer, follow the steps under your operating system.

### Linux

```
# Ubuntu/Debian
sudo apt install clang lld

# Fedora
sudo dnf install clang lld

# Arch
sudo pacman -S clang lld

git clone https://github.com/solarbrowser/quanta # If you haven't already cloned 
cd quanta
./build.sh
# or
make release # also you can use cmake
```

### macOS

```
xcode-select --install

git clone https://github.com/solarbrowser/quanta # If you haven't already cloned 
cd quanta
./build.sh
# or
make release # also you can use make or cmake
```

### Windows

Install [Visual Studio](https://visualstudio.microsoft.com/downloads/), under the Workloads tab, check the box for Desktop development with C++.

After that you can install [LLVM](https://releases.llvm.org/) and add it to your PATH.

```
clang++ --version # see if it is installed

git clone https://github.com/solarbrowser/quanta # If you haven't already cloned 
cd quanta
build-windows.bat # also you can use make cmake
```

## 5. Troubleshooting

### Clang not found

* Windows: Ensure you installed C++ development tools from Visual Studio correctly and LLVM is in PATH
* Linux: Install clang and lld using proper command for your package manager
* macOS: Use `xcode-select --install` command

### Build fails

* Run `git submodule update --init --recursive`, try again.
* If still fails, check [actions](https://github.com/solarbrowser/quanta/actions), if it is failing on there too clone a older commit or a release.

## 6. Build Artifacts

A successful build may produce:

`build` folder in root of Quanta which contains `bin` and `obj` folders along build logs and static library, bin folder contains executable for Quanta

`./build.sh lib` / `make lib` additionally produce `build/lib`, holding the embedding library (`libquanta.a`) and the optional allocator object (`quanta_mimalloc_override.o`).
