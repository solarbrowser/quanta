# Quanta

Quanta is an experimental ECMAScript (or JS) engine focused on both **memory efficiency** and **execution speed**.  


## Specification Support

> Tested with Kangax compatibility tables

| ECMAScript Version | Support |
|--------------------|--------|
| ES1-ES5            | ~100%  |
| ES6                | ~90%   |
| ES2016+            | ~57%   |

## Test262
  
You can review executed tests here: [quanta.js.org](https://quanta.js.org/pages/test262/test262.html)

If the results are outdated, use the dedicated [runner](https://github.com/ataturkcu/quanta-test262-runner)

---

## For Developers

### Documentation

You can reach all the documentation files from [here](https://quanta.js.org/pages/docs/docs.html).

### Build
<details>
  
Quanta uses **Clang++** across all platforms.

<details>
<summary><strong>Windows</strong></summary>

```bash
# Install LLVM and add to PATH
clang++ --version

git clone https://github.com/solarbrowser/quanta
cd quanta
build-windows.bat
```
</details>

<details>
<summary><strong>Linux</strong></summary>

```bash
# Ubuntu/Debian
sudo apt install clang lld

# Fedora
sudo dnf install clang lld

# Arch
sudo pacman -S clang lld

git clone https://github.com/solarbrowser/quanta
cd quanta
./build.sh
# or
make -j$(nproc)
```
</details>

<details>
<summary><strong>macOS</strong></summary>

```bash
xcode-select --install

git clone https://github.com/solarbrowser/quanta
cd quanta
./build.sh
# or
make -j$(nproc)
```
</details>

<details>
<summary><strong>Build Outputs</strong></summary>

- **Windows:** `build/bin/quanta.exe`  
- **Linux/macOS:** `build/bin/quanta`  
- **Static Library:** `build/libquanta.a`  
- **Logs:** `build/build.log`, `build/errors.log`  
</details>


<details>
<summary><strong>Usage</strong></summary>

```bash
# Run a JavaScript file
./build/bin/quanta example.js

# Start REPL
./build/bin/quanta
```
</details>
</details>

### Troubleshooting
<details>

### Clang not found
- Windows: Ensure LLVM is in PATH, restart terminal  
- Linux: Install clang and lld  

### Build errors
```bash
make clean
# or delete build/ directory
```

Check logs:
```
build/errors.log
```
</details>

## Roadmap

- Replace `std::regex` with **PCRE2**  
- Improve **ES6 and ES2016+** support  
- Implement **bytecode virtual machine (VM)**  

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

---

## License

This project is licensed under the **Mozilla Public License 2.0** - see the LICENSE file for details.

