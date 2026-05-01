# Quanta

Quanta is an experimental ECMAScript (or JS) engine focused on both **memory efficiency** and **execution speed**.  


## Specification Support

> Tested with Kangax compatibility tables

| ECMAScript Version | Support |
|--------------------|--------|
| ES1-ES5            | ~100%  |
| ES6                | ~99%   |
| ES2016+            | ~85%   |

## Test262
  
You can review executed tests here: [quanta.js.org](https://quanta.js.org/test262/test262.html)

If the results are outdated, use the dedicated [runner](https://github.com/ataturkcu/quanta-test262-runner)

---

## For Developers

### Documentation

You can reach all the documentation files from [here](https://quanta.js.org/docen/index.html).

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

- Improve **spec compliance** 
- Implement **bytecode virtual machine (VM)**

More [here](https://quanta.js.org/roadmap.html)

## Contributing

Please ignore the `CONTRIBUTING.md` file for now. I kept it because I don’t want to recreate it when the initial release is ready.

At this stage, I want to fully shape Quanta’s architecture on my own, without external changes. The engine is evolving rapidly, and even small additions can unintentionally break other parts of the system. For that reason, I prefer to develop it solo until the initial release. Thanks for your understanding and for considering contributing!

---

## License

This project is licensed under the **Mozilla Public License 2.0** - see the LICENSE file for details.

