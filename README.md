<div align="center">
  <img src="docs/images/quanta_transparent.png" alt="Quanta Logo" width="200"/>

  # Quanta

  *Spec-first, experimental JavaScript engine written in C++.*
 <br><br>
  <span>
    <img src="https://img.shields.io/github/stars/solarbrowser/quanta?style=for-the-badge&logo=github&labelColor=1b1f23&color=2f81f7" />
    <img src="https://img.shields.io/github/forks/solarbrowser/quanta?style=for-the-badge&logo=github&labelColor=1b1f23&color=2f81f7" />
    ![Issues](https://img.shields.io/github/issues/solarbrowser/quanta?style=for-the-badge&labelColor=1b1f23&color=6e40c9)
    ![PRs](https://img.shields.io/github/issues-pr/solarbrowser/quanta?style=for-the-badge&labelColor=1b1f23&color=6e40c9)
  </span>
  <br><br>
</div>

---

## Table of Contents

- [Motivation](#motivation)
- [What is Quanta](#what-is-quanta)
- [Project Goals](#project-goals)
- [Current Status](#current-status)
- [ECMAScript Compatibility](#ecmascript-compatibility)
- [Design Philosophy](#design-philosophy)
- [Known Limitations](#known-limitations)
- [What Quanta is NOT](#what-quanta-is-not)
- [Roadmap](#roadmap)
- [For Developers](#for-developers)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [Star History](#star-history)
- [Author](#author)
- [License](#license)

---

## Motivation

Quanta originally started as a part of my personal browser project. However, modern browsers consist of many complex subsystems, and implementing all of them solo, including a JavaScript engine, quickly became unrealistic.

Instead of building everything superficially, I decided to focus deeply on one core component: the JavaScript engine. Quanta exists as a result of that decision.

--- 

## What is Quanta

Quanta is a spec-first JavaScript engine written in C++.

It is built to explore how JavaScript actually works beneath the surface, not how fast it can run, but how precisely it follows the ECMAScript specification. The project emphasizes correctness, explicit semantics, and test-driven validation using the official Test262 suite.

---

## Project Goals

- Achieve 80%+ overall Test262 pass rate
- Maintain spec-correct behavior before performance optimizations
- Become fast enough to handle complex real-world computations
- Serve as a long-term learning and research project in language implementation

---

## Current Status

**Quanta is not production-ready.** It is currently in an early development stage and not performance-optimized. The project prioritizes correctness, explicit semantics, and long-term maintainability over short-term speed gains.

---

## ECMAScript Compatibility

Quanta has been tested multiple times against the official **Test262** test suite.

Early test runs can be misleading: while fixing ES1, ES3, and ES5 behavior, overall pass rates temporarily dropped. This was expected, as earlier results contained false positives caused by incomplete or incorrect implementations.

Additionally, Quanta does **not yet have a fully mature Test262 runner**, so some results may still include false passes or failures.

**Current Approximate Results (as of February 2026)**

- ECMAScript 1: 100% pass rate (as of February 1, 2026)
- ECMAScript 3: 100% pass rate (as of February 6, 2026)
- ECMAScript 5: 100% pass rate (as of February 7, 2026)

*Tested with conformance of **[JavaScript Engines zoo.](https://github.com/ivankra/javascript-zoo/tree/master/conformance)***

### Test262 Compliance Results

**Test262 results should be interpreted with caution, as [the runner](https://github.com/ataturkcu/quanta-test262-runner) is still under active development.**

**[View Test Results →](docs/documents/test262-results.md)**

---

## Design Philosophy

- Specification-first implementation
- Correctness over performance
- Explicit behavior over implicit shortcuts
- Minimal abstractions
- Test-driven development using Test262

---

## Known Limitations

- Most ES6+ features are not implemented
- Performance is currently slow
- No JIT or advanced optimization pipeline

---

## What Quanta Is Not

- Not a production browser engine
- Not a V8, SpiderMonkey, or JavaScriptCore replacement
- Not optimized for benchmarks

**Quanta is a learning-oriented, correctness-driven engine, not a commercial runtime.**

---

## Roadmap

- Improve performance
- Reach 80%+ overall Test262 pass rate
- Implement core ECMAScript 2015 (ES6) features
- Improve memory management
- Stabilize the Test262 runner

---

## For Developers

### Building Quanta

Quanta uses **Clang++** across all platforms.

#### Requirements

- C++17 compatible compiler
- Clang/LLVM 10 or higher
- Make (for Makefile builds)

**Windows:**
1. Download LLVM from [llvm.org/releases](https://github.com/llvm/llvm-project/releases)
2. Install and add to PATH
3. Verify: `clang++ --version`

**Linux:**
```bash
# Ubuntu/Debian
sudo apt install clang lld

# Fedora
sudo dnf install clang lld

# Arch
sudo pacman -S clang lld
```

**macOS:**
```bash
# Clang included with Xcode Command Line Tools
xcode-select --install
```

#### Quick Start

Clone the repository:
```bash
git clone https://github.com/solarbrowser/quanta
cd quanta
```

**Windows:**
```cmd
build-windows.bat
```

**Linux/macOS:**
```bash
./build.sh
# or
make -j$(nproc)
```

#### Build Outputs

- **Windows**: `build/bin/quanta.exe`
- **Linux/macOS**: `build/bin/quanta`
- **Static Library**: `build/libquanta.a`
- **Logs**: `build/build.log`, `build/errors.log`

### Testing

```bash
# Execute JavaScript file
./build/bin/quanta example.js

# Interactive REPL
./build/bin/quanta
```

### Troubleshooting

**Clang not found:**
- Windows: Verify LLVM is in PATH, restart terminal
- Linux: Install clang and lld packages

**Build errors:**
- Clean: `make clean` or delete `build/` directory
- Check: `build/errors.log`

---

## Documentation

You can reach all the documentation files from [here](https://quanta.js.org/pages/docs/docs.html).

---

## Contributing

We welcome contributions! Areas for enhancement:

### Development Areas
- **ECMAScript Features** - Additional ES6+ feature implementation
- **Performance** - Optimization improvements and benchmarking
- **Testing** - Expand test coverage and compatibility testing
- **Documentation** - Improve documentation and examples
- **Platform Support** - Enhanced cross-platform compatibility

### Contribution Guidelines
1. Fork the repository
2. Create a feature branch
3. Follow the modular architecture patterns
4. Add comprehensive tests
5. Update documentation as needed
6. Submit a pull request

### Priority Areas
- ECMAScript 2015+ compatibility testing
- Performance optimization
- Memory management improvements
- Cross-platform build enhancements

---

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=solarbrowser/quanta&type=date&legend=top-left)](https://www.star-history.com/#solarbrowser/quanta&type=date&legend=top-left)

---

## Author

Quanta is developed by a single student developer as a long-term systems programming project,
with the goal of deeply understanding JavaScript semantics, ECMAScript specifications, and language runtime design.

---

## License

This project is licensed under the **Mozilla Public License 2.0** - see the LICENSE file for details.

