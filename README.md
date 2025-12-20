<div align="center">
  <img src="docs/images/quanta_transparent.png" alt="Quanta Logo" width="200"/>

  # Quanta

  *A high-performance, modular JavaScript engine written in C++*
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

- [What is Quanta](#what-is-quanta)
- [ECMAScript Compatibility](#ecmascript-compatibility)
- [For Developers](#for-developers)
- [Planned Release Date](#planned-release-date)
- [Contributing](#contributing)
- [License](#license)

---

## What is Quanta

Quanta is a modern JavaScript engine designed for the **Solar Project** with a focus on **modular architecture** and **high performance**. Built from the ground up in C++17, Quanta provides:

### Core Features
- **Modular Architecture** - Clean separation of concerns with 15+ focused modules
- **High Performance** - Optimized execution with advanced compilation techniques
- **Production Ready** - Comprehensive test coverage with 100% success rate
- **Cross Platform** - Windows, Linux, and macOS support

### JavaScript Support
- **ES6+ Compatibility** - Modern JavaScript features
- **Complete Built-ins** - Math, String, Array, Object, JSON, Date
- **Advanced Operations** - Nested objects, complex arrays, functional programming

---

## ECMAScript Compatibility

Quanta has been tested against the official **Test262** ECMAScript test suite (51,216 tests). Results demonstrate strong foundational JavaScript support with ongoing development for advanced features.

### Test262 Compliance Results
**Last Test Date:** December 18, 2025 | **Engine Version:** q120525c153l

| Metric | Count | % of Total | % of Executed |
|--------|-------:|-----------:|---------------:|
| **Total Tests** | 51,216 | 100.0% | - |
| **Tests Run** | 35,697 | 69.7% | 100.00% |
| **Passed** | 13,427 | 26.22% | **37.61%** |
| **Failed** | 22,270 | 43.48% | 65.93% |
| **Skipped** | 15,519 | 30.3% | - |

**[View Detailed & Other Test Results →](docs/documents/test262-results.md)**

---

## For Developers

### Building Quanta

Quanta uses **platform-native compilers** for optimal performance:
- **Windows**: MSVC (Visual Studio) - Native Windows build
- **Linux**: GCC - Native Linux build
- **macOS**: Clang/AppleClang - Native macOS build

#### Quick Start

**Universal Build Script** (Recommended):
```bash
./build.sh           # Build with Makefile
./build.sh cmake     # Build with CMake
./build.sh clean     # Clean all builds
```

**Windows (Native MSVC)**:
```cmd
build-windows.bat    # MSVC
```
*Requires: Visual Studio 2019/2022 + CMake*

**Linux/macOS**:
```bash
make -j$(nproc)      # Makefile build
# or
./build.sh cmake     # CMake build
```

#### Build System Options

1. **CMake** (Cross-platform, native compilers)
   - Windows: MSVC with `/O2 /GL /LTCG` optimizations
   - Linux: GCC with `-O3 -march=native`
   - macOS: Clang with native optimizations

2. **Makefile** (GCC/MinGW)
   - Traditional make-based build
   - Works on all platforms with GCC

#### Build Outputs
- **Windows MSVC**: `build-cmake/bin/Release/quanta.exe` (native)
- **Windows MinGW**: `build/bin/quanta.exe`
- **Linux/macOS**: `build/bin/quanta`
- **Static Library**: `libquanta.a` or `quanta.lib` (MSVC)

### Testing

#### Run Test Suite
```bash
# Execute JavaScript code directly
./build/bin/quanta -c "console.log('Hello World');"

# Execute JavaScript file
./build/bin/quanta example_file.js

# Interactive REPL
./build/bin/quanta
```

---

## Planned Release Date

### Development Timeline

- **2025 Q3**: Engine foundations (parser, lexer, core modules)
- **2025 Q4**: Modern JavaScript features implementation, testing and ECMAScript compatibility verification
- **2026 Q1**: Performance optimizations and improvements
- **2026 April**: Production release

### Milestones
- [x] Modular architecture implementation
- [x] Core JavaScript functionality
- [x] Build system optimization
- [x] Comprehensive testing framework
- [X] (little bit of) Modern JavaScript features (ES6+)
- [ ] ECMAScript compliance verification
- [ ] Performance benchmarking
- [ ] Production deployment

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

## License

This project is licensed under the **Mozilla Public License 2.0** - see the LICENSE file for details.

---

<div align="center">
  <strong>Built with ❤️ for the Solar Project</strong>
  <br>
  <sub>Modular • Fast • Reliable</sub>
</div>