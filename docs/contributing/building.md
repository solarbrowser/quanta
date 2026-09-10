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
