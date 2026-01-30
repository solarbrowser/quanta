# Quanta Engine - Internal Architecture and Design Principles

> **Last Updated:** January 30, 2026  
> **Version:** 0.1.0  
> **Author:** Ata Türkçü

---

## Table of Contents

1. [Overview](#overview)
2. [Core Design Principles](#core-design-principles)
3. [Architectural Components](#architectural-components)
4. [Execution Pipeline](#execution-pipeline)
5. [Memory Management](#memory-management)
6. [Data Structures and Representations](#data-structures-and-representations)
7. [Module System](#module-system)
8. [Runtime Environment](#runtime-environment)
9. [Performance Considerations](#performance-considerations)
10. [Future Architecture Plans](#future-architecture-plans)

---

## Overview

Quanta is a **spec-first JavaScript engine** written in modern C++17. Unlike production engines that prioritize performance (V8, SpiderMonkey, JavaScriptCore), Quanta focuses on **correctness, explicit behavior, and strict ECMAScript compliance**. IT'S LIKE THAT BECAUSE I CAN'T RACE WITH THAT BIG COMPANIES AT THIS AGE and learning comes first to me.

### Key Characteristics

- **Specification-Driven**: Every implementation decision references the ECMAScript specification
- **Tree-Walking Interpreter**: Direct AST evaluation without bytecode compilation
- **Explicit Semantics**: Minimal abstractions, clear control flow
- **Test-First Development**: Validated against the official Test262 test suite

---

## Core Design Principles

### 1. Specification-First Implementation

Every feature is implemented by directly following the ECMAScript specification.

- Each AST node's `evaluate()` method mirrors spec algorithms
- No implicit behavior or "clever shortcuts"

### 2. Correctness Over Performance

The engine prioritizes getting JavaScript semantics exactly right before optimizing:

- **Phase 1**: Correct implementation (current phase)
- **Phase 2**: Optimization where spec-compliant (maybe in summer of 2026)
- **Phase 3**: Advanced JIT compilation (future)

### 3. Minimal Abstractions

Quanta avoids excessive abstraction layers:

- Direct AST traversal (no intermediate bytecode in current version)
- Explicit type conversions following spec rules
- Clear call stacks and execution contexts

### 4. Explicit Behavior

All JavaScript behavior is made explicit in code:

- Type coercions are explicit function calls
- Scope chain lookups are traceable
- Exception handling mirrors spec control flow

## Architectural Components

Quanta follows a classic compiler/interpreter architecture with clear separation of concerns:

```
┌─────────────────────────────────────────────────────────────┐
│                         Quanta Engine                        │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐ │
│  │    Lexer     │───▶│    Parser    │───▶│   Evaluator  │ │
│  │  (Tokenize)  │    │  (Build AST) │    │ (Interpret)  │ │
│  └──────────────┘    └──────────────┘    └──────────────┘ │
│                                                              │
├─────────────────────────────────────────────────────────────┤
│                     Runtime Environment                      │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────────┐  │
│  │   Context    │  │    Object    │  │  Built-in APIs  │  │
│  │   (Scope)    │  │    System    │  │  (String, Math) │  │
│  └──────────────┘  └──────────────┘  └─────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                    Memory Management                         │
│  ┌──────────────────────────────────────────────────────┐  │
│  │     Garbage Collector (Mark-and-Sweep GC)            │  │
│  │     - Generational Collection                         │  │
│  │     - Young/Old/Permanent Generations                 │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

### Component Hierarchy

#### **1. Frontend (Lexical Analysis & Parsing)**

**Location**: `src/lexer/`, `src/parser/`, `include/quanta/lexer/`, `include/quanta/parser/`

##### Lexer (`Lexer.h`, `Lexer.cpp`)
- **Purpose**: Converts source code into tokens
- **Process**: Character stream → Token stream
- **Key Features**:
  - Regex literal context detection
  - Template literal support
  - Automatic semicolon insertion (ASI) preparation
  - Position tracking for error reporting

**Token Types**: Keywords, identifiers, literals, operators, punctuation

##### Parser (`Parser.h`, `Parser.cpp`)
- **Purpose**: Builds Abstract Syntax Tree (AST)
- **Algorithm**: Recursive descent parser
- **Parse Strategies**:
  - **Pratt parsing** for expressions (operator precedence)
  - **Recursive descent** for statements
  - **Lookahead** for disambiguation (e.g., function vs. arrow function)

**Key Methods**:
```cpp
parse_program()           // Entry point
parse_statement()         // Statement parsing
parse_expression()        // Expression parsing with precedence
parse_assignment_expression()
parse_conditional_expression()
parse_binary_expression() // Handles all operators
```

##### Abstract Syntax Tree (`AST.h`, `AST.cpp`)
- **Base Class**: `ASTNode` (all nodes inherit from this)
- **Node Types**: 80+ node types covering full JavaScript syntax (maybe not I am not sure)
- **Core Interface**:
  ```cpp
  class ASTNode {
      virtual Value evaluate(Context& ctx) = 0;
      virtual std::string to_string() const = 0;
      virtual std::unique_ptr<ASTNode> clone() const = 0;
  };
  ```

**Major Node Categories**:
- **Literals**: NumberLiteral, StringLiteral, BooleanLiteral, ObjectLiteral, ArrayLiteral
- **Expressions**: BinaryExpression, UnaryExpression, CallExpression, MemberExpression
- **Statements**: IfStatement, ForStatement, WhileStatement, TryStatement
- **Declarations**: VariableDeclaration, FunctionDeclaration, ClassDeclaration
- **ES6+**: ArrowFunction, TemplateString, SpreadElement, DestructuringAssignment

#### **2. Runtime Core**

**Location**: `src/core/`, `include/quanta/core/`

##### Engine (`Engine.h`, `Engine.cpp`)
- **Purpose**: Main entry point and orchestrator
- **Responsibilities**:
  - Initialize runtime environment
  - Execute JavaScript code
  - Manage global context
  - Coordinate garbage collection
  - Handle module loading

**Key Methods**:
```cpp
bool initialize()                              // Setup engine
Result execute(const string& source)          // Run JavaScript code
Result execute_file(const string& filename)   // Load and execute file
Result evaluate(const string& expression)     // REPL-style evaluation
void collect_garbage()                         // Trigger GC
```

**Engine Configuration**:
```cpp
struct Config {
    bool strict_mode = false;
    bool enable_optimizations = true;
    size_t max_heap_size = 512 * 1024 * 1024;
    size_t initial_heap_size = 32 * 1024 * 1024;
    size_t max_stack_size = 8 * 1024 * 1024;
    bool enable_debugger = false;
    bool enable_profiler = false;
};
```

##### Context (`Context.h`, `Context.cpp`)
- **Purpose**: JavaScript execution context (scope and state)
- **Spec Mapping**: Directly implements ECMAScript Execution Context
- **Context Types**:
  - **Global Context**: Top-level execution
  - **Function Context**: Function execution
  - **Eval Context**: `eval()` execution
  - **Module Context**: ES6 module execution

**Context State**:
```cpp
class Context {
    Type type_;                          // Global/Function/Eval/Module
    State state_;                        // Running/Suspended/Completed
    Environment* lexical_environment_;    // Lexical scope (let/const)
    Environment* variable_environment_;   // Variable scope (var)
    Object* this_binding_;               // The 'this' value
    Value return_value_;                 // Return value
    Value current_exception_;            // Exception state
    std::vector<StackFrame*> call_stack_; // Call stack
};
```

**Key Responsibilities**:
- Variable binding management (let, const, var)
- Scope chain resolution
- Exception handling
- Call stack management
- `this` binding resolution

#### **3. Value Representation**

**Location**: `include/quanta/core/runtime/Value.h`

##### Value Type System
Quanta uses **NaN-boxing** for efficient value representation:

```cpp
class Value {
    union {
        uint64_t bits_;   // Tagged representation
        double number_;   // Double-precision float
    };
};
```

**NaN-Boxing Strategy**:
- All JavaScript values fit in 64 bits
- Uses IEEE 754 NaN space for type tags
- Immediate values (numbers, booleans) don't require heap allocation
- Pointers are tagged and stored in the NaN payload

**Type Tags** (48-bit payload space):
```
Undefined:  0x7FF0 0000 0000 0000
Null:       0x7FF1 0000 0000 0000
Boolean:    0x7FF2/3 (false/true)
Number:     Direct IEEE 754 (if not NaN)
String:     0x7FF4 | pointer
Symbol:     0x7FF5 | pointer
BigInt:     0x7FF6 | pointer
Object:     0x7FF7 | pointer
Function:   0x7FF9 | pointer
```

**Benefits**:
- Cache-friendly (fits in register/cache line)
- Fast type checking (bit mask operations)
- No vtable overhead for primitives
- Efficient for number-heavy workloads

#### **4. Object System**

**Location**: `include/quanta/core/runtime/Object.h`

##### Object Representation
```cpp
class Object {
    ObjectHeader header_;                           // Metadata
    std::unordered_map<std::string, Property> properties_;
    Object* prototype_;                             // Prototype chain
    bool is_extensible_;
    
    struct Property {
        Value value;
        bool writable;
        bool enumerable;
        bool configurable;
    };
};
```

**Property Attributes** (ECMAScript compliant):
- **[[Value]]**: The property value
- **[[Writable]]**: Can be changed
- **[[Enumerable]]**: Shows up in `for-in`
- **[[Configurable]]**: Can be deleted/reconfigured

**Built-in Objects**:
- Global object
- Object, Array, String, Number, Boolean
- Function, Date, RegExp, Error
- Math, JSON
- ES6+: Map, Set, Promise, Proxy, Symbol
- Modern: BigInt, TypedArray, ArrayBuffer

---

## Execution Pipeline

### Complete Flow

```
Source Code (String)
    ↓
[Lexer] → Token Stream
    ↓
[Parser] → Abstract Syntax Tree (AST)
    ↓
[Evaluator] → Result Value
    ↓
Output / Side Effects
```

### Detailed Execution Steps

#### 1. **Tokenization (Lexer)**
```cpp
Lexer lexer(source_code);
TokenSequence tokens = lexer.tokenize();
```

**Process**:
- Read character by character
- Group into tokens (keywords, identifiers, literals, operators)
- Track line/column positions
- Handle special cases (regex literals, template strings)

#### 2. **Parsing (Parser)**
```cpp
Parser parser(tokens);
std::unique_ptr<Program> ast = parser.parse_program();
```

**Process**:
- Recursive descent parsing
- Build AST node hierarchy
- Validate syntax
- Report parse errors with location

#### 3. **Evaluation (Interpreter)**
```cpp
Value result = ast->evaluate(context);
```

**Process**:
- Traverse AST recursively
- Each node's `evaluate()` method produces a Value
- Respect execution context (scope, `this`, etc.)
- Handle control flow (return, break, continue, throw)

**Example - Binary Expression**:
```cpp
Value BinaryExpression::evaluate(Context& ctx) {
    Value left_val = left_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    Value right_val = right_->evaluate(ctx);
    if (ctx.has_exception()) return Value();
    
    switch (operator_) {
        case TokenType::PLUS:
            return perform_addition(left_val, right_val);
        case TokenType::MINUS:
            return perform_subtraction(left_val, right_val);
        // ... other operators
    }
}
```

### Control Flow Handling

#### Exception Propagation
```cpp
// Exceptions bubble up through evaluate() calls
Value result = expression->evaluate(ctx);
if (ctx.has_exception()) {
    // Stop evaluation and propagate
    return Value();
}
```

#### Return Statements
```cpp
// Function returns set a flag in context
ctx.set_return_value(value);
// Caller checks the flag and stops evaluation
```

#### Break/Continue
```cpp
// Loop statements check for break/continue flags
if (ctx.has_break()) {
    ctx.clear_break();
    break;
}
```

---

## Memory Management

### Garbage Collection Strategy

**Type**: Mark-and-Sweep with Generational Collection

**Location**: `src/core/gc/GC.h`, `src/core/gc/GC.cpp`

#### Architecture

```
┌──────────────────────────────────────────────┐
│          Garbage Collector                    │
├──────────────────────────────────────────────┤
│  Generational Heap:                          │
│                                              │
│  ┌────────────────┐  ┌─────────────────┐   │
│  │   Young Gen    │  │    Old Gen       │   │
│  │ (Short-lived)  │  │  (Long-lived)    │   │
│  │   Fast GC      │  │   Slow GC        │   │
│  └────────────────┘  └─────────────────┘   │
│           │                   │              │
│           └───────┬───────────┘              │
│                   ▼                          │
│          ┌─────────────────┐                │
│          │  Permanent Gen  │                │
│          │  (Never freed)  │                │
│          └─────────────────┘                │
└──────────────────────────────────────────────┘
```

#### Collection Phases

**1. Mark Phase**
```cpp
// Start from roots (global object, contexts)
void mark_from_roots() {
    for (auto* ctx : root_contexts_) {
        mark_context(ctx);
    }
    for (auto* obj : root_objects_) {
        mark_object(obj);
    }
}
```

**2. Sweep Phase**
```cpp
// Free unmarked objects
void sweep() {
    for (auto* managed : managed_objects_) {
        if (!managed->is_marked) {
            delete managed->object;
            stats_.bytes_freed += managed->size;
        }
    }
}
```

#### Generational Collection

**Young Generation**:
- Newly allocated objects
- Frequent, fast collection
- Most objects die young (generational hypothesis)

**Old Generation**:
- Objects that survive multiple young GC cycles
- Infrequent, thorough collection
- Long-lived objects (global objects, prototypes)

**Permanent Generation**:
- Built-in objects (Object.prototype, Array.prototype, etc.)
- Never collected

#### Collection Modes

```cpp
enum class CollectionMode {
    Manual,       // User triggers with collect_garbage()
    Automatic,    // Triggered by allocation threshold
    Incremental   // Background, time-sliced collection
};
```

#### Optimization Features

- **Parallel Collection**: Multi-threaded marking (future)
- **Incremental Collection**: Avoid long pauses
- **Write Barriers**: Track old→young references
- **Emergency Cleanup**: Prevent out-of-memory

### Memory Allocation Strategy

**Object Pools**: Pre-allocated memory for common objects (future optimization)

**Inline Storage**: Small objects/strings stored inline in Value

**String Interning**: String deduplication (partially implemented)

---

## Data Structures and Representations

### String Table

**Purpose**: Deduplication and fast comparison

**Location**: `include/quanta/lexer/StringTable.h`

Identifier strings are interned for:
- Fast equality checks (pointer comparison)
- Memory efficiency (single copy)
- Symbol implementation

### Hash Maps

**Property Storage**: `std::unordered_map<std::string, Property>`

Fast property access for objects (average O(1))

### Prototype Chains

**Implementation**: Direct pointer chasing
```cpp
Value get_property(const std::string& name) {
    if (properties_.count(name)) {
        return properties_[name].value;
    }
    if (prototype_) {
        return prototype_->get_property(name);  // Chain lookup
    }
    return Value();  // undefined
}
```

---

## Module System

**Location**: `src/core/modules/ModuleLoader.h`

### ES6 Module Support

```javascript
// app.js
import { foo } from './module.js';
export const bar = 42;
```

#### Module Loading Process

```
1. Parse import statement
   ↓
2. Resolve module path
   ↓
3. Check if already loaded (cache)
   ↓
4. Load and parse module file
   ↓
5. Create module context
   ↓
6. Evaluate module
   ↓
7. Export bindings
   ↓
8. Return exported values
```

#### Module Loader Architecture

```cpp
class ModuleLoader {
    std::unordered_map<std::string, std::unique_ptr<Module>> modules_;
    
    Module* load_module(const std::string& module_id, 
                        const std::string& from_path);
    std::string resolve_module_path(const std::string& module_id);
};

class Module {
    std::string id_;
    std::string filename_;
    std::unordered_map<std::string, Value> exports_;
    std::unique_ptr<Context> module_context_;
};
```

**Features**:
- Circular dependency detection
- Module caching (load once)
- Named and default exports
- Namespace imports (`import * as ns`)

---

## Runtime Environment

### Built-in Objects

**Global Object**:
```cpp
void setup_built_in_objects() {
    // Object constructor and prototype
    setup_object_constructor();
    
    // Function constructor
    setup_function_constructor();
    
    // Array, String, Number, Boolean
    setup_array_constructor();
    setup_string_constructor();
    
    // ES6+
    setup_promise_constructor();
    setup_map_set_constructors();
    setup_symbol_constructor();
    
    // Utility objects
    setup_math_object();
    setup_json_object();
}
```

### Exception System

**Error Types**: `Error`, `TypeError`, `ReferenceError`, `RangeError`, `SyntaxError`, `URIError`

**Exception Handling**:
```cpp
// Throw
ctx.throw_exception(Value(error_object));

// Catch
if (ctx.has_exception()) {
    Value exception = ctx.get_exception();
    ctx.clear_exception();
    // Handle error
}
```

**Try-Catch-Finally**:
```cpp
Value TryStatement::evaluate(Context& ctx) {
    Value result = try_block_->evaluate(ctx);
    
    if (ctx.has_exception() && catch_clause_) {
        result = catch_clause_->evaluate(ctx);
    }
    
    if (finally_block_) {
        finally_block_->evaluate(ctx);
    }
    
    return result;
}
```

### Asynchronous Support

**Promises** (`Promise.h`):
- ECMAScript compliant Promise implementation
- State machine: Pending → Fulfilled/Rejected
- Microtask queue for `.then()` callbacks

**Async/Await** (`Async.h`):
- Syntactic sugar over Promises
- State preservation for suspended functions

**Generators** (`Generator.h`):
- Iterator protocol implementation
- `yield` expression support

---

## Performance Considerations

### Current Performance Characteristics

**Interpreter Nature**:
- Tree-walking: O(n) traversal of AST
- No bytecode compilation (yet)
- No JIT optimization

**Trade-offs**:
- Simple, correct implementation
- Easy to debug and understand
- Small code footprint
- Slower than production engines (expected)

### Optimization Strategies (Current)

**1. NaN-Boxing**
- Fast type checks
- Efficient primitive operations
- Cache-friendly memory layout

**2. Inline Caching** (Planned)
- Property access optimization
- Method call optimization

**3. Optimized Math Loops** (Experimental)
```cpp
// Detect simple counting loops
if (is_simple_mathematical_loop(ast)) {
    return execute_optimized_mathematical_loop(ast);
}
```

### Development Philosophy

> "Make it work, make it right, make it fast."
> -Kent Beck

Quanta is currently in the **"make it right"** phase, ensuring spec-correct behavior before pursuing aggressive optimizations.

---

## Contributing to Architecture

When adding new features or modifying architecture:

1. **Consult the ECMAScript spec** — all behavior must match spec algorithms
2. **Write tests first** — use Test262 or write custom tests
3. **Keep it simple** — avoid premature optimization
4. **Document decisions** — explain architectural choices
5. **Maintain separation** — respect component boundaries

---

## References

- [ECMAScript Language Specification](https://tc39.es/ecma262/)
- [Test262 Test Suite](https://github.com/tc39/test262)
- [JavaScript Engines Zoo](https://github.com/ivankra/javascript-zoo)
- [Quanta GitHub Repository](https://github.com/solarbrowser/quanta)

---

This document describes Quanta's architecture as of version 0.1.0. As the project evolves, this documentation will be updated to reflect architectural changes.
